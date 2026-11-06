#include "xr/xr_vulkan_app.hpp"

#include "common/crash_guard.hpp"

#include <atomic>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <functional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace vrp {
namespace {

PFN_xrVoidFunction load_xr(XrInstance instance, const char* name) {
  PFN_xrVoidFunction fn = nullptr;
  XrResult r = xrGetInstanceProcAddr(instance, name, &fn);
  if (XR_FAILED(r) || !fn) {
    vrp_fatal(std::string("Missing OpenXR function: ") + name);
  }
  return fn;
}

PFN_xrVoidFunction try_load_xr(XrInstance instance, const char* name) {
  PFN_xrVoidFunction fn = nullptr;
  const XrResult r = xrGetInstanceProcAddr(instance, name, &fn);
  if (XR_FAILED(r) || !fn) return nullptr;
  return fn;
}

const char* xr_result_string(XrResult r) {
  switch (r) {
    case XR_SUCCESS: return "XR_SUCCESS";
    case XR_TIMEOUT_EXPIRED: return "XR_TIMEOUT_EXPIRED";
    case XR_SESSION_LOSS_PENDING: return "XR_SESSION_LOSS_PENDING";
    case XR_EVENT_UNAVAILABLE: return "XR_EVENT_UNAVAILABLE";
    case XR_SPACE_BOUNDS_UNAVAILABLE: return "XR_SPACE_BOUNDS_UNAVAILABLE";
    case XR_SESSION_NOT_FOCUSED: return "XR_SESSION_NOT_FOCUSED";
    case XR_FRAME_DISCARDED: return "XR_FRAME_DISCARDED";
    case XR_ERROR_VALIDATION_FAILURE: return "XR_ERROR_VALIDATION_FAILURE";
    case XR_ERROR_RUNTIME_FAILURE: return "XR_ERROR_RUNTIME_FAILURE";
    case XR_ERROR_OUT_OF_MEMORY: return "XR_ERROR_OUT_OF_MEMORY";
    case XR_ERROR_API_VERSION_UNSUPPORTED: return "XR_ERROR_API_VERSION_UNSUPPORTED";
    case XR_ERROR_INITIALIZATION_FAILED: return "XR_ERROR_INITIALIZATION_FAILED";
    case XR_ERROR_FUNCTION_UNSUPPORTED: return "XR_ERROR_FUNCTION_UNSUPPORTED";
    case XR_ERROR_FEATURE_UNSUPPORTED: return "XR_ERROR_FEATURE_UNSUPPORTED";
    case XR_ERROR_EXTENSION_NOT_PRESENT: return "XR_ERROR_EXTENSION_NOT_PRESENT";
    case XR_ERROR_LIMIT_REACHED: return "XR_ERROR_LIMIT_REACHED";
    case XR_ERROR_SIZE_INSUFFICIENT: return "XR_ERROR_SIZE_INSUFFICIENT";
    case XR_ERROR_HANDLE_INVALID: return "XR_ERROR_HANDLE_INVALID";
    case XR_ERROR_INSTANCE_LOST: return "XR_ERROR_INSTANCE_LOST";
    case XR_ERROR_SESSION_RUNNING: return "XR_ERROR_SESSION_RUNNING";
    case XR_ERROR_SESSION_NOT_RUNNING: return "XR_ERROR_SESSION_NOT_RUNNING";
    case XR_ERROR_SESSION_LOST: return "XR_ERROR_SESSION_LOST";
    case XR_ERROR_SYSTEM_INVALID: return "XR_ERROR_SYSTEM_INVALID";
    case XR_ERROR_PATH_INVALID: return "XR_ERROR_PATH_INVALID";
    case XR_ERROR_PATH_COUNT_EXCEEDED: return "XR_ERROR_PATH_COUNT_EXCEEDED";
    case XR_ERROR_PATH_FORMAT_INVALID: return "XR_ERROR_PATH_FORMAT_INVALID";
    case XR_ERROR_PATH_UNSUPPORTED: return "XR_ERROR_PATH_UNSUPPORTED";
    case XR_ERROR_LAYER_INVALID: return "XR_ERROR_LAYER_INVALID";
    case XR_ERROR_LAYER_LIMIT_EXCEEDED: return "XR_ERROR_LAYER_LIMIT_EXCEEDED";
    case XR_ERROR_SWAPCHAIN_RECT_INVALID: return "XR_ERROR_SWAPCHAIN_RECT_INVALID";
    case XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED: return "XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED";
    case XR_ERROR_ACTION_TYPE_MISMATCH: return "XR_ERROR_ACTION_TYPE_MISMATCH";
    case XR_ERROR_SESSION_NOT_READY: return "XR_ERROR_SESSION_NOT_READY";
    case XR_ERROR_SESSION_NOT_STOPPING: return "XR_ERROR_SESSION_NOT_STOPPING";
    case XR_ERROR_TIME_INVALID: return "XR_ERROR_TIME_INVALID";
    case XR_ERROR_REFERENCE_SPACE_UNSUPPORTED: return "XR_ERROR_REFERENCE_SPACE_UNSUPPORTED";
    case XR_ERROR_FILE_ACCESS_ERROR: return "XR_ERROR_FILE_ACCESS_ERROR";
    case XR_ERROR_FILE_CONTENTS_INVALID: return "XR_ERROR_FILE_CONTENTS_INVALID";
    case XR_ERROR_FORM_FACTOR_UNSUPPORTED: return "XR_ERROR_FORM_FACTOR_UNSUPPORTED";
    case XR_ERROR_FORM_FACTOR_UNAVAILABLE: return "XR_ERROR_FORM_FACTOR_UNAVAILABLE";
    case XR_ERROR_API_LAYER_NOT_PRESENT: return "XR_ERROR_API_LAYER_NOT_PRESENT";
    case XR_ERROR_CALL_ORDER_INVALID: return "XR_ERROR_CALL_ORDER_INVALID";
    case XR_ERROR_GRAPHICS_DEVICE_INVALID: return "XR_ERROR_GRAPHICS_DEVICE_INVALID";
    case XR_ERROR_POSE_INVALID: return "XR_ERROR_POSE_INVALID";
    case XR_ERROR_INDEX_OUT_OF_RANGE: return "XR_ERROR_INDEX_OUT_OF_RANGE";
    case XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED:
      return "XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED";
    case XR_ERROR_ENVIRONMENT_BLEND_MODE_UNSUPPORTED:
      return "XR_ERROR_ENVIRONMENT_BLEND_MODE_UNSUPPORTED";
    case XR_ERROR_NAME_DUPLICATED: return "XR_ERROR_NAME_DUPLICATED";
    case XR_ERROR_NAME_INVALID: return "XR_ERROR_NAME_INVALID";
    case XR_ERROR_ACTIONSET_NOT_ATTACHED: return "XR_ERROR_ACTIONSET_NOT_ATTACHED";
    case XR_ERROR_ACTIONSETS_ALREADY_ATTACHED: return "XR_ERROR_ACTIONSETS_ALREADY_ATTACHED";
    case XR_ERROR_LOCALIZED_NAME_DUPLICATED: return "XR_ERROR_LOCALIZED_NAME_DUPLICATED";
    case XR_ERROR_LOCALIZED_NAME_INVALID: return "XR_ERROR_LOCALIZED_NAME_INVALID";
    case XR_ERROR_GRAPHICS_REQUIREMENTS_CALL_MISSING:
      return "XR_ERROR_GRAPHICS_REQUIREMENTS_CALL_MISSING";
    case XR_ERROR_RUNTIME_UNAVAILABLE: return "XR_ERROR_RUNTIME_UNAVAILABLE";
    default: return "XR_ERROR_UNKNOWN";
  }
}

const char* vk_result_string(VkResult r) {
  switch (r) {
    case VK_SUCCESS: return "VK_SUCCESS";
    case VK_NOT_READY: return "VK_NOT_READY";
    case VK_TIMEOUT: return "VK_TIMEOUT";
    case VK_EVENT_SET: return "VK_EVENT_SET";
    case VK_EVENT_RESET: return "VK_EVENT_RESET";
    case VK_INCOMPLETE: return "VK_INCOMPLETE";
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_MEMORY_MAP_FAILED: return "VK_ERROR_MEMORY_MAP_FAILED";
    case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
    case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
    case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
    case VK_ERROR_TOO_MANY_OBJECTS: return "VK_ERROR_TOO_MANY_OBJECTS";
    case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
    case VK_ERROR_FRAGMENTED_POOL: return "VK_ERROR_FRAGMENTED_POOL";
    case VK_ERROR_UNKNOWN: return "VK_ERROR_UNKNOWN";
    case VK_ERROR_OUT_OF_POOL_MEMORY: return "VK_ERROR_OUT_OF_POOL_MEMORY";
    case VK_ERROR_INVALID_EXTERNAL_HANDLE: return "VK_ERROR_INVALID_EXTERNAL_HANDLE";
    case VK_ERROR_FRAGMENTATION: return "VK_ERROR_FRAGMENTATION";
    case VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS: return "VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS";
    case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
    case VK_ERROR_NATIVE_WINDOW_IN_USE_KHR: return "VK_ERROR_NATIVE_WINDOW_IN_USE_KHR";
    case VK_ERROR_VALIDATION_FAILED_EXT: return "VK_ERROR_VALIDATION_FAILED_EXT";
    default: return "VK_RESULT_UNKNOWN";
  }
}

std::string vk_api_version_string(uint32_t v) {
  return std::to_string(VK_API_VERSION_MAJOR(v)) + "." + std::to_string(VK_API_VERSION_MINOR(v)) +
         "." + std::to_string(VK_API_VERSION_PATCH(v));
}

/** OpenXR stores Vulkan apiVersion fields as XrVersion (48/32/0 packing), not VkApiVersion. */
std::string xr_packed_vk_version_string(XrVersion v) {
  return std::to_string(XR_VERSION_MAJOR(v)) + "." + std::to_string(XR_VERSION_MINOR(v)) + "." +
         std::to_string(XR_VERSION_PATCH(v));
}

std::string format_xr_vk_fail(const char* what, XrResult xr, VkResult vk) {
  return std::string(what) + " failed: xr=" + xr_result_string(xr) + "(" + std::to_string(static_cast<int>(xr)) +
         ") vk=" + vk_result_string(vk) + "(" + std::to_string(static_cast<int>(vk)) + ")";
}

void check_xr(XrResult r, const char* what) {
  if (XR_FAILED(r)) {
    vrp_fatal(std::string(what) + " failed: " + xr_result_string(r) + " (" + std::to_string(static_cast<int>(r)) +
              ")");
  }
}

bool has_extension(const std::vector<XrExtensionProperties>& props, const char* name) {
  for (const auto& p : props) {
    if (std::strcmp(p.extensionName, name) == 0) return true;
  }
  return false;
}

}  // namespace

