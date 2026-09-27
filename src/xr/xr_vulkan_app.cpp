#include "xr/xr_vulkan_app.hpp"

#include <atomic>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <functional>
#include <string>
#include <thread>
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
    case XR_ERROR_FORM_FACTOR_UNAVAILABLE: return "XR_ERROR_FORM_FACTOR_UNAVAILABLE";
    case XR_ERROR_VALIDATION_FAILURE: return "XR_ERROR_VALIDATION_FAILURE";
    default: return "XR_ERROR";
  }
}

void check_xr(XrResult r, const char* what) {
  if (XR_FAILED(r)) {
    vrp_fatal(std::string(what) + " failed: " + xr_result_string(r) + " (" + std::to_string(r) + ")");
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
}

void XrVulkanApp::request_exit() {
  // Safe to call from the UI thread while the XR thread blocks in xrWaitFrame.
  if (!session_) return;
  const XrResult r = xrRequestExitSession(session_);
  if (XR_FAILED(r) && r != XR_ERROR_SESSION_NOT_RUNNING && r != XR_ERROR_SESSION_LOST) {
    VRP_ERR("xrRequestExitSession failed (%d)", static_cast<int>(r));
  }
}

void XrVulkanApp::shutdown() {
  // OpenXR session must be torn down before the Vulkan device it uses.
  if (session_ && session_running_) {
    request_exit();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (session_running_ && std::chrono::steady_clock::now() < deadline) {
      if (!pump_events()) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (session_running_) {
      xrEndSession(session_);
      session_running_ = false;
    }
  }

  destroy_swapchains();

  if (app_space_) {
    xrDestroySpace(app_space_);
    app_space_ = XR_NULL_HANDLE;
  }
  if (local_space_) {
    xrDestroySpace(local_space_);
    local_space_ = XR_NULL_HANDLE;
  }
  if (view_space_) {
    xrDestroySpace(view_space_);
    view_space_ = XR_NULL_HANDLE;
  }
  tracking_origin_inited_ = false;
  if (action_set_) {
    xrDestroyActionSet(action_set_);
    action_set_ = XR_NULL_HANDLE;
  }
  actions_ready_ = false;
  play_action_ = seek_back_action_ = seek_fwd_action_ = recenter_action_ = XR_NULL_HANDLE;
  navigate_action_ = confirm_action_ = back_action_ = menu_toggle_action_ = XR_NULL_HANDLE;

  if (session_) {
    xrDestroySession(session_);
    session_ = XR_NULL_HANDLE;
  }

  if (device_ != VK_NULL_HANDLE) {
    // HMD unplug can wedge the GPU/compositor path; never block forever here.
    std::atomic<bool> idle_done{false};
    std::thread idle_thr([this, &idle_done] {
      (void)vkDeviceWaitIdle(device_);
      idle_done.store(true, std::memory_order_release);
    });
    const auto idle_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!idle_done.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < idle_deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (idle_done.load(std::memory_order_acquire)) {
      idle_thr.join();
    } else {
      VRP_ERR("vkDeviceWaitIdle timed out during XR shutdown — destroying device anyway");
      idle_thr.detach();
    }
    if (command_pool_) {
      vkDestroyCommandPool(device_, command_pool_, nullptr);
      command_pool_ = VK_NULL_HANDLE;
    }
    if (color_pass_) {
      vkDestroyRenderPass(device_, color_pass_, nullptr);
      color_pass_ = VK_NULL_HANDLE;
    }
    vkDestroyDevice(device_, nullptr);
    device_ = VK_NULL_HANDLE;
  }
  queue_ = VK_NULL_HANDLE;
  physical_device_ = VK_NULL_HANDLE;
  if (vk_instance_) {
    vkDestroyInstance(vk_instance_, nullptr);
    vk_instance_ = VK_NULL_HANDLE;
  }

  if (instance_) {
    xrDestroyInstance(instance_);
    instance_ = XR_NULL_HANDLE;
  }
  session_focused_ = false;
  tracking_valid_ = false;
  last_display_time_ = 0;
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

  VkApplicationInfo app_info{};
  app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  app_info.pApplicationName = "VRP";
  app_info.applicationVersion = 1;
  app_info.pEngineName = "VRP";
  app_info.engineVersion = 1;
  app_info.apiVersion = VK_API_VERSION_1_1;

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

  note("Vulkan: creating instance…");
  check_cancel();
  VkResult vk_r = VK_SUCCESS;
  check_xr(xrCreateVulkanInstanceKHR(instance_, &xvici, &vk_instance_, &vk_r),
           "xrCreateVulkanInstanceKHR");
  VRP_CHECK(vk_r == VK_SUCCESS, "Vulkan instance create failed");

  note("Vulkan: picking GPU…");
  check_cancel();
  XrVulkanGraphicsDeviceGetInfoKHR gdgi{XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR};
  gdgi.systemId = system_;
  gdgi.vulkanInstance = vk_instance_;
  check_xr(xrGetVulkanGraphicsDevice2KHR(instance_, &gdgi, &physical_device_), "get graphics device");

  // Find a graphics queue.
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

  float priority = 1.f;
  VkDeviceQueueCreateInfo qci{};
  qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
  qci.queueFamilyIndex = queue_family_index_;
  qci.queueCount = 1;
  qci.pQueuePriorities = &priority;

  // Optional extensions for CUDA↔Vulkan zero-copy (opaque FD).
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
  std::vector<const char*> dev_exts;
  if (has_ext(VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME)) {
    dev_exts.push_back(VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME);
  }
  if (has_ext(VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME)) {
    dev_exts.push_back(VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME);
    external_memory_fd_ = true;
  }
  const bool dma_buf = has_ext(VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME);
  const bool drm_mod = has_ext(VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME);
  if (dma_buf && drm_mod && external_memory_fd_) {
    dev_exts.push_back(VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME);
    dev_exts.push_back(VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME);
    dma_buf_import_ = true;
  }

  VkDeviceCreateInfo dci{};
  dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
  dci.queueCreateInfoCount = 1;
  dci.pQueueCreateInfos = &qci;
  dci.enabledExtensionCount = static_cast<uint32_t>(dev_exts.size());
  dci.ppEnabledExtensionNames = dev_exts.empty() ? nullptr : dev_exts.data();

  XrVulkanDeviceCreateInfoKHR xvdci{XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR};
  xvdci.systemId = system_;
  xvdci.pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
  xvdci.vulkanPhysicalDevice = physical_device_;
  xvdci.vulkanCreateInfo = &dci;
  xvdci.vulkanAllocator = nullptr;

  note("Vulkan: creating device…");
  check_cancel();
  vk_r = VK_SUCCESS;
  check_xr(xrCreateVulkanDeviceKHR(instance_, &xvdci, &device_, &vk_r), "xrCreateVulkanDeviceKHR");
  VRP_CHECK(vk_r == VK_SUCCESS, "Vulkan device create failed");

  vkGetDeviceQueue(device_, queue_family_index_, 0, &queue_);
  if (external_memory_fd_) {
    VRP_LOG("Vulkan external memory FD: yes (NVIDIA CUDA zero-copy)");
  } else {
    VRP_LOG("Vulkan external memory FD: no");
  }
  if (dma_buf_import_) {
    VRP_LOG("Vulkan dma-buf import: enabled (VA-API NV12)");
  } else {
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
  for (auto& sc : swapchains_) {
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
  make_bool("confirm", "Confirm", &confirm_action_);
  make_bool("back", "Back", &back_action_);
  make_bool("menu_toggle", "Menu Toggle", &menu_toggle_action_);

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
  auto hand_path = [&](const char* hand, const char* suffix) {
    char buf[128];
    std::snprintf(buf, sizeof(buf), "/user/hand/%s/input/%s", hand, suffix);
    return path(buf);
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
  if (want(ControllerProfile::OculusTouch)) {
    suggest("/interaction_profiles/oculus/touch_controller",
            {{navigate_action_, hand_path(primary, "thumbstick")},
             {confirm_action_, hand_path(primary, left ? "x/click" : "a/click")},
             {back_action_, hand_path(primary, left ? "y/click" : "b/click")},
             {menu_toggle_action_, hand_path(secondary, left ? "b/click" : "y/click")},
             {play_action_, hand_path(primary, "squeeze/click")},
             {seek_back_action_, hand_path(secondary, left ? "a/click" : "x/click")},
             {seek_fwd_action_, hand_path(primary, "thumbstick/click")},
             {recenter_action_, hand_path(secondary, "menu/click")}});
  }

  // Microsoft motion
  if (want(ControllerProfile::MicrosoftMotion)) {
    suggest("/interaction_profiles/microsoft/motion_controller",
            {{navigate_action_, hand_path(primary, "thumbstick")},
             {confirm_action_, hand_path(primary, "squeeze/click")},
             {back_action_, hand_path(primary, "trackpad/click")},
             {menu_toggle_action_, hand_path(secondary, "menu/click")},
             {play_action_, hand_path(primary, "trackpad/click")},
             {recenter_action_, hand_path(secondary, "thumbstick/click")}});
  }

  // Vive controller
  if (want(ControllerProfile::ViveController)) {
    suggest("/interaction_profiles/htc/vive_controller",
            {{navigate_action_, hand_path(primary, "trackpad")},
             {confirm_action_, hand_path(primary, "trigger/click")},
             {back_action_, hand_path(primary, "menu/click")},
             {menu_toggle_action_, hand_path(secondary, "menu/click")},
             {play_action_, hand_path(primary, "squeeze/click")},
             {recenter_action_, hand_path(secondary, "trackpad/click")}});
  }

  // Valve Index
  if (want(ControllerProfile::ValveIndex)) {
    suggest("/interaction_profiles/valve/index_controller",
            {{navigate_action_, hand_path(primary, "thumbstick")},
             {confirm_action_, hand_path(primary, "a/click")},
             {back_action_, hand_path(primary, "b/click")},
             {menu_toggle_action_, hand_path(secondary, "b/click")},
             {play_action_, hand_path(primary, "trigger/click")},
             {seek_back_action_, hand_path(secondary, "a/click")},
             {seek_fwd_action_, hand_path(primary, "thumbstick/click")},
             {recenter_action_, hand_path(secondary, "thumbstick/click")}});
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
          case XR_SESSION_STATE_STOPPING:
            xrEndSession(session_);
            session_running_ = false;
            tracking_origin_inited_ = false;
            break;
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
        VRP_LOG("Display refresh rate changed: %.2f → %.2f Hz", e->fromDisplayRefreshRate,
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

void XrVulkanApp::recenter() {
  if (!session_ || !view_space_ || !local_space_) {
    VRP_ERR("recenter: session/spaces not ready");
    return;
  }
  if (last_display_time_ == 0) {
    VRP_ERR("recenter: no predicted display time yet（HMD 接続後に再度）");
    return;
  }

  // Always locate against the identity LOCAL space — locating in app_space_ after a prior
  // recenter compounds offsets and makes the HMD button feel broken.
  XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
  const XrResult lr = xrLocateSpace(view_space_, local_space_, last_display_time_, &loc);
  if (XR_FAILED(lr)) {
    VRP_ERR("recenter: xrLocateSpace failed (%d)", static_cast<int>(lr));
    return;
  }
  const auto flags = loc.locationFlags;
  if ((flags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) == 0 ||
      (flags & XR_SPACE_LOCATION_POSITION_VALID_BIT) == 0) {
    VRP_ERR("recenter: head pose invalid (flags=0x%llx)",
            static_cast<unsigned long long>(flags));
    return;
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
}

void XrVulkanApp::maybe_init_tracking_origin() {
  if (tracking_origin_inited_ || !session_running_ || !session_focused_) return;
  if (!tracking_valid_ || last_display_time_ == 0) return;
  recenter();
  if (tracking_origin_inited_) {
    VRP_LOG("Tracking origin initialized (first valid head pose)");
  }
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
  st.seek_back =
      key_repeat(action_down(seek_back_action_), seek_back_held_, seek_back_repeating_, seek_back_next_, now);
  st.seek_forward =
      key_repeat(action_down(seek_fwd_action_), seek_fwd_held_, seek_fwd_repeating_, seek_fwd_next_, now);
  st.seek_scrubbing = seek_back_repeating_ || seek_fwd_repeating_;
  st.recenter = edge(recenter_action_);
  st.confirm = edge(confirm_action_);
  st.confirm_held = action_down(confirm_action_);
  st.back = edge(back_action_);
  st.back_held = action_down(back_action_);
  st.menu_toggle = edge(menu_toggle_action_);

  if (navigate_action_) {
    XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
    gi.action = navigate_action_;
    XrActionStateVector2f v{XR_TYPE_ACTION_STATE_VECTOR2F};
    if (XR_SUCCEEDED(xrGetActionStateVector2f(session_, &gi, &v)) && v.isActive) {
      st.stick_x = v.currentState.x;
      st.stick_y = v.currentState.y;
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
  VRP_LOG("xrRequestDisplayRefreshRateFB → %.1f Hz (pref %.1f)", target, hz);
  if (pfn_get_refresh_rate_) {
    float now = 0.f;
    if (XR_SUCCEEDED(pfn_get_refresh_rate_(session_, &now)) && now > 0.f) ext_display_hz_ = now;
  }
  return true;
}

}  // namespace vrp
