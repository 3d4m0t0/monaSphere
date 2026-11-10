#include "capture/pipewire_capture.hpp"

#include "common.hpp"

#include <pipewire/pipewire.h>
#include <spa/buffer/buffer.h>
#include <spa/buffer/meta.h>
#include <spa/param/buffers.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/video/raw.h>
#include <spa/pod/builder.h>
#include <spa/utils/result.h>

#include <drm/drm_fourcc.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <sys/mman.h>
#include <vector>

namespace vrp {
namespace {

constexpr uint64_t kDrmModInvalid = DRM_FORMAT_MOD_INVALID;

uint32_t spa_to_drm_fourcc(spa_video_format fmt) {
  switch (fmt) {
    case SPA_VIDEO_FORMAT_BGRx:
      return DRM_FORMAT_XRGB8888;
    case SPA_VIDEO_FORMAT_BGRA:
      return DRM_FORMAT_ARGB8888;
    case SPA_VIDEO_FORMAT_RGBx:
      return DRM_FORMAT_XBGR8888;
    case SPA_VIDEO_FORMAT_RGBA:
      return DRM_FORMAT_ABGR8888;
    case SPA_VIDEO_FORMAT_xRGB:
      return DRM_FORMAT_XRGB8888;
    case SPA_VIDEO_FORMAT_ARGB:
      return DRM_FORMAT_ARGB8888;
    default:
      return 0;
  }
}

const char* spa_fmt_name(spa_video_format f) {
  switch (f) {
    case SPA_VIDEO_FORMAT_BGRx:
      return "BGRx";
    case SPA_VIDEO_FORMAT_BGRA:
      return "BGRA";
    case SPA_VIDEO_FORMAT_RGBx:
      return "RGBx";
    case SPA_VIDEO_FORMAT_RGBA:
      return "RGBA";
    case SPA_VIDEO_FORMAT_xRGB:
      return "xRGB";
    case SPA_VIDEO_FORMAT_ARGB:
      return "ARGB";
    default:
      return "?";
  }
}

void rgba_copy(const uint8_t* src, int src_stride, uint8_t* dst, int w, int h) {
  for (int y = 0; y < h; ++y) {
    std::memcpy(dst + static_cast<size_t>(y) * static_cast<size_t>(w) * 4u,
                src + static_cast<size_t>(y) * static_cast<size_t>(src_stride),
                static_cast<size_t>(w) * 4u);
  }
}

void bgrx_to_rgba(const uint8_t* src, int src_stride, uint8_t* dst, int w, int h) {
  for (int y = 0; y < h; ++y) {
    const uint8_t* row = src + static_cast<size_t>(y) * static_cast<size_t>(src_stride);
    uint8_t* out = dst + static_cast<size_t>(y) * static_cast<size_t>(w) * 4u;
    for (int x = 0; x < w; ++x) {
      out[0] = row[2];
      out[1] = row[1];
      out[2] = row[0];
      out[3] = 255;
      row += 4;
      out += 4;
    }
  }
}

void rgbx_to_rgba(const uint8_t* src, int src_stride, uint8_t* dst, int w, int h) {
  for (int y = 0; y < h; ++y) {
    const uint8_t* row = src + static_cast<size_t>(y) * static_cast<size_t>(src_stride);
    uint8_t* out = dst + static_cast<size_t>(y) * static_cast<size_t>(w) * 4u;
    for (int x = 0; x < w; ++x) {
      out[0] = row[0];
      out[1] = row[1];
      out[2] = row[2];
      out[3] = 255;
      row += 4;
      out += 4;
    }
  }
}

struct PwState {
  PipewireCapture* self = nullptr;
  pw_main_loop* loop = nullptr;
  pw_context* context = nullptr;
  pw_core* core = nullptr;
  pw_stream* stream = nullptr;
  spa_video_info_raw info{};
  spa_video_format format = SPA_VIDEO_FORMAT_UNKNOWN;
  uint32_t drm_fourcc = 0;
  uint64_t modifier = DRM_FORMAT_MOD_LINEAR;
  bool have_modifier = false;
  bool logged_dma = false;
  bool logged_cpu = false;
  bool logged_sample = false;
  bool logged_demote = false;
  /** After first good DMA, ignore LINEAR/SHM flip-flops (GNOME RecordVirtual flicker). */
  bool format_locked = false;
  uint32_t locked_w = 0;
  uint32_t locked_h = 0;
  uint64_t locked_modifier = DRM_FORMAT_MOD_LINEAR;
  int sync_seq = 0;
  /** Consumer-sampleable DRM modifiers (LINEAR at minimum). Copied from PipewireCapture. */
  std::vector<uint64_t> consumer_modifiers;
  /** Preferred stream size — KWin virtual MONITOR follows this on negotiate. */
  uint32_t prefer_w = 1920;
  uint32_t prefer_h = 1080;
  /** Live pool size from add_buffer/remove_buffer (must match ParamBuffers request). */
  int n_buffers = 0;
  int requested_buffers = 0;
};

void on_core_done(void* data, uint32_t /*id*/, int seq) {
  auto* st = static_cast<PwState*>(data);
  if (seq == st->sync_seq && st->loop) pw_main_loop_quit(st->loop);
}

void on_core_error(void* data, uint32_t id, int seq, int res, const char* message) {
  auto* st = static_cast<PwState*>(data);
  char buf[256];
  std::snprintf(buf, sizeof(buf), "pw core error id=%u seq=%d res=%d: %s", id, seq, res,
                message ? message : "");
  if (st->self) st->self->set_error_public(buf);
  if (st->loop) pw_main_loop_quit(st->loop);
}

const pw_core_events kCoreEvents = {
    .version = PW_VERSION_CORE_EVENTS,
    .info = nullptr,
    .done = on_core_done,
    .ping = nullptr,
    .error = on_core_error,
    .remove_id = nullptr,
    .bound_id = nullptr,
    .add_mem = nullptr,
    .remove_mem = nullptr,
};

/** Plain EnumFormat (MemFd-friendly). Keep this first in connect params. */
spa_pod* build_format_plain(spa_pod_builder* b, uint32_t prefer_w, uint32_t prefer_h) {
  spa_rectangle def_size{prefer_w, prefer_h};
  spa_rectangle min_size{64, 64};
  spa_rectangle max_size{8192, 8192};
  spa_fraction def_fps{60, 1};
  // Keep min=0 — Mutter RecordVirtual often only offers variable/0 fps. Forcing
  // min=60 made format negotiate fail → PW thread exit → Host capture_dead stop
  // before Chrome launch (Live 切替で即停止).
  spa_fraction min_fps{0, 1};
  spa_fraction max_fps{240, 1};
  return static_cast<spa_pod*>(spa_pod_builder_add_object(
      b, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat, SPA_FORMAT_mediaType,
      SPA_POD_Id(SPA_MEDIA_TYPE_video), SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
      SPA_FORMAT_VIDEO_format,
      SPA_POD_CHOICE_ENUM_Id(5, SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_BGRA,
                             SPA_VIDEO_FORMAT_RGBx, SPA_VIDEO_FORMAT_RGBA),
      SPA_FORMAT_VIDEO_size, SPA_POD_CHOICE_RANGE_Rectangle(&def_size, &min_size, &max_size),
      SPA_FORMAT_VIDEO_framerate, SPA_POD_CHOICE_RANGE_Fraction(&def_fps, &min_fps, &max_fps)));
}

/**
 * Modifier-aware EnumFormat (DMA-BUF). Must be announced *before* the plain
 * MemFd EnumFormat so PipeWire prioritises DMA-BUF
 * (https://docs.pipewire.org/page_dma_buf.html).
 *
 * Announce Vulkan-sampleable modifiers (no MOD_INVALID — that made KWin pick
 * modifier-less, fail on NVIDIA, and demote to MemFd).
 */
spa_pod* build_format_modifier(spa_pod_builder* b, const std::vector<uint64_t>& mods,
                               uint32_t prefer_w, uint32_t prefer_h) {
  spa_pod_frame f[2];
  spa_rectangle def_size{prefer_w, prefer_h};
  spa_rectangle min_size{64, 64};
  spa_rectangle max_size{8192, 8192};
  spa_fraction def_fps{60, 1};
  spa_fraction min_fps{0, 1};
  spa_fraction max_fps{240, 1};

  std::vector<uint64_t> list = mods;
  if (list.empty()) list.push_back(DRM_FORMAT_MOD_LINEAR);
  if (list.size() > 64) list.resize(64);
  // Prefer tiled as default — Mutter RecordVirtual flip-flops LINEAR↔tiled when
  // LINEAR is the SPA default, which restarts buffers and flickers the HMD.
  {
    std::vector<uint64_t> ordered;
    ordered.reserve(list.size());
    for (uint64_t m : list) {
      if (m != DRM_FORMAT_MOD_LINEAR) ordered.push_back(m);
    }
    for (uint64_t m : list) {
      if (m == DRM_FORMAT_MOD_LINEAR) ordered.push_back(m);
    }
    if (!ordered.empty()) list.swap(ordered);
  }

  spa_pod_builder_push_object(b, &f[0], SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
  spa_pod_builder_add(b, SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video), 0);
  spa_pod_builder_add(b, SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw), 0);
  spa_pod_builder_add(b, SPA_FORMAT_VIDEO_format,
                      SPA_POD_CHOICE_ENUM_Id(5, SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_BGRx,
                                             SPA_VIDEO_FORMAT_BGRA, SPA_VIDEO_FORMAT_RGBx,
                                             SPA_VIDEO_FORMAT_RGBA),
                      0);
  spa_pod_builder_add(b, SPA_FORMAT_VIDEO_size,
                      SPA_POD_CHOICE_RANGE_Rectangle(&def_size, &min_size, &max_size), 0);
  spa_pod_builder_add(b, SPA_FORMAT_VIDEO_framerate,
                      SPA_POD_CHOICE_RANGE_Fraction(&def_fps, &min_fps, &max_fps), 0);
  spa_pod_builder_prop(b, SPA_FORMAT_VIDEO_modifier,
                       SPA_POD_PROP_FLAG_MANDATORY | SPA_POD_PROP_FLAG_DONT_FIXATE);
  spa_pod_builder_push_choice(b, &f[1], SPA_CHOICE_Enum, 0);
  spa_pod_builder_long(b, static_cast<int64_t>(list.front()));
  for (uint64_t m : list) spa_pod_builder_long(b, static_cast<int64_t>(m));
  spa_pod_builder_pop(b, &f[1]);
  return static_cast<spa_pod*>(spa_pod_builder_pop(b, &f[0]));
}

void on_stream_param_changed(void* data, uint32_t id, const spa_pod* param) {
  auto* st = static_cast<PwState*>(data);
  if (!param || id != SPA_PARAM_Format || !st->stream) return;

  spa_video_info info{};
  if (spa_format_parse(param, &info.media_type, &info.media_subtype) < 0) return;
  if (info.media_type != SPA_MEDIA_TYPE_video || info.media_subtype != SPA_MEDIA_SUBTYPE_raw) return;
  if (spa_format_video_raw_parse(param, &info.info.raw) < 0) return;

  // Docs: use spa_pod_find_prop — presence means DMA-BUF path, absence means SHM.
  const spa_pod_prop* mod_prop = spa_pod_find_prop(param, nullptr, SPA_FORMAT_VIDEO_modifier);
  const bool negotiated_mod = mod_prop != nullptr;
  const bool was_mod = st->have_modifier;

  const uint32_t w = info.info.raw.size.width;
  const uint32_t h = info.info.raw.size.height;
  const uint64_t new_mod = negotiated_mod ? info.info.raw.modifier : kDrmModInvalid;
  const bool is_tiled = negotiated_mod && new_mod != DRM_FORMAT_MOD_LINEAR &&
                        new_mod != kDrmModInvalid;
  // After first fixate at this size, ignore renegotiation (restarts buffers → black).
  if (st->format_locked && w == st->locked_w && h == st->locked_h) {
    if (negotiated_mod && new_mod == st->locked_modifier) return;
    if (!negotiated_mod && st->locked_modifier == kDrmModInvalid) return;  // SHM locked
    if (!is_tiled || new_mod != st->locked_modifier) {
      if (!st->logged_demote) {
        st->logged_demote = true;
        VRP_LOG("pipewire: ignoring format flip-flop (locked modifier=0x%llx)",
                static_cast<unsigned long long>(st->locked_modifier));
      }
      return;
    }
  }

  st->info = info.info.raw;
  st->format = info.info.raw.format;
  st->drm_fourcc = spa_to_drm_fourcc(st->format);
  st->have_modifier = negotiated_mod && (info.info.raw.flags & SPA_VIDEO_FLAG_MODIFIER) != 0;
  st->modifier = info.info.raw.modifier;
  if (!negotiated_mod) {
    st->have_modifier = false;
    st->modifier = kDrmModInvalid;
  }
  // Lock tiled DMA or SHM once size is known (stop update_params storms).
  if (!st->format_locked && w > 0 && h > 0 && (is_tiled || !negotiated_mod)) {
    st->format_locked = true;
    st->locked_w = w;
    st->locked_h = h;
    st->locked_modifier = is_tiled ? new_mod : kDrmModInvalid;
    VRP_LOG("pipewire: format locked early %ux%u modifier=0x%llx%s", w, h,
            static_cast<unsigned long long>(st->locked_modifier),
            negotiated_mod ? "" : " (shm)");
  }

  if (was_mod && !negotiated_mod && !st->logged_demote) {
    st->logged_demote = true;
    VRP_LOG("pipewire: producer demoted DMA-BUF -> SHM (modifier alloc failed?)");
  }
  VRP_LOG("pipewire: format %s %ux%u modifier=0x%llx drm=0x%x negotiated_mod=%d",
          spa_fmt_name(st->format), w, h, static_cast<unsigned long long>(st->modifier),
          st->drm_fourcc, negotiated_mod ? 1 : 0);
  if (st->self) {
    char line[192];
    std::snprintf(line, sizeof(line), "pipewire: format %s %ux%u modifier=0x%llx%s",
                  spa_fmt_name(st->format), w, h, static_cast<unsigned long long>(st->modifier),
                  negotiated_mod ? " (dmabuf)" : " (shm)");
    st->self->push_status(line);
  }

  // Docs: modifiers negotiated → DmaBuf only; else MemFd/MemPtr.
  //   DMA copy-out: 2  (Meta write + consumer hold; Vk ping-pong owns pixels after blit)
  //   SHM/CPU:      8  (Meta write + fast staging memcpy + XR mailbox + spare). Pool=4
  //                  let Mutter republish the same 4 images for seconds → recent-fp hold.
  //                  Prefer 8 via CHOICE_RANGE (min 4) so older Mutter can still settle at 4.
  const int want_bufs = negotiated_mod ? 2 : 8;
  const int want_bufs_min = negotiated_mod ? 2 : 4;
  st->requested_buffers = want_bufs;
  st->n_buffers = 0;
  const uint32_t stride = w * 4u;
  const uint32_t size = stride * h;
  uint8_t buffer[1024];
  spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
  const uint32_t data_types =
      negotiated_mod ? (1u << SPA_DATA_DmaBuf)
                     : ((1u << SPA_DATA_MemFd) | (1u << SPA_DATA_MemPtr));
  const spa_pod* params[] = {static_cast<spa_pod*>(spa_pod_builder_add_object(
      &b, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers, SPA_PARAM_BUFFERS_buffers,
      SPA_POD_CHOICE_RANGE_Int(want_bufs, want_bufs_min, want_bufs), SPA_PARAM_BUFFERS_blocks,
      SPA_POD_Int(1), SPA_PARAM_BUFFERS_size,
      SPA_POD_CHOICE_RANGE_Int(static_cast<int>(size), 0, INT32_MAX), SPA_PARAM_BUFFERS_stride,
      SPA_POD_CHOICE_RANGE_Int(static_cast<int>(stride), 0, INT32_MAX),
      SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int(data_types)))};
  if (pw_stream_update_params(st->stream, params, 1) < 0) {
    VRP_LOG("pipewire: update_params(Buffers) failed — using stream defaults");
  } else {
    VRP_LOG("pipewire: request %d buffers (min %d) dataType=%s (display Vk triple=%d)", want_bufs,
            want_bufs_min, negotiated_mod ? "DmaBuf" : "MemFd/MemPtr", negotiated_mod ? 2 : 3);
  }

  if (st->self) {
    st->self->set_negotiated(static_cast<uint32_t>(st->format), st->drm_fourcc, st->modifier,
                             negotiated_mod);
  }
}

/** Convert staged CPU pixels (tight or strided) into RGBA VideoFrame and publish. */
void publish_cpu_from_local(PwState* st, const uint8_t* src, int stride, int w, int h) {
  if (!st->self || !src || w <= 0 || h <= 0 || stride < w * 4) return;
  VideoFrame frame;
  frame.width = w;
  frame.height = h;
  frame.rgba.resize(static_cast<size_t>(w) * static_cast<size_t>(h) * 4u);
  switch (st->format) {
    case SPA_VIDEO_FORMAT_RGBA:
      rgba_copy(src, stride, frame.rgba.data(), w, h);
      break;
    case SPA_VIDEO_FORMAT_RGBx:
      rgbx_to_rgba(src, stride, frame.rgba.data(), w, h);
      break;
    case SPA_VIDEO_FORMAT_BGRx:
    case SPA_VIDEO_FORMAT_BGRA:
    case SPA_VIDEO_FORMAT_xRGB:
    case SPA_VIDEO_FORMAT_ARGB:
    default:
      bgrx_to_rgba(src, stride, frame.rgba.data(), w, h);
      break;
  }
  if (!st->logged_cpu) {
    st->logged_cpu = true;
    VRP_LOG("pipewire: CPU RGBA path %dx%d (stage-then-release)", w, h);
    char line[128];
    std::snprintf(line, sizeof(line), "pipewire: CPU RGBA path %dx%d", w, h);
    st->self->push_status(line);
  }
  // Once: sample whether Meta sent a blank buffer (common until stage damage).
  if (!st->logged_sample) {
    st->logged_sample = true;
    const uint8_t* p = frame.rgba.data();
    const size_t n = frame.rgba.size();
    size_t lit = 0;
    uint32_t maxc = 0;
    for (size_t i = 0; i + 3 < n; i += 16) {  // every 4th pixel
      const uint32_t r = p[i], g = p[i + 1], b = p[i + 2];
      const uint32_t m = r > g ? (r > b ? r : b) : (g > b ? g : b);
      if (m > maxc) maxc = m;
      if (m > 8) ++lit;
    }
    VRP_LOG("pipewire: first CPU sample lit=%zu max=%u (%s)", lit, maxc,
            lit == 0 ? "black/empty Meta?" : "has content");
  }
  st->self->publish_frame(std::move(frame));
}

/**
 * Counter-A staging: memcpy SHM → local, return pw_buffer, THEN convert/publish.
 * Holding the PipeWire buffer through BGRX→RGBA + content_fp let Mutter stall and
 * republish the same pool images (multi-second recent-fp holds on Meta-*).
 */
void stage_cpu_release_then_publish(PwState* st, pw_buffer* b, const uint8_t* src, int stride,
                                    int w, int h) {
  if (!st->self || !b || !src || w <= 0 || h <= 0 || stride < w * 4) {
    if (b && st->stream) pw_stream_queue_buffer(st->stream, b);
    return;
  }
  const size_t row = static_cast<size_t>(w) * 4u;
  const size_t bytes = row * static_cast<size_t>(h);
  // Reuse one staging buffer on the PW thread (on_process is single-threaded).
  static thread_local std::vector<uint8_t> stage;
  stage.resize(bytes);
  if (stride == static_cast<int>(row)) {
    std::memcpy(stage.data(), src, bytes);
  } else {
    for (int y = 0; y < h; ++y) {
      std::memcpy(stage.data() + static_cast<size_t>(y) * row,
                  src + static_cast<size_t>(y) * static_cast<size_t>(stride), row);
    }
  }
  pw_stream_queue_buffer(st->stream, b);
  publish_cpu_from_local(st, stage.data(), static_cast<int>(row), w, h);
}

void on_process(void* data) {
  auto* st = static_cast<PwState*>(data);
  if (!st->stream || !st->self) return;
  st->self->flush_released(st->stream);

  // WebRTC pattern: drain to newest, return older buffers immediately.
  pw_buffer* b = nullptr;
  for (;;) {
    pw_buffer* next = pw_stream_dequeue_buffer(st->stream);
    if (!next) break;
    if (b) pw_stream_queue_buffer(st->stream, b);
    b = next;
  }
  if (!b) return;
  spa_buffer* buf = b->buffer;
  if (!buf || buf->n_datas < 1) {
    pw_stream_queue_buffer(st->stream, b);
    return;
  }
  if (auto* hdr = static_cast<spa_meta_header*>(
          spa_buffer_find_meta_data(buf, SPA_META_Header, sizeof(spa_meta_header)))) {
    if (hdr->flags & SPA_META_HEADER_FLAG_CORRUPTED) {
      pw_stream_queue_buffer(st->stream, b);
      return;
    }
  }

  spa_data* d = &buf->datas[0];
  const int w = static_cast<int>(st->info.size.width);
  const int h = static_cast<int>(st->info.size.height);
  if (w <= 0 || h <= 0) {
    pw_stream_queue_buffer(st->stream, b);
    return;
  }

  if (!st->logged_dma && !st->logged_cpu) {
    const char* t = d->type == SPA_DATA_DmaBuf   ? "DmaBuf"
                    : d->type == SPA_DATA_MemFd  ? "MemFd"
                    : d->type == SPA_DATA_MemPtr ? "MemPtr"
                                                 : "other";
    VRP_LOG("pipewire: first buffer type=%s fd=%d n_datas=%u modifier=0x%llx", t, d->fd, buf->n_datas,
            static_cast<unsigned long long>(st->modifier));
  }

  const uint32_t stride =
      d->chunk && d->chunk->stride > 0 ? static_cast<uint32_t>(d->chunk->stride)
                                      : static_cast<uint32_t>(w) * 4u;
  const uint32_t offset = d->chunk ? static_cast<uint32_t>(d->chunk->offset) : 0u;
  uint32_t size =
      d->chunk && d->chunk->size > 0 ? static_cast<uint32_t>(d->chunk->size) : stride * static_cast<uint32_t>(h);
  if (size == 0) size = stride * static_cast<uint32_t>(h);

  // DMA-BUF: dup fd for Vulkan import (zero-copy). Skip CPU when DMA is used —
  // mmap/convert of tiled NVIDIA buffers is slow and caused stutter + false CPU logs.
  bool published_dma = false;
  if (d->type == SPA_DATA_DmaBuf && d->fd >= 0 && st->drm_fourcc != 0) {
    const int dupfd = ::dup(d->fd);
    if (dupfd >= 0) {
      PipewireDmaFrame dma;
      dma.fd = dupfd;
      dma.width = w;
      dma.height = h;
      dma.drm_fourcc = st->drm_fourcc;
      dma.modifier = st->modifier;
      // LINEAR (0) is a real modifier; INVALID is the modifier-less token.
      dma.has_modifier = st->modifier != kDrmModInvalid;
      dma.stride = stride;
      dma.offset = offset;
      dma.size = size;
      if (!st->logged_dma) {
        st->logged_dma = true;
        VRP_LOG("pipewire: DMA-BUF path %dx%d fourcc=0x%x modifier=0x%llx stride=%u", w, h,
                st->drm_fourcc, static_cast<unsigned long long>(st->modifier), stride);
        char line[192];
        std::snprintf(line, sizeof(line),
                      "pipewire: DMA-BUF path %dx%d fourcc=0x%x modifier=0x%llx", w, h,
                      st->drm_fourcc, static_cast<unsigned long long>(st->modifier));
        st->self->push_status(line);
      }
      // Lock tiled DMA so RecordVirtual stops LINEAR↔tiled flicker.
      if (!st->format_locked && st->modifier != DRM_FORMAT_MOD_LINEAR &&
          st->modifier != kDrmModInvalid) {
        st->format_locked = true;
        st->locked_w = static_cast<uint32_t>(w);
        st->locked_h = static_cast<uint32_t>(h);
        st->locked_modifier = st->modifier;
        VRP_LOG("pipewire: format locked %ux%u modifier=0x%llx", st->locked_w, st->locked_h,
                static_cast<unsigned long long>(st->locked_modifier));
      }
      st->self->set_negotiated(static_cast<uint32_t>(st->format), st->drm_fourcc, st->modifier, true);
      // Hold until GPU copy-out finishes — dup shares backing pages with the
      // producer, so queue_buffer before copy lets Mutter rewrite mid-blit (flicker).
      // Cap holds at 1; XR release_seq returns the buffer right after blit.
      const uint64_t seq = st->self->publish_dma(std::move(dma));
      st->self->hold_pw_buffer(st->stream, b, seq);
      published_dma = true;
    }
  }

  if (!published_dma) {
    // CPU RGBA: MAP_BUFFERS fills d->data for MemFd; mmap for MemFd fallback.
    // Stage memcpy → queue_buffer → convert (do not hold SHM through BGRX convert).
    const uint8_t* src = nullptr;
    void* map = MAP_FAILED;
    size_t map_len = 0;
    if (d->data) {
      src = static_cast<const uint8_t*>(d->data) + offset;
    } else if ((d->type == SPA_DATA_MemFd || d->type == SPA_DATA_DmaBuf) && d->fd >= 0) {
      map_len = d->maxsize > 0 ? d->maxsize : (offset + size);
      if (map_len == 0) map_len = static_cast<size_t>(stride) * static_cast<size_t>(h) + offset;
      map = ::mmap(nullptr, map_len, PROT_READ, MAP_SHARED, d->fd, static_cast<off_t>(d->mapoffset));
      if (map != MAP_FAILED) {
        src = static_cast<const uint8_t*>(map) + offset;
      }
    }
    if (src) {
      stage_cpu_release_then_publish(st, b, src, static_cast<int>(stride), w, h);
      b = nullptr;  // already queued
    }
    if (map != MAP_FAILED) ::munmap(map, map_len);
    if (b) pw_stream_queue_buffer(st->stream, b);
  }
}

void on_state_changed(void* data, pw_stream_state /*old*/, pw_stream_state state, const char* error) {
  auto* st = static_cast<PwState*>(data);
  if (state == PW_STREAM_STATE_ERROR && st->self) {
    VRP_LOG("pipewire: stream ERROR: %s", error ? error : "?");
    st->self->set_error_public(error ? error : "stream error");
    if (st->self) st->self->push_status(std::string("pipewire: stream ERROR: ") + (error ? error : "?"));
    if (st->loop) pw_main_loop_quit(st->loop);
  } else if (state == PW_STREAM_STATE_STREAMING) {
    VRP_LOG("pipewire: streaming");
    if (st->self) st->self->push_status("pipewire: streaming");
  }
}

void on_add_buffer(void* data, pw_buffer* /*buffer*/) {
  auto* st = static_cast<PwState*>(data);
  ++st->n_buffers;
  VRP_LOG("pipewire: add_buffer pool=%d (requested %d)", st->n_buffers, st->requested_buffers);
  if (st->requested_buffers > 0 && st->n_buffers == st->requested_buffers) {
    VRP_LOG("pipewire: buffer pool matched request (%d)", st->n_buffers);
  } else if (st->requested_buffers > 0 && st->n_buffers > st->requested_buffers) {
    VRP_LOG("pipewire: WARNING pool %d > requested %d — display depth mismatch risk",
            st->n_buffers, st->requested_buffers);
  }
}

void on_remove_buffer(void* data, pw_buffer* /*buffer*/) {
  auto* st = static_cast<PwState*>(data);
  if (st->n_buffers > 0) --st->n_buffers;
  VRP_LOG("pipewire: remove_buffer pool=%d", st->n_buffers);
}

const pw_stream_events kStreamEvents = {
    .version = PW_VERSION_STREAM_EVENTS,
    .destroy = nullptr,
    .state_changed = on_state_changed,
    .control_info = nullptr,
    .io_changed = nullptr,
    .param_changed = on_stream_param_changed,
    .add_buffer = on_add_buffer,
    .remove_buffer = on_remove_buffer,
    .process = on_process,
    .drained = nullptr,
    .command = nullptr,
    .trigger_done = nullptr,
};

}  // namespace