XrVulkanApp::XrVulkanApp() = default;

XrVulkanApp::~XrVulkanApp() { shutdown(); }

void XrVulkanApp::init() { init(nullptr, {}); }

void XrVulkanApp::init(std::atomic<bool>* cancel, const std::function<void(const std::string&)>& progress) {
  auto note = [&](const char* msg) {
    VRP_LOG("%s", msg);
    if (progress) progress(msg);
  };
  auto check_cancel = [&] {
    if (cancel && cancel->load()) {
      throw std::runtime_error("Connect cancelled");
    }
  };

  note("Connecting to OpenXR…");
  check_cancel();
  create_instance(cancel, progress);
  check_cancel();
  note("Init Vulkan device…");
  create_vulkan(cancel, progress);
  check_cancel();
  note("Creating OpenXR session…");
  create_session();
  check_cancel();
  create_spaces();
  check_cancel();
  note("Preparing swapchains…");
  create_swapchains();
  check_cancel();
  note("Setting up controllers…");
  create_actions();
  note("OpenXR + Vulkan ready");
  VRP_LOG("OpenXR + Vulkan session ready");
  // Cookie so the next launch can restart Monado after SEGV / hard kill.
  crash_guard::mark_session_active();
}

void XrVulkanApp::request_exit() {
  // Safe to call from the UI thread while the XR thread blocks in xrWaitFrame.
  exit_requested_.store(true, std::memory_order_release);
  if (!session_) return;
  const XrResult r = xrRequestExitSession(session_);
  // Already STOPPING from a prior request — Monado warns; treat as ok.
  if (XR_FAILED(r) && r != XR_ERROR_SESSION_NOT_RUNNING && r != XR_ERROR_SESSION_LOST) {
    VRP_ERR("xrRequestExitSession failed (%d)", static_cast<int>(r));
  }
}

