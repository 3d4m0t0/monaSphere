#pragma once

#ifndef XR_USE_GRAPHICS_API_VULKAN
#define XR_USE_GRAPHICS_API_VULKAN
#endif

#include "common.hpp"
#include "options.hpp"

#include <vulkan/vulkan.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

namespace vrp {

struct SwapchainImage {
  XrSwapchainImageVulkan2KHR xr_image{};
  VkImageView view = VK_NULL_HANDLE;
  VkFramebuffer framebuffer = VK_NULL_HANDLE;
};

struct ViewSwapchain {
  XrSwapchain handle = XR_NULL_HANDLE;
  int32_t width = 0;
  int32_t height = 0;
  VkFormat format = VK_FORMAT_UNDEFINED;
  std::vector<SwapchainImage> images;
};

// Thin OpenXR + Vulkan session used by all VRP apps.
class XrVulkanApp {
 public:
  XrVulkanApp();
  ~XrVulkanApp();

  XrVulkanApp(const XrVulkanApp&) = delete;
  XrVulkanApp& operator=(const XrVulkanApp&) = delete;

  void init();
  /**
   * OpenXR + Vulkan bring-up. Reports progress; aborts when cancel becomes true
   * (e.g. between xrGetSystem retries). Throws std::runtime_error on failure.
   */
  void init(std::atomic<bool>* cancel, const std::function<void(const std::string&)>& progress = {});
  void shutdown();

  /** Must be set before init(). Defaults: Oculus Touch + right hand. */
  void set_controller_prefs(ControllerProfile profile, ControllerHand hand);

  // Returns false when the session should exit.
  bool pump_events();
  bool is_session_running() const { return session_running_; }
  bool is_session_focused() const { return session_focused_; }

  /** Unblock xrWaitFrame from another thread (window close / disconnect). */
  void request_exit();

  struct FrameInfo {
    XrTime display_time = 0;
    XrDuration display_period = 0;
    XrSpaceLocation head{};
    std::array<XrView, 2> views{};
    uint32_t view_count = 0;
  };

  // Begin frame; fills views. Returns false if nothing to render this tick.
  bool begin_frame(FrameInfo& out);
  void end_frame(const FrameInfo& info, const std::vector<XrCompositionLayerBaseHeader*>& layers);

  XrInstance instance() const { return instance_; }
  XrSession session() const { return session_; }
  XrSpace app_space() const { return app_space_; }
  VkInstance vk_instance() const { return vk_instance_; }
  VkDevice device() const { return device_; }
  VkPhysicalDevice physical_device() const { return physical_device_; }
  VkQueue queue() const { return queue_; }
  uint32_t queue_family() const { return queue_family_index_; }
  VkCommandPool command_pool() const { return command_pool_; }
  VkRenderPass color_pass() const { return color_pass_; }
  std::mutex& queue_mutex() { return queue_mu_; }
  bool external_memory_fd() const { return external_memory_fd_; }
  const std::vector<ViewSwapchain>& swapchains() const { return swapchains_; }
  std::vector<ViewSwapchain>& swapchains() { return swapchains_; }

  // Place LOCAL origin at current head (yaw+position). Call after a successful begin_frame.
  void recenter();
  /** Auto-recenter once when head tracking first becomes valid (session focus). */
  void maybe_init_tracking_origin();
  XrTime last_display_time() const { return last_display_time_; }

  struct XrStatus {
    std::string system_name;
    uint32_t vendor_id = 0;
    std::string session_state;
    bool session_running = false;
    bool focused = false;
    bool tracking_valid = false;
    int view_width = 0;
    int view_height = 0;
    double display_hz = 0.0;
    double app_frame_ms = 0.0;
    std::string sync_note;
    bool refresh_rate_ext = false;
    std::string refresh_rates;  // e.g. "90, 120" when OpenXR can switch
    std::string controller_profile;  // active left/right interaction profiles
    std::string controller_pref;     // user selection summary
  };
  XrStatus status() const;
  void note_frame_ms(double ms) { last_frame_ms_ = ms; }

  /** XR_FB_display_refresh_rate available on this runtime. */
  bool has_display_refresh_rate_ext() const { return refresh_rate_ext_; }
  /** Supported rates (Hz), empty if extension unavailable / session not ready. */
  std::vector<float> list_display_refresh_rates() const;
  /** Current rate from extension, or 0 if unknown. */
  float current_display_refresh_rate() const;
  /**
   * Request a display refresh rate (OpenXR). hz<=0 → highest supported.
   * Returns true if the runtime accepted the request (not a guarantee of switch).
   */
  bool request_display_refresh_rate(float hz);
  /** Stored preference applied when the session becomes READY (and on request). */
  void set_preferred_display_refresh_hz(float hz);