PipewireCapture::~PipewireCapture() { stop(); }

void PipewireCapture::set_error(const std::string& e) {
  std::lock_guard<std::mutex> lock(err_mu_);
  last_error_ = e;
}

void PipewireCapture::set_error_public(const std::string& e) {
  set_error(e);
  push_status(std::string("PipeWire: ") + e);
}

std::string PipewireCapture::last_error() const {
  std::lock_guard<std::mutex> lock(err_mu_);
  return last_error_;
}

void PipewireCapture::push_status(std::string line) {
  if (line.empty()) return;
  std::lock_guard<std::mutex> lock(status_mu_);
  constexpr size_t kMax = 32;
  if (status_lines_.size() >= kMax) status_lines_.pop_front();
  status_lines_.push_back(std::move(line));
}

std::vector<std::string> PipewireCapture::drain_status() {
  std::lock_guard<std::mutex> lock(status_mu_);
  std::vector<std::string> out;
  out.reserve(status_lines_.size());
  while (!status_lines_.empty()) {
    out.push_back(std::move(status_lines_.front()));
    status_lines_.pop_front();
  }
  return out;
}

void PipewireCapture::set_negotiated(uint32_t /*spa_format*/, uint32_t drm_fourcc, uint64_t modifier,
                                     bool using_dmabuf) {
  drm_fourcc_.store(drm_fourcc, std::memory_order_relaxed);
  modifier_.store(modifier, std::memory_order_relaxed);
  using_dmabuf_.store(using_dmabuf, std::memory_order_relaxed);
}

