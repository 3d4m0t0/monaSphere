#include "audio/vosk_bundle.hpp"

#include "common.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <vector>

#if defined(__linux__)
#include <unistd.h>
#endif

namespace vrp {
namespace {

// Pinned official vosk-linux-x86_64-0.3.45.zip → libvosk.so
constexpr const char* kOfficialVersion = "0.3.45";
constexpr const char* kOfficialZipUrl =
    "https://github.com/alphacep/vosk-api/releases/download/v0.3.45/"
    "vosk-linux-x86_64-0.3.45.zip";
constexpr const char* kOfficialLibSha256 =
    "85c4654de3acdeb99abab86eeb2a6e603927d37089597c0fcc33d8638dc2ccaf";

// Compact SHA-256 (public-domain style).
struct Sha256 {
  void init() {
    h_[0] = 0x6a09e667u;
    h_[1] = 0xbb67ae85u;
    h_[2] = 0x3c6ef372u;
    h_[3] = 0xa54ff53au;
    h_[4] = 0x510e527fu;
    h_[5] = 0x9b05688cu;
    h_[6] = 0x1f83d9abu;
    h_[7] = 0x5be0cd19u;
    len_ = 0;
    off_ = 0;
  }

  void update(const uint8_t* data, size_t n) {
    for (size_t i = 0; i < n; ++i) {
      buf_[off_++] = data[i];
      if (off_ == 64) {
        transform(buf_);
        off_ = 0;
      }
    }
    len_ += n;
  }

  void final(uint8_t out[32]) {
    const uint64_t bit_len = len_ * 8;
    buf_[off_++] = 0x80;
    if (off_ > 56) {
      while (off_ < 64) buf_[off_++] = 0;
      transform(buf_);
      off_ = 0;
    }
    while (off_ < 56) buf_[off_++] = 0;
    for (int i = 7; i >= 0; --i) buf_[off_++] = static_cast<uint8_t>((bit_len >> (i * 8)) & 0xff);
    transform(buf_);
    for (int i = 0; i < 8; ++i) {
      out[i * 4 + 0] = static_cast<uint8_t>((h_[i] >> 24) & 0xff);
      out[i * 4 + 1] = static_cast<uint8_t>((h_[i] >> 16) & 0xff);
      out[i * 4 + 2] = static_cast<uint8_t>((h_[i] >> 8) & 0xff);
      out[i * 4 + 3] = static_cast<uint8_t>(h_[i] & 0xff);
    }
  }

 private:
  static uint32_t rotr(uint32_t x, uint32_t n) { return (x >> n) | (x << (32 - n)); }

  void transform(const uint8_t block[64]) {
    static const uint32_t k[64] = {
        0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u,
        0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu,
        0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu,
        0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
        0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
        0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
        0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u,
        0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
        0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u,
        0xc67178f2u};
    uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
      w[i] = (uint32_t(block[i * 4]) << 24) | (uint32_t(block[i * 4 + 1]) << 16) |
             (uint32_t(block[i * 4 + 2]) << 8) | uint32_t(block[i * 4 + 3]);
    }
    for (int i = 16; i < 64; ++i) {
      const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
      const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4], f = h_[5], g = h_[6],
             hh = h_[7];
    for (int i = 0; i < 64; ++i) {
      const uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
      const uint32_t ch = (e & f) ^ ((~e) & g);
      const uint32_t t1 = hh + S1 + ch + k[i] + w[i];
      const uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
      const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
      const uint32_t t2 = S0 + maj;
      hh = g;
      g = f;
      f = e;
      e = d + t1;
      d = c;
      c = b;
      b = a;
      a = t1 + t2;
    }
    h_[0] += a;
    h_[1] += b;
    h_[2] += c;
    h_[3] += d;
    h_[4] += e;
    h_[5] += f;
    h_[6] += g;
    h_[7] += hh;
  }

