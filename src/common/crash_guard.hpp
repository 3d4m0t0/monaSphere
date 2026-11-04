#pragma once

#include <string>

namespace vrp {

/**
 * Crash cookie + fatal-signal handlers.
 *
 * Signal handlers are async-signal-safe only (no Vulkan / FFmpeg / Qt / malloc).
 * A leftover cookie means the previous XR session did not shut down cleanly —
 * HostWindow should restart owned Monado before connecting again.
 */
namespace crash_guard {

/** Install SIGSEGV/ABRT/BUS/FPE/ILL handlers and std::terminate. Call once from main. */
void install();

/** Absolute path to the session cookie (empty if HOME/XDG unset). */
std::string cookie_path();

/** Write "active\\n<pid>\\n". Call when an XR+Vulkan session is up. */
void mark_session_active();

/** Unlink cookie. Call from clean XrVulkanApp::shutdown. */
void clear_session_cookie();

/** True when cookie exists (previous run crashed, was SIGKILL'd, or still active). */
bool previous_session_unclean();

/** Consume unclean state: true if unclean, then clear cookie. */
bool consume_unclean_session();

}  // namespace crash_guard
}  // namespace vrp