void PipewireCapture::publish_frame(VideoFrame frame) {
  width_.store(frame.width, std::memory_order_relaxed);
  height_.store(frame.height, std::memory_order_relaxed);
  // Assign under lock, THEN publish seq — bumping seq before latest_ lets
  // take_latest() move a stale/empty frame tagged as the new generation
  // (old Meta pixels with a new seq → hitch without DISRUPT rewind).
  {
    std::lock_guard<std::mutex> lock(frame_mu_);
    const uint64_t s = seq_.load(std::memory_order_relaxed) + 1;
    frame.capture_seq = s;
    if (frame.content_fp == 0 && !frame.rgba.empty() && frame.width > 1 && frame.height > 1) {
      frame.content_fp = rgba_content_fp(frame.rgba.data(), frame.width, frame.height);
    }
    latest_ = std::move(frame);
    seq_.store(s, std::memory_order_release);
  }
}

uint64_t PipewireCapture::publish_dma(PipewireDmaFrame frame) {
  width_.store(frame.width, std::memory_order_relaxed);
  height_.store(frame.height, std::memory_order_relaxed);
  dma_seen_.store(true, std::memory_order_relaxed);
  const uint64_t seq = dma_seq_.load(std::memory_order_relaxed) + 1;
  frame.seq = seq;
  uint64_t superseded = 0;
  {
    std::lock_guard<std::mutex> lock(frame_mu_);
    if (latest_dma_) superseded = latest_dma_.seq;
    latest_dma_ = std::move(frame);
  }
  dma_seq_.store(seq, std::memory_order_release);
  // Dropped before XR took it — return that pw_buffer immediately.
  if (superseded != 0) release_held_dma(superseded);
  return seq;
}