namespace {

/** Run a runtime/driver call that can block until Monado dies. Detach on timeout. */
template <typename Fn>
bool run_timed(const char* what, int timeout_ms, Fn&& fn) {
  std::atomic<bool> done{false};
  std::thread worker([&] {
    fn();
    done.store(true, std::memory_order_release);
  });
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (!done.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  if (done.load(std::memory_order_acquire)) {
    worker.join();
    return true;
  }
  VRP_ERR("%s timed out after %dms — leak and continue", what, timeout_ms);
  worker.detach();
  return false;
}

}  // namespace

void XrVulkanApp::finish_exit() {
  VRP_LOG("shutdown: finish_exit begin running=%d session=%d", session_running_ ? 1 : 0,
          session_ ? 1 : 0);
  exit_requested_.store(true, std::memory_order_release);
  if (!session_) {
    session_running_ = false;
    VRP_LOG("shutdown: finish_exit end (no session)");
    return;
  }
  if (session_running_) {
    VRP_LOG("shutdown: finish_exit request_exit");
    request_exit();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    while (session_running_ && std::chrono::steady_clock::now() < deadline) {
      if (!pump_events()) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (session_running_) {
      // STOPPING event missed (blocked in WaitFrame earlier) — EndSession best-effort.
      const XrSession session = session_;
      const bool ended = run_timed("finish_exit xrEndSession", 1000, [session] {
        const XrResult er = xrEndSession(session);
        if (XR_FAILED(er) && er != XR_ERROR_SESSION_NOT_STOPPING &&
            er != XR_ERROR_SESSION_NOT_RUNNING) {
          VRP_ERR("finish_exit: xrEndSession (%d)", static_cast<int>(er));
        }
      });
      session_running_ = false;
      if (!ended) session_ = XR_NULL_HANDLE;
    }
  }
  VRP_LOG("shutdown: finish_exit end running=%d session=%d", session_running_ ? 1 : 0,
          session_ ? 1 : 0);
}

void XrVulkanApp::report_if_device_lost(VkResult result) const {
  if (result != VK_ERROR_DEVICE_LOST) return;
  VRP_ERR("VK_ERROR_DEVICE_LOST — GPU/context gone (restart Monado if Connect hangs)");
#if defined(VK_EXT_DEVICE_FAULT_EXTENSION_NAME)
  if (!device_fault_ext_ || !pfn_get_device_fault_ || !device_) return;
  VkDeviceFaultCountsEXT counts{VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT};
  if (pfn_get_device_fault_(device_, &counts, nullptr) != VK_SUCCESS) return;
  std::vector<VkDeviceFaultAddressInfoEXT> addrs(counts.addressInfoCount);
  std::vector<VkDeviceFaultVendorInfoEXT> vendors(counts.vendorInfoCount);
  std::vector<std::uint8_t> binary(static_cast<size_t>(counts.vendorBinarySize));
  VkDeviceFaultInfoEXT info{VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT};
  info.pAddressInfos = addrs.empty() ? nullptr : addrs.data();
  info.pVendorInfos = vendors.empty() ? nullptr : vendors.data();
  info.pVendorBinaryData = binary.empty() ? nullptr : binary.data();
  if (pfn_get_device_fault_(device_, &counts, &info) != VK_SUCCESS) return;
  if (info.description[0]) {
    VRP_ERR("device_fault: %s (addr=%u vendor=%u)", info.description, counts.addressInfoCount,
            counts.vendorInfoCount);
  } else {
    VRP_ERR("device_fault: (no description) addr=%u vendor=%u", counts.addressInfoCount,
            counts.vendorInfoCount);
  }
#else
  (void)0;
#endif
}

void XrVulkanApp::shutdown() {
  if (shutdown_done_.exchange(true, std::memory_order_acq_rel)) return;

  // Drop every handle without another runtime call. Used when a call is still
  // running on a detached thread, or the decode queue never went idle.
  auto abandon = [this] {
    swapchains_.clear();
    app_space_ = XR_NULL_HANDLE;
    local_space_ = XR_NULL_HANDLE;
    view_space_ = XR_NULL_HANDLE;
    action_set_ = XR_NULL_HANDLE;
    session_ = XR_NULL_HANDLE;
    session_running_ = false;
    actions_ready_ = false;
    if (ffmpeg_vulkan_) {
      static FfmpegVulkanDevice* leaked_hwctx = nullptr;
      leaked_hwctx = ffmpeg_vulkan_.release();
      (void)leaked_hwctx;
    }
    video_decode_ready_ = false;
    video_decode_queue_ = VK_NULL_HANDLE;
    video_decode_queue_family_ = UINT32_MAX;
    command_pool_ = VK_NULL_HANDLE;
    color_pass_ = VK_NULL_HANDLE;
    device_ = VK_NULL_HANDLE;
    queue_ = VK_NULL_HANDLE;
    physical_device_ = VK_NULL_HANDLE;
    vk_instance_ = VK_NULL_HANDLE;
    instance_ = XR_NULL_HANDLE;
    session_focused_ = false;
    tracking_valid_ = false;
    crash_guard::clear_session_cookie();
  };

  VRP_LOG("shutdown: begin running=%d session=%d device=%d", session_running_ ? 1 : 0,
          session_ ? 1 : 0, device_ != VK_NULL_HANDLE ? 1 : 0);
  // OpenXR session must be torn down before the Vulkan device it uses.
  if (session_ && session_running_) {
    VRP_LOG("shutdown: session still running -> request_exit");
    request_exit();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    while (session_running_ && std::chrono::steady_clock::now() < deadline) {
      if (!pump_events()) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (session_running_ && session_) {
      const XrSession session = session_;
      if (!run_timed("xrEndSession", 1000, [session] { (void)xrEndSession(session); })) {
        abandon();
        return;
      }
      session_running_ = false;
    }
  }

  // A wedged decode CB makes xrDestroySession / vkDestroyDevice block until
  // Monado is killed. Idle first; if the queue does not drain, leak and return
  // so the XR thread can finish without Broken pipe.
  if (device_ != VK_NULL_HANDLE) {
    VRP_LOG("shutdown: vkDeviceWaitIdle");
    const VkDevice dev = device_;
    if (!run_timed("vkDeviceWaitIdle", 1000, [dev] { (void)vkDeviceWaitIdle(dev); })) {
      VRP_ERR("shutdown: device not idle — skip Destroy* (decode queue still busy)");
      abandon();
      return;
    }
    VRP_LOG("shutdown: vkDeviceWaitIdle done");
  }

  VRP_LOG("shutdown: destroy_swapchains");
  if (!run_timed("destroy_swapchains", 2000, [this] { destroy_swapchains(); })) {
    abandon();
    return;
  }
  VRP_LOG("shutdown: destroy_swapchains done");

  VRP_LOG("shutdown: destroy spaces");
  const XrSpace app_space = app_space_;
  const XrSpace local_space = local_space_;
  const XrSpace view_space = view_space_;
  app_space_ = XR_NULL_HANDLE;
  local_space_ = XR_NULL_HANDLE;
  view_space_ = XR_NULL_HANDLE;
  if (!run_timed("xrDestroySpace", 1000, [app_space, local_space, view_space] {
        if (app_space) xrDestroySpace(app_space);
        if (local_space) xrDestroySpace(local_space);
        if (view_space) xrDestroySpace(view_space);
      })) {
    abandon();
    return;
  }
  VRP_LOG("shutdown: destroy spaces done");
  tracking_origin_inited_ = false;
  if (action_set_) {
    VRP_LOG("shutdown: xrDestroyActionSet");
    const XrActionSet set = action_set_;
    action_set_ = XR_NULL_HANDLE;
    if (!run_timed("xrDestroyActionSet", 1000, [set] { xrDestroyActionSet(set); })) {
      abandon();
      return;
    }
    VRP_LOG("shutdown: xrDestroyActionSet done");
  }
  actions_ready_ = false;
  play_action_ = seek_back_action_ = seek_fwd_action_ = recenter_action_ = XR_NULL_HANDLE;
  navigate_action_ = confirm_action_ = back_action_ = menu_toggle_action_ = XR_NULL_HANDLE;
  flat_scale_action_ = view_reset_action_ = XR_NULL_HANDLE;
  look_action_ = XR_NULL_HANDLE;
  l2_action_ = r2_action_ = XR_NULL_HANDLE;
  menu_btn_down_ = false;
  menu_long_fired_ = false;

  if (session_) {
    VRP_LOG("shutdown: xrDestroySession");
    const XrSession session = session_;
    session_ = XR_NULL_HANDLE;
    if (!run_timed("xrDestroySession", 2000, [session] { xrDestroySession(session); })) {
      abandon();
      return;
    }
    VRP_LOG("shutdown: xrDestroySession done");
  }

  // Drop FFmpeg hwctx before DestroyDevice (inject does not own the VkDevice).
  // reset() takes queue_mu_ / waits on a decode CB and used to stall with no log.
  if (ffmpeg_vulkan_) {
    VRP_LOG("shutdown: ffmpeg hwctx reset");
    FfmpegVulkanDevice* ff = ffmpeg_vulkan_.get();
    if (!run_timed("ffmpeg hwctx reset", 1000, [ff] { ff->reset(); })) {
      static FfmpegVulkanDevice* leaked_hwctx = nullptr;
      leaked_hwctx = ffmpeg_vulkan_.release();
      (void)leaked_hwctx;
    } else {
      ffmpeg_vulkan_.reset();
    }
    VRP_LOG("shutdown: ffmpeg hwctx reset done");
  }
  video_decode_ready_ = false;
  video_decode_queue_ = VK_NULL_HANDLE;
  video_decode_queue_family_ = UINT32_MAX;
  device_fault_ext_ = false;
  pfn_get_device_fault_ = nullptr;
  enabled_dev_exts_.clear();

  // WaitIdle already succeeded above. vkDestroyDevice can still block in the
  // driver until Monado's IPC dies — that path printed no timeout of its own.
  if (device_ != VK_NULL_HANDLE) {
    VRP_LOG("shutdown: vkDestroyDevice");
    const VkDevice dev = device_;
    const VkCommandPool pool = command_pool_;
    const VkRenderPass pass = color_pass_;
    const VkInstance vk_inst = vk_instance_;
    command_pool_ = VK_NULL_HANDLE;
    color_pass_ = VK_NULL_HANDLE;
    device_ = VK_NULL_HANDLE;
    queue_ = VK_NULL_HANDLE;
    physical_device_ = VK_NULL_HANDLE;
    vk_instance_ = VK_NULL_HANDLE;
    if (!run_timed("vkDestroyDevice", 2000, [dev, pool, pass, vk_inst] {
          if (pool) vkDestroyCommandPool(dev, pool, nullptr);
          if (pass) vkDestroyRenderPass(dev, pass, nullptr);
          vkDestroyDevice(dev, nullptr);
          if (vk_inst) vkDestroyInstance(vk_inst, nullptr);
        })) {
      if (instance_) instance_ = XR_NULL_HANDLE;
      crash_guard::clear_session_cookie();
      VRP_LOG("shutdown: vkDestroyDevice timed out — leaked device");
      return;
    }
    VRP_LOG("shutdown: vkDestroyDevice done");
  } else {
    queue_ = VK_NULL_HANDLE;
    physical_device_ = VK_NULL_HANDLE;
    if (vk_instance_) {
      VRP_LOG("shutdown: vkDestroyInstance (no device)");
      const VkInstance vk_inst = vk_instance_;
      vk_instance_ = VK_NULL_HANDLE;
      (void)run_timed("vkDestroyInstance", 2000, [vk_inst] { vkDestroyInstance(vk_inst, nullptr); });
    }
  }

  if (instance_) {
    VRP_LOG("shutdown: xrDestroyInstance");
    const XrInstance instance = instance_;
    instance_ = XR_NULL_HANDLE;
    if (!run_timed("xrDestroyInstance", 2000, [instance] { xrDestroyInstance(instance); })) {
      crash_guard::clear_session_cookie();
      return;
    }
    VRP_LOG("shutdown: xrDestroyInstance done");
  }
  session_focused_ = false;
  tracking_valid_ = false;
  last_display_time_ = 0;
  crash_guard::clear_session_cookie();
  VRP_LOG("shutdown: end");
}

void XrVulkanApp::create_instance() { create_instance(nullptr, {}); }

void XrVulkanApp::create_instance(std::atomic<bool>* cancel,
                                  const std::function<void(const std::string&)>& progress) {
  uint32_t ext_count = 0;
  XrResult er = xrEnumerateInstanceExtensionProperties(nullptr, 0, &ext_count, nullptr);
  if (XR_FAILED(er)) {
    vrp_fatal(
        "No OpenXR runtime found. Install Monado and set "
        "XR_RUNTIME_JSON=/usr/share/openxr/1/openxr_monado.json");
  }
  std::vector<XrExtensionProperties> exts(ext_count, {XR_TYPE_EXTENSION_PROPERTIES});
  check_xr(xrEnumerateInstanceExtensionProperties(nullptr, ext_count, &ext_count, exts.data()),
           "enumerate XR extensions data");

  VRP_CHECK(has_extension(exts, XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME),
            "Runtime missing XR_KHR_vulkan_enable2");

  std::vector<const char*> enabled = {XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME};
  refresh_rate_ext_ = has_extension(exts, XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
  if (refresh_rate_ext_) {
    enabled.push_back(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
    VRP_LOG("OpenXR extension: %s", XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
  }

  XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};
  std::strncpy(ci.applicationInfo.applicationName, "VRP", XR_MAX_APPLICATION_NAME_SIZE - 1);
  ci.applicationInfo.applicationVersion = 1;
  std::strncpy(ci.applicationInfo.engineName, "VRP", XR_MAX_ENGINE_NAME_SIZE - 1);
  ci.applicationInfo.engineVersion = 1;
  ci.applicationInfo.apiVersion = XR_API_VERSION_1_0;
  ci.enabledExtensionCount = static_cast<uint32_t>(enabled.size());
  ci.enabledExtensionNames = enabled.data();

  check_xr(xrCreateInstance(&ci, &instance_), "xrCreateInstance");
  load_refresh_rate_extensions();

  XrSystemGetInfo sys{XR_TYPE_SYSTEM_GET_INFO};
  sys.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;

  // Monado returns FORM_FACTOR_UNAVAILABLE until the HMD is ready; poll instead of failing once.
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
  auto last_note = std::chrono::steady_clock::now() - std::chrono::seconds(2);
  for (;;) {
    if (cancel && cancel->load()) {
      throw std::runtime_error("Connect cancelled (waiting for HMD)");
    }
    const XrResult gr = xrGetSystem(instance_, &sys, &system_);
    if (XR_SUCCEEDED(gr)) break;
    if (gr != XR_ERROR_FORM_FACTOR_UNAVAILABLE) {
      check_xr(gr, "xrGetSystem");
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      throw std::runtime_error(
          "HMD not found within 60s (power, USB, Monado log)");
    }
    const auto now = std::chrono::steady_clock::now();
    if (progress && now - last_note >= std::chrono::seconds(1)) {
      last_note = now;
      progress("Waiting for HMD (power on / wear)");
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }

  XrSystemProperties props{XR_TYPE_SYSTEM_PROPERTIES};
  check_xr(xrGetSystemProperties(instance_, system_, &props), "xrGetSystemProperties");
  system_name_ = props.systemName;
  vendor_id_ = props.vendorId;
  VRP_LOG("OpenXR system id=%llu name=\"%s\" vendor=0x%x", static_cast<unsigned long long>(system_),
          system_name_.c_str(), vendor_id_);
  if (progress) {
    progress(std::string("HMD found: ") + system_name_);
  }
}

void XrVulkanApp::create_vulkan(std::atomic<bool>* cancel,
                                const std::function<void(const std::string&)>& progress) {
  auto note = [&](const std::string& msg) {
    VRP_LOG("%s", msg.c_str());
    if (progress) progress(msg);
  };
  auto check_cancel = [&] {
    if (cancel && cancel->load()) throw std::runtime_error("Connect cancelled");
  };

  auto xrGetVulkanGraphicsRequirements2KHR = reinterpret_cast<PFN_xrGetVulkanGraphicsRequirements2KHR>(
      load_xr(instance_, "xrGetVulkanGraphicsRequirements2KHR"));
  auto xrCreateVulkanInstanceKHR =
      reinterpret_cast<PFN_xrCreateVulkanInstanceKHR>(load_xr(instance_, "xrCreateVulkanInstanceKHR"));
  auto xrGetVulkanGraphicsDevice2KHR = reinterpret_cast<PFN_xrGetVulkanGraphicsDevice2KHR>(
      load_xr(instance_, "xrGetVulkanGraphicsDevice2KHR"));
  auto xrCreateVulkanDeviceKHR =
      reinterpret_cast<PFN_xrCreateVulkanDeviceKHR>(load_xr(instance_, "xrCreateVulkanDeviceKHR"));

  note("Vulkan: graphics requirements…");
  check_cancel();
  XrGraphicsRequirementsVulkan2KHR req{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR};
  check_xr(xrGetVulkanGraphicsRequirements2KHR(instance_, system_, &req), "graphics requirements");
  const std::string req_min = xr_packed_vk_version_string(req.minApiVersionSupported);
  const std::string req_max = xr_packed_vk_version_string(req.maxApiVersionSupported);
  VRP_LOG("Vulkan graphics requirements: min=%s max=%s", req_min.c_str(), req_max.c_str());

  VkApplicationInfo app_info{};
  app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  app_info.pApplicationName = "VRP";
  app_info.applicationVersion = 1;
  app_info.pEngineName = "VRP";
  app_info.engineVersion = 1;
  // FFmpeg AVVulkanDeviceContext wants ≥1.3; fall back to 1.1 if the runtime refuses.
  app_info.apiVersion = VK_API_VERSION_1_3;

  VkInstanceCreateInfo vici{};
  vici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  vici.pApplicationInfo = &app_info;
  vici.enabledLayerCount = 0;
  vici.ppEnabledLayerNames = nullptr;

  XrVulkanInstanceCreateInfoKHR xvici{XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR};
  xvici.systemId = system_;
  xvici.pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
  xvici.vulkanCreateInfo = &vici;
  xvici.vulkanAllocator = nullptr;

  auto create_vk_instance = [&](uint32_t api, const char* label) -> std::pair<XrResult, VkResult> {
    app_info.apiVersion = api;
    vk_instance_ = VK_NULL_HANDLE;
    VkResult vk_out = VK_SUCCESS;
    note(std::string("Vulkan: creating instance (") + label + " " + vk_api_version_string(api) +
         ", req " + req_min + ".." + req_max + ")…");
    check_cancel();
    // Monado compositor IPC can block forever here after a prior Broken pipe.
    std::atomic<bool> creating{true};
    std::thread watchdog([&] {
      for (int sec = 0; creating.load(std::memory_order_acquire); ++sec) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        if (!creating.load(std::memory_order_acquire)) return;
        if (sec > 0 && (sec % 5) == 0) {
          const std::string stall = std::string("Vulkan instance create blocked (") +
                                    std::to_string(sec) + "s, " + label +
                                    ") — Monado compositor may be wedged";
          VRP_ERR("%s", stall.c_str());
          if (progress) progress(stall);
        }
        if (cancel && cancel->load(std::memory_order_acquire)) {
          VRP_ERR("Vulkan instance create: cancel requested while blocked (%s)", label);
        }
      }
    });
    const XrResult xr_out = xrCreateVulkanInstanceKHR(instance_, &xvici, &vk_instance_, &vk_out);
    creating.store(false, std::memory_order_release);
    watchdog.join();
    return {xr_out, vk_out};
  };

  auto [xr_inst, vk_r] = create_vk_instance(VK_API_VERSION_1_3, "api=1.3");
  if (XR_FAILED(xr_inst) || vk_r != VK_SUCCESS || !vk_instance_) {
    const std::string fail13 = format_xr_vk_fail("xrCreateVulkanInstanceKHR(api=1.3)", xr_inst, vk_r);
    VRP_ERR("%s — retrying apiVersion 1.1 (req %s..%s)", fail13.c_str(), req_min.c_str(),
            req_max.c_str());
    if (progress) progress(fail13 + " — retry 1.1");

    auto [xr11, vk11] = create_vk_instance(VK_API_VERSION_1_1, "api=1.1");
    xr_inst = xr11;
    vk_r = vk11;
    if (XR_FAILED(xr_inst) || vk_r != VK_SUCCESS || !vk_instance_) {
      const std::string fail11 =
          format_xr_vk_fail("xrCreateVulkanInstanceKHR(api=1.1)", xr_inst, vk_r);
      VRP_ERR("%s (after 1.3 also failed). Restart monado-service / Connect again.", fail11.c_str());
      vrp_fatal(fail11 + " | earlier: " + fail13 + " | req " + req_min + ".." + req_max);
    }
    VRP_LOG("Vulkan instance apiVersion 1.1 (1.3 unavailable)");
  } else {
    VRP_LOG("Vulkan instance apiVersion 1.3");
  }

  note("Vulkan: picking GPU…");
  check_cancel();
  XrVulkanGraphicsDeviceGetInfoKHR gdgi{XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR};
  gdgi.systemId = system_;
  gdgi.vulkanInstance = vk_instance_;
  physical_device_ = VK_NULL_HANDLE;
  {
    const XrResult gd = xrGetVulkanGraphicsDevice2KHR(instance_, &gdgi, &physical_device_);
    if (XR_FAILED(gd) || !physical_device_) {
      const std::string detail =
          std::string("xrGetVulkanGraphicsDevice2KHR failed: ") + xr_result_string(gd) + " (" +
          std::to_string(static_cast<int>(gd)) +
          "). Often Monado compositor IPC died (Broken pipe) — restart monado-service.";
      VRP_ERR("%s", detail.c_str());
      vrp_fatal(detail);
    }
  }

  // Phase 0: log FFmpeg Vulkan Video zero-copy feasibility (does not change decode path).
  vulkan_video_probe_ = probe_vulkan_video(physical_device_);

  // Find a graphics queue (+ optional video decode family from Phase 0 probe).
  uint32_t qcount = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(physical_device_, &qcount, nullptr);
  std::vector<VkQueueFamilyProperties> qprops(qcount);
  vkGetPhysicalDeviceQueueFamilyProperties(physical_device_, &qcount, qprops.data());
  queue_family_index_ = UINT32_MAX;
  for (uint32_t i = 0; i < qcount; ++i) {
    if (qprops[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
      queue_family_index_ = i;
      break;
    }
  }
  VRP_CHECK(queue_family_index_ != UINT32_MAX, "No graphics queue");

  const uint32_t family_queues = qprops[queue_family_index_].queueCount;
  const bool split_compute = family_queues >= 2 &&
                             (qprops[queue_family_index_].queueFlags & VK_QUEUE_COMPUTE_BIT);
  float priorities[2] = {1.f, 0.5f};
  float decode_priority = 0.5f;
  video_decode_queue_family_ = vulkan_video_probe_.decode_queue_family;

  // Optional extensions for CUDA↔Vulkan zero-copy (opaque FD) + optional Video Decode.
  uint32_t ext_count = 0;
  vkEnumerateDeviceExtensionProperties(physical_device_, nullptr, &ext_count, nullptr);
  std::vector<VkExtensionProperties> avail(ext_count);
  vkEnumerateDeviceExtensionProperties(physical_device_, nullptr, &ext_count, avail.data());
  auto has_ext = [&](const char* name) {
    for (const auto& e : avail) {
      if (std::strcmp(e.extensionName, name) == 0) return true;
    }
    return false;
  };

  VkPhysicalDeviceFeatures gpu_features{};
  vkGetPhysicalDeviceFeatures(physical_device_, &gpu_features);
  VkPhysicalDeviceFeatures enabled_features{};
  if (gpu_features.textureCompressionBC) enabled_features.textureCompressionBC = VK_TRUE;

  const bool want_video = vulkan_video_probe_.candidate && app_info.apiVersion >= VK_API_VERSION_1_3 &&
                          video_decode_queue_family_ != UINT32_MAX;

  auto build_exts = [&](bool with_video, std::vector<const char*>* out, bool* out_ext_fd,
                        bool* out_dma, bool* out_fault) {
    out->clear();
    *out_ext_fd = false;
    *out_dma = false;
    *out_fault = false;
    if (has_ext(VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME)) {
      out->push_back(VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME);
    }
    if (has_ext(VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME)) {
      out->push_back(VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME);
      *out_ext_fd = true;
    }
    const bool dma_buf = has_ext(VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME);
    const bool drm_mod = has_ext(VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME);
    if (dma_buf && drm_mod && *out_ext_fd) {
      out->push_back(VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME);
      out->push_back(VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME);
      *out_dma = true;
    }
#if defined(VK_EXT_DEVICE_FAULT_EXTENSION_NAME)
    if (has_ext(VK_EXT_DEVICE_FAULT_EXTENSION_NAME)) {
      out->push_back(VK_EXT_DEVICE_FAULT_EXTENSION_NAME);
      *out_fault = true;
    }
#endif
    if (with_video) {
      auto add = [&](const char* n) {
        if (has_ext(n)) out->push_back(n);
      };
      add(VK_KHR_VIDEO_QUEUE_EXTENSION_NAME);
      add(VK_KHR_VIDEO_DECODE_QUEUE_EXTENSION_NAME);
#if defined(VK_KHR_VIDEO_DECODE_H264_EXTENSION_NAME)
      add(VK_KHR_VIDEO_DECODE_H264_EXTENSION_NAME);
#endif
#if defined(VK_KHR_VIDEO_DECODE_H265_EXTENSION_NAME)
      add(VK_KHR_VIDEO_DECODE_H265_EXTENSION_NAME);
#endif
#if defined(VK_KHR_VIDEO_DECODE_AV1_EXTENSION_NAME)
      add(VK_KHR_VIDEO_DECODE_AV1_EXTENSION_NAME);
#endif
      // Core in 1.2/1.3 but still listed by some stacks as device exts — add if advertised.
      add(VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME);
      add(VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME);
    }
  };

  auto try_create_device = [&](bool with_video) -> bool {
    std::vector<const char*> dev_exts;
    bool ext_fd = false;
    bool dma = false;
    bool fault = false;
    build_exts(with_video, &dev_exts, &ext_fd, &dma, &fault);

    std::vector<VkDeviceQueueCreateInfo> qcis;
    VkDeviceQueueCreateInfo gq{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    gq.queueFamilyIndex = queue_family_index_;
    gq.queueCount = split_compute ? 2u : 1u;
    gq.pQueuePriorities = priorities;
    qcis.push_back(gq);
    if (with_video && video_decode_queue_family_ != UINT32_MAX &&
        video_decode_queue_family_ != queue_family_index_) {
      VkDeviceQueueCreateInfo dq{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
      dq.queueFamilyIndex = video_decode_queue_family_;
      dq.queueCount = 1;
      dq.pQueuePriorities = &decode_priority;
      qcis.push_back(dq);
    }

    VkPhysicalDeviceVulkan13Features v13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    v13.synchronization2 = VK_TRUE;
    VkPhysicalDeviceVulkan12Features v12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    v12.timelineSemaphore = VK_TRUE;
    v12.pNext = with_video ? &v13 : nullptr;
#if defined(VK_EXT_DEVICE_FAULT_EXTENSION_NAME)
    VkPhysicalDeviceFaultFeaturesEXT fault_feat{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT};
    fault_feat.deviceFault = fault ? VK_TRUE : VK_FALSE;
    fault_feat.deviceFaultVendorBinary = VK_FALSE;
#endif
    VkPhysicalDeviceFeatures2 feats2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    feats2.features = enabled_features;
    if (with_video) {
#if defined(VK_EXT_DEVICE_FAULT_EXTENSION_NAME)
      if (fault) {
        fault_feat.pNext = &v12;
        feats2.pNext = &fault_feat;
      } else
#endif
      {
        feats2.pNext = &v12;
      }
    } else {
#if defined(VK_EXT_DEVICE_FAULT_EXTENSION_NAME)
      feats2.pNext = fault ? &fault_feat : nullptr;
#else
      feats2.pNext = nullptr;
#endif
    }

    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = static_cast<uint32_t>(qcis.size());
    dci.pQueueCreateInfos = qcis.data();
    dci.enabledExtensionCount = static_cast<uint32_t>(dev_exts.size());
    dci.ppEnabledExtensionNames = dev_exts.empty() ? nullptr : dev_exts.data();
    if (with_video || fault) {
      dci.pNext = &feats2;
      dci.pEnabledFeatures = nullptr;
    } else {
      dci.pEnabledFeatures = &enabled_features;
    }

    XrVulkanDeviceCreateInfoKHR xvdci{XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR};
    xvdci.systemId = system_;
    xvdci.pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
    xvdci.vulkanPhysicalDevice = physical_device_;
    xvdci.vulkanCreateInfo = &dci;
    xvdci.vulkanAllocator = nullptr;

    device_ = VK_NULL_HANDLE;
    vk_r = VK_SUCCESS;
    const XrResult xr_dev = xrCreateVulkanDeviceKHR(instance_, &xvdci, &device_, &vk_r);
    if (XR_FAILED(xr_dev) || vk_r != VK_SUCCESS || !device_) {
      VRP_ERR("%s", format_xr_vk_fail(with_video ? "xrCreateVulkanDeviceKHR(video)"
                                                 : "xrCreateVulkanDeviceKHR(baseline)",
                                      xr_dev, vk_r)
                        .c_str());
      device_ = VK_NULL_HANDLE;
      return false;
    }
    external_memory_fd_ = ext_fd;
    dma_buf_import_ = dma;
    device_fault_ext_ = fault;
    pfn_get_device_fault_ = nullptr;
#if defined(VK_EXT_DEVICE_FAULT_EXTENSION_NAME)
    if (fault) {
      pfn_get_device_fault_ = reinterpret_cast<PFN_vkGetDeviceFaultInfoEXT>(
          vkGetDeviceProcAddr(device_, "vkGetDeviceFaultInfoEXT"));
      VRP_LOG("Vulkan VK_EXT_device_fault: %s", pfn_get_device_fault_ ? "enabled" : "ext without entry");
    }
#endif
    enabled_dev_exts_.clear();
    enabled_dev_exts_.reserve(dev_exts.size());
    for (const char* e : dev_exts) enabled_dev_exts_.emplace_back(e);
    return true;
  };

  note("Vulkan: creating device…");
  check_cancel();
  video_decode_ready_ = false;
  video_decode_queue_ = VK_NULL_HANDLE;
  if (want_video) {
    if (try_create_device(true)) {
      video_decode_ready_ = true;
    } else {
      VRP_LOG("Vulkan Video: falling back to baseline device (CUDA path unchanged)");
      if (!try_create_device(false)) {
        vrp_fatal("Vulkan device create failed (baseline after video). "
                  "See xr=/vk= lines above; Monado Broken pipe -> restart monado-service.");
      }
    }
  } else if (!try_create_device(false)) {
    vrp_fatal("Vulkan device create failed (baseline). "
              "See xr=/vk= lines above; Monado Broken pipe -> restart monado-service.");
  }

  vkGetDeviceQueue(device_, queue_family_index_, 0, &queue_);
  if (split_compute) {
    vkGetDeviceQueue(device_, queue_family_index_, 1, &compute_queue_);
    note("Vulkan compute queue: index 1");
  } else {
    note("Vulkan compute queue: sharing the frame queue");
  }
  if (video_decode_ready_ && video_decode_queue_family_ != UINT32_MAX) {
    vkGetDeviceQueue(device_, video_decode_queue_family_, 0, &video_decode_queue_);
    vulkan_video_probe_.enabled_on_device = true;
    VRP_LOG("Vulkan Video: enabled on XR device (decode_qf=%u)", video_decode_queue_family_);

    // Phase 2: inject XR device into FFmpeg AVVulkanDeviceContext (playback still CUDA).
    std::vector<const char*> ext_ptrs;
    ext_ptrs.reserve(enabled_dev_exts_.size());
    for (const auto& s : enabled_dev_exts_) ext_ptrs.push_back(s.c_str());
    const uint32_t gq_count = split_compute ? 2u : 1u;
    ffmpeg_vulkan_ = std::make_unique<FfmpegVulkanDevice>();
    if (!ffmpeg_vulkan_->init(vk_instance_, physical_device_, device_, queue_family_index_, gq_count,
                              video_decode_queue_family_, 1u, &queue_mu_, ext_ptrs)) {
      VRP_LOG("FFmpeg Vulkan hwctx inject failed: %s — keep CUDA path",
              ffmpeg_vulkan_->last_error().c_str());
      ffmpeg_vulkan_.reset();
    } else {
      VRP_LOG("FFmpeg Vulkan hwctx: ready (decode still CUDA until Phase 3 bind)");
    }
  } else {
    vulkan_video_probe_.enabled_on_device = false;
    video_decode_queue_family_ = UINT32_MAX;
    ffmpeg_vulkan_.reset();
  }
  if (external_memory_fd_) {
    VRP_LOG("Vulkan external memory FD: yes (NVIDIA CUDA zero-copy)");
  } else {
    VRP_LOG("Vulkan external memory FD: no");
  }
  if (dma_buf_import_) {
    VRP_LOG("Vulkan dma-buf import: enabled (VA-API NV12)");
  } else {
    const bool dma_buf = has_ext(VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME);
    const bool drm_mod = has_ext(VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME);
    VRP_LOG("Vulkan dma-buf import: %s%s", dma_buf ? "available" : "missing",
            dma_buf && drm_mod ? " (+DRM modifiers, not enabled)" : "");
  }

  note("Vulkan: command pool…");
  check_cancel();
  VkCommandPoolCreateInfo cpci{};
  cpci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  cpci.queueFamilyIndex = queue_family_index_;
  VRP_CHECK(vkCreateCommandPool(device_, &cpci, nullptr, &command_pool_) == VK_SUCCESS, "command pool");
  color_pass_ = VK_NULL_HANDLE;
}

void XrVulkanApp::create_session() {
  XrGraphicsBindingVulkan2KHR binding{XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR};
  binding.instance = vk_instance_;
  binding.physicalDevice = physical_device_;
  binding.device = device_;
  binding.queueFamilyIndex = queue_family_index_;
  binding.queueIndex = 0;

  XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO};
  sci.next = &binding;
  sci.systemId = system_;
  check_xr(xrCreateSession(instance_, &sci, &session_), "xrCreateSession");
}

void XrVulkanApp::create_spaces() {
  XrReferenceSpaceCreateInfo rsci{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
  rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
  rsci.poseInReferenceSpace.orientation.w = 1.f;
  check_xr(xrCreateReferenceSpace(session_, &rsci, &local_space_), "local space");
  check_xr(xrCreateReferenceSpace(session_, &rsci, &app_space_), "app space");

  rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
  check_xr(xrCreateReferenceSpace(session_, &rsci, &view_space_), "view space");
  tracking_origin_inited_ = false;
}

void XrVulkanApp::create_swapchains() {
  uint32_t view_count = 0;
  check_xr(xrEnumerateViewConfigurationViews(instance_, system_, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0,
                                             &view_count, nullptr),
           "view config count");
  VRP_CHECK(view_count >= 2, "Need stereo views");
  config_views_.fill({XR_TYPE_VIEW_CONFIGURATION_VIEW});
  check_xr(xrEnumerateViewConfigurationViews(instance_, system_, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                             view_count, &view_count, config_views_.data()),
           "view config data");

  uint32_t fmt_count = 0;
  check_xr(xrEnumerateSwapchainFormats(session_, 0, &fmt_count, nullptr), "swapchain formats");
  std::vector<int64_t> formats(fmt_count);
  check_xr(xrEnumerateSwapchainFormats(session_, fmt_count, &fmt_count, formats.data()), "swapchain formats data");

  VkFormat selected = VK_FORMAT_UNDEFINED;
  const VkFormat preferred[] = {VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_R8G8B8A8_UNORM,
                                VK_FORMAT_B8G8R8A8_UNORM};
  for (VkFormat want : preferred) {
    for (int64_t f : formats) {
      if (static_cast<VkFormat>(f) == want) {
        selected = want;
        break;
      }
    }
    if (selected != VK_FORMAT_UNDEFINED) break;
  }
  VRP_CHECK(selected != VK_FORMAT_UNDEFINED, "No suitable swapchain format");

  // Recreate render pass with real format.
  if (color_pass_) {
    vkDestroyRenderPass(device_, color_pass_, nullptr);
    color_pass_ = VK_NULL_HANDLE;
  }
  {
    VkAttachmentDescription color{};
    color.format = selected;
    color.samples = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkAttachmentReference cref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &cref;
    VkRenderPassCreateInfo rpci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rpci.attachmentCount = 1;
    rpci.pAttachments = &color;
    rpci.subpassCount = 1;
    rpci.pSubpasses = &sub;
    VRP_CHECK(vkCreateRenderPass(device_, &rpci, nullptr, &color_pass_) == VK_SUCCESS, "render pass recreate");
  }

  swapchains_.resize(2);
  for (uint32_t i = 0; i < 2; ++i) {
    auto& sc = swapchains_[i];
    sc.width = static_cast<int32_t>(config_views_[i].recommendedImageRectWidth);
    sc.height = static_cast<int32_t>(config_views_[i].recommendedImageRectHeight);
    sc.format = selected;

    XrSwapchainCreateInfo sci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    sci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    sci.format = selected;
    sci.sampleCount = 1;
    sci.width = static_cast<uint32_t>(sc.width);
    sci.height = static_cast<uint32_t>(sc.height);
    sci.faceCount = 1;
    sci.arraySize = 1;
    sci.mipCount = 1;
    check_xr(xrCreateSwapchain(session_, &sci, &sc.handle), "xrCreateSwapchain");

    uint32_t img_count = 0;
    check_xr(xrEnumerateSwapchainImages(sc.handle, 0, &img_count, nullptr), "swapchain images");
    sc.images.resize(img_count);
    std::vector<XrSwapchainImageVulkan2KHR> xr_images(img_count, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR});
    check_xr(xrEnumerateSwapchainImages(sc.handle, img_count, &img_count,
                                        reinterpret_cast<XrSwapchainImageBaseHeader*>(xr_images.data())),
             "swapchain images data");

    for (uint32_t j = 0; j < img_count; ++j) {
      sc.images[j].xr_image = xr_images[j];

      VkImageViewCreateInfo ivci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
      ivci.image = xr_images[j].image;
      ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
      ivci.format = selected;
      ivci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
      ivci.subresourceRange.levelCount = 1;
      ivci.subresourceRange.layerCount = 1;
      VRP_CHECK(vkCreateImageView(device_, &ivci, nullptr, &sc.images[j].view) == VK_SUCCESS, "image view");

      VkFramebufferCreateInfo fbci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
      fbci.renderPass = color_pass_;
      fbci.attachmentCount = 1;
      fbci.pAttachments = &sc.images[j].view;
      fbci.width = static_cast<uint32_t>(sc.width);
      fbci.height = static_cast<uint32_t>(sc.height);
      fbci.layers = 1;
      VRP_CHECK(vkCreateFramebuffer(device_, &fbci, nullptr, &sc.images[j].framebuffer) == VK_SUCCESS,
                "framebuffer");
    }
    VRP_LOG("Swapchain[%u] %dx%d format=%d images=%u", i, sc.width, sc.height, static_cast<int>(selected),
            img_count);
  }
}

void XrVulkanApp::destroy_swapchains() {
  // Move out first so a timed caller can leak this object without racing the vector.
  auto local = std::move(swapchains_);
  swapchains_.clear();
  for (auto& sc : local) {
    for (auto& img : sc.images) {
      if (device_) {
        if (img.framebuffer) vkDestroyFramebuffer(device_, img.framebuffer, nullptr);
        if (img.view) vkDestroyImageView(device_, img.view, nullptr);
      }
    }
    if (sc.handle) {
      xrDestroySwapchain(sc.handle);
      sc.handle = XR_NULL_HANDLE;
    }
  }
  swapchains_.clear();
}

void XrVulkanApp::set_controller_prefs(ControllerProfile profile, ControllerHand hand) {
  controller_profile_ = profile;
  controller_hand_ = hand;
  VRP_LOG("Controller prefs: profile=%s hand=%s", controller_profile_name(profile),
          controller_hand_name(hand));
}

void XrVulkanApp::create_actions() {
  // TODO: VR controllers should drive the playback controls by aiming with the
  // controller pose (position and orientation). Bindings here are buttons and
  // the stick only.
  XrActionSetCreateInfo asci{XR_TYPE_ACTION_SET_CREATE_INFO};
  std::strncpy(asci.actionSetName, "vrp", XR_MAX_ACTION_SET_NAME_SIZE - 1);
  std::strncpy(asci.localizedActionSetName, "VRP", XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE - 1);
  if (XR_FAILED(xrCreateActionSet(instance_, &asci, &action_set_))) {
    VRP_LOG("Action set unavailable; controller input disabled");
    return;
  }

  auto make_bool = [&](const char* name, const char* localized, XrAction* out) {
    XrActionCreateInfo aci{XR_TYPE_ACTION_CREATE_INFO};
    aci.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
    std::strncpy(aci.actionName, name, XR_MAX_ACTION_NAME_SIZE - 1);
    std::strncpy(aci.localizedActionName, localized, XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
    check_xr(xrCreateAction(action_set_, &aci, out), name);
  };
  auto make_vec2 = [&](const char* name, const char* localized, XrAction* out) {
    XrActionCreateInfo aci{XR_TYPE_ACTION_CREATE_INFO};
    aci.actionType = XR_ACTION_TYPE_VECTOR2F_INPUT;
    std::strncpy(aci.actionName, name, XR_MAX_ACTION_NAME_SIZE - 1);
    std::strncpy(aci.localizedActionName, localized, XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
    check_xr(xrCreateAction(action_set_, &aci, out), name);
  };
  make_bool("play_toggle", "Play Toggle", &play_action_);
  make_bool("seek_back", "Seek Back", &seek_back_action_);
  make_bool("seek_forward", "Seek Forward", &seek_fwd_action_);
  make_bool("recenter", "Recenter", &recenter_action_);
  make_vec2("navigate", "Navigate", &navigate_action_);
  make_vec2("look", "Look", &look_action_);
  make_bool("confirm", "Confirm", &confirm_action_);
  make_bool("back", "Back", &back_action_);
  make_bool("menu_toggle", "Menu Toggle", &menu_toggle_action_);
  make_bool("flat_scale", "Flat Scale", &flat_scale_action_);
  make_bool("view_reset", "Reset View", &view_reset_action_);
  make_bool("rate_slower", "Slower", &l2_action_);
  make_bool("rate_faster", "Faster", &r2_action_);

  auto suggest = [&](const char* profile_path, const std::vector<XrActionSuggestedBinding>& binds) {
    XrPath profile = XR_NULL_PATH;
    if (XR_FAILED(xrStringToPath(instance_, profile_path, &profile))) return;
    XrInteractionProfileSuggestedBinding sug{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    sug.interactionProfile = profile;
    sug.suggestedBindings = binds.data();
    sug.countSuggestedBindings = static_cast<uint32_t>(binds.size());
    if (XR_SUCCEEDED(xrSuggestInteractionProfileBindings(instance_, &sug))) {
      VRP_LOG("Suggested bindings: %s", profile_path);
    } else {
      VRP_LOG("Binding suggest failed (ignored): %s", profile_path);
    }
  };

  auto path = [&](const char* s) {
    XrPath p = XR_NULL_PATH;
    xrStringToPath(instance_, s, &p);
    return p;
  };

  const bool left = controller_hand_ == ControllerHand::Left;
  const char* primary = left ? "left" : "right";
  const char* secondary = left ? "right" : "left";
  // Right-stick look. When the right stick is already navigate, the gamepad stick still turns the view.
  const bool look_on_right = left;
  auto hand_path = [&](const char* hand, const char* suffix) {
    char buf[128];
    std::snprintf(buf, sizeof(buf), "/user/hand/%s/input/%s", hand, suffix);
    return path(buf);
  };
  auto add_look = [&](std::vector<XrActionSuggestedBinding> binds, const char* component) {
    if (look_on_right) binds.push_back({look_action_, hand_path("right", component)});
    return binds;
  };

  const auto want = [&](ControllerProfile p) {
    if (controller_profile_ == ControllerProfile::Gamepad) return false;  // XR pads off
    return controller_profile_ == ControllerProfile::Auto || controller_profile_ == p;
  };

  // Khronos simple
  if (want(ControllerProfile::Simple)) {
    suggest("/interaction_profiles/khr/simple_controller",
            {{play_action_, hand_path(primary, "select/click")},
             {confirm_action_, hand_path(primary, "select/click")},
             {recenter_action_, hand_path(secondary, "menu/click")},
             {seek_back_action_, hand_path(secondary, "menu/click")},
             {seek_fwd_action_, hand_path(secondary, "select/click")},
             {back_action_, hand_path(secondary, "menu/click")},
             {menu_toggle_action_, hand_path(primary, "menu/click")}});
  }

  // Oculus Touch / PSVR2 Sense (typical under Monado)
  // Options long = menu; HMD system = recenter (pad: PS long=menu, L3+R3=recenter).
  // △ = Flat seats. R3 = reset look. ○ = back. Recenter also on HMD function.
  if (want(ControllerProfile::OculusTouch)) {
    suggest("/interaction_profiles/oculus/touch_controller",
            add_look({{navigate_action_, hand_path(primary, "thumbstick")},
                      {confirm_action_, hand_path(primary, left ? "x/click" : "a/click")},
                      {back_action_, hand_path("right", "b/click")},
                      {menu_toggle_action_, hand_path("left", "menu/click")},
                      {flat_scale_action_, hand_path("left", "y/click")},
                      {view_reset_action_, hand_path("right", "thumbstick/click")},
                      {play_action_, hand_path(primary, "squeeze/click")},
                      {seek_back_action_, hand_path(secondary, left ? "a/click" : "x/click")},
                      {seek_fwd_action_, hand_path("left", "thumbstick/click")},
                      {l2_action_, hand_path("left", "trigger/click")},
                      {r2_action_, hand_path("right", "trigger/click")}},
                     "thumbstick"));
  }

  // Microsoft motion
  if (want(ControllerProfile::MicrosoftMotion)) {
    suggest("/interaction_profiles/microsoft/motion_controller",
            add_look({{navigate_action_, hand_path(primary, "thumbstick")},
                      {confirm_action_, hand_path(primary, "squeeze/click")},
                      {back_action_, hand_path(primary, "trackpad/click")},
                      {menu_toggle_action_, hand_path(secondary, "menu/click")},
                      {play_action_, hand_path(primary, "trackpad/click")},
                      {l2_action_, hand_path("left", "trigger/click")},
                      {r2_action_, hand_path("right", "trigger/click")},
                      {recenter_action_, hand_path(secondary, "thumbstick/click")}},
                     "thumbstick"));
  }

  // Vive controller
  if (want(ControllerProfile::ViveController)) {
    suggest("/interaction_profiles/htc/vive_controller",
            add_look({{navigate_action_, hand_path(primary, "trackpad")},
                      {confirm_action_, hand_path(primary, "trigger/click")},
                      {back_action_, hand_path(primary, "menu/click")},
                      {menu_toggle_action_, hand_path(secondary, "menu/click")},
                      {play_action_, hand_path(primary, "squeeze/click")},
                      {l2_action_, hand_path("left", "trigger/click")},
                      {r2_action_, hand_path("right", "trigger/click")},
                      {recenter_action_, hand_path(secondary, "trackpad/click")}},
                     "trackpad"));
  }

  // Valve Index
  if (want(ControllerProfile::ValveIndex)) {
    suggest("/interaction_profiles/valve/index_controller",
            add_look({{navigate_action_, hand_path(primary, "thumbstick")},
                      {confirm_action_, hand_path(primary, "a/click")},
                      {back_action_, hand_path(primary, "b/click")},
                      {menu_toggle_action_, hand_path(secondary, "b/click")},
                      {play_action_, hand_path(primary, "trigger/click")},
                      {l2_action_, hand_path("left", "trigger/click")},
                      {r2_action_, hand_path("right", "trigger/click")},
                      {seek_back_action_, hand_path(secondary, "a/click")},
                      {seek_fwd_action_, hand_path(primary, "thumbstick/click")},
                      {recenter_action_, hand_path(secondary, "thumbstick/click")}},
                     "thumbstick"));
  }

  // Always: PSVR2 HMD function button → recenter
  {
    XrPath system_click{};
    if (XR_SUCCEEDED(xrStringToPath(instance_, "/user/head/input/system/click", &system_click))) {
      suggest("/interaction_profiles/htc/vive_pro", {{recenter_action_, system_click}});
    }
  }

  XrSessionActionSetsAttachInfo attach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
  attach.countActionSets = 1;
  attach.actionSets = &action_set_;
  if (XR_SUCCEEDED(xrAttachSessionActionSets(session_, &attach))) {
    actions_ready_ = true;
    VRP_LOG("OpenXR actions attached (profile=%s hand=%s)",
            controller_profile_name(controller_profile_), controller_hand_name(controller_hand_));
  }
}

bool XrVulkanApp::pump_events() {
  XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
  while (true) {
    ev = {XR_TYPE_EVENT_DATA_BUFFER};
    XrResult r = xrPollEvent(instance_, &ev);
    if (r == XR_EVENT_UNAVAILABLE) break;
    check_xr(r, "xrPollEvent");

    switch (ev.type) {
      case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
        auto* s = reinterpret_cast<XrEventDataSessionStateChanged*>(&ev);
        session_state_ = s->state;
        VRP_LOG("Session state -> %d", static_cast<int>(session_state_));
        switch (session_state_) {
          case XR_SESSION_STATE_READY: {
            XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO};
            bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
            check_xr(xrBeginSession(session_, &bi), "xrBeginSession");
            session_running_ = true;
            tracking_origin_inited_ = false;
            if (apply_preferred_refresh_rate()) {
              VRP_LOG("Applied preferred display refresh (%.1f Hz pref)", preferred_refresh_hz_);
            }
            break;
          }
          case XR_SESSION_STATE_STOPPING: {
            // Untimed xrEndSession here blocked teardown with no "timed out" line:
            // the UI only saw Session state 6 and then killed Monado.
            VRP_LOG("shutdown: pump STOPPING -> xrEndSession");
            const XrSession stopping = session_;
            const bool ended = run_timed("pump xrEndSession", 1000, [stopping] {
              const XrResult er = xrEndSession(stopping);
              if (XR_FAILED(er) && er != XR_ERROR_SESSION_NOT_STOPPING &&
                  er != XR_ERROR_SESSION_NOT_RUNNING) {
                VRP_ERR("pump xrEndSession (%d)", static_cast<int>(er));
              }
            });
            VRP_LOG("shutdown: pump xrEndSession done ok=%d", ended ? 1 : 0);
            session_running_ = false;
            tracking_origin_inited_ = false;
            if (!ended) session_ = XR_NULL_HANDLE;
            break;
          }
          case XR_SESSION_STATE_EXITING:
          case XR_SESSION_STATE_LOSS_PENDING:
            return false;
          case XR_SESSION_STATE_FOCUSED:
            session_focused_ = true;
            break;
          case XR_SESSION_STATE_VISIBLE:
            session_focused_ = false;
            break;
          default:
            break;
        }
        break;
      }
      case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
        return false;
      case XR_TYPE_EVENT_DATA_DISPLAY_REFRESH_RATE_CHANGED_FB: {
        auto* e = reinterpret_cast<XrEventDataDisplayRefreshRateChangedFB*>(&ev);
        ext_display_hz_ = e->toDisplayRefreshRate;
        VRP_LOG("Display refresh rate changed: %.2f -> %.2f Hz", e->fromDisplayRefreshRate,
                e->toDisplayRefreshRate);
        break;
      }
      default:
        break;
    }
  }
  return true;
}

bool XrVulkanApp::begin_frame(FrameInfo& out) {
  out = {};
  if (!session_running_) return false;
  // Never enter xrWaitFrame during close — it can block until Monado is killed
  // (Broken pipe) when the previous EndFrame is stuck on queue_mu_ / GPU.
  if (exit_requested_.load(std::memory_order_acquire)) return false;

  // Soften waitFrame errors after exit request — session may already be gone.
  XrFrameWaitInfo wi{XR_TYPE_FRAME_WAIT_INFO};
  XrFrameState fs{XR_TYPE_FRAME_STATE};
  {
    const XrResult wr = xrWaitFrame(session_, &wi, &fs);
    if (XR_FAILED(wr)) {
      VRP_ERR("xrWaitFrame failed (%d) — ending session loop", static_cast<int>(wr));
      session_running_ = false;
      return false;
    }
  }
  if (exit_requested_.load(std::memory_order_acquire)) {
    // Began wait before exit — discard without rendering.
    XrFrameEndInfo ei{XR_TYPE_FRAME_END_INFO};
    ei.displayTime = fs.predictedDisplayTime;
    ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    std::unique_lock<std::mutex> qlock(queue_mu_, std::try_to_lock);
    (void)xrEndFrame(session_, &ei);
    return false;
  }
  out.display_time = fs.predictedDisplayTime;
  out.display_period = fs.predictedDisplayPeriod;
  last_display_time_ = fs.predictedDisplayTime;
  last_display_period_ = fs.predictedDisplayPeriod;

  XrFrameBeginInfo bi{XR_TYPE_FRAME_BEGIN_INFO};
  check_xr(xrBeginFrame(session_, &bi), "xrBeginFrame");

  if (!fs.shouldRender) {
    XrFrameEndInfo ei{XR_TYPE_FRAME_END_INFO};
    ei.displayTime = fs.predictedDisplayTime;
    ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    check_xr(xrEndFrame(session_, &ei), "xrEndFrame empty");
    return false;
  }

  XrViewLocateInfo vli{XR_TYPE_VIEW_LOCATE_INFO};
  vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
  vli.displayTime = fs.predictedDisplayTime;
  vli.space = app_space_;
  XrViewState vs{XR_TYPE_VIEW_STATE};
  uint32_t view_count = 0;
  out.views[0].type = XR_TYPE_VIEW;
  out.views[1].type = XR_TYPE_VIEW;
  check_xr(xrLocateViews(session_, &vli, &vs, 2, &view_count, out.views.data()), "xrLocateViews");
  out.view_count = view_count;

  out.head = {XR_TYPE_SPACE_LOCATION};
  xrLocateSpace(view_space_, app_space_, fs.predictedDisplayTime, &out.head);
  const auto flags = out.head.locationFlags;
  tracking_valid_ = (flags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) != 0;
  return true;
}

XrVulkanApp::XrStatus XrVulkanApp::status() const {
  XrStatus s;
  s.system_name = system_name_.empty() ? "(unknown HMD)" : system_name_;
  s.vendor_id = vendor_id_;
  s.session_running = session_running_;
  s.focused = session_focused_;
  s.tracking_valid = tracking_valid_;
  switch (session_state_) {
    case XR_SESSION_STATE_UNKNOWN: s.session_state = "UNKNOWN"; break;
    case XR_SESSION_STATE_IDLE: s.session_state = "IDLE"; break;
    case XR_SESSION_STATE_READY: s.session_state = "READY"; break;
    case XR_SESSION_STATE_SYNCHRONIZED: s.session_state = "SYNCHRONIZED"; break;
    case XR_SESSION_STATE_VISIBLE: s.session_state = "VISIBLE"; break;
    case XR_SESSION_STATE_FOCUSED: s.session_state = "FOCUSED"; break;
    case XR_SESSION_STATE_STOPPING: s.session_state = "STOPPING"; break;
    case XR_SESSION_STATE_LOSS_PENDING: s.session_state = "LOSS_PENDING"; break;
    case XR_SESSION_STATE_EXITING: s.session_state = "EXITING"; break;
    default: s.session_state = "OTHER"; break;
  }
  if (!swapchains_.empty()) {
    s.view_width = swapchains_[0].width;
    s.view_height = swapchains_[0].height;
  }
  // Prefer predictedDisplayPeriod (actual pacing). FB get can lag / stay stale after mode switch.
  if (last_display_period_ > 0) {
    s.display_hz = 1e9 / static_cast<double>(last_display_period_);
  }
  if (s.display_hz <= 0.0 && refresh_rate_ext_ && pfn_get_refresh_rate_ && session_) {
    float hz = 0.f;
    if (XR_SUCCEEDED(pfn_get_refresh_rate_(session_, &hz)) && hz > 0.f) {
      s.display_hz = hz;
    }
  }
  if (s.display_hz <= 0.0 && ext_display_hz_ > 0.f) {
    s.display_hz = ext_display_hz_;
  }
  s.app_frame_ms = last_frame_ms_;
  if (s.display_hz > 0 && last_frame_ms_ > 0) {
    const double budget_ms = 1000.0 / s.display_hz;
    if (last_frame_ms_ > budget_ms * 1.15) {
      s.sync_note = "lag";
    } else {
      s.sync_note = "OK";
    }
  }
  s.refresh_rate_ext = refresh_rate_ext_;
  if (refresh_rate_ext_) {
    const auto rates = list_display_refresh_rates();
    std::string list;
    for (float r : rates) {
      if (!list.empty()) list += ", ";
      char buf[16];
      std::snprintf(buf, sizeof(buf), "%.0f", r);
      list += buf;
    }
    s.refresh_rates = list;
  }

  s.controller_pref = std::string(controller_profile_name(controller_profile_)) + "/" +
                      controller_hand_name(controller_hand_);
  if (session_ && instance_) {
    auto profile_name = [&](const char* top_level) -> std::string {
      XrPath top = XR_NULL_PATH;
      if (XR_FAILED(xrStringToPath(instance_, top_level, &top))) return {};
      XrInteractionProfileState st{XR_TYPE_INTERACTION_PROFILE_STATE};
      if (XR_FAILED(xrGetCurrentInteractionProfile(session_, top, &st)) || !st.interactionProfile) {
        return "(none)";
      }
      char buf[XR_MAX_PATH_LENGTH] = {};
      uint32_t len = 0;
      if (XR_FAILED(xrPathToString(instance_, st.interactionProfile, sizeof(buf), &len, buf))) {
        return "(?)";
      }
      std::string full(buf);
      const auto slash = full.find_last_of('/');
      return slash == std::string::npos ? full : full.substr(slash + 1);
    };
    s.controller_profile = "L:" + profile_name("/user/hand/left") + " R:" + profile_name("/user/hand/right");
  }
  return s;
}

void XrVulkanApp::end_frame(const FrameInfo& info, const std::vector<XrCompositionLayerBaseHeader*>& layers) {
  XrFrameEndInfo ei{XR_TYPE_FRAME_END_INFO};
  ei.displayTime = info.display_time;
  ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
  ei.layerCount = static_cast<uint32_t>(layers.size());
  ei.layers = layers.empty() ? nullptr : layers.data();
  // Monado's compositor submits on the same VkQueue during xrEndFrame. Serialize with
  // CudaNv12Texture / VideoTexture / eye submits that also use queue_mu_ — otherwise the
  // NVIDIA driver can SEGV when decode ensure_size transitions while end_frame runs.
  // On exit, decode may hold queue_mu_ inside WaitForFences for seconds — never block
  // EndFrame forever or the next WaitFrame / UI join hangs until Monado is killed.
  if (exit_requested_.load(std::memory_order_acquire)) {
    std::unique_lock<std::mutex> qlock(queue_mu_, std::defer_lock);
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(50);
    while (!qlock.try_lock()) {
      if (std::chrono::steady_clock::now() >= deadline) {
        VRP_LOG("end_frame: exit — EndFrame without queue_mu (decode holds it)");
        (void)xrEndFrame(session_, &ei);
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    (void)xrEndFrame(session_, &ei);
    return;
  }
  std::lock_guard<std::mutex> qlock(queue_mu_);
  check_xr(xrEndFrame(session_, &ei), "xrEndFrame");
}

namespace {

XrPosef yaw_position_pose(const XrPosef& head) {
  // Look direction = rotate (0,0,-1) by orientation.
  const auto& q = head.orientation;
  const float fx = 2.f * (q.x * q.z + q.w * q.y);
  const float fz = 1.f - 2.f * (q.x * q.x + q.y * q.y);
  // Rotated +Z is (fx, fy, fz); we want -Z look → negate.
  const float lx = -fx;
  const float lz = -fz;
  const float yaw = std::atan2(-lx, -lz);
  const float half = yaw * 0.5f;
  XrPosef out{};
  out.position = head.position;
  out.orientation = {0.f, std::sin(half), 0.f, std::cos(half)};
  return out;
}

}  // namespace

bool XrVulkanApp::recenter() {
  if (!session_ || !view_space_ || !local_space_) {
    VRP_ERR("recenter: session/spaces not ready");
    return false;
  }
  if (last_display_time_ == 0) {
    VRP_ERR("recenter: no predicted display time yet（HMD 接続後に再度）");
    return false;
  }

  // Always locate against the identity LOCAL space — locating in app_space_ after a prior
  // recenter compounds offsets and makes the HMD button feel broken.
  XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
  const XrResult lr = xrLocateSpace(view_space_, local_space_, last_display_time_, &loc);
  if (XR_FAILED(lr)) {
    VRP_ERR("recenter: xrLocateSpace failed (%d)", static_cast<int>(lr));
    return false;
  }
  const auto flags = loc.locationFlags;
  if ((flags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) == 0 ||
      (flags & XR_SPACE_LOCATION_POSITION_VALID_BIT) == 0) {
    VRP_ERR("recenter: head pose invalid (flags=0x%llx)",
            static_cast<unsigned long long>(flags));
    return false;
  }

  // New LOCAL origin at head position, yaw-only (keep horizon level for video).
  const XrPosef pose = yaw_position_pose(loc.pose);

  if (app_space_) {
    xrDestroySpace(app_space_);
    app_space_ = XR_NULL_HANDLE;
  }

  XrReferenceSpaceCreateInfo rsci{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
  rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
  rsci.poseInReferenceSpace = pose;
  check_xr(xrCreateReferenceSpace(session_, &rsci, &app_space_), "recenter space");
  tracking_origin_inited_ = true;
  VRP_LOG("Recentered (LOCAL at head yaw/position)");
  return true;
}

bool XrVulkanApp::maybe_init_tracking_origin() {
  if (tracking_origin_inited_ || !session_running_ || !session_focused_) return false;
  if (!tracking_valid_ || last_display_time_ == 0) return false;
  if (!recenter()) return false;
  VRP_LOG("Tracking origin initialized (first valid head pose)");
  return true;
}

XrVulkanApp::InputState XrVulkanApp::poll_actions() {
  InputState st{};
  if (!actions_ready_ || !session_running_) return st;

  XrActiveActionSet active{action_set_, XR_NULL_PATH};
  XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO};
  sync.countActiveActionSets = 1;
  sync.activeActionSets = &active;
  if (XR_FAILED(xrSyncActions(session_, &sync))) return st;

  auto edge = [&](XrAction action) -> bool {
    if (!action) return false;
    XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
    gi.action = action;
    XrActionStateBoolean b{XR_TYPE_ACTION_STATE_BOOLEAN};
    if (XR_FAILED(xrGetActionStateBoolean(session_, &gi, &b))) return false;
    return b.isActive && b.changedSinceLastSync && b.currentState;
  };

  auto key_repeat = [](bool down, bool& held, bool& repeating,
                       std::chrono::steady_clock::time_point& next,
                       std::chrono::steady_clock::time_point now) -> bool {
    constexpr auto kInitial = std::chrono::milliseconds(400);
    constexpr auto kInterval = std::chrono::milliseconds(120);
    if (!down) {
      held = false;
      repeating = false;
      return false;
    }
    if (!held) {
      held = true;
      repeating = false;
      next = now + kInitial;
      return true;
    }
    if (now >= next) {
      next = now + kInterval;
      repeating = true;
      return true;
    }
    return false;
  };

  auto action_down = [&](XrAction action) -> bool {
    if (!action) return false;
    XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
    gi.action = action;
    XrActionStateBoolean b{XR_TYPE_ACTION_STATE_BOOLEAN};
    if (XR_FAILED(xrGetActionStateBoolean(session_, &gi, &b))) return false;
    return b.isActive && b.currentState;
  };

  st.play_toggle = edge(play_action_);
  const auto now = std::chrono::steady_clock::now();
  const bool seek_back_down = action_down(seek_back_action_);
  const bool seek_fwd_down = action_down(seek_fwd_action_);
  st.seek_back_held = seek_back_down;
  st.seek_forward_held = seek_fwd_down;
  st.l2_held = action_down(l2_action_);
  st.r2_held = action_down(r2_action_);
  st.seek_back = key_repeat(seek_back_down, seek_back_held_, seek_back_repeating_, seek_back_next_, now);
  st.seek_forward = key_repeat(seek_fwd_down, seek_fwd_held_, seek_fwd_repeating_, seek_fwd_next_, now);
  st.seek_scrubbing = seek_back_repeating_ || seek_fwd_repeating_;
  st.recenter = edge(recenter_action_);
  st.confirm = edge(confirm_action_);
  st.confirm_held = action_down(confirm_action_);
  st.back = edge(back_action_);
  st.back_held = action_down(back_action_);
  st.flat_scale = edge(flat_scale_action_);
  st.view_reset = edge(view_reset_action_);

  // Options / menu button: long press toggles the VR menu (same as video). Short is unused
  // so DualSense Options can pass through to desktop apps. Pad recenter is L3+R3; HMD system stays.
  {
    constexpr auto kMenuLongPress = std::chrono::milliseconds(700);
    const bool menu_down = action_down(menu_toggle_action_);
    if (menu_down) {
      if (!menu_btn_down_) {
        menu_btn_down_ = true;
        menu_long_fired_ = false;
        menu_down_at_ = now;
      } else if (!menu_long_fired_ && now - menu_down_at_ >= kMenuLongPress) {
        st.menu_toggle = true;
        menu_long_fired_ = true;
      }
    } else if (menu_btn_down_) {
      menu_btn_down_ = false;
      menu_long_fired_ = false;
    }
  }

  if (navigate_action_) {
    XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
    gi.action = navigate_action_;
    XrActionStateVector2f v{XR_TYPE_ACTION_STATE_VECTOR2F};
    if (XR_SUCCEEDED(xrGetActionStateVector2f(session_, &gi, &v)) && v.isActive) {
      st.stick_x = v.currentState.x;
      st.stick_y = v.currentState.y;
    }
  }
  if (look_action_) {
    XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
    gi.action = look_action_;
    XrActionStateVector2f v{XR_TYPE_ACTION_STATE_VECTOR2F};
    if (XR_SUCCEEDED(xrGetActionStateVector2f(session_, &gi, &v)) && v.isActive) {
      st.stick_rx = v.currentState.x;
      st.stick_ry = v.currentState.y;
    }
  }
  return st;
}

void XrVulkanApp::load_refresh_rate_extensions() {
  pfn_enumerate_refresh_rates_ = nullptr;
  pfn_get_refresh_rate_ = nullptr;
  pfn_request_refresh_rate_ = nullptr;
  if (!refresh_rate_ext_ || !instance_) {
    refresh_rate_ext_ = false;
    return;
  }
  pfn_enumerate_refresh_rates_ = reinterpret_cast<PFN_xrEnumerateDisplayRefreshRatesFB>(
      try_load_xr(instance_, "xrEnumerateDisplayRefreshRatesFB"));
  pfn_get_refresh_rate_ = reinterpret_cast<PFN_xrGetDisplayRefreshRateFB>(
      try_load_xr(instance_, "xrGetDisplayRefreshRateFB"));
  pfn_request_refresh_rate_ = reinterpret_cast<PFN_xrRequestDisplayRefreshRateFB>(
      try_load_xr(instance_, "xrRequestDisplayRefreshRateFB"));
  if (!pfn_enumerate_refresh_rates_ || !pfn_get_refresh_rate_ || !pfn_request_refresh_rate_) {
    VRP_ERR("XR_FB_display_refresh_rate enabled but procs missing — disabling");
    refresh_rate_ext_ = false;
    pfn_enumerate_refresh_rates_ = nullptr;
    pfn_get_refresh_rate_ = nullptr;
    pfn_request_refresh_rate_ = nullptr;
  }
}

void XrVulkanApp::set_preferred_display_refresh_hz(float hz) {
  preferred_refresh_hz_ = hz;
}

std::vector<float> XrVulkanApp::list_display_refresh_rates() const {
  std::vector<float> out;
  if (!refresh_rate_ext_ || !pfn_enumerate_refresh_rates_ || !session_) return out;
  uint32_t count = 0;
  XrResult r = pfn_enumerate_refresh_rates_(session_, 0, &count, nullptr);
  if (XR_FAILED(r) || count == 0) return out;
  out.resize(count);
  r = pfn_enumerate_refresh_rates_(session_, count, &count, out.data());
  if (XR_FAILED(r)) {
    out.clear();
    return out;
  }
  out.resize(count);
  return out;
}

float XrVulkanApp::current_display_refresh_rate() const {
  if (refresh_rate_ext_ && pfn_get_refresh_rate_ && session_) {
    float hz = 0.f;
    if (XR_SUCCEEDED(pfn_get_refresh_rate_(session_, &hz)) && hz > 0.f) return hz;
  }
  if (ext_display_hz_ > 0.f) return ext_display_hz_;
  if (last_display_period_ > 0) return static_cast<float>(1e9 / static_cast<double>(last_display_period_));
  return 0.f;
}

bool XrVulkanApp::apply_preferred_refresh_rate() {
  return request_display_refresh_rate(preferred_refresh_hz_);
}

bool XrVulkanApp::request_display_refresh_rate(float hz) {
  preferred_refresh_hz_ = hz;
  if (!refresh_rate_ext_ || !pfn_request_refresh_rate_ || !session_) return false;
  // Preference is applied from SESSION_STATE_READY via apply_preferred_refresh_rate().
  if (!session_running_) return false;

  const auto rates = list_display_refresh_rates();
  float target = 0.f;
  if (rates.empty()) {
    // Some runtimes accept a direct request without a useful enumerate list.
    target = (hz <= 0.f) ? 0.f : hz;
  } else if (hz <= 0.f) {
    target = *std::max_element(rates.begin(), rates.end());
  } else {
    float best = rates.front();
    float best_err = std::fabs(best - hz);
    for (float r : rates) {
      const float err = std::fabs(r - hz);
      if (err < best_err) {
        best_err = err;
        best = r;
      }
    }
    if (best_err > 3.f) {
      VRP_ERR("No display refresh near %.1f Hz (closest %.1f, err %.1f)", hz, best, best_err);
      return false;
    }
    target = best;
  }

  const XrResult r = pfn_request_refresh_rate_(session_, target);
  if (XR_FAILED(r)) {
    VRP_ERR("xrRequestDisplayRefreshRateFB(%.1f) failed (%d)", target, static_cast<int>(r));
    return false;
  }
  VRP_LOG("xrRequestDisplayRefreshRateFB -> %.1f Hz (pref %.1f)", target, hz);
  if (pfn_get_refresh_rate_) {
    float now = 0.f;
    if (XR_SUCCEEDED(pfn_get_refresh_rate_(session_, &now)) && now > 0.f) ext_display_hz_ = now;
  }
  return true;
}

}  // namespace vrp
