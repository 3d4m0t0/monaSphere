#pragma once

#include <string>

namespace vrp {

/** Provenance status for bundled / resolved libvosk.so. */
struct VoskLibStatus {
  enum class Kind {
    Missing,           // no candidate found
    OfficialMatch,     // path SHA-256 == pinned official prebuilt
    OfficialMismatch,  // bundled path present but digest wrong (refused)
    Override,          // MONASPHERE_VOSK_LIB or system lib (not pinned proof)
  };

  Kind kind = Kind::Missing;
  std::string path;
  std::string actual_sha256;    // lowercase hex, empty if unreadable
  std::string expected_sha256;  // pinned official digest
  std::string detail;           // short human note
};

/** Official vosk-api release pinned in third_party/vosk/SHA256SUMS. */
const char* vosk_official_version();
const char* vosk_official_zip_url();
const char* vosk_official_lib_sha256();

/** SHA-256 (lowercase hex) of a file; empty on I/O error. */
std::string file_sha256_hex(const std::string& path);

/**
 * Resolve preferred libvosk path and verify bundled copy against the official
 * digest. Does not dlopen. Safe to call from UI thread.
 */
VoskLibStatus probe_vosk_lib();

/** Absolute path suitable for dlopen, or empty if none / bundled mismatch-only. */
std::string resolve_vosk_lib_for_dlopen(VoskLibStatus* status_out = nullptr);

}  // namespace vrp