void PipewireCapture::release_held_dma(uint64_t seq) {
  if (seq == 0) return;
  {
    std::lock_guard<std::mutex> lock(hold_mu_);
    for (auto it = held_dma_.begin(); it != held_dma_.end(); ++it) {
      if (it->seq != seq) continue;
      if (it->buf) release_bufs_.push_back(it->buf);
      held_dma_.erase(it);
      break;
    }
  }
  request_flush_released();
}

namespace {
int flush_released_invoke(struct spa_loop* /*loop*/, bool /*async*/, uint32_t /*seq*/,
                          const void* /*data*/, size_t /*size*/, void* user_data) {
  auto* self = static_cast<PipewireCapture*>(user_data);
  if (self) self->flush_released_on_loop();
  return 0;
}
}  // namespace

void PipewireCapture::flush_released_on_loop() {
  flush_released(pw_stream_.load(std::memory_order_acquire));
}

void PipewireCapture::request_flush_released() {
  auto* loop = static_cast<pw_loop*>(pw_loop_.load(std::memory_order_acquire));
  if (!loop) return;
  // XR thread → PW loop: queue_buffer must run on the stream's loop thread.
  (void)pw_loop_invoke(loop, flush_released_invoke, /*seq=*/1, nullptr, 0, false, this);
}

