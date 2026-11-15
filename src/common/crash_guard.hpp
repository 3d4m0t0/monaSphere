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

/**
 * After vkDestroyInstance: NVIDIA may still hold the old client for a few seconds.
 * Stamp the release time so the next connect can settle / restart Monado.
 */
void mark_vulkan_client_released();

/**
 * True when a release stamp exists and is younger than max_age_ms (default 5s).
 * Does not clear the stamp.
 */
bool vulkan_client_release_recent(int max_age_ms = 5000);

/**
 * If a recent release stamp exists, sleep until min_age_ms have passed since it,
 * then clear the stamp. Returns true when a wait was performed.
 */
bool settle_vulkan_client_release(int min_age_ms = 2500);

}  // namespace crash_guard
}  // namespace vrp
