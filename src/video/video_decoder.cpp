#include "video/video_decoder.hpp"

#include "options.hpp"
#include "video/cuda_nv12_texture.hpp"
#include "video/vaapi_nv12_texture.hpp"
#include "video/vulkan_video_texture.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

#if defined(VRP_HAS_FFMPEG)
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libavutil/pixfmt.h>
#include <libavutil/rational.h>
#include <libswscale/swscale.h>
}
#endif

namespace vrp {

std::mutex& cuda_decode_mu() {
  static std::mutex mu;
  return mu;
}

/** Browser preview and seek-thumb strip share libav; do not run them together. */
std::mutex& ffmpeg_offline_mu() {
  static std::mutex mu;
  return mu;
}

namespace {

#if defined(VRP_HAS_FFMPEG)
/** Leak an AVFrame (and its AVVkFrame) instead of unref while GPU may still use it. */
void leak_avframe(AVFrame* f) {
  if (!f) return;
  static std::mutex mu;
  static std::vector<AVFrame*> leaks;
  std::lock_guard<std::mutex> lock(mu);
  leaks.push_back(f);
}

/**
 * avcodec_send_packet can block forever in Vulkan WaitForFences when a prior
 * decode CB is wedged. Run it on a side thread with a deadline so seek_req can run.
 * On timeout the caller must abandon `codec` (leak) — the worker may still be inside it.
 */
int send_packet_timed(AVCodecContext* codec, AVPacket* pkt, int timeout_ms) {
  if (!codec) return AVERROR(EINVAL);
  std::atomic<int> serr{INT_MIN};
  std::atomic<bool> done{false};
  std::thread worker([&]() {
    const int r = avcodec_send_packet(codec, pkt);
    serr.store(r, std::memory_order_release);
    done.store(true, std::memory_order_release);
  });
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (!done.load(std::memory_order_acquire) &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  if (done.load(std::memory_order_acquire)) {
    worker.join();
    return serr.load(std::memory_order_acquire);
  }
  VRP_ERR("seekdbg: avcodec_send_packet timed out after %dms — abandon codec", timeout_ms);
  worker.detach();
  return AVERROR_EXTERNAL;
}

std::string av_err(int err) {
  char buf[AV_ERROR_MAX_STRING_SIZE] = {};
  av_strerror(err, buf, sizeof(buf));
  return buf;
}

/** YUVJ* is deprecated; swscale warns on every getContext unless mapped to YUV* + range. */
AVPixelFormat sws_drop_j(AVPixelFormat fmt, bool* was_full) {
  switch (fmt) {
    case AV_PIX_FMT_YUVJ420P:
      if (was_full) *was_full = true;
      return AV_PIX_FMT_YUV420P;
    case AV_PIX_FMT_YUVJ422P:
      if (was_full) *was_full = true;
      return AV_PIX_FMT_YUV422P;
    case AV_PIX_FMT_YUVJ444P:
      if (was_full) *was_full = true;
      return AV_PIX_FMT_YUV444P;
    case AV_PIX_FMT_YUVJ440P:
      if (was_full) *was_full = true;
      return AV_PIX_FMT_YUV440P;
    default:
      return fmt;
  }
}

void sws_set_range(SwsContext* sws, bool src_full, bool dst_full) {
  if (!sws) return;
  const int* c = sws_getCoefficients(SWS_CS_DEFAULT);
  sws_setColorspaceDetails(sws, c, src_full ? 1 : 0, c, dst_full ? 1 : 0, 0, 1 << 16, 1 << 16);
}

/** sws_getContext that never passes deprecated YUVJ*; applies JPEG range when needed. */
SwsContext* sws_make(int src_w, int src_h, AVPixelFormat src_fmt, int dst_w, int dst_h, AVPixelFormat dst_fmt,
                     int flags, bool src_full = false, bool dst_full = false) {
  bool src_j = false;
  bool dst_j = false;
  const AVPixelFormat s = sws_drop_j(src_fmt, &src_j);
  const AVPixelFormat d = sws_drop_j(dst_fmt, &dst_j);
  SwsContext* sws = sws_getContext(src_w, src_h, s, dst_w, dst_h, d, flags, nullptr, nullptr, nullptr);
  if (!sws) return nullptr;
  const bool sf = src_full || src_j;
  const bool df = dst_full || dst_j;
  if (sf || df) sws_set_range(sws, sf, df);
  return sws;
}

struct HwOpaque {
  AVPixelFormat hw_pix_fmt = AV_PIX_FMT_NONE;
};

int snap_block(double v) {
  int n = static_cast<int>(std::lround(v));
  if (n < 4) return 4;
  const int down = n & ~3;
  if (n - down <= 2) return std::max(4, down);
  return down + 4;
}

// Pixel count matches a long-edge fit of the coded frame. Width and height then
// follow the picture: sample aspect, and the unsqueeze of half SBS / half OU.
void fit_seek_thumb(int src_w, int src_h, double par, int edge_cap, const std::string& path, int& thumb_w,
                    int& thumb_h) {
  double bw = src_w;
  double bh = src_h;
  const double longest = std::max(bw, bh);
  if (longest > edge_cap) {
    const double scale = static_cast<double>(edge_cap) / longest;
    bw *= scale;
    bh *= scale;
  }
  const double pixels = std::max(16.0, bw * bh);
  double frame_aspect = (static_cast<double>(src_w) * par) / static_cast<double>(src_h);
  if (!(frame_aspect > 0.05) || frame_aspect > 50.0) frame_aspect = bw / bh;

  ProjectionMode projection = ProjectionMode::Flat;
  StereoLayout stereo = StereoLayout::Mono;
  bool stereo_full = true;
  detect_format_from_filename(path, projection, stereo, stereo_full);
  (void)projection;
  double stored = frame_aspect;
  if (!stereo_full && stereo == StereoLayout::Sbs) stored = frame_aspect * 2.0;
  else if (!stereo_full && stereo == StereoLayout::OverUnder) stored = frame_aspect * 0.5;
  if (stored < 0.05) stored = 0.05;
  if (stored > 50.0) stored = 50.0;

  thumb_w = snap_block(std::sqrt(pixels * stored));
  thumb_h = snap_block(std::sqrt(pixels / stored));
  constexpr int kSideMax = 8192;
  if (thumb_w > kSideMax || thumb_h > kSideMax) {
    const double scale = static_cast<double>(kSideMax) / static_cast<double>(std::max(thumb_w, thumb_h));
    thumb_w = snap_block(thumb_w * scale);
    thumb_h = snap_block(thumb_h * scale);
  }
}

enum AVPixelFormat get_hw_format(AVCodecContext* ctx, const enum AVPixelFormat* pix_fmts) {
  auto* hw = static_cast<HwOpaque*>(ctx->opaque);
  if (!hw) return AV_PIX_FMT_NONE;
  for (const enum AVPixelFormat* p = pix_fmts; *p != AV_PIX_FMT_NONE; ++p) {
    if (*p == hw->hw_pix_fmt) return *p;
  }
  // Vulkan/CUDA hwaccel init can fail after open2 ("Failed setup for format …").
  // Returning NONE yields "no frame!"; pick a software format so the caller can
  // detect non-hw frames and fall back instead of stalling the stream.
  for (const enum AVPixelFormat* p = pix_fmts; *p != AV_PIX_FMT_NONE; ++p) {
    if (*p != hw->hw_pix_fmt) {
      VRP_ERR("get_hw_format: %s unavailable — falling back to pix_fmt=%d",
              av_get_pix_fmt_name(hw->hw_pix_fmt) ? av_get_pix_fmt_name(hw->hw_pix_fmt) : "?",
              static_cast<int>(*p));
      return *p;
    }
  }
  return AV_PIX_FMT_NONE;
}

struct JpegStill {
  AVCodecContext* ctx = nullptr;
  AVFrame* yuv = nullptr;
  AVPacket* pkt = nullptr;
  SwsContext* sws = nullptr;
  int w = 0;
  int h = 0;
  int64_t pts = 0;

  ~JpegStill() { reset(); }

  void reset() {
    if (sws) sws_freeContext(sws);
    sws = nullptr;
    if (pkt) av_packet_free(&pkt);
    if (yuv) av_frame_free(&yuv);
    if (ctx) avcodec_free_context(&ctx);
    w = h = 0;
  }

  bool open(int width, int height) {
    reset();
    if (width < 2 || height < 2) return false;
    const AVCodec* enc = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
    if (!enc) return false;
    ctx = avcodec_alloc_context3(enc);
    if (!ctx) return false;
    ctx->width = width;
    ctx->height = height;
    // YUV420P + JPEG range (not deprecated YUVJ420P — avoids swscaler log spam).
    ctx->pix_fmt = AV_PIX_FMT_YUV420P;
    ctx->time_base = AVRational{1, 25};
    ctx->color_range = AVCOL_RANGE_JPEG;
    ctx->strict_std_compliance = FF_COMPLIANCE_UNOFFICIAL;
    ctx->flags |= AV_CODEC_FLAG_QSCALE;
    ctx->global_quality = FF_QP2LAMBDA * kSeekThumbJpegQscale;
    if (avcodec_open2(ctx, enc, nullptr) < 0) {
      reset();
      return false;
    }
    yuv = av_frame_alloc();
    pkt = av_packet_alloc();
    if (!yuv || !pkt) {
      reset();
      return false;
    }
    yuv->format = AV_PIX_FMT_YUV420P;
    yuv->color_range = AVCOL_RANGE_JPEG;
    yuv->width = width;
    yuv->height = height;
    if (av_frame_get_buffer(yuv, 32) < 0) {
      reset();
      return false;
    }
    sws = sws_make(width, height, AV_PIX_FMT_RGBA, width, height, AV_PIX_FMT_YUV420P, SWS_FAST_BILINEAR,
                   /*src_full=*/true, /*dst_full=*/true);
    if (!sws) {
      reset();
      return false;
    }
    w = width;
    h = height;
    return true;
  }

  bool encode(const uint8_t* rgba, int stride, std::vector<uint8_t>& out) {
    out.clear();
    if (!rgba) return false;
    for (int attempt = 0; attempt < 2; ++attempt) {
      if (!ctx || !yuv || !sws) return false;
      if (av_frame_make_writable(yuv) < 0) return false;
      uint8_t* src_slices[4] = {const_cast<uint8_t*>(rgba), nullptr, nullptr, nullptr};
      int src_stride[4] = {stride, 0, 0, 0};
      sws_scale(sws, src_slices, src_stride, 0, h, yuv->data, yuv->linesize);
      // The encoder rejects a repeated pts and then refuses every later frame.
      yuv->pts = pts++;
      yuv->quality = ctx->global_quality;
      out.clear();
      if (avcodec_send_frame(ctx, yuv) < 0) {
        const int rw = w;
        const int rh = h;
        if (attempt == 0 && open(rw, rh)) continue;
        return false;
      }
      while (avcodec_receive_packet(ctx, pkt) == 0) {
        out.insert(out.end(), pkt->data, pkt->data + pkt->size);
        av_packet_unref(pkt);
      }
      if (!out.empty()) return true;
    }
    return false;
  }
};

bool decode_seek_jpeg(const uint8_t* data, size_t n, std::vector<uint8_t>& rgba, int& w, int& h) {
  rgba.clear();
  w = h = 0;
  if (!data || n == 0) return false;
  const AVCodec* dec = avcodec_find_decoder(AV_CODEC_ID_MJPEG);
  if (!dec) return false;
  AVCodecContext* ctx = avcodec_alloc_context3(dec);
  AVPacket* pkt = av_packet_alloc();
  AVFrame* frame = av_frame_alloc();
  SwsContext* sws = nullptr;
  bool ok = false;
  if (ctx && pkt && frame && avcodec_open2(ctx, dec, nullptr) == 0 && av_new_packet(pkt, static_cast<int>(n)) == 0) {
    std::memcpy(pkt->data, data, n);
    pkt->flags |= AV_PKT_FLAG_KEY;
    if (avcodec_send_packet(ctx, pkt) == 0 && avcodec_receive_frame(ctx, frame) == 0 && frame->width > 0 &&
        frame->height > 0) {
      const bool full = frame->color_range == AVCOL_RANGE_JPEG;
      sws = sws_make(frame->width, frame->height, static_cast<AVPixelFormat>(frame->format), frame->width,
                     frame->height, AV_PIX_FMT_RGBA, SWS_FAST_BILINEAR, full, /*dst_full=*/true);
      if (sws) {
        w = frame->width;
        h = frame->height;
        // SIMD stores spill past a tightly packed row and corrupt the next heap chunk.
        const int row = w * 4;
        const int stride = (row + 63) & ~63;
        std::vector<uint8_t> padded(static_cast<size_t>(stride) * static_cast<size_t>(h) + 64);
        uint8_t* dst_slices[4] = {padded.data(), nullptr, nullptr, nullptr};
        int dst_stride[4] = {stride, 0, 0, 0};
        sws_scale(sws, frame->data, frame->linesize, 0, h, dst_slices, dst_stride);
        rgba.resize(static_cast<size_t>(row) * static_cast<size_t>(h));
        for (int y = 0; y < h; ++y) {
          std::memcpy(rgba.data() + static_cast<size_t>(y) * static_cast<size_t>(row),
                      padded.data() + static_cast<size_t>(y) * static_cast<size_t>(stride),
                      static_cast<size_t>(row));
        }
        ok = true;
      }
    }
  }
  if (sws) sws_freeContext(sws);
  av_frame_free(&frame);
  av_packet_free(&pkt);
  avcodec_free_context(&ctx);
  if (!ok) {
    rgba.clear();
    w = h = 0;
  }
  return ok;
}

const char* hw_type_label(AVHWDeviceType t) {
  switch (t) {
    case AV_HWDEVICE_TYPE_CUDA:
      return "cuda (NVDEC)";
    case AV_HWDEVICE_TYPE_VAAPI:
      return "vaapi";
    case AV_HWDEVICE_TYPE_VDPAU:
      return "vdpau";
    case AV_HWDEVICE_TYPE_VULKAN:
      return "vulkan";
    default:
      return av_hwdevice_get_type_name(t);
  }
}
#endif

std::string format_duration(double sec) {
  if (sec <= 0) return "-";
  const int total = static_cast<int>(sec + 0.5);
  const int h = total / 3600;
  const int m = (total % 3600) / 60;
  const int s = total % 60;
  char buf[32];
  if (h > 0) {
    std::snprintf(buf, sizeof(buf), "%d:%02d:%02d", h, m, s);
  } else {
    std::snprintf(buf, sizeof(buf), "%d:%02d", m, s);
  }
  return buf;
}

std::string format_bitrate(int64_t bps) {
  if (bps <= 0) return "-";
  char buf[32];
  if (bps >= 1'000'000) {
    std::snprintf(buf, sizeof(buf), "%.2f Mbps", bps / 1'000'000.0);
  } else {
    std::snprintf(buf, sizeof(buf), "%.0f kbps", bps / 1000.0);
  }
  return buf;
}

}  // namespace

bool ThumbnailStrip::sample_at(double sec, std::vector<uint8_t>& out_rgba, int& w, int& h) const {
  out_rgba.clear();
  w = h = 0;
#if !defined(VRP_HAS_FFMPEG)
  (void)sec;
  return false;
#else
  if (!ready || count <= 0 || jpeg.empty()) return false;
  int idx = 0;
  if (interval_sec > 0) idx = static_cast<int>(sec / interval_sec + 0.5);
  idx = std::clamp(idx, 0, count - 1);
  if (idx >= static_cast<int>(jpeg.size()) || jpeg[static_cast<size_t>(idx)].empty()) return false;
  const std::vector<uint8_t>& blob = jpeg[static_cast<size_t>(idx)];
  return decode_seek_jpeg(blob.data(), blob.size(), out_rgba, w, h);
#endif
}

std::string VideoInfo::summary_line() const {
  if (!ok) {
    if (!error.empty()) return "再生不可: " + error;
    return "動画情報なし";
  }
  std::ostringstream oss;
  oss << codec;
  if (!profile.empty()) oss << " (" << profile << ")";
  oss << "  " << width << "x" << height;
  if (display_width > 0 && (display_width != width || display_height != height)) {
    oss << "->" << display_width << "x" << display_height;
  }
  if (fps > 0) {
    char fps_buf[24];
    std::snprintf(fps_buf, sizeof(fps_buf), "  %.3g fps", fps);
    oss << fps_buf;
  }
  oss << "  " << format_duration(duration_sec);
  if (bitrate > 0) oss << "  " << format_bitrate(bitrate);
  if (!pixel_format.empty()) oss << "  " << pixel_format;
  if (!container.empty()) oss << "  [" << container << "]";
  if (!hwaccel.empty()) oss << "  hw=" << hwaccel;
  return oss.str();
}

ThumbnailStrip build_thumbnail_strip(const std::string& path, int max_thumbs, int thumb_w, int thumb_h,
                                     SeekThumbStep on_thumb, SeekThumbRawStep on_raw, int max_edge,
                                     bool allow_hwaccel) {
  ThumbnailStrip strip;
  strip.thumb_w = thumb_w;
  strip.thumb_h = thumb_h;
  strip.status = "生成失敗";
#if !defined(VRP_HAS_FFMPEG)
  (void)path;
  (void)max_thumbs;
  (void)max_edge;
  (void)allow_hwaccel;
  strip.status = "FFmpeg なし";
  return strip;
#else
  std::lock_guard<std::mutex> offline(ffmpeg_offline_mu());
  AVFormatContext* fmt = nullptr;
  if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) < 0) {
    strip.status = "ファイルを開けません";
    return strip;
  }
  if (avformat_find_stream_info(fmt, nullptr) < 0) {
    avformat_close_input(&fmt);
    strip.status = "ストリーム情報なし";
    return strip;
  }
  const int vstream = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
  if (vstream < 0) {
    avformat_close_input(&fmt);
    strip.status = "動画ストリームなし";
    return strip;
  }
  AVStream* st = fmt->streams[vstream];
  double duration = 0;
  if (st->duration > 0 && st->time_base.den > 0) {
    duration = static_cast<double>(st->duration) * av_q2d(st->time_base);
  } else if (fmt->duration > 0) {
    duration = static_cast<double>(fmt->duration) / AV_TIME_BASE;
  }
  if (duration <= 0) duration = 60.0;
  strip.duration_sec = duration;