namespace {
bool stream_can_queue(pw_stream* s) {
  if (!s) return false;
  const pw_stream_state st = pw_stream_get_state(s, nullptr);
  return st == PW_STREAM_STATE_STREAMING || st == PW_STREAM_STATE_PAUSED;
}
}  // namespace

void PipewireCapture::hold_pw_buffer(void* stream, void* buf, uint64_t seq) {
  auto* s = static_cast<pw_stream*>(stream);
  auto* b = static_cast<pw_buffer*>(buf);
  if (!s || !b) return;
  if (seq == 0) {
    pw_stream_queue_buffer(s, b);
    return;
  }
  std::vector<void*> overflow;
  {
    std::lock_guard<std::mutex> lock(hold_mu_);
    held_dma_.push_back(HeldDma{seq, buf});
    // At most 1 held pw_buffer: copy-out returns it as soon as the GPU blit
    // finishes. A second hold was "latest waiting" but publish_dma already
    // supersedes latest_dma_ — extra dequeued buffers just starve Mutter.
    while (held_dma_.size() > 1) {
      overflow.push_back(held_dma_.front().buf);
      held_dma_.erase(held_dma_.begin());
    }
  }
  if (stream_can_queue(s)) {
    for (void* p : overflow) {
      if (p) pw_stream_queue_buffer(s, static_cast<pw_buffer*>(p));
    }
  }
  flush_released(stream);
}