  uint32_t h_[8]{};
  uint64_t len_ = 0;
  size_t off_ = 0;
  uint8_t buf_[64]{};
};

std::string hex32(const uint8_t dig[32]) {
  static const char* kHex = "0123456789abcdef";
  std::string out(64, '0');
  for (int i = 0; i < 32; ++i) {
    out[size_t(i) * 2] = kHex[(dig[i] >> 4) & 0xf];
    out[size_t(i) * 2 + 1] = kHex[dig[i] & 0xf];
  }
  return out;
}

std::string exe_dir() {
#if defined(__linux__)
  std::array<char, 4096> buf{};
  const ssize_t n = readlink("/proc/self/exe", buf.data(), buf.size() - 1);
  if (n <= 0) return {};
  std::string path(buf.data(), static_cast<size_t>(n));
  const auto slash = path.find_last_of('/');
  if (slash == std::string::npos) return {};
  return path.substr(0, slash);
#else
  return {};
#endif
}

bool file_readable(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  return static_cast<bool>(in);
}

std::vector<std::string> bundled_candidates() {
  std::vector<std::string> out;
  const std::string bin = exe_dir();
  if (!bin.empty()) {
    out.push_back(bin + "/../lib/monasphere/libvosk.so");
    out.push_back(bin + "/lib/monasphere/libvosk.so");
    out.push_back(bin + "/libvosk.so");
  }
  out.push_back("/usr/lib/monasphere/libvosk.so");
  out.push_back("/usr/lib64/monasphere/libvosk.so");
  // Dev tree (optional): next to sources after tools/fetch_vosk.sh
#ifdef VRP_SOURCE_DIR
  out.push_back(std::string(VRP_SOURCE_DIR) + "/third_party/vosk/linux-x86_64/libvosk.so");
#endif
  return out;
}

VoskLibStatus status_for_path(const std::string& path, bool treat_as_bundled) {
  VoskLibStatus st;
  st.path = path;
  st.expected_sha256 = kOfficialLibSha256;
  st.actual_sha256 = file_sha256_hex(path);
  if (st.actual_sha256.empty()) {
    st.kind = VoskLibStatus::Kind::Missing;
    st.detail = "unreadable";
    return st;
  }
  if (st.actual_sha256 == kOfficialLibSha256) {
    st.kind = VoskLibStatus::Kind::OfficialMatch;
    st.detail = std::string("official vosk-api v") + kOfficialVersion + " prebuilt";
    return st;
  }
  if (treat_as_bundled) {
    st.kind = VoskLibStatus::Kind::OfficialMismatch;
    st.detail = "SHA-256 mismatch — refusing bundled libvosk.so";
    return st;
  }
  st.kind = VoskLibStatus::Kind::Override;
  st.detail = "not the pinned official digest";
  return st;
}

}  // namespace

const char* vosk_official_version() { return kOfficialVersion; }
const char* vosk_official_zip_url() { return kOfficialZipUrl; }
const char* vosk_official_lib_sha256() { return kOfficialLibSha256; }

std::string file_sha256_hex(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return {};
  Sha256 ctx;
  ctx.init();
  std::array<char, 1 << 16> buf{};
  while (in) {
    in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
    const auto n = in.gcount();
    if (n > 0) ctx.update(reinterpret_cast<const uint8_t*>(buf.data()), static_cast<size_t>(n));
  }
  uint8_t dig[32];
  ctx.final(dig);
  return hex32(dig);
}

VoskLibStatus probe_vosk_lib() {
  if (const char* env = std::getenv("MONASPHERE_VOSK_LIB"); env && *env) {
    if (!file_readable(env)) {
      VoskLibStatus st;
      st.kind = VoskLibStatus::Kind::Missing;
      st.path = env;
      st.expected_sha256 = kOfficialLibSha256;
      st.detail = "MONASPHERE_VOSK_LIB not readable";
      return st;
    }
    auto st = status_for_path(env, /*treat_as_bundled=*/false);
    if (st.kind == VoskLibStatus::Kind::OfficialMatch) {
      st.detail = std::string("MONASPHERE_VOSK_LIB — official vosk-api v") + kOfficialVersion;
    } else {
      st.kind = VoskLibStatus::Kind::Override;
      st.detail = "MONASPHERE_VOSK_LIB override";
    }
    return st;
  }

  for (const std::string& path : bundled_candidates()) {
    if (!file_readable(path)) continue;
    return status_for_path(path, /*treat_as_bundled=*/true);
  }

  static const char* kSystem[] = {"libvosk.so", "libvosk.so.0", "/usr/lib64/libvosk.so",
                                  "/usr/lib/libvosk.so"};
  for (const char* path : kSystem) {
    // dlopen will resolve bare names; only probe absolute ones for hash display.
    if (path[0] != '/') continue;
    if (!file_readable(path)) continue;
    auto st = status_for_path(path, /*treat_as_bundled=*/false);
    st.kind = VoskLibStatus::Kind::Override;
    st.detail = "system libvosk.so";
    return st;
  }

  VoskLibStatus st;
  st.kind = VoskLibStatus::Kind::Missing;
  st.expected_sha256 = kOfficialLibSha256;
  st.detail = "libvosk.so not found (run tools/fetch_vosk.sh or set MONASPHERE_VOSK_LIB)";
  return st;
}

std::string resolve_vosk_lib_for_dlopen(VoskLibStatus* status_out) {
  VoskLibStatus st = probe_vosk_lib();
  if (status_out) *status_out = st;

  if (const char* env = std::getenv("MONASPHERE_VOSK_LIB"); env && *env) {
    if (file_readable(env)) return env;
    return {};
  }

  if (st.kind == VoskLibStatus::Kind::OfficialMatch) return st.path;
  if (st.kind == VoskLibStatus::Kind::OfficialMismatch) {
    VRP_LOG("voice-stt: refusing mismatched bundled libvosk.so (%s)", st.path.c_str());
    // Fall through to system names without returning the bad path.
  } else if (st.kind == VoskLibStatus::Kind::Override && !st.path.empty() && st.path[0] == '/') {
    return st.path;
  }

  return "libvosk.so";
}

}  // namespace vrp