  // 0 means "fit the video". Pixel count stays on the long-edge budget; the
  // sides follow the picture aspect, including half SBS / half OU unsqueeze.
  if (thumb_w <= 0 || thumb_h <= 0) {
    const int src_w = st->codecpar->width > 0 ? st->codecpar->width : 1280;
    const int src_h = st->codecpar->height > 0 ? st->codecpar->height : 720;
    int edge_cap = max_edge > 0 ? max_edge : kSeekThumbMaxEdge;
    if (edge_cap < kSeekThumbEdgeMin) edge_cap = kSeekThumbEdgeMin;
    if (edge_cap > kSeekThumbEdgeMax) edge_cap = kSeekThumbEdgeMax;
    double par = 1.0;
    const AVRational sar = st->sample_aspect_ratio.num > 0 ? st->sample_aspect_ratio
                                                           : st->codecpar->sample_aspect_ratio;
    if (sar.num > 0 && sar.den > 0) par = av_q2d(sar);
    fit_seek_thumb(src_w, src_h, par, edge_cap, path, thumb_w, thumb_h);
  }
  strip.thumb_w = thumb_w;
  strip.thumb_h = thumb_h;

  max_thumbs = std::max(kSeekThumbCountMin, std::min(max_thumbs, kSeekThumbCountMax));
  strip.interval_sec = duration / static_cast<double>(max_thumbs);
  if (strip.interval_sec < 1.0) strip.interval_sec = 1.0;
  const int count = std::min(max_thumbs, std::max(1, static_cast<int>(duration / strip.interval_sec) + 1));

  const AVCodec* dec = avcodec_find_decoder(st->codecpar->codec_id);
  if (!dec) {
    avformat_close_input(&fmt);
    strip.status = "デコーダなし";
    return strip;
  }
  AVCodecContext* codec = avcodec_alloc_context3(dec);
  avcodec_parameters_to_context(codec, st->codecpar);
  codec->thread_count = 1;

  // Same NVDEC device type as playback. The frame is downloaded only after decode,
  // so the 8K HEVC parse stays on the GPU. Software remains the fallback.
  HwOpaque hw_opaque{};
  AVBufferRef* hw_device = nullptr;
  bool nvdec = false;
  AVPixelFormat hw_pix = AV_PIX_FMT_NONE;
  if (allow_hwaccel) {
    for (int i = 0;; ++i) {
      const AVCodecHWConfig* cfg = avcodec_get_hw_config(dec, i);
      if (!cfg) break;
      if ((cfg->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) &&
          cfg->device_type == AV_HWDEVICE_TYPE_CUDA) {
        hw_pix = cfg->pix_fmt;
        break;
      }
    }
  }
  if (hw_pix != AV_PIX_FMT_NONE) {
    std::lock_guard<std::mutex> lock(cuda_decode_mu());
    if (av_hwdevice_ctx_create(&hw_device, AV_HWDEVICE_TYPE_CUDA, nullptr, nullptr, 0) >= 0) {
      hw_opaque.hw_pix_fmt = hw_pix;
      codec->opaque = &hw_opaque;
      codec->get_format = get_hw_format;
      codec->hw_device_ctx = av_buffer_ref(hw_device);
      if (avcodec_open2(codec, dec, nullptr) >= 0) {
        nvdec = true;
        VRP_LOG("Thumbnail hwaccel: cuda (NVDEC)");
      } else {
        av_buffer_unref(&codec->hw_device_ctx);
        av_buffer_unref(&hw_device);
        codec->hw_device_ctx = nullptr;
        codec->opaque = nullptr;
        codec->get_format = avcodec_default_get_format;
      }
    }
  }
  if (!nvdec) codec->get_format = avcodec_default_get_format;
  if (!nvdec && avcodec_open2(codec, dec, nullptr) < 0) {
    av_buffer_unref(&hw_device);
    avcodec_free_context(&codec);
    avformat_close_input(&fmt);
    strip.status = "デコーダ open 失敗";
    return strip;
  }
  if (!nvdec) VRP_LOG("Thumbnail hwaccel: software");

  AVFrame* frame = av_frame_alloc();
  AVFrame* sw_frame = av_frame_alloc();
  AVFrame* rgba = av_frame_alloc();
  AVPacket* packet = av_packet_alloc();
  SwsContext* sws = nullptr;
  JpegStill enc;
  const size_t thumb_bytes = static_cast<size_t>(thumb_w) * static_cast<size_t>(thumb_h) * 4;
  std::vector<uint8_t> packed(thumb_bytes);
  strip.jpeg.reserve(static_cast<size_t>(count));
  strip.times_sec.reserve(static_cast<size_t>(count));

  auto* rgba_buf = static_cast<uint8_t*>(av_malloc(av_image_get_buffer_size(AV_PIX_FMT_RGBA, thumb_w, thumb_h, 1)));
  av_image_fill_arrays(rgba->data, rgba->linesize, rgba_buf, AV_PIX_FMT_RGBA, thumb_w, thumb_h, 1);
  const bool enc_ok = static_cast<bool>(on_raw) || enc.open(thumb_w, thumb_h);
  int made = 0;
  bool stop = !enc_ok;

  auto store = [&](double t) -> bool {
    if (on_raw) {
      if (!on_raw(made, t, count, strip.interval_sec, thumb_w, thumb_h, packed.data(), packed.size())) {
        stop = true;
        return false;
      }
      strip.times_sec.push_back(t);
      return true;
    }
    std::vector<uint8_t> jpeg;
    if (!enc.encode(packed.data(), thumb_w * 4, jpeg) || jpeg.empty()) return false;
    strip.jpeg.push_back(std::move(jpeg));
    strip.times_sec.push_back(t);
    const std::vector<uint8_t>& blob = strip.jpeg.back();
    if (on_thumb &&
        !on_thumb(static_cast<int>(strip.jpeg.size()) - 1, t, count, strip.interval_sec, thumb_w, thumb_h,
                  blob.data(), blob.size())) {
      stop = true;
    }
    return true;
  };

  for (int i = 0; i < count && !stop; ++i) {
    const double t = std::min(duration, i * strip.interval_sec);
    const int64_t ts = static_cast<int64_t>(t / av_q2d(st->time_base));
    av_seek_frame(fmt, vstream, ts, AVSEEK_FLAG_BACKWARD);
    if (nvdec) {
      std::lock_guard<std::mutex> lock(cuda_decode_mu());
      avcodec_flush_buffers(codec);
    } else {
      avcodec_flush_buffers(codec);
    }

    bool got = false;
    while (av_read_frame(fmt, packet) >= 0) {
      if (packet->stream_index != vstream) {
        av_packet_unref(packet);
        continue;
      }
      std::unique_lock<std::mutex> cuda_lock(cuda_decode_mu(), std::defer_lock);
      if (nvdec) cuda_lock.lock();
      if (avcodec_send_packet(codec, packet) < 0) {
        av_packet_unref(packet);
        continue;
      }
      av_packet_unref(packet);
      while (avcodec_receive_frame(codec, frame) == 0) {
        AVFrame* src = frame;
        if (nvdec && frame->format == hw_pix) {
          av_frame_unref(sw_frame);
          if (av_hwframe_transfer_data(sw_frame, frame, 0) < 0) {
            VRP_LOG("Thumbnail NVDEC download failed");
            continue;
          }
          src = sw_frame;
        }
        if (cuda_lock.owns_lock()) cuda_lock.unlock();
        if (!sws) {
          const bool full = src->color_range == AVCOL_RANGE_JPEG;
          sws = sws_make(src->width, src->height, static_cast<AVPixelFormat>(src->format), thumb_w, thumb_h,
                         AV_PIX_FMT_RGBA, SWS_FAST_BILINEAR, full, /*dst_full=*/true);
          if (!sws) break;
        }
        sws_scale(sws, src->data, src->linesize, 0, src->height, rgba->data, rgba->linesize);
        for (int y = 0; y < thumb_h; ++y) {
          std::memcpy(packed.data() + static_cast<size_t>(y * thumb_w * 4), rgba->data[0] + y * rgba->linesize[0],
                      static_cast<size_t>(thumb_w * 4));
        }
        if (store(t)) ++made;
        got = true;
        break;
      }
      if (got || stop) break;
    }
    if (!got && !stop) {
      std::memset(packed.data(), 0, thumb_bytes);
      if (store(t)) ++made;
    }
    // Software strip seeks the same file Vulkan Video is demuxing (often USB).
    // Yield so playback read-ahead / decode timelines keep moving.
    if (!nvdec && !stop) {
      std::this_thread::sleep_for(std::chrono::milliseconds(80));
    }
  }

  strip.count = made;
  strip.ready = made > 0;
  if (on_raw) {
    const int blocks_x = (thumb_w + 3) / 4;
    const int blocks_y = (thumb_h + 3) / 4;
    const double mb = static_cast<double>(made) * blocks_x * blocks_y * 8.0 / (1024.0 * 1024.0);
    char buf[96];
    std::snprintf(buf, sizeof(buf), "%d 枚 / %.1fs 間隔 (%dx%d, BC1 %.0fMB%s)", made, strip.interval_sec, thumb_w,
                  thumb_h, mb, nvdec ? ", NVDEC" : "");
    strip.status = buf;
  } else if (enc_ok) {
    size_t jpeg_bytes = 0;
    for (const std::vector<uint8_t>& blob : strip.jpeg) jpeg_bytes += blob.size();
    char buf[96];
    std::snprintf(buf, sizeof(buf), "%d 枚 / %.1fs 間隔 (%dx%d, %.1fMB%s)", made, strip.interval_sec, thumb_w,
                  thumb_h, static_cast<double>(jpeg_bytes) / (1024.0 * 1024.0), nvdec ? ", NVDEC" : "");
    strip.status = buf;
  }

  if (sws) sws_freeContext(sws);
  av_free(rgba_buf);
  av_frame_free(&rgba);
  av_frame_free(&sw_frame);
  av_frame_free(&frame);
  av_packet_free(&packet);
  avcodec_free_context(&codec);
  av_buffer_unref(&hw_device);
  avformat_close_input(&fmt);
  VRP_LOG("Thumbnail strip: %s — %s", path.c_str(), strip.status.c_str());
  return strip;
#endif
}

bool extract_video_preview(const std::string& path, int max_edge, std::vector<uint8_t>& out_rgba, int& out_w,
                           int& out_h) {
  out_rgba.clear();
  out_w = out_h = 0;
#if !defined(VRP_HAS_FFMPEG)
  (void)path;
  (void)max_edge;
  return false;
#else
  std::lock_guard<std::mutex> offline(ffmpeg_offline_mu());
  max_edge = std::max(64, std::min(max_edge, 2048));
  AVFormatContext* fmt = nullptr;
  if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) < 0) return false;
  if (avformat_find_stream_info(fmt, nullptr) < 0) {
    avformat_close_input(&fmt);
    return false;
  }
  const int vstream = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
  if (vstream < 0) {
    avformat_close_input(&fmt);
    return false;
  }
  AVStream* st = fmt->streams[vstream];
  double duration = 0;
  if (st->duration > 0 && st->time_base.den > 0) {
    duration = static_cast<double>(st->duration) * av_q2d(st->time_base);
  } else if (fmt->duration > 0) {
    duration = static_cast<double>(fmt->duration) / AV_TIME_BASE;
  }
  const double t = duration > 1.0 ? duration * 0.1 : 0.0;

  const AVCodec* dec = avcodec_find_decoder(st->codecpar->codec_id);
  if (!dec) {
    avformat_close_input(&fmt);
    return false;
  }
  AVCodecContext* codec = avcodec_alloc_context3(dec);
  if (!codec) {
    avformat_close_input(&fmt);
    return false;
  }
  if (avcodec_parameters_to_context(codec, st->codecpar) < 0) {
    avcodec_free_context(&codec);
    avformat_close_input(&fmt);
    return false;
  }
  codec->thread_count = 1;
  codec->get_format = avcodec_default_get_format;
  // Browser thumbs stay on the CPU path — never attach NVDEC here.
  codec->hw_device_ctx = nullptr;
  if (avcodec_open2(codec, dec, nullptr) < 0) {
    avcodec_free_context(&codec);
    avformat_close_input(&fmt);
    return false;
  }
  if (t > 0 && st->time_base.den > 0) {
    const int64_t ts = static_cast<int64_t>(t / av_q2d(st->time_base));
    av_seek_frame(fmt, vstream, ts, AVSEEK_FLAG_BACKWARD);
    avcodec_flush_buffers(codec);
  }

  AVFrame* frame = av_frame_alloc();
  AVFrame* rgba = av_frame_alloc();
  AVPacket* packet = av_packet_alloc();
  if (!frame || !rgba || !packet) {
    av_frame_free(&rgba);
    av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_free_context(&codec);
    avformat_close_input(&fmt);
    return false;
  }
  SwsContext* sws = nullptr;
  uint8_t* rgba_buf = nullptr;
  int dst_w = 0;
  int dst_h = 0;
  int rgba_buf_size = 0;

  bool ok = false;
  bool scale_failed = false;
  while (av_read_frame(fmt, packet) >= 0 && !scale_failed) {
    if (packet->stream_index != vstream) {
      av_packet_unref(packet);
      continue;
    }
    if (avcodec_send_packet(codec, packet) < 0) {
      av_packet_unref(packet);
      continue;
    }
    av_packet_unref(packet);
    while (avcodec_receive_frame(codec, frame) == 0) {
      if (frame->width < 2 || frame->height < 2 || frame->format < 0 || !frame->data[0]) continue;
      // Reject hardware frames — preview is software-only.
      if (frame->format == AV_PIX_FMT_CUDA || frame->format == AV_PIX_FMT_VAAPI ||
          frame->format == AV_PIX_FMT_DRM_PRIME || frame->format == AV_PIX_FMT_VULKAN) {
        continue;
      }
      if (!sws) {
        dst_w = std::max(2, frame->width);
        dst_h = std::max(2, frame->height);
        const int long_edge = std::max(dst_w, dst_h);
        if (long_edge > max_edge) {
          const double s = static_cast<double>(max_edge) / static_cast<double>(long_edge);
          dst_w = std::max(2, static_cast<int>(std::lround(dst_w * s)));
          dst_h = std::max(2, static_cast<int>(std::lround(dst_h * s)));
        }
        dst_w &= ~1;
        dst_h &= ~1;
        rgba_buf_size = av_image_get_buffer_size(AV_PIX_FMT_RGBA, dst_w, dst_h, 1);
        if (rgba_buf_size <= 0) {
          scale_failed = true;
          break;
        }
        rgba_buf = static_cast<uint8_t*>(av_malloc(static_cast<size_t>(rgba_buf_size)));
        if (!rgba_buf) {
          scale_failed = true;
          break;
        }
        if (av_image_fill_arrays(rgba->data, rgba->linesize, rgba_buf, AV_PIX_FMT_RGBA, dst_w, dst_h, 1) < 0) {
          av_free(rgba_buf);
          rgba_buf = nullptr;
          scale_failed = true;
          break;
        }
        const bool full = frame->color_range == AVCOL_RANGE_JPEG;
        sws = sws_make(frame->width, frame->height, static_cast<AVPixelFormat>(frame->format), dst_w, dst_h,
                       AV_PIX_FMT_RGBA, SWS_BILINEAR, full, /*dst_full=*/true);
        if (!sws) {
          av_free(rgba_buf);
          rgba_buf = nullptr;
          scale_failed = true;
          break;
        }
      }
      if (sws_scale(sws, frame->data, frame->linesize, 0, frame->height, rgba->data, rgba->linesize) <= 0) {
        continue;
      }
      if (!rgba->data[0] || rgba->linesize[0] < dst_w * 4) continue;
      const size_t row = static_cast<size_t>(dst_w) * 4u;
      const size_t need = row * static_cast<size_t>(dst_h);
      try {
        out_rgba.resize(need);
      } catch (...) {
        scale_failed = true;
        break;
      }
      for (int y = 0; y < dst_h; ++y) {
        std::memcpy(out_rgba.data() + static_cast<size_t>(y) * row, rgba->data[0] + y * rgba->linesize[0],
                    row);
      }
      out_w = dst_w;
      out_h = dst_h;
      ok = true;
      break;
    }
    if (ok || scale_failed) break;
  }

  if (sws) sws_freeContext(sws);
  av_free(rgba_buf);
  av_frame_free(&rgba);
  av_frame_free(&frame);
  av_packet_free(&packet);
  avcodec_free_context(&codec);
  avformat_close_input(&fmt);
  return ok;
#endif
}

struct VideoDecoder::FFmpegState {
#if defined(VRP_HAS_FFMPEG)
  AVFormatContext* fmt = nullptr;
  AVCodecContext* codec = nullptr;
  SwsContext* sws = nullptr;
  AVPixelFormat sws_src_fmt = AV_PIX_FMT_NONE;
  int sws_src_w = 0;
  int sws_src_h = 0;
  AVFrame* frame = nullptr;
  AVFrame* sw_frame = nullptr;
  AVFrame* rgba = nullptr;
  AVPacket* packet = nullptr;
  AVBufferRef* hw_device_ctx = nullptr;
  HwOpaque hw_opaque{};
  int video_stream = -1;
#endif
};

VideoDecoder::VideoDecoder() : ff_(std::make_unique<FFmpegState>()) {
#if defined(VRP_HAS_FFMPEG)
  has_ffmpeg_ = true;
#else
  has_ffmpeg_ = false;
#endif
}