void PipewireCapture::flush_released(void* stream) {
  auto* s = static_cast<pw_stream*>(stream);
  std::vector<void*> q;
  {
    std::lock_guard<std::mutex> lock(hold_mu_);
    q.swap(release_bufs_);
  }
  if (!stream_can_queue(s)) return;  // drop — stream already dead / disconnecting
  for (void* p : q) {
    if (p) pw_stream_queue_buffer(s, static_cast<pw_buffer*>(p));
  }
}

void PipewireCapture::release_all_held(void* stream) {
  auto* s = static_cast<pw_stream*>(stream);
  std::vector<void*> q;
  {
    std::lock_guard<std::mutex> lock(hold_mu_);
    for (auto& h : held_dma_) {
      if (h.buf) q.push_back(h.buf);
    }
    held_dma_.clear();
    q.insert(q.end(), release_bufs_.begin(), release_bufs_.end());
    release_bufs_.clear();
  }
  // Teardown / ERROR / UNCONNECTED: never queue — pw_buffer* are often dangling
  // after portal Session.Close and queue_buffer SEGVs (see coredump).
  if (!stream_can_queue(s)) return;
  for (void* p : q) {
    if (p) pw_stream_queue_buffer(s, static_cast<pw_buffer*>(p));
  }
}

bool PipewireCapture::take_latest(VideoFrame& out, uint64_t& seen_seq) {
  if (seq_.load(std::memory_order_acquire) == seen_seq) return false;
  std::lock_guard<std::mutex> lock(frame_mu_);
  const uint64_t s = seq_.load(std::memory_order_relaxed);
  if (s == seen_seq || latest_.rgba.empty()) return false;
  // Move — avoid duplicating ~W×H×4 on the CPU fallback path every frame.
  out = std::move(latest_);
  latest_ = {};
  if (out.capture_seq != 0 && out.capture_seq != s) {
    VRP_LOG("pipewire: take_latest SEQ DESYNC frame=%llu pub=%llu fp=0x%llx",
            static_cast<unsigned long long>(out.capture_seq),
            static_cast<unsigned long long>(s),
            static_cast<unsigned long long>(out.content_fp));
  }
  if (out.capture_seq == 0) out.capture_seq = s;
  seen_seq = s;
  return true;
}