  // Action polling (optional; safe if actions failed to init).
  struct InputState {
    bool play_toggle = false;
    bool seek_back = false;
    bool seek_forward = false;
    /** True while seek key-repeat is active (after initial delay, until release). */
    bool seek_scrubbing = false;
    bool recenter = false;
    // HMD UI / pad
    float stick_x = 0.f;
    float stick_y = 0.f;
    bool confirm = false;
    bool confirm_held = false;  // A / confirm level
    bool back = false;
    bool back_held = false;  // B / back button level
    bool menu_toggle = false;
  };
  InputState poll_actions();

 private:
  void create_instance();
  void create_instance(std::atomic<bool>* cancel, const std::function<void(const std::string&)>& progress);
  void create_vulkan(std::atomic<bool>* cancel, const std::function<void(const std::string&)>& progress);
  void create_session();
  void create_spaces();
  void create_swapchains();
  void create_actions();
  void destroy_swapchains();
  void load_refresh_rate_extensions();
  bool apply_preferred_refresh_rate();

  XrInstance instance_ = XR_NULL_HANDLE;
  XrSystemId system_ = XR_NULL_SYSTEM_ID;
  XrSession session_ = XR_NULL_HANDLE;
  XrSpace app_space_ = XR_NULL_HANDLE;     // may be offset after recenter
  XrSpace local_space_ = XR_NULL_HANDLE;   // persistent identity LOCAL (recenter base)
  XrSpace view_space_ = XR_NULL_HANDLE;

  bool session_running_ = false;
  bool session_focused_ = false;
  bool tracking_valid_ = false;
  bool tracking_origin_inited_ = false;
  XrSessionState session_state_ = XR_SESSION_STATE_UNKNOWN;
  std::string system_name_;
  uint32_t vendor_id_ = 0;

  VkInstance vk_instance_ = VK_NULL_HANDLE;
  VkPhysicalDevice physical_device_ = VK_NULL_HANDLE;
  VkDevice device_ = VK_NULL_HANDLE;
  VkQueue queue_ = VK_NULL_HANDLE;
  uint32_t queue_family_index_ = 0;
  VkCommandPool command_pool_ = VK_NULL_HANDLE;
  VkRenderPass color_pass_ = VK_NULL_HANDLE;
  std::mutex queue_mu_;
  bool external_memory_fd_ = false;

  std::vector<ViewSwapchain> swapchains_;
  std::array<XrViewConfigurationView, 2> config_views_{};

  XrTime last_display_time_ = 0;
  XrDuration last_display_period_ = 0;
  double last_frame_ms_ = 0.0;

  // XR_FB_display_refresh_rate (optional)
  bool refresh_rate_ext_ = false;
  float preferred_refresh_hz_ = 0.f;  // 0 = auto (highest)
  float ext_display_hz_ = 0.f;       // last known from get/event
  PFN_xrEnumerateDisplayRefreshRatesFB pfn_enumerate_refresh_rates_ = nullptr;
  PFN_xrGetDisplayRefreshRateFB pfn_get_refresh_rate_ = nullptr;
  PFN_xrRequestDisplayRefreshRateFB pfn_request_refresh_rate_ = nullptr;

  // Actions
  XrActionSet action_set_ = XR_NULL_HANDLE;
  XrAction play_action_ = XR_NULL_HANDLE;
  XrAction seek_back_action_ = XR_NULL_HANDLE;
  XrAction seek_fwd_action_ = XR_NULL_HANDLE;
  XrAction recenter_action_ = XR_NULL_HANDLE;
  XrAction navigate_action_ = XR_NULL_HANDLE;  // VECTOR2
  XrAction confirm_action_ = XR_NULL_HANDLE;
  XrAction back_action_ = XR_NULL_HANDLE;
  XrAction menu_toggle_action_ = XR_NULL_HANDLE;
  bool actions_ready_ = false;
  ControllerProfile controller_profile_ = ControllerProfile::Gamepad;
  ControllerHand controller_hand_ = ControllerHand::Right;

  // LB/RB-equivalent seek: keyboard-style hold repeat
  bool seek_back_held_ = false;
  bool seek_fwd_held_ = false;
  bool seek_back_repeating_ = false;
  bool seek_fwd_repeating_ = false;
  std::chrono::steady_clock::time_point seek_back_next_{};
  std::chrono::steady_clock::time_point seek_fwd_next_{};
};

}  // namespace vrp