VideoDecoder::~VideoDecoder() { close(); }

bool VideoDecoder::available() const { return has_ffmpeg_; }

bool VideoDecoder::gpu_path_active() const {
  return cuda_tex_ && cuda_tex_->enabled() && info_.hwaccel_active &&
         info_.hwaccel.find("cuda") != std::string::npos;
}

void VideoDecoder::set_vulkan_texture(VulkanVideoTexture* tex) {
  if (vulkan_tex_ && vulkan_tex_ != tex) vulkan_tex_->set_seek_interrupt(nullptr);
  vulkan_tex_ = tex;
  if (tex) tex->set_seek_interrupt(&seek_req_);
}

bool VideoDecoder::vulkan_path_active() const {
  // Do NOT require has_valid_frame() — release_held() clears it during scrub/seek and
  // the host would leave the Vulkan path (vulkan=0) until a present succeeds.
  return vulkan_tex_ && vulkan_tex_->enabled() && info_.hwaccel_active &&
         info_.hwaccel.find("vulkan") != std::string::npos;
}

bool VideoDecoder::vaapi_path_active() const {
  return vaapi_tex_ && vaapi_tex_->has_valid_frame() && info_.hwaccel_active &&
         info_.hwaccel.find("vaapi") != std::string::npos;
}

void VideoDecoder::choose_display_size() {
  display_w_ = width_;
  display_h_ = height_;
  info_.display_note.clear();
  // Native resolution when GPU NV12 / AVVkFrame path is available; otherwise cap for CPU RGBA.
  if (vulkan_tex_ && vulkan_tex_->enabled() && info_.hwaccel.find("vulkan") != std::string::npos) {
    info_.display_note = "FFmpeg Vulkan Video zero-copy (AVVkFrame)";
    info_.display_width = display_w_;
    info_.display_height = display_h_;
    VRP_LOG("display path: Vulkan Video -> AVVkFrame zero-copy %dx%d", display_w_, display_h_);
    return;
  }
  if (cuda_tex_ && cuda_tex_->enabled() && info_.hwaccel.find("cuda") != std::string::npos) {
    info_.display_note = "Vulkan NV12 zero-copy (native)";
    info_.display_width = display_w_;
    info_.display_height = display_h_;
    VRP_LOG("display path: NVDEC -> Vulkan NV12 zero-copy %dx%d", display_w_, display_h_);
    return;
  }
  if (info_.hwaccel.find("vaapi") != std::string::npos && vaapi_tex_ && vaapi_tex_->enabled()) {
    // Same native-resolution contract as CUDA NV12 — do not apply the CPU RGBA pixel cap.
    info_.display_note = "VA-API → Vulkan NV12 zero-copy";
    info_.display_width = display_w_;
    info_.display_height = display_h_;
    VRP_LOG("display path: VA-API -> Vulkan NV12 zero-copy %dx%d", display_w_, display_h_);
    return;
  }
  if (info_.hwaccel.find("vaapi") != std::string::npos) {
    info_.display_note = "VA-API -> CPU RGBA";
  }
  constexpr int kMaxPixels = 3840 * 2160;
  const int64_t pixels = static_cast<int64_t>(width_) * height_;
  if (pixels > kMaxPixels && width_ > 0 && height_ > 0) {
    const double scale = std::sqrt(static_cast<double>(kMaxPixels) / static_cast<double>(pixels));
    display_w_ = std::max(2, (static_cast<int>(width_ * scale) + 1) & ~1);
    display_h_ = std::max(2, (static_cast<int>(height_ * scale) + 1) & ~1);
    char buf[192];
    std::snprintf(buf, sizeof(buf), "display capped %dx%d (CPU path, source %dx%d)", display_w_,
                  display_h_, width_, height_);
    if (info_.display_note.empty()) info_.display_note = buf;
    else info_.display_note += std::string(" — ") + buf;
    VRP_LOG("%s", info_.display_note.c_str());
  } else if (info_.hwaccel.find("vaapi") != std::string::npos) {
    VRP_LOG("display path: VA-API -> CPU RGBA %dx%d (dma-buf import unavailable)", display_w_,
            display_h_);
  } else if (!info_.hwaccel_active || info_.hwaccel.find("software") != std::string::npos) {
    VRP_LOG("display path: software decode -> CPU RGBA %dx%d", display_w_, display_h_);
  }
  info_.display_width = display_w_;
  info_.display_height = display_h_;
}

void VideoDecoder::clear_packet_queue() {
#if defined(VRP_HAS_FFMPEG)
  std::lock_guard<std::mutex> lk(pkt_mu_);
  for (void* p : pkt_q_) {
    auto* pkt = static_cast<AVPacket*>(p);
    av_packet_free(&pkt);
  }
  pkt_q_.clear();
  pkt_bytes_ = 0;
  pkt_cv_.notify_all();
#else
  std::lock_guard<std::mutex> lk(pkt_mu_);
  pkt_q_.clear();
  pkt_bytes_ = 0;
#endif
}

size_t VideoDecoder::prefetch_bytes_for_seconds(double sec, int64_t bitrate) {
  // Cap read-ahead at ~2s; resume thresholds use ~1s of the same scale.
  constexpr double kRefSec = 2.0;
  constexpr size_t kMinAt2s = 4u << 20;   // 4 MiB
  constexpr size_t kMaxAt2s = 24u << 20;  // 24 MiB
  const double scale = std::clamp(sec, 0.25, kRefSec) / kRefSec;
  const size_t min_b = std::max(size_t{256u << 10}, static_cast<size_t>(kMinAt2s * scale));
  const size_t max_b = std::max(min_b, static_cast<size_t>(kMaxAt2s * scale));
  if (bitrate > 0) {
    return static_cast<size_t>(
        std::clamp(static_cast<double>(bitrate) / 8.0 * sec,
                   static_cast<double>(min_b), static_cast<double>(max_b)));
  }
  return std::max(min_b, static_cast<size_t>((8u << 20) * scale));
}

bool VideoDecoder::prefetch_enough(size_t need_bytes) const {
  std::lock_guard<std::mutex> lk(pkt_mu_);
  if (demux_eof_.load()) return true;
  constexpr size_t kMaxPackets = 400;
  return pkt_bytes_ >= need_bytes || pkt_q_.size() >= kMaxPackets;
}

void VideoDecoder::begin_priming(size_t need_bytes) {
  playing_.store(false);
  priming_need_bytes_.store(need_bytes);
  priming_.store(true);
  playback_started_.store(false);
  pkt_cv_.notify_all();
}

void VideoDecoder::set_seek_scrubbing(bool active) {
  const bool was = seek_scrubbing_.exchange(active);
  if (!was && active) {
    // Entering key-repeat: pause output; stay priming until scrub ends + 1s fill.
    if (playing_.load() || priming_.load()) {
      begin_priming(prefetch_resume_bytes_);
    }
  } else if (was && !active) {
    // Key-repeat ended — fill ~1s then resume playback.
    if (playing_.load() || priming_.load()) {
      begin_priming(prefetch_resume_bytes_);
    } else {
      pkt_cv_.notify_all();
    }
  }
}

void VideoDecoder::start_decode_thread() {
  stop_decode_thread();
  decode_stop_.store(false);
  demux_stop_.store(false);
  demux_pause_.store(false);
  demux_eof_.store(false);
  demux_thread_ = std::thread([this] { demux_loop(); });
  decode_thread_ = std::thread([this] { decode_loop(); });
}

void VideoDecoder::stop_decode_thread() {
  decode_stop_.store(true);
  demux_stop_.store(true);
  demux_pause_.store(true);
  io_abort_.store(true);
  pkt_cv_.notify_all();
  if (vulkan_tex_) vulkan_tex_->set_present_abort(true);
  {
    std::lock_guard<std::mutex> fl(frame_mu_);
    consumed_seq_ = latest_seq_;
  }
  if (demux_thread_.joinable()) demux_thread_.join();
  if (decode_thread_.joinable()) decode_thread_.join();
  clear_packet_queue();
  decode_stop_.store(false);
  demux_stop_.store(false);
  demux_pause_.store(false);
  io_abort_.store(false);
  if (vulkan_tex_) vulkan_tex_->set_present_abort(false);
}

int VideoDecoder::ffmpeg_io_interrupt(void* opaque) {
  auto* self = static_cast<VideoDecoder*>(opaque);
  if (!self) return 1;
  return (self->demux_stop_.load() || self->decode_stop_.load() || self->io_abort_.load()) ? 1 : 0;
}

void VideoDecoder::install_io_interrupt() {
#if defined(VRP_HAS_FFMPEG)
  if (!ff_ || !ff_->fmt) return;
  ff_->fmt->interrupt_callback.callback = &VideoDecoder::ffmpeg_io_interrupt;
  ff_->fmt->interrupt_callback.opaque = this;
#endif
}

void VideoDecoder::demux_loop() {
#if !defined(VRP_HAS_FFMPEG)
  while (!demux_stop_.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
#else
  constexpr size_t kMaxPackets = 400;
  constexpr size_t kHoldMaxBytes = 512u * 1024u;  // tiny queue while key-repeat scrubbing
  constexpr size_t kHoldMaxPackets = 3;
  while (!demux_stop_.load()) {
    if (!open_.load() || demux_pause_.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(3));
      continue;
    }
    if (demux_eof_.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      continue;
    }

    // Cap prefetch while scrubbing or any seek is in flight (avoid q=400 during
    // wait_avframe stalls that block apply).
    const bool hold = seek_scrubbing_.load() || seek_busy_.load() || seek_req_.load();
    const size_t target_bytes = hold ? kHoldMaxBytes : prefetch_target_bytes_;
    const size_t max_pkts = hold ? kHoldMaxPackets : kMaxPackets;

    if (priming_.load() && !hold) {
      const size_t need = priming_need_bytes_.load();
      if (prefetch_enough(need > 0 ? need : prefetch_resume_bytes_)) {
        priming_.store(false);
        playback_started_.store(true);
      }
    }

    {
      std::unique_lock<std::mutex> lk(pkt_mu_);
      if (pkt_bytes_ >= target_bytes || pkt_q_.size() >= max_pkts) {
        pkt_cv_.wait_for(lk, std::chrono::milliseconds(8));
        continue;
      }
    }

    const uint64_t serial = seek_serial_.load();
    AVPacket* pkt = av_packet_alloc();
    if (!pkt) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      continue;
    }

    int r = -1;
    const auto read_t0 = std::chrono::steady_clock::now();
    {
      std::lock_guard<std::mutex> lock(ff_mu_);
      if (demux_stop_.load() || demux_pause_.load() || !ff_ || !ff_->fmt) {
        av_packet_free(&pkt);
        continue;
      }
      r = av_read_frame(ff_->fmt, pkt);
    }
    {
      const auto read_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - read_t0)
                               .count();
      // USB / network storage often stalls here — log without VRP_DEBUG so freezes are traceable.
      if (read_ms >= 40) {
        static std::atomic<int> slow_n{0};
        const int n = slow_n.fetch_add(1) + 1;
        if (n <= 8 || (n % 30) == 0) {
          size_t qn = 0, qb = 0;
          {
            std::lock_guard<std::mutex> lk(pkt_mu_);
            qn = pkt_q_.size();
            qb = pkt_bytes_;
          }
          VRP_LOG("demux: slow av_read_frame %lld ms (#%d) q=%zu bytes=%.2fMiB play=%d scrub=%d",
                  static_cast<long long>(read_ms), n, qn,
                  static_cast<double>(qb) / (1024.0 * 1024.0), playing_.load() ? 1 : 0,
                  seek_scrubbing_.load() ? 1 : 0);
        }
      }
    }

    if (r < 0) {
      av_packet_free(&pkt);
      demux_eof_.store(true);
      pkt_cv_.notify_all();
      continue;
    }

    if (pkt->stream_index != ff_->video_stream) {
      av_packet_unref(pkt);
      av_packet_free(&pkt);
      continue;
    }

    if (serial != seek_serial_.load() || demux_pause_.load()) {
      av_packet_free(&pkt);
      continue;
    }

    {
      std::lock_guard<std::mutex> lk(pkt_mu_);
      if (serial != seek_serial_.load() || demux_pause_.load()) {
        av_packet_free(&pkt);
      } else {
        pkt_bytes_ += static_cast<size_t>(std::max(0, pkt->size));
        pkt_q_.push_back(pkt);
        pkt = nullptr;
      }
    }
    if (pkt) av_packet_free(&pkt);
    pkt_cv_.notify_all();
  }
#endif
}

bool VideoDecoder::pop_video_packet() {
#if !defined(VRP_HAS_FFMPEG)
  return false;
#else
  if (!ff_ || !ff_->packet) return false;
  std::unique_lock<std::mutex> lk(pkt_mu_);
  if (pkt_q_.empty()) {
    if (demux_eof_.load() || demux_pause_.load() || decode_stop_.load()) return false;
    pkt_cv_.wait_for(lk, std::chrono::milliseconds(40), [this] {
      return !pkt_q_.empty() || demux_eof_.load() || demux_pause_.load() || decode_stop_.load();
    });
    if (pkt_q_.empty()) return false;
  }
  auto* pkt = static_cast<AVPacket*>(pkt_q_.front());
  pkt_q_.pop_front();
  pkt_bytes_ -= static_cast<size_t>(std::max(0, pkt->size));
  av_packet_unref(ff_->packet);
  av_packet_move_ref(ff_->packet, pkt);
  av_packet_free(&pkt);
  pkt_cv_.notify_all();
  return true;
#endif
}