bool PipewireCapture::take_latest_dma(PipewireDmaFrame& out, uint64_t& seen_seq) {
  const uint64_t s = dma_seq_.load(std::memory_order_acquire);
  if (s == seen_seq) return false;
  std::lock_guard<std::mutex> lock(frame_mu_);
  if (!latest_dma_) return false;
  out = std::move(latest_dma_);
  seen_seq = s;
  return true;
}

bool PipewireCapture::start(int pw_fd, uint32_t node_id, uint64_t pipewire_serial,
                            std::vector<uint64_t> consumer_modifiers, int prefer_w, int prefer_h,
                            bool prefer_shm) {
  stop();
  // pw_fd < 0 → default user PipeWire socket (in-session Mutter ScreenCast).
  prefer_shm_ = prefer_shm;
  consumer_modifiers_ = std::move(consumer_modifiers);
  if (!prefer_shm_ && consumer_modifiers_.empty())
    consumer_modifiers_.push_back(DRM_FORMAT_MOD_LINEAR);
  prefer_w_ = prefer_w > 0 ? prefer_w : 0;
  prefer_h_ = prefer_h > 0 ? prefer_h : 0;
  stop_.store(false);
  running_.store(true);
  thread_ = std::thread([this, pw_fd, node_id, pipewire_serial] {
    thread_main(pw_fd, node_id, pipewire_serial);
    running_.store(false);
  });
  return true;
}

void PipewireCapture::stop() {
  stop_.store(true);
  if (thread_.joinable()) thread_.join();
  running_.store(false);
  {
    std::lock_guard<std::mutex> lock(frame_mu_);
    latest_ = {};
    latest_dma_.release();
  }
  {
    std::lock_guard<std::mutex> lock(status_mu_);
    status_lines_.clear();
  }
  seq_.store(0);
  dma_seq_.store(0);
  dma_seen_.store(false);
  width_.store(0);
  height_.store(0);
}

void PipewireCapture::thread_main(int pw_fd, uint32_t node_id, uint64_t serial) {
  pw_init(nullptr, nullptr);
  PwState st;
  st.self = this;
  const bool own_fd = pw_fd >= 0;

  st.loop = pw_main_loop_new(nullptr);
  if (!st.loop) {
    set_error("pw_main_loop_new");
    if (own_fd) ::close(pw_fd);
    pw_deinit();
    return;
  }

  st.context = pw_context_new(pw_main_loop_get_loop(st.loop), nullptr, 0);
  if (!st.context) {
    set_error("pw_context_new");
    pw_main_loop_destroy(st.loop);
    if (own_fd) ::close(pw_fd);
    pw_deinit();
    return;
  }

  if (own_fd) {
    st.core = pw_context_connect_fd(st.context, pw_fd, nullptr, 0);
    if (!st.core) {
      set_error("pw_context_connect_fd");
      pw_context_destroy(st.context);
      pw_main_loop_destroy(st.loop);
      pw_deinit();
      return;
    }
  } else {
    st.core = pw_context_connect(st.context, nullptr, 0);
    if (!st.core) {
      set_error("pw_context_connect");
      pw_context_destroy(st.context);
      pw_main_loop_destroy(st.loop);
      pw_deinit();
      return;
    }
    VRP_LOG("pipewire: connected via default socket (Mutter node %u)", node_id);
  }

  spa_hook core_listener{};
  pw_core_add_listener(st.core, &core_listener, &kCoreEvents, &st);

  st.stream = pw_stream_new(st.core, "monaSphere-screencast",
                            pw_properties_new(PW_KEY_MEDIA_TYPE, "Video", PW_KEY_MEDIA_CATEGORY,
                                              "Capture", PW_KEY_MEDIA_ROLE, "Screen", nullptr));
  if (!st.stream) {
    set_error("pw_stream_new");
    pw_core_disconnect(st.core);
    pw_context_destroy(st.context);
    pw_main_loop_destroy(st.loop);
    pw_deinit();
    return;
  }

  spa_hook stream_listener{};
  pw_stream_add_listener(st.stream, &stream_listener, &kStreamEvents, &st);

  st.consumer_modifiers = consumer_modifiers_;
  // KWin resizes virtual MONITOR streams to the preferred SPA size. Prefer the
  // configured Virtual-* mode (e.g. 3840×2160); never default to 1920×1080 or a
  // 4K setup is silently shrunk on negotiate.
  st.prefer_w = prefer_w_ > 0 ? static_cast<uint32_t>(prefer_w_) : 1920u;
  st.prefer_h = prefer_h_ > 0 ? static_cast<uint32_t>(prefer_h_) : 1080u;
  {
    std::string line = "pipewire: announcing ";
    line += std::to_string(st.consumer_modifiers.size());
    line += " DRM modifier(s):";
    for (size_t i = 0; i < st.consumer_modifiers.size() && i < 8; ++i) {
      char buf[32];
      std::snprintf(buf, sizeof(buf), " 0x%llx",
                    static_cast<unsigned long long>(st.consumer_modifiers[i]));
      line += buf;
    }
    if (st.consumer_modifiers.size() > 8) line += " …";
    VRP_LOG("%s", line.c_str());
    VRP_LOG("pipewire: prefer size %ux%u (virtual MONITOR follows this on KWin)", st.prefer_w,
            st.prefer_h);
  }

  uint8_t pod_buf[8192];
  spa_pod_builder builder = SPA_POD_BUILDER_INIT(pod_buf, sizeof(pod_buf));
  // GNOME RecordVirtual (Meta-*): SHM-only. Virtual monitors have no KMS CRTC;
  // DMA-BUF + NVIDIA implicit sync is a known flicker class (niri/WebRTC/Fjarr).
  // WebRTC processes + queue_buffer in the same on_process; SHM makes that safe.
  const spa_pod* params[2];
  uint32_t n_params = 0;
  if (prefer_shm_) {
    params[n_params++] = build_format_plain(&builder, st.prefer_w, st.prefer_h);
    VRP_LOG("pipewire: SHM-only EnumFormat (prefer_shm — GNOME VIRTUAL / Meta-*)");
  } else {
    params[n_params++] =
        build_format_modifier(&builder, st.consumer_modifiers, st.prefer_w, st.prefer_h);
    params[n_params++] = build_format_plain(&builder, st.prefer_w, st.prefer_h);
  }

  // MAP_BUFFERS: required for MemFd CPU path.
  const auto flags = static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT |
                                                   PW_STREAM_FLAG_INACTIVE | PW_STREAM_FLAG_MAP_BUFFERS);
  int cret = pw_stream_connect(st.stream, PW_DIRECTION_INPUT, node_id, flags, params, n_params);
  if (cret < 0) {
    char buf[128];
    std::snprintf(buf, sizeof(buf), "pw_stream_connect: %s", spa_strerror(cret));
    set_error(buf);
    pw_stream_destroy(st.stream);
    pw_core_disconnect(st.core);
    pw_context_destroy(st.context);
    pw_main_loop_destroy(st.loop);
    pw_deinit();
    return;
  }
  (void)serial;

  pw_stream_set_active(st.stream, true);
  pw_loop_.store(pw_main_loop_get_loop(st.loop), std::memory_order_release);
  pw_stream_.store(st.stream, std::memory_order_release);

  std::thread stopper([&st, this] {
    while (!stop_.load(std::memory_order_relaxed)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (st.loop) pw_main_loop_quit(st.loop);
  });

  pw_main_loop_run(st.loop);
  stop_.store(true);
  if (stopper.joinable()) stopper.join();

  pw_loop_.store(nullptr, std::memory_order_release);
  pw_stream_.store(nullptr, std::memory_order_release);
  // Drop holds without queue_buffer — loop has quit; stream may already be ERROR.
  release_all_held(nullptr);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  pw_stream_disconnect(st.stream);
  pw_stream_destroy(st.stream);
  spa_hook_remove(&core_listener);
  pw_core_disconnect(st.core);
  pw_context_destroy(st.context);
  pw_main_loop_destroy(st.loop);
  pw_deinit();
}

}  // namespace vrp