void VideoDecoder::decode_loop() {
  using clock = std::chrono::steady_clock;
  auto next_deadline = clock::now();
  bool was_playing = false;
  VideoFrame scratch;
  int seek_skips = 0;
  while (!decode_stop_.load()) {
    if (!open_.load()) {
      next_deadline = clock::now();
      std::this_thread::sleep_for(std::chrono::milliseconds(4));
      continue;
    }
    // When paused, still process seeks and decode one frame for A/V resync / scrub preview.
    if (!playing_.load() && !seek_req_.load() && !awaiting_frame_after_seek_) {
      was_playing = false;
      next_deadline = clock::now();
      std::this_thread::sleep_for(std::chrono::milliseconds(4));
      continue;
    }

    if (consume_seek_abandon()) {
      VRP_LOG("seek: decode abandon awaiting (host timeout/clear)");
      awaiting_frame_after_seek_ = false;
      seek_skips = 0;
      if (vulkan_tex_) vulkan_tex_->set_present_abort(false);
    }

    if (seek_req_.exchange(false)) {
      seek_skips = 0;
      seek_abandon_.store(false);
      if (vulkan_tex_) vulkan_tex_->set_present_abort(false);
      const double t = seek_target_.load();
      const uint64_t issue = seek_issue_gen_.load(std::memory_order_acquire);
      seek_apply_gen_.store(issue, std::memory_order_release);
      set_decode_phase("seek_apply");
      VRP_LOG("seek: decode seek_req target=%.3f scrub=%d play=%d issue=%llu", t,
              seek_scrubbing_.load() ? 1 : 0, playing_.load() ? 1 : 0,
              static_cast<unsigned long long>(issue));
      demux_pause_.store(true);
      seek_serial_.fetch_add(1);
      pkt_cv_.notify_all();
      // Let demux drop any in-flight read before we seek the format context.
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      clear_packet_queue();
      demux_eof_.store(false);
#if defined(VRP_HAS_FFMPEG)
      // Finish in-flight Vulkan decode and return EVERY mailbox ref BEFORE flush.
      // Do not use release_held() here — it may defer front for stereo.
      bool flush_unsafe = false;
      bool superseded = false;
      if (vulkan_tex_ && vulkan_tex_->enabled()) {
        vulkan_tex_->release_all_for_flush();
        if (ff_ && ff_->frame && ff_->frame->format == AV_PIX_FMT_VULKAN &&
            ff_->frame->data[0]) {
          set_decode_phase("seek_pre_flush_wait");
          const char* why = "ok";
          const auto wt0 = clock::now();
          // PowerMizer AUTO: kick_gpu during wait raises clocks. Keep a short
          // last-resort timeout — do NOT host-signal need (wedges later send).
          constexpr uint64_t kPreFlushWaitNs = 2'000'000'000ull;
          bool ok = vulkan_tex_->recycle_frame(ff_->frame, kPreFlushWaitNs, &why);
          const double wms =
              std::chrono::duration<double, std::milli>(clock::now() - wt0).count();
          if (!ok || wms >= 20.0) {
            VRP_LOG("seekdbg: pre_flush recycle ok=%d why=%s %.1fms | %s", ok ? 1 : 0,
                    why ? why : "?", wms, vulkan_tex_->debug_state().c_str());
          }
          if (!ok && why && std::strcmp(why, "seek_intr") == 0) {
            // Newer seek while we waited — do not av_seek/flush/reopen for this stale issue.
            superseded = true;
            VRP_LOG("seekdbg: pre_flush superseded by newer seek_req — requeue");
          } else if (!ok) {
            // Orphan the in-flight AVVkFrame (no unref / no host need-signal) and reopen.
            leak_avframe(ff_->frame);
            ff_->frame = av_frame_alloc();
            flush_unsafe = true;
            VRP_ERR("seekdbg: pre_flush timeout — orphan frame, reopen codec (no force)");
          }
        }
      }
      if (superseded) {
        // Keep demux paused only briefly; re-arm seek_req so the loop applies latest.
        seek_req_.store(true, std::memory_order_release);
        demux_pause_.store(false);
        pkt_cv_.notify_all();
        set_decode_phase("seek_superseded");
        continue;
      }
#endif
      {
        std::lock_guard<std::mutex> lock(ff_mu_);
#if defined(VRP_HAS_FFMPEG)
        if (ff_ && ff_->fmt && ff_->video_stream >= 0) {
          // Prefer latest target if host scrubbed during pre_flush.
          const uint64_t latest_issue = seek_issue_gen_.load(std::memory_order_acquire);
          const double t_apply =
              (latest_issue != issue) ? seek_target_.load(std::memory_order_acquire) : t;
          if (latest_issue != issue) {
            seek_apply_gen_.store(latest_issue, std::memory_order_release);
            seek_req_.store(false, std::memory_order_release);
            VRP_LOG("seekdbg: apply coalesced issue %llu→%llu target=%.3f",
                    static_cast<unsigned long long>(issue),
                    static_cast<unsigned long long>(latest_issue), t_apply);
          }
          // Drop our ref before flush so the pool is not waiting on ff_->frame.
          if (ff_->frame) av_frame_unref(ff_->frame);
          if (ff_->sw_frame) av_frame_unref(ff_->sw_frame);
          AVStream* st = ff_->fmt->streams[ff_->video_stream];
          const double sec =
              std::clamp(t_apply, 0.0, duration_sec_ > 0 ? duration_sec_ : t_apply);
          int64_t ts = static_cast<int64_t>(sec / av_q2d(st->time_base));
          const auto seek_t0 = clock::now();
          set_decode_phase("seek_av_seek");
          av_seek_frame(ff_->fmt, ff_->video_stream, ts, AVSEEK_FLAG_BACKWARD);
          const double seek_ms =
              std::chrono::duration<double, std::milli>(clock::now() - seek_t0).count();
          set_decode_phase("seek_flush");
          const uint64_t applied = seek_apply_gen_.load(std::memory_order_acquire);
          VRP_LOG("seekdbg: enter flush target=%.3f issue=%llu unsafe=%d | %s", sec,
                  static_cast<unsigned long long>(applied), flush_unsafe ? 1 : 0,
                  vulkan_tex_ && vulkan_tex_->enabled() ? vulkan_tex_->debug_state().c_str()
                                                         : "novk");
          const auto flush_t0 = clock::now();
          double flush_ms = 0.0;
          if (flush_unsafe && vulkan_tex_ && vulkan_tex_->enabled() &&
              info_.hwaccel.find("vulkan") != std::string::npos) {
            // Last resort: force-recycle may have failed; still try to close epoch first.
            vulkan_tex_->sync_gpu_for_seek_reset();
            if (reopen_vulkan_codec_keep_hw()) {
              flush_ms =
                  std::chrono::duration<double, std::milli>(clock::now() - flush_t0).count();
              VRP_LOG("seekdbg: reopen codec (skip flush) %.1fms target=%.3f", flush_ms, sec);
            } else {
              VRP_ERR("seekdbg: reopen failed — attempting flush anyway");
              avcodec_flush_buffers(ff_->codec);
              flush_ms =
                  std::chrono::duration<double, std::milli>(clock::now() - flush_t0).count();
            }
          } else {
            avcodec_flush_buffers(ff_->codec);
            flush_ms =
                std::chrono::duration<double, std::milli>(clock::now() - flush_t0).count();
            if (seek_ms >= 20.0 || flush_ms >= 20.0) {
              VRP_LOG("seekdbg: av_seek_frame %.1fms flush %.1fms target=%.3f", seek_ms, flush_ms,
                      sec);
            } else {
              VRP_LOG("seekdbg: flush done %.1fms target=%.3f", flush_ms, sec);
            }
          }
          position_sec_.store(sec);
          if (flush_unsafe && (seek_ms >= 20.0 || flush_ms >= 20.0)) {
            VRP_LOG("seekdbg: av_seek_frame %.1fms recovery %.1fms target=%.3f", seek_ms, flush_ms,
                    sec);
          }
        }
#endif
      }
      demux_pause_.store(false);
      pkt_cv_.notify_all();
      next_deadline = clock::now();
      // Forward scrub keeps the newest PTS already shown, so the re-decode from
      // the keyframe does not replay earlier pictures (the 9↔10 flicker).
      // A backward seek clears it and may show earlier frames again.
      {
        const double applied = position_sec_.load();
        if (!(seek_shown_pts_ >= 0.0 && applied + frame_duration_ >= seek_shown_pts_))
          seek_shown_pts_ = -1.0;
      }
      awaiting_frame_after_seek_ = true;
      set_decode_phase("seek_await");
      {
        std::lock_guard<std::mutex> fl(frame_mu_);
        consumed_seq_ = latest_seq_;
      }
    }

    const auto now = clock::now();
    const double rate = std::clamp(static_cast<double>(rate_.load()), 0.5, 20.0);
    const double step = frame_duration_ / rate;
    const bool playing_now = playing_.load();
    // A pause leaves next_deadline in the past. Catch-up drops would skip the
    // frames queued after a paused seek while audio stays on the still frame.
    if (playing_now && !was_playing) next_deadline = now;
    was_playing = playing_now;
    if (rate_reset_deadline_.exchange(false, std::memory_order_acq_rel)) {
      next_deadline = now;
    }
    // If far behind, drain without RGBA convert (drop frames).
    // After a seek this pacing drain stays off. Frames before the requested time
    // are discarded in decode_next so playback starts at the bar.
    int drops = 0;
    while (!awaiting_frame_after_seek_ &&
           now > next_deadline + std::chrono::duration<double>(step * 1.5) && drops < 45) {
      VideoFrame dump;
      bool ok = false;
      if (!open_.load()) break;
      ok = decode_next(dump, false);
      if (!ok) break;
      next_deadline += std::chrono::duration_cast<clock::duration>(
          std::chrono::duration<double>(step));
      ++drops;
    }

    bool got = false;
    const bool scrub_preview = seek_scrubbing_.load();
    // Never start another decode/present while a newer seek is already armed —
    // return to loop top and consume seek_req first.
    if (seek_req_.load()) {
      next_deadline = clock::now();
      continue;
    }
    if (open_.load() && (playing_.load() || awaiting_frame_after_seek_)) {
      // Scrub shows each newer approach frame (the responsive path). Stop-seek
      // discards until the bar. Frames older than the newest shown PTS are
      // dropped in decode_next so the picture never steps backward.
      const double present_after =
          (awaiting_frame_after_seek_ && seek_skips < 300 && !scrub_preview)
              ? seek_target_.load()
              : -1.0;
      // Discard (present_after / seek bail) uses recycle_frame (wait + consumer +1).
      got = decode_next(scratch, /*convert_rgba=*/true, present_after);
    }
    if (got && awaiting_frame_after_seek_ && scratch.width == 0) {
      ++seek_skips;
      continue;
    }
    if (got) {
      // Newer seek armed while this frame was in flight — do not land on it.
      if (awaiting_frame_after_seek_ && seek_req_.load()) {
        next_deadline = clock::now();
        continue;
      }
      {
        std::lock_guard<std::mutex> fl(frame_mu_);
        latest_ = std::move(scratch);
        ++latest_seq_;
      }
      if (awaiting_frame_after_seek_) {
        const double target = seek_target_.load();
        // Rewind used to use (pts >= target) which instantly "lands" on the old
        // forward frame (e.g. pts=315 target=307) and clears seek_busy early.
        const bool near =
            std::fabs(latest_.pts_sec - target) <= 0.08 || seek_skips >= 300;
        // Vulkan scrub must land a gpu frame — gpu=0 must not clear seek_busy.
        const bool scrub_needs_picture =
            scrub_preview && vulkan_tex_ && vulkan_tex_->enabled();
        if (scrub_preview && (!near || (scrub_needs_picture && !latest_.gpu && seek_skips < 300))) {
          ++seek_skips;
          if ((seek_skips % 30) == 1 || (near && !latest_.gpu)) {
            VRP_LOG("seek: scrub approach pts=%.3f target=%.3f skips=%d gpu=%d near=%d",
                    latest_.pts_sec, target, seek_skips, latest_.gpu ? 1 : 0, near ? 1 : 0);
          }
          next_deadline = clock::now();
          continue;
        }
        // Stop-seek: also require proximity (same fabs rule).
        if (!scrub_preview && !near && seek_skips < 300) {
          ++seek_skips;
          next_deadline = clock::now();
          continue;
        }
        VRP_LOG("seek: decode landed pts=%.3f target=%.3f scrub=%d gpu=%d skips=%d",
                latest_.pts_sec, target, scrub_preview ? 1 : 0, latest_.gpu ? 1 : 0,
                seek_skips);
        seek_skips = 0;
        awaiting_frame_after_seek_ = false;
        seek_busy_.store(false);
        av_resync_.store(true);
        // Start pacing from this frame's presentation time.
        next_deadline = clock::now() + std::chrono::duration_cast<clock::duration>(
                                           std::chrono::duration<double>(step));
      } else {
        seek_skips = 0;
        next_deadline += std::chrono::duration_cast<clock::duration>(
            std::chrono::duration<double>(step));
      }
      auto after = clock::now();
      if (next_deadline > after + std::chrono::milliseconds(2)) {
        std::this_thread::sleep_until(next_deadline);
      } else if (after - next_deadline > std::chrono::milliseconds(250)) {
        next_deadline = after;
      }
    } else {
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
  }
}

void VideoDecoder::fail_open(const std::string& path, const std::string& reason) {
  info_ = {};
  info_.path = path;
  info_.ok = false;
  info_.error = reason;
  info_.hwaccel = "none";
  info_.thumb_status = "-";
  VRP_ERR("再生できません: %s — %s", path.c_str(), reason.c_str());
}

void VideoDecoder::stop_thumbnail_build() {
  thumbs_cancel_.store(true);
  thumbs_gen_.fetch_add(1);
  // Join so a cancelled NVDEC thumb worker cannot hold cuda_decode_mu across an
  // Hz reconnect (detached workers made the next open_media look "not ready").
  if (thumbs_thread_.joinable()) thumbs_thread_.join();
}

void VideoDecoder::disable_thumbnail_strip() {
  stop_thumbnail_build();
  const uint64_t epoch = thumbs_epoch_.fetch_add(1) + 1;
  {
    std::lock_guard<std::mutex> lock(thumbs_mu_);
    thumbs_ = {};
    thumbs_.status = "-";
    info_.thumb_count = 0;
    info_.thumb_status = "-";
  }
  thumbs_ready_.store(false);
  // Full timeline seekable — no cover ramp from a strip worker.
  thumb_cover_sec_.store(duration_sec_ > 0.0 ? duration_sec_ : 1.0e9);
  uint64_t seen = thumbs_done_epoch_.load();
  while (seen < epoch && !thumbs_done_epoch_.compare_exchange_weak(seen, epoch)) {
  }
}

void VideoDecoder::start_thumbnail_build(std::function<void(ThumbnailStrip)> on_done, int max_thumbs,
                                           int thumb_w, int thumb_h, SeekThumbRawStep on_raw, int max_edge,
                                           bool allow_hwaccel) {
  stop_thumbnail_build();
  thumbs_cancel_.store(false);
  thumbs_ready_.store(false);
  thumb_cover_sec_.store(0);
  const uint64_t epoch = thumbs_epoch_.fetch_add(1) + 1;
  {
    std::lock_guard<std::mutex> lock(thumbs_mu_);
    thumbs_ = {};
    thumbs_.status = "生成中…";
  }
  info_.thumb_status = "生成中…";
  info_.thumb_count = 0;
  const std::string path = info_.path;
  if (path.empty()) {
    thumbs_done_epoch_.store(epoch);
    return;
  }

  const uint64_t gen = thumbs_gen_.load();
  thumbs_thread_ = std::thread([this, path, gen, epoch, max_thumbs, thumb_w, thumb_h, max_edge, allow_hwaccel,
                                 on_done = std::move(on_done), on_raw = std::move(on_raw)]() mutable {
    auto step = [this, gen](int index, double time_sec, int planned, double interval, int w, int h,
                            const uint8_t* jpeg, size_t jpeg_bytes) {
      if (thumbs_cancel_.load() || thumbs_gen_.load() != gen || !jpeg || jpeg_bytes == 0 || w <= 0 || h <= 0) {
        return false;
      }
      std::vector<uint8_t> blob(jpeg, jpeg + jpeg_bytes);
      {
        std::lock_guard<std::mutex> lock(thumbs_mu_);
        if (thumbs_.jpeg.size() < static_cast<size_t>(std::max(planned, index + 1))) {
          thumbs_.jpeg.resize(static_cast<size_t>(std::max(planned, index + 1)));
        }
        thumbs_.jpeg[static_cast<size_t>(index)] = std::move(blob);
        if (static_cast<int>(thumbs_.times_sec.size()) == index) thumbs_.times_sec.push_back(time_sec);
        thumbs_.count = index + 1;
        thumbs_.thumb_w = w;
        thumbs_.thumb_h = h;
        thumbs_.interval_sec = interval;
      }
      thumb_cover_sec_.store(time_sec);
      return true;
    };
    auto raw_step = [this, gen, on_raw](int index, double time_sec, int planned, double interval, int w, int h,
                                        const uint8_t* rgba, size_t rgba_bytes) {
      if (thumbs_cancel_.load() || thumbs_gen_.load() != gen || !rgba || rgba_bytes == 0 || w <= 0 || h <= 0) {
        return false;
      }
      {
        std::lock_guard<std::mutex> lock(thumbs_mu_);
        if (static_cast<int>(thumbs_.times_sec.size()) == index) thumbs_.times_sec.push_back(time_sec);
        thumbs_.count = index + 1;
        thumbs_.thumb_w = w;
        thumbs_.thumb_h = h;
        thumbs_.interval_sec = interval;
      }
      thumb_cover_sec_.store(time_sec);
      (void)planned;
      return on_raw && on_raw(index, time_sec, planned, interval, w, h, rgba, rgba_bytes);
    };
    ThumbnailStrip strip =
        on_raw ? build_thumbnail_strip(path, max_thumbs, thumb_w, thumb_h, {}, raw_step, max_edge, allow_hwaccel)
               : build_thumbnail_strip(path, max_thumbs, thumb_w, thumb_h, step, {}, max_edge, allow_hwaccel);
    if (thumbs_cancel_.load() || thumbs_gen_.load() != gen) return;
    const bool ready = strip.ready;
    const int count = strip.count;
    const double duration = strip.duration_sec;
    const std::string status = strip.status;
    {
      std::lock_guard<std::mutex> lock(thumbs_mu_);
      thumbs_ = std::move(strip);
      info_.thumb_count = count;
      info_.thumb_status = status;
    }
    thumbs_ready_.store(ready);
    // A finished strip covers the whole file. A failed build must not leave seek stuck.
    thumb_cover_sec_.store(ready && duration > 0 ? duration : std::max(duration, 1.0e9));
    uint64_t seen = thumbs_done_epoch_.load();
    while (seen < epoch && !thumbs_done_epoch_.compare_exchange_weak(seen, epoch)) {
    }
    if (on_done) {
      ThumbnailStrip brief;
      brief.ready = ready;
      brief.count = count;
      brief.status = status;
      on_done(std::move(brief));
    }
  });
}

bool VideoDecoder::copy_thumb(double sec, int& index, VideoFrame& out) const {
  std::vector<uint8_t> jpeg;
  int idx = 0;
  {
    std::lock_guard<std::mutex> lock(thumbs_mu_);
    if (thumbs_.count <= 0 || thumbs_.jpeg.empty() || thumbs_.thumb_w <= 0 || thumbs_.thumb_h <= 0) return false;
    if (thumbs_.interval_sec > 0) idx = static_cast<int>(sec / thumbs_.interval_sec);
    idx = std::clamp(idx, 0, thumbs_.count - 1);
    if (idx == index) return false;
    if (idx >= static_cast<int>(thumbs_.jpeg.size()) || thumbs_.jpeg[static_cast<size_t>(idx)].empty()) return false;
    jpeg = thumbs_.jpeg[static_cast<size_t>(idx)];
  }
  std::vector<uint8_t> rgba;
  int w = 0;
  int h = 0;
#if defined(VRP_HAS_FFMPEG)
  const bool ok = decode_seek_jpeg(jpeg.data(), jpeg.size(), rgba, w, h);
#else
  const bool ok = false;
  (void)jpeg;
#endif
  index = idx;
  if (!ok) return false;
  out.rgba = std::move(rgba);
  out.width = w;
  out.height = h;
  out.pts_sec = sec;
  return true;
}

ThumbnailStrip VideoDecoder::thumbnails_copy() const {
  std::lock_guard<std::mutex> lock(thumbs_mu_);
  return thumbs_;
}

bool VideoDecoder::open(const std::string& path, bool allow_hwaccel) {
  close();
  info_ = {};
  info_.path = path;
  info_.hwaccel = "software";
  info_.thumb_status = "未生成";
#if !defined(VRP_HAS_FFMPEG)
  (void)allow_hwaccel;
  fail_open(path, "このビルドに FFmpeg が含まれていません（devel パッケージを入れて再ビルド）");
  return false;
#else
  int err = 0;
  io_abort_.store(false);
  VRP_LOG("decoder.open: begin %s", path.c_str());
  {
    // Cap probe so switching high-bitrate files does not stall the XR thread for seconds.
    AVDictionary* open_opts = nullptr;
    av_dict_set(&open_opts, "probesize", "262144", 0);            // 256 KiB
    av_dict_set(&open_opts, "analyzeduration", "500000", 0);      // 0.5s
    // Local USB can block forever without rw_timeout; keep it modest for XR.
    av_dict_set(&open_opts, "rw_timeout", "5000000", 0);  // 5s µs
    if (path.rfind("http://", 0) == 0 || path.rfind("https://", 0) == 0) {
      av_dict_set(&open_opts, "rw_timeout", "8000000", 0);  // 8s µs
      av_dict_set(&open_opts, "reconnect", "1", 0);
      av_dict_set(&open_opts, "reconnect_streamed", "1", 0);
      av_dict_set(&open_opts, "user_agent", "monaSphere/1.0", 0);
      // Recorders often need a longer analyze window for MPEG-TS.
      av_dict_set(&open_opts, "analyzeduration", "2000000", 0);
      av_dict_set(&open_opts, "probesize", "1048576", 0);
    }
    // Install interrupt before open_input so USB stalls are abortable.
    ff_->fmt = avformat_alloc_context();
    if (!ff_->fmt) {
      av_dict_free(&open_opts);
      fail_open(path, "AVFormatContext を確保できません");
      return false;
    }
    install_io_interrupt();
    err = avformat_open_input(&ff_->fmt, path.c_str(), nullptr, &open_opts);
    av_dict_free(&open_opts);
  }
  if (err < 0) {
    fail_open(path, "ファイルを開けません: " + av_err(err));
    return false;
  }
  install_io_interrupt();
  VRP_LOG("decoder.open: demux opened, find_stream_info…");
  ff_->fmt->probesize = 256 * 1024;
  ff_->fmt->max_analyze_duration = 500 * 1000;  // microseconds
  err = avformat_find_stream_info(ff_->fmt, nullptr);
  if (err < 0) {
    fail_open(path, "ストリーム情報を取得できません: " + av_err(err));
    close();
    return false;
  }
  ff_->video_stream = av_find_best_stream(ff_->fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
  if (ff_->video_stream < 0) {
    fail_open(path, "動画ストリームが見つかりません");
    close();
    return false;
  }
  AVStream* st = ff_->fmt->streams[ff_->video_stream];
  const AVCodec* dec = avcodec_find_decoder(st->codecpar->codec_id);
  if (!dec) {
    const char* id_name = avcodec_get_name(st->codecpar->codec_id);
    fail_open(path, std::string("デコーダが見つかりません (codec=") + (id_name ? id_name : "?") + ")");
    close();
    return false;
  }
  ff_->codec = avcodec_alloc_context3(dec);
  if (!ff_->codec) {
    fail_open(path, "コーデックコンテキストを確保できません");
    close();
    return false;
  }
  err = avcodec_parameters_to_context(ff_->codec, st->codecpar);
  if (err < 0) {
    fail_open(path, "コーデックパラメータの適用に失敗: " + av_err(err));
    close();
    return false;
  }

  // Default: CUDA/NVDEC (NVIDIA standard). Opt-in Vulkan Video via prefer_vulkan_video_
  // (monasphere.conf vulkan_video=1). H.264 still preflight-skips >4096 / level>52.
  bool hw_ok = false;
  AVHWDeviceType try_types_buf[5];
  int n_try = 0;
  const bool vulkan_ok =
      prefer_vulkan_video_ && vulkan_hw_device_ && vulkan_tex_ && vulkan_tex_->enabled();
  if (vulkan_ok) {
    try_types_buf[n_try++] = AV_HWDEVICE_TYPE_VULKAN;
    try_types_buf[n_try++] = AV_HWDEVICE_TYPE_CUDA;
  } else {
    try_types_buf[n_try++] = AV_HWDEVICE_TYPE_CUDA;
  }
  try_types_buf[n_try++] = AV_HWDEVICE_TYPE_VAAPI;
  try_types_buf[n_try++] = AV_HWDEVICE_TYPE_VDPAU;
  try_types_buf[n_try++] = AV_HWDEVICE_TYPE_NONE;
  for (int ti = 0; allow_hwaccel && try_types_buf[ti] != AV_HWDEVICE_TYPE_NONE; ++ti) {
    const AVHWDeviceType type = try_types_buf[ti];
    AVPixelFormat hw_pix = AV_PIX_FMT_NONE;
    for (int i = 0;; ++i) {
      const AVCodecHWConfig* cfg = avcodec_get_hw_config(dec, i);
      if (!cfg) break;
      if ((cfg->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) && cfg->device_type == type) {
        hw_pix = cfg->pix_fmt;
        break;
      }
    }
    if (hw_pix == AV_PIX_FMT_NONE) {
      VRP_DBG("hwaccel skip %s: codec has no HW_DEVICE_CTX config", av_hwdevice_get_type_name(type));
      continue;
    }

    // Preflight (NVIDIA only): Vulkan Video H.264 maxCodedExtent 4096², maxLevel 5.2
    // (ffmpeg -v verbose on RTX 5060 Ti). Plus Constrained Baseline long≥3840 corruption
    // class (vulkan_h264_cb_cuda=1). On AMD/RADV these NVIDIA caps/fingerprints are not
    // applied — open2 / VA-API fallthrough decide.
    constexpr uint32_t kVendorNvidia = 0x10DEu;
    if (type == AV_HWDEVICE_TYPE_VULKAN && dec->id == AV_CODEC_ID_H264 &&
        gpu_vendor_id_ == kVendorNvidia) {
      const int w = ff_->codec->width > 0 ? ff_->codec->width : st->codecpar->width;
      const int h = ff_->codec->height > 0 ? ff_->codec->height : st->codecpar->height;
      const int level = ff_->codec->level > 0 ? ff_->codec->level : st->codecpar->level;
      const int profile = ff_->codec->profile != AV_PROFILE_UNKNOWN ? ff_->codec->profile
                                                                    : st->codecpar->profile;
      const int long_edge = std::max(w, h);
      if (w > 4096 || h > 4096 || level > 52) {
        VRP_LOG("hwaccel skip vulkan: H.264 %dx%d level=%d outside NVIDIA Vulkan Video caps "
                "(≤4096 / level 52; fall through to CUDA)",
                w, h, level);
        continue;
      }
      if (vulkan_h264_cb_cuda_ && profile == AV_PROFILE_H264_CONSTRAINED_BASELINE &&
          long_edge >= 3840) {
        VRP_LOG("hwaccel skip vulkan: H.264 Constrained Baseline %dx%d "
                "(vulkan_h264_cb_cuda=1 NVIDIA fingerprint; fall through to CUDA)",
                w, h);
        continue;
      }
    }

    AVBufferRef* ctx = nullptr;
    std::unique_lock<std::mutex> cuda_lock(cuda_decode_mu(), std::defer_lock);
    if (type == AV_HWDEVICE_TYPE_CUDA) cuda_lock.lock();
    if (type == AV_HWDEVICE_TYPE_VULKAN) {
      if (!vulkan_hw_device_) continue;
      ctx = av_buffer_ref(vulkan_hw_device_);
      if (!ctx) {
        VRP_DBG("hwaccel skip vulkan: av_buffer_ref failed");
        continue;
      }
    } else if (av_hwdevice_ctx_create(&ctx, type, nullptr, nullptr, 0) < 0) {
      VRP_DBG("hwaccel skip %s: av_hwdevice_ctx_create failed", av_hwdevice_get_type_name(type));
      continue;
    }

    ff_->hw_opaque.hw_pix_fmt = hw_pix;
    ff_->codec->opaque = &ff_->hw_opaque;
    ff_->codec->get_format = get_hw_format;
    ff_->codec->hw_device_ctx = av_buffer_ref(ctx);
    ff_->hw_device_ctx = ctx;

    ff_->codec->thread_count = 1;  // safer with hwaccel
    // Vulkan Video DPB is small by default; mailbox front+back plus references
    // can block avcodec_receive_frame (and thus seek_req) without spare slots.
    if (type == AV_HWDEVICE_TYPE_VULKAN) ff_->codec->extra_hw_frames = 32;
    err = avcodec_open2(ff_->codec, dec, nullptr);
    if (err < 0) {
      VRP_DBG("hwaccel skip %s: avcodec_open2 failed (%s)", av_hwdevice_get_type_name(type),
              av_err(err).c_str());
      av_buffer_unref(&ff_->codec->hw_device_ctx);
      av_buffer_unref(&ff_->hw_device_ctx);
      ff_->codec->hw_device_ctx = nullptr;
      ff_->hw_device_ctx = nullptr;
      ff_->codec->opaque = nullptr;
      ff_->codec->get_format = nullptr;
      continue;
    }
    info_.hwaccel = hw_type_label(type);
    info_.hwaccel_active = true;
    hw_ok = true;
    VRP_LOG("Video hwaccel: %s (pix_fmt=%d)", info_.hwaccel.c_str(), static_cast<int>(hw_pix));
    if (type == AV_HWDEVICE_TYPE_VAAPI && vaapi_tex_ && vaapi_tex_->enabled()) {
      VRP_LOG("VA-API: dma-buf import armed (CPU RGBA if the modifier cannot be sampled)");
    }
    if (type == AV_HWDEVICE_TYPE_VULKAN) {
      VRP_LOG("Vulkan Video: decoder open with injected XR hwctx");
    }
    break;
  }

  if (!hw_ok) {
    // HEVC still offers CUDA/VAAPI formats. A null get_format is called and SIGSEGV.
    // Frame threads decode several pictures at once. Slice-only stays on one core.
    ff_->codec->thread_count = 0;
    ff_->codec->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
    ff_->codec->opaque = nullptr;
    ff_->codec->get_format = avcodec_default_get_format;
    err = avcodec_open2(ff_->codec, dec, nullptr);
    if (err < 0) {
      fail_open(path, std::string("デコーダを開けません (") + dec->name + "): " + av_err(err));
      close();
      return false;
    }
    info_.hwaccel = "software";
    info_.hwaccel_active = false;
    VRP_LOG("Video hwaccel: software");
  }

  width_ = ff_->codec->width;
  height_ = ff_->codec->height;
  if (width_ <= 0 || height_ <= 0) {
    fail_open(path, "無効な解像度です");
    close();
    return false;
  }
  choose_display_size();
  if (st->duration > 0 && st->time_base.den > 0) {
    duration_sec_ = static_cast<double>(st->duration) * av_q2d(st->time_base);
  } else if (ff_->fmt->duration > 0) {
    duration_sec_ = static_cast<double>(ff_->fmt->duration) / AV_TIME_BASE;
  }
  double fps = 0.0;
  if (st->avg_frame_rate.num > 0 && st->avg_frame_rate.den > 0) {
    fps = av_q2d(st->avg_frame_rate);
    frame_duration_ = 1.0 / fps;
  } else if (st->r_frame_rate.num > 0 && st->r_frame_rate.den > 0) {
    fps = av_q2d(st->r_frame_rate);
    frame_duration_ = 1.0 / fps;
  }

  ff_->frame = av_frame_alloc();
  ff_->sw_frame = av_frame_alloc();
  ff_->rgba = av_frame_alloc();
  ff_->packet = av_packet_alloc();
  if (!ff_->frame || !ff_->sw_frame || !ff_->rgba || !ff_->packet) {
    fail_open(path, "フレームバッファを確保できません");
    close();
    return false;
  }
  int buf_size = av_image_get_buffer_size(AV_PIX_FMT_RGBA, display_w_, display_h_, 1);
  // GPU NV12 / AVVkFrame paths never fill this; tiny stub so 8K open does not OOM.
  const bool gpu_display =
      (cuda_tex_ && cuda_tex_->enabled() && info_.hwaccel.find("cuda") != std::string::npos) ||
      (vulkan_tex_ && vulkan_tex_->enabled() && info_.hwaccel.find("vulkan") != std::string::npos);
  if (gpu_display) {
    buf_size = av_image_get_buffer_size(AV_PIX_FMT_RGBA, 2, 2, 1);
  }
  auto* buf = static_cast<uint8_t*>(av_malloc(buf_size));
  if (!buf) {
    fail_open(path, "RGBA バッファを確保できません");
    close();
    return false;
  }
  const int rgba_w = gpu_display ? 2 : display_w_;
  const int rgba_h = gpu_display ? 2 : display_h_;
  av_image_fill_arrays(ff_->rgba->data, ff_->rgba->linesize, buf, AV_PIX_FMT_RGBA, rgba_w, rgba_h, 1);

  info_.path = path;
  info_.ok = true;
  info_.error.clear();
  info_.width = width_;
  info_.height = height_;
  info_.fps = fps;
  info_.duration_sec = duration_sec_;
  info_.codec = dec->name ? dec->name : "?";
  info_.codec_long = dec->long_name ? dec->long_name : info_.codec;
  if (ff_->fmt->iformat && ff_->fmt->iformat->name) {
    info_.container = ff_->fmt->iformat->name;
  }
  if (const char* pix = av_get_pix_fmt_name(ff_->codec->pix_fmt)) {
    info_.pixel_format = pix;
  }
  if (ff_->codec->profile != AV_PROFILE_UNKNOWN) {
    if (const char* prof = av_get_profile_name(dec, ff_->codec->profile)) {
      info_.profile = prof;
    } else {
      info_.profile = "profile=" + std::to_string(ff_->codec->profile);
    }
  }
  {
    const AVColorSpace sp = ff_->codec->colorspace != AVCOL_SPC_UNSPECIFIED
                                ? ff_->codec->colorspace
                                : st->codecpar->color_space;
    const AVColorRange rg = ff_->codec->color_range != AVCOL_RANGE_UNSPECIFIED
                                ? ff_->codec->color_range
                                : st->codecpar->color_range;
    const char* spn = av_color_space_name(sp);
    const char* rgn = av_color_range_name(rg);
    if (spn && sp != AVCOL_SPC_UNSPECIFIED) {
      info_.color_space = spn;
      if (rgn && rg != AVCOL_RANGE_UNSPECIFIED) {
        info_.color_space += " ";
        info_.color_space += rgn;
      }
    } else if (!info_.pixel_format.empty()) {
      info_.color_space = info_.pixel_format;
    }
  }
  if (ff_->codec->bit_rate > 0) {
    info_.bitrate = ff_->codec->bit_rate;
  } else if (ff_->fmt->bit_rate > 0) {
    info_.bitrate = ff_->fmt->bit_rate;
  } else if (st->codecpar->bit_rate > 0) {
    info_.bitrate = st->codecpar->bit_rate;
  }
  {
    const int astream = av_find_best_stream(ff_->fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (astream >= 0) {
      AVCodecParameters* ap = ff_->fmt->streams[astream]->codecpar;
      if (const char* an = avcodec_get_name(ap->codec_id)) info_.audio_codec = an;
      info_.audio_sample_rate = ap->sample_rate;
      info_.audio_channels = ap->ch_layout.nb_channels;
    }
  }
  info_.thumb_status = "未生成";

  {
    std::lock_guard<std::mutex> fl(frame_mu_);
    latest_ = {};
    latest_seq_ = 0;
    consumed_seq_ = 0;
  }
  position_sec_.store(0);
  open_.store(true);
  playing_.store(false);
  priming_.store(false);
  playback_started_.store(false);
  seek_busy_.store(false);
  seek_scrubbing_.store(false);
  looped_.store(false);
  prefetch_target_bytes_ = prefetch_bytes_for_seconds(2.0, info_.bitrate);
  prefetch_resume_bytes_ = prefetch_bytes_for_seconds(1.0, info_.bitrate);
  demux_eof_.store(false);
  VRP_LOG("Opened video %s", path.c_str());
  VRP_LOG("  %s", info_.summary_line().c_str());
  VRP_LOG("  read-ahead max %.1f MiB (~2s), resume %.1f MiB (~1s)",
          static_cast<double>(prefetch_target_bytes_) / (1024.0 * 1024.0),
          static_cast<double>(prefetch_resume_bytes_) / (1024.0 * 1024.0));
  start_decode_thread();
  return true;
#endif
}

bool VideoDecoder::reopen_vulkan_codec_keep_hw() {
#if defined(VRP_HAS_FFMPEG)
  if (!ff_ || !ff_->fmt || ff_->video_stream < 0) return false;
  if (!vulkan_hw_device_ && !ff_->hw_device_ctx) return false;
  if (info_.hwaccel.find("vulkan") == std::string::npos) return false;

  AVStream* st = ff_->fmt->streams[ff_->video_stream];
  const AVCodec* dec = avcodec_find_decoder(st->codecpar->codec_id);
  if (!dec) return false;

  AVPixelFormat hw_pix = ff_->hw_opaque.hw_pix_fmt;
  if (hw_pix == AV_PIX_FMT_NONE) hw_pix = AV_PIX_FMT_VULKAN;

  AVBufferRef* device = nullptr;
  if (ff_->hw_device_ctx) device = av_buffer_ref(ff_->hw_device_ctx);
  else if (vulkan_hw_device_) device = av_buffer_ref(vulkan_hw_device_);
  if (!device) return false;

  AVCodecContext* old = ff_->codec;
  ff_->codec = nullptr;
  if (old) {
    // free_context → exec_pool_free → WaitForFences + lock_queue(queue_mu_).
    // If the decode CB is wedged, free never returns; a timed detach would leave
    // queue_mu_ held forever and freeze XR + the new codec. Intentional leak.
    VRP_ERR("seekdbg: leaking old Vulkan codec (skip free_context — avoids queue_mu deadlock)");
    (void)old;
  }

  ff_->codec = avcodec_alloc_context3(dec);
  if (!ff_->codec) {
    av_buffer_unref(&device);
    return false;
  }
  if (avcodec_parameters_to_context(ff_->codec, st->codecpar) < 0) {
    avcodec_free_context(&ff_->codec);
    av_buffer_unref(&device);
    return false;
  }
  ff_->hw_opaque.hw_pix_fmt = hw_pix;
  ff_->codec->opaque = &ff_->hw_opaque;
  ff_->codec->get_format = get_hw_format;
  ff_->codec->hw_device_ctx = av_buffer_ref(device);
  ff_->codec->thread_count = 1;
  ff_->codec->extra_hw_frames = 32;
  const int err = avcodec_open2(ff_->codec, dec, nullptr);
  av_buffer_unref(&device);
  if (err < 0) {
    VRP_ERR("seekdbg: reopen vulkan codec open2 failed: %s", av_err(err).c_str());
    avcodec_free_context(&ff_->codec);
    return false;
  }
  VRP_LOG("seekdbg: vulkan codec reopened (hwctx kept)");
  return true;
#else
  return false;
#endif
}

void VideoDecoder::abort_decode_async() {
  decode_stop_.store(true);
  demux_stop_.store(true);
  io_abort_.store(true);
  demux_pause_.store(true);
  seek_req_.store(false);
  playing_.store(false);
  priming_.store(false);
  pkt_cv_.notify_all();
  if (vulkan_tex_) vulkan_tex_->set_present_abort(true);
}

void VideoDecoder::halt_decode() {
  pause();
  seek_req_.store(false);
  seek_busy_.store(false);
  awaiting_frame_after_seek_ = false;
  stop_decode_thread();
}

void* VideoDecoder::cuda_context() const {
#if defined(VRP_HAS_FFMPEG)
  if (!ff_ || !ff_->hw_device_ctx) return nullptr;
  struct AvCudaDeviceCtx {
    void* cuda_ctx = nullptr;
    void* stream = nullptr;
    void* internal = nullptr;
  };
  auto* hw = reinterpret_cast<AVHWDeviceContext*>(ff_->hw_device_ctx->data);
  if (!hw || hw->type != AV_HWDEVICE_TYPE_CUDA) return nullptr;
  auto* cuc = reinterpret_cast<AvCudaDeviceCtx*>(hw->hwctx);
  return cuc ? cuc->cuda_ctx : nullptr;
#else
  return nullptr;
#endif
}

void VideoDecoder::close() {
  stop_decode_thread();
  stop_thumbnail_build();
  playing_.store(false);
  priming_.store(false);
  playback_started_.store(false);
  seek_scrubbing_.store(false);
  seek_req_.store(false);
  seek_busy_.store(false);
  awaiting_frame_after_seek_ = false;
  open_.store(false);
#if defined(VRP_HAS_FFMPEG)
  std::lock_guard<std::mutex> lock(ff_mu_);
  if (ff_) {
    if (ff_->rgba && ff_->rgba->data[0]) {
      av_free(ff_->rgba->data[0]);
      ff_->rgba->data[0] = nullptr;
    }
    if (ff_->rgba) av_frame_free(&ff_->rgba);
    if (ff_->sw_frame) av_frame_free(&ff_->sw_frame);
    if (ff_->frame) av_frame_free(&ff_->frame);
    if (ff_->packet) av_packet_free(&ff_->packet);
    if (ff_->sws) {
      sws_freeContext(ff_->sws);
      ff_->sws = nullptr;
    }
    // free_context destroys VkVideoSession / NVDEC while hw_device_ctx is still
    // set. Dropping that ref first makes FFmpeg skip the session teardown, and
    // vkDestroyDevice then waits on the leftover driver objects.
    if (ff_->codec) {
      VRP_LOG("decoder.close: free codec hwaccel=%s", info_.hwaccel.c_str());
      avcodec_free_context(&ff_->codec);
    }
    if (ff_->hw_device_ctx) {
      VRP_LOG("decoder.close: unref hw device");
      av_buffer_unref(&ff_->hw_device_ctx);
    }
    if (ff_->fmt) avformat_close_input(&ff_->fmt);
    ff_->sws_src_fmt = AV_PIX_FMT_NONE;
    ff_->sws_src_w = 0;
    ff_->sws_src_h = 0;
    ff_->video_stream = -1;
  }
#endif
  width_ = height_ = display_w_ = display_h_ = 0;
  duration_sec_ = 0;
  position_sec_.store(0);
  thumbs_ready_.store(false);
  thumb_cover_sec_.store(0);
  {
    std::lock_guard<std::mutex> lock(thumbs_mu_);
    thumbs_ = {};
  }
  {
    std::lock_guard<std::mutex> fl(frame_mu_);
    latest_ = {};
    latest_seq_ = consumed_seq_ = 0;
  }
}

void VideoDecoder::play() {
  if (!open_.load()) return;
  if (playing_.load() || priming_.load()) return;
  // halt_decode() joins the thread when the picture is dropped (Live / close).
  // Resume must bring demux and decode back; otherwise play() only sets a flag.
  if (!decode_thread_.joinable()) start_decode_thread();
  seek_scrubbing_.store(false);
  begin_priming(prefetch_target_bytes_);  // fill full ~2s before start
  if (prefetch_enough(prefetch_target_bytes_)) {
    priming_.store(false);
    playback_started_.store(true);
  }
}

void VideoDecoder::start_presentation() {
  if (!open_.load() || playing_.load()) return;
  priming_.store(false);
  playing_.store(true);
}

void VideoDecoder::pause() {
  playing_.store(false);
  priming_.store(false);
}

void VideoDecoder::toggle() {
  if (playing_.load() || priming_.load()) pause();
  else play();
}

void VideoDecoder::set_rate(float rate) {
  rate_.store(std::clamp(rate, 0.5f, 20.f));
  rate_reset_deadline_.store(true, std::memory_order_release);
}

void VideoDecoder::seek_relative(double delta_sec) { seek_to(position_sec_.load() + delta_sec); }

void VideoDecoder::set_decode_phase(const char* phase) {
  decode_phase_.store(phase, std::memory_order_release);
  last_progress_ms_.store(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count(),
      std::memory_order_release);
}

std::string VideoDecoder::seek_debug_state() const {
  size_t qn = 0, qb = 0;
  packet_queue_stats(qn, qb);
  const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now().time_since_epoch())
                             .count();
  const int64_t last = last_progress_ms_.load(std::memory_order_acquire);
  const int64_t stale = last > 0 ? (now_ms - last) : -1;
  const char* phase = decode_phase_.load(std::memory_order_acquire);
  std::ostringstream os;
  os << "phase=" << (phase ? phase : "?")
     << " stale_ms=" << stale
     << " issue=" << seek_issue_gen_.load(std::memory_order_acquire)
     << " apply=" << seek_apply_gen_.load(std::memory_order_acquire)
     << " req=" << (seek_req_.load() ? 1 : 0)
     << " busy=" << (seek_busy_.load() ? 1 : 0)
     << " scrub=" << (seek_scrubbing_.load() ? 1 : 0)
     << " play=" << (playing_.load() ? 1 : 0)
     << " prime=" << (priming_.load() ? 1 : 0)
     << " demux_pause=" << (demux_pause_.load() ? 1 : 0)
     << " eof=" << (demux_eof_.load() ? 1 : 0)
     << " target=" << seek_target_.load()
     << " pos=" << position_sec_.load()
     << " seq=" << latest_seq_  // unlocked; debug snapshot only
     << " q=" << qn << "/" << (qb / 1024) << "KiB";
  if (vulkan_tex_) os << " | " << vulkan_tex_->debug_state();
  return os.str();
}

void VideoDecoder::seek_to(double sec) {
  if (!open_.load()) return;
  if (duration_sec_ > 0) sec = std::clamp(sec, 0.0, duration_sec_);
  else if (sec < 0) sec = 0;
  // Freeze presentation until the host has armed audio at the landed frame.
  // Otherwise video runs while audio is still seeking and the offset sticks.
  playing_.store(false);
  priming_.store(false);
  playback_started_.store(false);
  seek_busy_.store(true);
  seek_target_.store(sec);
  position_sec_.store(sec);
  const uint64_t issue = seek_issue_gen_.fetch_add(1, std::memory_order_acq_rel) + 1;
  seek_req_.store(true);
  av_resync_.store(false);
  VRP_LOG("seekdbg: host seek_to sec=%.3f issue=%llu apply=%llu phase=%s req=1 busy=1 scrub=%d",
          sec, static_cast<unsigned long long>(issue),
          static_cast<unsigned long long>(seek_apply_gen_.load(std::memory_order_acquire)),
          decode_phase_.load(std::memory_order_acquire)
              ? decode_phase_.load(std::memory_order_acquire)
              : "?",
          seek_scrubbing_.load() ? 1 : 0);
}

void VideoDecoder::clear_seek_in_flight() {
  seek_req_.store(false);
  seek_busy_.store(false);
  seek_abandon_.store(true);
  if (vulkan_tex_) vulkan_tex_->set_present_abort(false);
  VRP_LOG("seekdbg: clear_seek_in_flight | %s", seek_debug_state().c_str());
}

void VideoDecoder::force_resume_after_seek() {
  seek_req_.store(false);
  seek_busy_.store(false);
  seek_abandon_.store(true);
  seek_scrubbing_.store(false);
  priming_.store(false);
  playback_started_.store(true);
  // If decode wedged inside seek_av_seek/flush, demux_pause would otherwise stick
  // and starve the pipeline forever after host TIMEOUT.
  demux_pause_.store(false);
  pkt_cv_.notify_all();
  if (vulkan_tex_) vulkan_tex_->set_present_abort(false);
  VRP_LOG("seekdbg: force_resume_after_seek | %s", seek_debug_state().c_str());
}

bool VideoDecoder::consume_seek_abandon() { return seek_abandon_.exchange(false); }

bool VideoDecoder::consume_av_resync() { return av_resync_.exchange(false); }

bool VideoDecoder::consume_playback_started() { return playback_started_.exchange(false); }

bool VideoDecoder::consume_looped() { return looped_.exchange(false); }

bool VideoDecoder::decode_next(VideoFrame& out, bool convert_rgba, double present_after_sec) {
#if !defined(VRP_HAS_FFMPEG)
  (void)convert_rgba;
  (void)present_after_sec;
  out = {};
  out.width = display_w_ > 0 ? display_w_ : 64;
  out.height = display_h_ > 0 ? display_h_ : 64;
  out.rgba.assign(static_cast<size_t>(out.width * out.height * 4), 0);
  for (int i = 0; i < out.width * out.height; ++i) {
    out.rgba[i * 4 + 0] = 180;
    out.rgba[i * 4 + 1] = 40;
    out.rgba[i * 4 + 2] = 180;
    out.rgba[i * 4 + 3] = 255;
  }
  out.pts_sec = position_sec_.load();
  return true;
#else
  if (!ff_ || !ff_->fmt) return false;
  while (!decode_stop_.load() && !demux_pause_.load()) {
    // Host armed a newer seek — return to decode_loop to consume seek_req
    // instead of waiting on the previous frame's GPU semaphore.
    if (seek_req_.load()) {
      set_decode_phase("decode_next_bail_req");
      out = {};
      out.width = 0;
      out.height = 0;
      return true;
    }
    set_decode_phase("pop_packet");
    if (!pop_video_packet()) {
      if (demux_eof_.load()) {
        playing_.store(false);
        seek_busy_.store(true);
        seek_target_.store(0);
        seek_req_.store(true);
        looped_.store(true);
      } else if (playing_.load()) {
        static std::atomic<int> starve_n{0};
        const int n = starve_n.fetch_add(1) + 1;
        if (n <= 6 || (n % 60) == 0) {
          size_t qn = 0, qb = 0;
          packet_queue_stats(qn, qb);
          VRP_LOG("decode: packet starve #%d q=%zu bytes=%.2fMiB eof=%d demux_pause=%d pos=%.3f",
                  n, qn, static_cast<double>(qb) / (1024.0 * 1024.0), demux_eof_.load() ? 1 : 0,
                  demux_pause_.load() ? 1 : 0, position_sec_.load());
        }
      }
      set_decode_phase("pop_empty");
      return false;
    }
    set_decode_phase("ff_mu_lock");
    std::unique_lock<std::mutex> lock(ff_mu_);
    if (!ff_->codec) return false;
    std::unique_lock<std::mutex> cuda_lock(cuda_decode_mu(), std::defer_lock);
    if (ff_->hw_device_ctx) {
      auto* hw = reinterpret_cast<AVHWDeviceContext*>(ff_->hw_device_ctx->data);
      if (hw && hw->type == AV_HWDEVICE_TYPE_CUDA) cuda_lock.lock();
    }

    // Send/receive with EAGAIN retry. Unlock ff_mu_ around send so demux can read;
    // seek apply stays on this thread and only runs after decode_next returns.
    bool packet_sent = false;
    for (int send_tries = 0; send_tries < 8 && !packet_sent; ++send_tries) {
      if (seek_req_.load()) {
        set_decode_phase("send_bail_req");
        out = {};
        out.width = 0;
        out.height = 0;
        return true;
      }
      AVCodecContext* codec = ff_->codec;
      AVPacket* pkt = ff_->packet;
      lock.unlock();
      set_decode_phase("send_packet");
      // Vulkan: bound send — wedged decode CB can block forever in WaitForFences.
      const bool vk_send = vulkan_tex_ && vulkan_tex_->enabled() &&
                           info_.hwaccel.find("vulkan") != std::string::npos;
      constexpr int kSendTimeoutMs = 3000;
      const int serr =
          vk_send ? send_packet_timed(codec, pkt, kSendTimeoutMs) : avcodec_send_packet(codec, pkt);
      lock.lock();
      if (vk_send && serr == AVERROR_EXTERNAL) {
        if (ff_->frame) {
          leak_avframe(ff_->frame);
          ff_->frame = av_frame_alloc();
        }
        ff_->codec = nullptr;  // abandon; worker holds `codec`
        (void)codec;
        av_packet_unref(ff_->packet);
        lock.unlock();
        VRP_ERR("seekdbg: send_packet timeout — kick then reopen");
        for (int i = 0; i < 4 && !decode_stop_.load(); ++i) {
          if (vulkan_tex_) vulkan_tex_->kick_gpu_activity();
          std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        if (decode_stop_.load()) {
          set_decode_phase("send_timeout_halt");
          out = {};
          out.width = 0;
          out.height = 0;
          return false;
        }
        lock.lock();
        vulkan_tex_->sync_gpu_for_seek_reset();
        if (!reopen_vulkan_codec_keep_hw()) {
          VRP_ERR("seekdbg: send_packet timeout reopen failed");
          playing_.store(false);
          seek_busy_.store(false);
          awaiting_frame_after_seek_ = false;
          demux_pause_.store(false);
          pkt_cv_.notify_all();
          set_decode_phase("send_timeout_reopen_fail");
          out = {};
          out.width = 0;
          out.height = 0;
          return true;
        }
        if (seek_busy_.load()) seek_req_.store(true, std::memory_order_release);
        set_decode_phase("send_timeout_reopen");
        out = {};
        out.width = 0;
        out.height = 0;
        return true;
      }
      if (serr == AVERROR(EAGAIN)) {
        set_decode_phase("send_eagain");
        // Must drain output before retrying the same packet.
      } else if (serr < 0) {
        av_packet_unref(ff_->packet);
        packet_sent = true;  // drop; outer loop pops next
        break;
      } else {
        av_packet_unref(ff_->packet);
        packet_sent = true;
      }

      set_decode_phase("receive_frame");
      while (avcodec_receive_frame(ff_->codec, ff_->frame) == 0) {
        set_decode_phase("got_frame");
        AVStream* st = ff_->fmt->streams[ff_->video_stream];
        if (ff_->frame->best_effort_timestamp != AV_NOPTS_VALUE) {
          position_sec_.store(ff_->frame->best_effort_timestamp * av_q2d(st->time_base));
        } else {
          position_sec_.store(position_sec_.load() + frame_duration_);
        }
        out = {};
        out.pts_sec = position_sec_.load();
        out.gpu = false;

        // Vulkan timeline: every received frame must either enter the mailbox
        // (consumer +1 on mailbox return) or recycle_frame (+1 here). Wait-only
        // discard left need=D+2 cur=D and wedged wait_avframe under scrub.
        if (ff_->hw_device_ctx && ff_->frame->format == AV_PIX_FMT_VULKAN && vulkan_tex_ &&
            vulkan_tex_->enabled()) {
          if (vulkan_tex_->present_abort()) {
            set_decode_phase("present_bail_abort");
            out.width = 0;
            out.height = 0;
            return true;
          }
          // Half a frame: 0.04s accepted the previous 60fps picture (~16ms early).
          const double seek_early = std::max(frame_duration_ * 0.5, 0.001);
          const bool skip_before =
              present_after_sec >= 0.0 && out.pts_sec + seek_early < present_after_sec;
          const bool drop_for_seek = seek_req_.load();
          const bool want_mailbox = convert_rgba && !skip_before && !drop_for_seek;
          // Seek land: kick_gpu during wait; short last-resort if still stalled.
          const uint64_t wait_ns =
              seek_busy_.load(std::memory_order_acquire) ? 2'000'000'000ull : 0ull;
          const char* wait_why = "ok";
          set_decode_phase("wait_avframe");
          lock.unlock();
          auto recover_land_timeout = [&]() {
            VRP_ERR("seekdbg: land wait timeout — orphan frame, reopen, re-seek (no force)");
            lock.lock();
            leak_avframe(ff_->frame);
            ff_->frame = av_frame_alloc();
            if (info_.hwaccel.find("vulkan") != std::string::npos) {
              vulkan_tex_->sync_gpu_for_seek_reset();
              (void)reopen_vulkan_codec_keep_hw();
            }
            // Fresh codec has no DPB at target — re-apply seek.
            seek_req_.store(true, std::memory_order_release);
            set_decode_phase("land_reopen_reseek");
            out.width = 0;
            out.height = 0;
          };
          if (!want_mailbox) {
            // Seek armed: do not block apply behind recycle wait — seek_req handler
            // waits for the real GPU timeline in pre_flush, then +1 / flush.
            if (drop_for_seek) {
              lock.lock();
              set_decode_phase("receive_bail_req");
              out.width = 0;
              out.height = 0;
              return true;
            }
            const bool recycled = vulkan_tex_->recycle_frame(ff_->frame, wait_ns, &wait_why);
            if (!recycled && wait_why && std::strcmp(wait_why, "timeout") == 0) {
              recover_land_timeout();
              return true;
            }
            lock.lock();
            if (!recycled) {
              set_decode_phase("present_bail_abort");
              out.width = 0;
              out.height = 0;
              return true;
            }
            if (skip_before) {
              set_decode_phase("present_after_skip");
              out.width = 0;
              out.height = 0;
              return true;
            }
            set_decode_phase("receive_bail_req");
            out.width = 0;
            out.height = 0;
            return true;
          }
          const bool full = ff_->codec && ff_->codec->color_range == AVCOL_RANGE_JPEG;
          // Show newer frames during the seek. Drop a picture older than the
          // newest one already shown (B-frame after its reference → 10 then 9).
          const bool seek_picture =
              awaiting_frame_after_seek_ || seek_busy_.load() || seek_scrubbing_.load();
          if (seek_picture && seek_shown_pts_ >= 0.0 &&
              out.pts_sec + frame_duration_ * 0.5 < seek_shown_pts_) {
            set_decode_phase("present_pts_regress");
            (void)vulkan_tex_->recycle_frame(ff_->frame, wait_ns, &wait_why);
            lock.lock();
            out.width = 0;
            out.height = 0;
            return true;
          }
          const bool presented =
              vulkan_tex_->try_present(ff_->frame, full, wait_ns, &wait_why);
          if (!presented && wait_why && std::strcmp(wait_why, "timeout") == 0 &&
              seek_busy_.load()) {
            recover_land_timeout();
            return true;
          }
          lock.lock();
          if (presented) {
            if (seek_picture && out.pts_sec > seek_shown_pts_) seek_shown_pts_ = out.pts_sec;
            set_decode_phase("presented");
            out.width = width_;
            out.height = height_;
            out.gpu = true;
            out.full_range = vulkan_tex_->full_range();
            return true;
          }
          // seek_intr: GPU has not reached need yet — defer +1 to seek pre_flush.
          if (wait_why && std::strcmp(wait_why, "seek_intr") == 0 && seek_req_.load()) {
            set_decode_phase("present_seek_intr");
            out.width = 0;
            out.height = 0;
            return true;
          }
          // Waited but did not park — must consumer-+1 ourselves (ledger-safe).
          lock.unlock();
          (void)vulkan_tex_->recycle_frame(ff_->frame, wait_ns, &wait_why);
          lock.lock();
          static std::atomic<int> drop_n{0};
          const int n = drop_n.fetch_add(1) + 1;
          if (n <= 8 || (n % 30) == 0 || (wait_why && std::strcmp(wait_why, "ok") != 0)) {
            VRP_LOG("seekdbg: try_present fail #%d why=%s pos=%.3f seek=%d req=%d | %s", n,
                    wait_why ? wait_why : "?", out.pts_sec, seek_busy_.load() ? 1 : 0,
                    seek_req_.load() ? 1 : 0, vulkan_tex_->debug_state().c_str());
          }
          set_decode_phase("present_fail");
          if (seek_busy_.load() || seek_req_.load()) {
            out.width = 0;
            out.height = 0;
            return true;
          }
          out.width = width_;
          out.height = height_;
          return true;
        }

        if (seek_req_.load()) {
          set_decode_phase("receive_bail_req");
          out.width = 0;
          out.height = 0;
          return true;
        }
        if (present_after_sec >= 0.0 &&
            out.pts_sec + std::max(frame_duration_ * 0.5, 0.001) < present_after_sec) {
          set_decode_phase("present_after_skip");
          out.width = 0;
          out.height = 0;
          return true;
        }
        if (!convert_rgba) {
          out.width = width_;
          out.height = height_;
          return true;
        }

        {
          static std::atomic<int> fmt_logs{0};
          const int n = fmt_logs.fetch_add(1);
          if (n < 6) {
            VRP_DBG("decode frame#%d fmt=%d (CUDA=%d) hw_pix=%d data0=%p data1=%p %dx%d cuda_tex=%d", n,
                    ff_->frame->format, static_cast<int>(AV_PIX_FMT_CUDA),
                    static_cast<int>(ff_->hw_opaque.hw_pix_fmt),
                    static_cast<void*>(ff_->frame->data[0]), static_cast<void*>(ff_->frame->data[1]),
                    ff_->frame->width, ff_->frame->height, (cuda_tex_ && cuda_tex_->enabled()) ? 1 : 0);
          }
        }

        // hw=vulkan/cuda claimed but frame is software — never sws_scale 8K (CPU crawl).
        if (ff_->hw_device_ctx && info_.hwaccel_active &&
            (info_.hwaccel.find("vulkan") != std::string::npos ||
             info_.hwaccel.find("cuda") != std::string::npos) &&
            ff_->frame->format != AV_PIX_FMT_VULKAN && ff_->frame->format != AV_PIX_FMT_CUDA &&
            ff_->frame->format != AV_PIX_FMT_VAAPI) {
          static std::atomic<int> bad_fmt_n{0};
          const int n = bad_fmt_n.fetch_add(1) + 1;
          if (n <= 8 || (n % 120) == 0) {
            VRP_ERR("decode: hw=%s but frame pix_fmt=%d (%s) — drop (no 8K CPU RGBA)",
                    info_.hwaccel.c_str(), ff_->frame->format,
                    av_get_pix_fmt_name(static_cast<AVPixelFormat>(ff_->frame->format))
                        ? av_get_pix_fmt_name(static_cast<AVPixelFormat>(ff_->frame->format))
                        : "?");
          }
          out.width = width_;
          out.height = height_;
          return true;
        }

        if (ff_->hw_device_ctx && ff_->frame->format == AV_PIX_FMT_CUDA && cuda_tex_ &&
            cuda_tex_->enabled()) {
          struct AvCudaDeviceCtx {
            void* cuda_ctx = nullptr;
            void* stream = nullptr;
            void* internal = nullptr;
          };
          auto* hw = reinterpret_cast<AVHWDeviceContext*>(ff_->hw_device_ctx->data);
          auto* cuc = reinterpret_cast<AvCudaDeviceCtx*>(hw->hwctx);
          if (!cuc || !cuc->cuda_ctx) {
            static std::atomic<bool> once{false};
            if (!once.exchange(true)) VRP_ERR("decode: AVCUDADeviceContext missing cuda_ctx");
            out.width = width_;
            out.height = height_;
            return true;
          }
          if (cuda_tex_->try_copy_from_cuda(ff_->frame, cuc->cuda_ctx, cuc->stream,
                                            ff_->codec && ff_->codec->color_range == AVCOL_RANGE_JPEG)) {
            out.width = width_;
            out.height = height_;
            out.gpu = true;
            out.full_range = cuda_tex_->full_range();
            {
              static std::atomic<bool> range_logged{false};
              if (!range_logged.exchange(true)) {
                VRP_LOG("NV12 color_range frame=%d codec=%d → %s",
                        static_cast<int>(ff_->frame->color_range),
                        ff_->codec ? static_cast<int>(ff_->codec->color_range) : -1,
                        out.full_range ? "full" : "limited");
              }
            }
            return true;
          }
          out.width = width_;
          out.height = height_;
          return true;
        }

        if (ff_->hw_device_ctx && ff_->frame->format == AV_PIX_FMT_VAAPI && vaapi_tex_ &&
            vaapi_tex_->enabled()) {
          const bool full = ff_->codec && ff_->codec->color_range == AVCOL_RANGE_JPEG;
          if (vaapi_tex_->try_import(ff_->frame, full)) {
            out.width = width_;
            out.height = height_;
            out.gpu = true;
            out.full_range = vaapi_tex_->full_range();
            return true;
          }
        }

        AVFrame* src = ff_->frame;
        if (ff_->hw_device_ctx && ff_->frame->format == ff_->hw_opaque.hw_pix_fmt) {
          if (av_hwframe_transfer_data(ff_->sw_frame, ff_->frame, 0) < 0) {
            VRP_ERR("av_hwframe_transfer_data failed");
            continue;
          }
          src = ff_->sw_frame;
        }
        const AVPixelFormat src_fmt = static_cast<AVPixelFormat>(src->format);
        if (!ff_->sws || ff_->sws_src_fmt != src_fmt || src->width != ff_->sws_src_w ||
            src->height != ff_->sws_src_h) {
          if (ff_->sws) sws_freeContext(ff_->sws);
          const bool full = src->color_range == AVCOL_RANGE_JPEG;
          ff_->sws = sws_make(src->width, src->height, src_fmt, display_w_, display_h_, AV_PIX_FMT_RGBA,
                              SWS_BILINEAR, full, /*dst_full=*/true);
          ff_->sws_src_fmt = src_fmt;
          ff_->sws_src_w = src->width;
          ff_->sws_src_h = src->height;
          if (!ff_->sws) {
            VRP_ERR("sws_getContext failed %dx%d → %dx%d fmt=%d", src->width, src->height, display_w_,
                    display_h_, static_cast<int>(src_fmt));
            continue;
          }
        }
        sws_scale(ff_->sws, src->data, src->linesize, 0, src->height, ff_->rgba->data, ff_->rgba->linesize);
        const size_t need = static_cast<size_t>(display_w_) * static_cast<size_t>(display_h_) * 4;
        out.width = display_w_;
        out.height = display_h_;
        out.rgba.resize(need);
        if (ff_->rgba->linesize[0] == display_w_ * 4) {
          std::memcpy(out.rgba.data(), ff_->rgba->data[0], need);
        } else {
          for (int y = 0; y < display_h_; ++y) {
            std::memcpy(out.rgba.data() + static_cast<size_t>(y * display_w_ * 4),
                        ff_->rgba->data[0] + y * ff_->rgba->linesize[0], static_cast<size_t>(display_w_ * 4));
          }
        }
        return true;
      }
      // EAGAIN path: drained; loop to retry send with same packet.
      if (packet_sent) break;
    }
  }
  return false;
#endif
}

bool VideoDecoder::update(double /*delta_sec*/, VideoFrame& out_frame) {
  if (!open_.load() || !playing_.load()) return false;
  return take_latest(out_frame);
}

uint64_t VideoDecoder::frame_seq() {
  std::lock_guard<std::mutex> lock(frame_mu_);
  return latest_seq_;
}

bool VideoDecoder::take_latest(VideoFrame& out_frame) {
  if (!open_.load()) return false;
  std::lock_guard<std::mutex> lock(frame_mu_);
  if (latest_seq_ == consumed_seq_) return false;
  out_frame = std::move(latest_);
  latest_ = {};
  consumed_seq_ = latest_seq_;
  return out_frame.gpu || !out_frame.rgba.empty();
}

void VideoDecoder::packet_queue_stats(size_t& packets, size_t& bytes) const {
  std::lock_guard<std::mutex> lk(pkt_mu_);
  packets = pkt_q_.size();
  bytes = pkt_bytes_;
}


// --- VideoTexture: ping-pong RGBA, non-blocking upload ---

namespace {

void transition_image(VkCommandBuffer cmd, VkImage image, VkImageLayout old_l, VkImageLayout new_l,
                      VkAccessFlags src_a, VkAccessFlags dst_a, VkPipelineStageFlags src_s,
                      VkPipelineStageFlags dst_s) {
  VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  b.oldLayout = old_l;
  b.newLayout = new_l;
  b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.image = image;
  b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  b.subresourceRange.levelCount = 1;
  b.subresourceRange.layerCount = 1;
  b.srcAccessMask = src_a;
  b.dstAccessMask = dst_a;
  vkCmdPipelineBarrier(cmd, src_s, dst_s, 0, 0, nullptr, 0, nullptr, 1, &b);
}

}  // namespace

void VideoTexture::init(VkPhysicalDevice phys, VkDevice device, VkQueue queue, uint32_t queue_family,
                        std::mutex* queue_mu, bool nearest_filter) {
  phys_ = phys;
  device_ = device;
  queue_ = queue;
  queue_family_ = queue_family;
  queue_mu_ = queue_mu;

  VkCommandPoolCreateInfo cpci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  cpci.queueFamilyIndex = queue_family_;
  VRP_CHECK(vkCreateCommandPool(device_, &cpci, nullptr, &pool_) == VK_SUCCESS, "upload pool");

  VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  ai.commandPool = pool_;
  ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  ai.commandBufferCount = 1;
  VRP_CHECK(vkAllocateCommandBuffers(device_, &ai, &cmd_) == VK_SUCCESS, "upload cmd");

  VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
  VRP_CHECK(vkCreateFence(device_, &fci, nullptr, &fence_) == VK_SUCCESS, "upload fence");

  VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  const VkFilter filter = nearest_filter ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
  sci.magFilter = filter;
  sci.minFilter = filter;
  sci.mipmapMode = nearest_filter ? VK_SAMPLER_MIPMAP_MODE_NEAREST : VK_SAMPLER_MIPMAP_MODE_LINEAR;
  sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  VRP_CHECK(vkCreateSampler(device_, &sci, nullptr, &sampler_) == VK_SUCCESS, "sampler");

  (void)upload_idle_screen();
}

bool VideoTexture::upload_idle_screen() {
  // Soft slate panel — alpha ~0.35 so the sky shows through until a video binds.
  // Prefer the current size when already allocated so we do not destroy images the
  // XR compositor may still reference (live → idle used to force a 2×2 resize).
  VideoFrame ph;
  constexpr uint8_t r = 36, g = 42, b = 56, a = 90;
  if (width_ >= 2 && height_ >= 2 && slots_[0].image && slots_[1].image && slots_[2].image) {
    ph.width = width_;
    ph.height = height_;
    ph.rgba.assign(static_cast<size_t>(ph.width) * static_cast<size_t>(ph.height) * 4u, 0);
    for (size_t i = 0; i + 3 < ph.rgba.size(); i += 4) {
      ph.rgba[i] = r;
      ph.rgba[i + 1] = g;
      ph.rgba[i + 2] = b;
      ph.rgba[i + 3] = a;
    }
  } else {
    ph.width = 2;
    ph.height = 2;
    ph.rgba = {r, g, b, a, r, g, b, a, r, g, b, a, r, g, b, a};
  }
  for (int i = 0; i < 64; ++i) {
    if (try_upload(ph)) break;
    wait_fence();
  }
  for (int i = 0; i < 64; ++i) {
    if (poll_present()) return true;
    wait_fence();
  }
  return view() != VK_NULL_HANDLE;
}

bool VideoTexture::try_upload_spinner(int width, int height, float phase) {
  // Same discrete 8-frame / 120° motif as Live Flat + DLNA menu spinner.
  const int w = std::clamp(width > 0 ? width : 640, 64, 1920);
  const int h = std::clamp(height > 0 ? height : 360, 64, 1080);
  constexpr float kPeriod = 2.09439510239f;
  constexpr int kFrames = 8;
  float p = std::fmod(phase, kPeriod);
  if (p < 0.f) p += kPeriod;
  const int idx =
      std::clamp(static_cast<int>(p / kPeriod * static_cast<float>(kFrames)), 0, kFrames - 1);
  const float snap = kPeriod * static_cast<float>(idx) / static_cast<float>(kFrames);

  VideoFrame ph;
  ph.width = w;
  ph.height = h;
  ph.rgba.assign(static_cast<size_t>(w) * static_cast<size_t>(h) * 4u, 0);
  constexpr uint8_t br = 28, bg = 32, bb = 44, ba = 200;
  for (size_t i = 0; i + 3 < ph.rgba.size(); i += 4) {
    ph.rgba[i] = br;
    ph.rgba[i + 1] = bg;
    ph.rgba[i + 2] = bb;
    ph.rgba[i + 3] = ba;
  }
  const float cx = 0.5f * static_cast<float>(w);
  const float cy = 0.5f * static_cast<float>(h);
  constexpr float orbit = 22.f;
  constexpr float rad = 5.5f;
  const float r2 = rad * rad;
  for (int di = 0; di < 3; ++di) {
    const float a = snap + static_cast<float>(di) * kPeriod;
    const float dx = orbit * std::cos(a);
    const float dy = orbit * std::sin(a);
    const float pulse =
        0.45f + 0.55f * (0.5f + 0.5f * std::sin(snap * 1.7f + static_cast<float>(di)));
    const int alpha = std::clamp(static_cast<int>(70.f + 185.f * pulse), 40, 255);
    const int x0 = std::max(0, static_cast<int>(cx + dx - rad - 1.f));
    const int y0 = std::max(0, static_cast<int>(cy + dy - rad - 1.f));
    const int x1 = std::min(w - 1, static_cast<int>(cx + dx + rad + 1.f));
    const int y1 = std::min(h - 1, static_cast<int>(cy + dy + rad + 1.f));
    for (int y = y0; y <= y1; ++y) {
      for (int x = x0; x <= x1; ++x) {
        const float fx = static_cast<float>(x) + 0.5f - (cx + dx);
        const float fy = static_cast<float>(y) + 0.5f - (cy + dy);
        if (fx * fx + fy * fy > r2) continue;
        const size_t o = (static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)) * 4u;
        ph.rgba[o] = 230;
        ph.rgba[o + 1] = 240;
        ph.rgba[o + 2] = 255;
        ph.rgba[o + 3] = static_cast<uint8_t>(alpha);
      }
    }
  }
  return try_upload(ph);
}

void VideoTexture::destroy_busy_atlas() {
  if (!device_) {
    busy_panel_ready_ = false;
    busy_spinners_ready_ = false;
    busy_spinner_idx_ = -1;
    return;
  }
  wait_fence();
  auto destroy_one = [&](Slot& s) {
    if (s.view) vkDestroyImageView(device_, s.view, nullptr);
    if (s.image) vkDestroyImage(device_, s.image, nullptr);
    if (s.memory) vkFreeMemory(device_, s.memory, nullptr);
    s = {};
  };
  destroy_one(busy_panel_);
  for (Slot& s : busy_spinners_) destroy_one(s);
  busy_panel_ready_ = false;
  busy_spinners_ready_ = false;
  busy_spinner_idx_ = -1;
}

bool VideoTexture::ensure_staging(VkDeviceSize bytes) {
  if (bytes <= staging_size_ && staging_mapped_) return true;
  if (staging_mapped_) {
    vkUnmapMemory(device_, staging_mem_);
    staging_mapped_ = nullptr;
  }
  if (staging_) vkDestroyBuffer(device_, staging_, nullptr);
  if (staging_mem_) vkFreeMemory(device_, staging_mem_, nullptr);
  staging_ = VK_NULL_HANDLE;
  staging_mem_ = VK_NULL_HANDLE;
  staging_size_ = 0;
  VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  bci.size = bytes;
  bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
  if (vkCreateBuffer(device_, &bci, nullptr, &staging_) != VK_SUCCESS) return false;
  VkMemoryRequirements breq{};
  vkGetBufferMemoryRequirements(device_, staging_, &breq);
  VkMemoryAllocateInfo bmai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  bmai.allocationSize = breq.size;
  bmai.memoryTypeIndex = find_memory_type(
      breq.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  if (vkAllocateMemory(device_, &bmai, nullptr, &staging_mem_) != VK_SUCCESS) return false;
  vkBindBufferMemory(device_, staging_, staging_mem_, 0);
  staging_size_ = bytes;
  return vkMapMemory(device_, staging_mem_, 0, staging_size_, 0, &staging_mapped_) == VK_SUCCESS;
}

bool VideoTexture::upload_slot_rgba(Slot& slot, int w, int h, const uint8_t* rgba, size_t rgba_bytes) {
  const VkDeviceSize bytes = static_cast<VkDeviceSize>(w) * static_cast<VkDeviceSize>(h) * 4u;
  if (!rgba || rgba_bytes < static_cast<size_t>(bytes) || !ensure_staging(bytes)) return false;
  if (!slot.image && !create_slot(slot, w, h)) return false;
  std::memcpy(staging_mapped_, rgba, static_cast<size_t>(bytes));

  std::unique_lock<std::mutex> qlock;
  if (queue_mu_) qlock = std::unique_lock<std::mutex>(*queue_mu_);
  vkResetCommandBuffer(cmd_, 0);
  VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (vkBeginCommandBuffer(cmd_, &bi) != VK_SUCCESS) return false;
  transition_image(cmd_, slot.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                   VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                   VK_PIPELINE_STAGE_TRANSFER_BIT);
  VkBufferImageCopy copy{};
  copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  copy.imageSubresource.layerCount = 1;
  copy.imageExtent = {static_cast<uint32_t>(w), static_cast<uint32_t>(h), 1};
  vkCmdCopyBufferToImage(cmd_, staging_, slot.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
  transition_image(cmd_, slot.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                   VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                   VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
  if (vkEndCommandBuffer(cmd_) != VK_SUCCESS) return false;
  if (vkResetFences(device_, 1, &fence_) != VK_SUCCESS) return false;
  VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  si.commandBufferCount = 1;
  si.pCommandBuffers = &cmd_;
  if (vkQueueSubmit(queue_, 1, &si, fence_) != VK_SUCCESS) return false;
  wait_fence();
  return true;
}

bool VideoTexture::ensure_busy_spinners(const VideoFrame* frames, int frame_count) {
  if (busy_spinners_ready_) return true;
  if (!frames || frame_count != kBusyFrameCount || !device_ || !cmd_ || !fence_) return false;
  for (int i = 0; i < frame_count; ++i) {
    if (frames[i].width != kBusySpinnerW || frames[i].height != kBusySpinnerH ||
        frames[i].rgba.size() <
            static_cast<size_t>(kBusySpinnerW) * static_cast<size_t>(kBusySpinnerH) * 4u)
      return false;
  }
  wait_fence();
  in_flight_ = false;
  for (int i = 0; i < kBusyFrameCount; ++i) {
    if (!upload_slot_rgba(busy_spinners_[i], kBusySpinnerW, kBusySpinnerH, frames[i].rgba.data(),
                          frames[i].rgba.size())) {
      destroy_busy_atlas();
      return false;
    }
  }
  busy_spinners_ready_ = true;
  busy_spinner_idx_ = -1;
  VRP_LOG("VideoTexture: busy spinners %dx%d × %d in VRAM", kBusySpinnerW, kBusySpinnerH,
          kBusyFrameCount);
  return true;
}

bool VideoTexture::upload_busy_panel(const VideoFrame& panel) {
  if (!device_ || !cmd_ || !fence_) return false;
  if (panel.width != kBusyPanelW || panel.height != kBusyPanelH ||
      panel.rgba.size() < static_cast<size_t>(kBusyPanelW) * static_cast<size_t>(kBusyPanelH) * 4u)
    return false;
  wait_fence();
  in_flight_ = false;
  // Recreate panel slot so a fresh UNDEFINED→upload clears any old spinner pixels.
  if (busy_panel_.image) {
    if (busy_panel_.view) vkDestroyImageView(device_, busy_panel_.view, nullptr);
    if (busy_panel_.image) vkDestroyImage(device_, busy_panel_.image, nullptr);
    if (busy_panel_.memory) vkFreeMemory(device_, busy_panel_.memory, nullptr);
    busy_panel_ = {};
  }
  if (!upload_slot_rgba(busy_panel_, kBusyPanelW, kBusyPanelH, panel.rgba.data(), panel.rgba.size()))
    return false;
  busy_panel_ready_ = true;
  busy_spinner_idx_ = -1;  // force center blit after text/bg rewrite
  return true;
}

VkImageView VideoTexture::present_busy(float phase_rad) {
  if (!busy_ready() || !busy_panel_.image) return VK_NULL_HANDLE;
  constexpr float kPeriod = 2.09439510239f;  // 2π/3 — same as kLiveBusyPeriod
  float p = std::fmod(phase_rad, kPeriod);
  if (p < 0.f) p += kPeriod;
  const int idx =
      std::clamp(static_cast<int>(p / kPeriod * static_cast<float>(kBusyFrameCount)), 0,
                 kBusyFrameCount - 1);
  if (idx == busy_spinner_idx_) return busy_panel_.view;

  // Partial update: only the center spinner rect (above status text).
  constexpr int kOx = (kBusyPanelW - kBusySpinnerW) / 2;
  constexpr int kOy = (kBusyPanelH - kBusySpinnerH) / 2 - 18;

  std::unique_lock<std::mutex> qlock;
  if (queue_mu_) qlock = std::unique_lock<std::mutex>(*queue_mu_);
  wait_fence();
  vkResetCommandBuffer(cmd_, 0);
  VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (vkBeginCommandBuffer(cmd_, &bi) != VK_SUCCESS) return busy_panel_.view;

  transition_image(cmd_, busy_panel_.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_SHADER_READ_BIT,
                   VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                   VK_PIPELINE_STAGE_TRANSFER_BIT);
  transition_image(cmd_, busy_spinners_[idx].image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_SHADER_READ_BIT,
                   VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                   VK_PIPELINE_STAGE_TRANSFER_BIT);

  VkImageCopy region{};
  region.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  region.srcSubresource.layerCount = 1;
  region.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  region.dstSubresource.layerCount = 1;
  region.dstOffset = {kOx, kOy, 0};
  region.extent = {static_cast<uint32_t>(kBusySpinnerW), static_cast<uint32_t>(kBusySpinnerH), 1};
  vkCmdCopyImage(cmd_, busy_spinners_[idx].image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, busy_panel_.image,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

  transition_image(cmd_, busy_panel_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                   VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                   VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
  transition_image(cmd_, busy_spinners_[idx].image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT,
                   VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                   VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

  if (vkEndCommandBuffer(cmd_) != VK_SUCCESS) return busy_panel_.view;
  if (vkResetFences(device_, 1, &fence_) != VK_SUCCESS) return busy_panel_.view;
  VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  si.commandBufferCount = 1;
  si.pCommandBuffers = &cmd_;
  if (vkQueueSubmit(queue_, 1, &si, fence_) != VK_SUCCESS) return busy_panel_.view;
  wait_fence();
  busy_spinner_idx_ = idx;
  return busy_panel_.view;
}

void VideoTexture::shutdown() {
  wait_fence();
  in_flight_ = false;
  destroy_busy_atlas();
  if (staging_mapped_) {
    vkUnmapMemory(device_, staging_mem_);
    staging_mapped_ = nullptr;
  }
  destroy_slots();
  if (staging_) vkDestroyBuffer(device_, staging_, nullptr);
  if (staging_mem_) vkFreeMemory(device_, staging_mem_, nullptr);
  staging_ = VK_NULL_HANDLE;
  staging_mem_ = VK_NULL_HANDLE;
  staging_size_ = 0;
  if (sampler_) vkDestroySampler(device_, sampler_, nullptr);
  sampler_ = VK_NULL_HANDLE;
  if (fence_) vkDestroyFence(device_, fence_, nullptr);
  fence_ = VK_NULL_HANDLE;
  if (cmd_) {
    vkFreeCommandBuffers(device_, pool_, 1, &cmd_);
    cmd_ = VK_NULL_HANDLE;
  }
  if (pool_) vkDestroyCommandPool(device_, pool_, nullptr);
  pool_ = VK_NULL_HANDLE;
}

void VideoTexture::wait_fence() {
  if (!fence_ || !device_) return;
  vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX);
}

bool VideoTexture::fence_ready() const {
  if (!fence_ || !device_) return true;
  return vkGetFenceStatus(device_, fence_) == VK_SUCCESS;
}

uint32_t VideoTexture::find_memory_type(uint32_t type_bits, VkMemoryPropertyFlags props) const {
  VkPhysicalDeviceMemoryProperties mp{};
  vkGetPhysicalDeviceMemoryProperties(phys_, &mp);
  for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
    if ((type_bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & props) == props) return i;
  }
  vrp_fatal("No suitable memory type");
}

void VideoTexture::destroy_slots() {
  if (!device_) return;
  for (Slot& s : slots_) {
    if (s.view) vkDestroyImageView(device_, s.view, nullptr);
    if (s.image) vkDestroyImage(device_, s.image, nullptr);
    if (s.memory) vkFreeMemory(device_, s.memory, nullptr);
    s = {};
  }
  for (uint64_t& seq : slot_seq_) seq = 0;
  upload_seq_ = 0;
  display_ = 0;
  prev_display_ = 0;
  write_ = 1;
  valid_ = false;
}

bool VideoTexture::create_slot(Slot& slot, int width, int height) {
  VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ici.imageType = VK_IMAGE_TYPE_2D;
  ici.format = VK_FORMAT_R8G8B8A8_SRGB;
  ici.extent = {static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1};
  ici.mipLevels = 1;
  ici.arrayLayers = 1;
  ici.samples = VK_SAMPLE_COUNT_1_BIT;
  ici.tiling = VK_IMAGE_TILING_OPTIMAL;
  ici.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
              VK_IMAGE_USAGE_SAMPLED_BIT;
  ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (vkCreateImage(device_, &ici, nullptr, &slot.image) != VK_SUCCESS) return false;

  VkMemoryRequirements req{};
  vkGetImageMemoryRequirements(device_, slot.image, &req);
  VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  mai.allocationSize = req.size;
  mai.memoryTypeIndex = find_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (vkAllocateMemory(device_, &mai, nullptr, &slot.memory) != VK_SUCCESS) {
    vkDestroyImage(device_, slot.image, nullptr);
    slot.image = VK_NULL_HANDLE;
    return false;
  }
  vkBindImageMemory(device_, slot.image, slot.memory, 0);

  VkImageViewCreateInfo ivci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  ivci.image = slot.image;
  ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
  ivci.format = VK_FORMAT_R8G8B8A8_SRGB;
  ivci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  ivci.subresourceRange.levelCount = 1;
  ivci.subresourceRange.layerCount = 1;
  if (vkCreateImageView(device_, &ivci, nullptr, &slot.view) != VK_SUCCESS) {
    vkFreeMemory(device_, slot.memory, nullptr);
    vkDestroyImage(device_, slot.image, nullptr);
    slot = {};
    return false;
  }
  return true;
}

void VideoTexture::ensure_size(int width, int height) {
  if (width == width_ && height == height_ && slots_[0].image && slots_[1].image &&
      slots_[2].image)
    return;
  wait_fence();
  in_flight_ = false;
  // Recreated images are UNDEFINED until the next upload finishes — force the sync path.
  valid_ = false;
  rebind_ = false;
  destroy_slots();
  width_ = width;
  height_ = height;
  for (int i = 0; i < kSlots; ++i) {
    slot_seq_[i] = 0;
    slot_fp_[i] = 0;
  }
  upload_seq_ = 0;
  display_ = 0;
  prev_display_ = 0;
  write_ = 1;
  VRP_CHECK(create_slot(slots_[0], width, height), "video A");
  VRP_CHECK(create_slot(slots_[1], width, height), "video B");
  VRP_CHECK(create_slot(slots_[2], width, height), "video C");

  const VkDeviceSize need = static_cast<VkDeviceSize>(width) * height * 4;
  if (need > staging_size_) {
    if (staging_mapped_) {
      vkUnmapMemory(device_, staging_mem_);
      staging_mapped_ = nullptr;
    }
    if (staging_) vkDestroyBuffer(device_, staging_, nullptr);
    if (staging_mem_) vkFreeMemory(device_, staging_mem_, nullptr);
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = need;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VRP_CHECK(vkCreateBuffer(device_, &bci, nullptr, &staging_) == VK_SUCCESS, "staging");
    VkMemoryRequirements breq{};
    vkGetBufferMemoryRequirements(device_, staging_, &breq);
    VkMemoryAllocateInfo bmai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    bmai.allocationSize = breq.size;
    bmai.memoryTypeIndex =
        find_memory_type(breq.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VRP_CHECK(vkAllocateMemory(device_, &bmai, nullptr, &staging_mem_) == VK_SUCCESS, "staging mem");
    vkBindBufferMemory(device_, staging_, staging_mem_, 0);
    staging_size_ = need;
    VRP_CHECK(vkMapMemory(device_, staging_mem_, 0, staging_size_, 0, &staging_mapped_) == VK_SUCCESS,
              "staging map");
  }
}

void VideoTexture::invalidate_content() {
  if (in_flight_ && device_ && fence_) {
    (void)vkWaitForFences(device_, 1, &fence_, VK_TRUE, 100'000'000ull);
    in_flight_ = false;
  }
  valid_ = false;
  width_ = 0;
  height_ = 0;
  for (int i = 0; i < kSlots; ++i) {
    slot_seq_[i] = 0;
    slot_fp_[i] = 0;
  }
  upload_seq_ = 0;
  rebind_ = true;
}

bool VideoTexture::poll_present() {
  bool changed = false;
  if (in_flight_ && fence_ready()) {
    const uint64_t new_seq = slot_seq_[write_];
    const uint64_t old_seq = slot_seq_[display_];
    // Refuse mild rewind (stale hidden slot). Large drop = PipeWire seq_ reset after stop.
    if (old_seq != 0 && new_seq != 0 && new_seq < old_seq && (old_seq - new_seq) < 1000ull) {
      VRP_LOG("VideoTexture: REJECT flip write=%d seq=%llu < display=%d seq=%llu (keep front)",
              write_, static_cast<unsigned long long>(new_seq), display_,
              static_cast<unsigned long long>(old_seq));
      in_flight_ = false;
      return false;
    }
    // Retire the previous front — XR may still sample it for one more frame.
    prev_display_ = display_;
    display_ = write_;
    in_flight_ = false;
    valid_ = true;
    changed = true;
  }
  if (rebind_) {
    rebind_ = false;
    changed = true;
  }
  return changed;
}

bool VideoTexture::try_upload(const VideoFrame& frame) {
  if (frame.rgba.empty() || !device_ || !cmd_ || !fence_) return false;
  if (in_flight_ || !fence_ready()) return false;

  ensure_size(frame.width, frame.height);
  if (!staging_mapped_ || !slots_[0].image || !slots_[1].image || !slots_[2].image) return false;

  // Pick the only slot that is neither current display nor previous display.
  // Writing the previous front while XR still samples it left stale/torn pixels
  // that later rotated back into view ("old frame then jump").
  write_ = -1;
  for (int i = 0; i < kSlots; ++i) {
    if (i != display_ && i != prev_display_) {
      write_ = i;
      break;
    }
  }
  if (write_ < 0) write_ = (display_ + 1) % kSlots;
  Slot& slot = slots_[write_];

  std::memcpy(staging_mapped_, frame.rgba.data(),
              std::min(staging_size_, static_cast<VkDeviceSize>(frame.rgba.size())));

  std::unique_lock<std::mutex> qlock;
  if (queue_mu_) qlock = std::unique_lock<std::mutex>(*queue_mu_);

  vkResetCommandBuffer(cmd_, 0);
  VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (vkBeginCommandBuffer(cmd_, &bi) != VK_SUCCESS) return false;

  transition_image(cmd_, slot.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                   VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

  VkBufferImageCopy copy{};
  copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  copy.imageSubresource.layerCount = 1;
  copy.imageExtent = {static_cast<uint32_t>(width_), static_cast<uint32_t>(height_), 1};
  vkCmdCopyBufferToImage(cmd_, staging_, slot.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

  transition_image(cmd_, slot.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                   VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                   VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

  if (vkEndCommandBuffer(cmd_) != VK_SUCCESS) return false;
  if (vkResetFences(device_, 1, &fence_) != VK_SUCCESS) return false;

  VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  si.commandBufferCount = 1;
  si.pCommandBuffers = &cmd_;
  if (vkQueueSubmit(queue_, 1, &si, fence_) != VK_SUCCESS) return false;
  if (qlock.owns_lock()) qlock.unlock();

  // Prefer capture_seq from Live so display order matches PipeWire generation.
  if (frame.capture_seq != 0) {
    slot_seq_[write_] = frame.capture_seq;
    if (frame.capture_seq > upload_seq_) upload_seq_ = frame.capture_seq;
  } else {
    slot_seq_[write_] = ++upload_seq_;
  }
  slot_fp_[write_] =
      frame.content_fp != 0
          ? frame.content_fp
          : (!frame.rgba.empty() ? rgba_content_fp(frame.rgba.data(), frame.width, frame.height)
                                 : 0);
  in_flight_ = true;

  // First frame after create/resize: finish before XR samples UNDEFINED layouts.
  if (!valid_) {
    wait_fence();
    prev_display_ = display_;
    display_ = write_;
    in_flight_ = false;
    valid_ = true;
    rebind_ = true;
  }
  return true;
}

}  // namespace vrp
