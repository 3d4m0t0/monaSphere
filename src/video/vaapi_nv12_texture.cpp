#include "video/vaapi_nv12_texture.hpp"

#include <unistd.h>

#if defined(VRP_HAS_FFMPEG)
extern "C" {
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_drm.h>
#include <libavutil/pixfmt.h>
}
#endif

namespace vrp {
namespace {
constexpr uint64_t kFenceTimeoutNs = 50'000'000ull;
constexpr uint64_t kDrmModInvalid = (1ull << 56) - 1ull;
constexpr uint32_t kDrmFormatNv12 = 0x3231564Eu;
}  // namespace

VaapiNv12Texture::~VaapiNv12Texture() { shutdown(); }

void VaapiNv12Texture::set_error(const std::string& e) { last_error_ = e; }

bool VaapiNv12Texture::init(VkPhysicalDevice phys, VkDevice device, VkQueue queue, uint32_t queue_family,
                            std::mutex* queue_mu, bool dma_buf_import) {
  shutdown();
  if (!dma_buf_import || !phys || !device || !queue) {
    set_error("dma-buf import not enabled");
    return false;
  }
  phys_ = phys;
  device_ = device;
  queue_ = queue;
  queue_family_ = queue_family;
  queue_mu_ = queue_mu;
  dying_ = false;

  get_fd_props_ = reinterpret_cast<PFN_vkGetMemoryFdPropertiesKHR>(
      vkGetDeviceProcAddr(device_, "vkGetMemoryFdPropertiesKHR"));
  if (!get_fd_props_) {
    set_error("vkGetMemoryFdPropertiesKHR missing");
    VRP_LOG("VaapiNv12Texture: %s", last_error_.c_str());
    device_ = VK_NULL_HANDLE;
    return false;
  }

  VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  pci.queueFamilyIndex = queue_family_;
  if (vkCreateCommandPool(device_, &pci, nullptr, &pool_) != VK_SUCCESS) {
    set_error("command pool");
    shutdown();
    return false;
  }
  VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  ai.commandPool = pool_;
  ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  ai.commandBufferCount = 1;
  if (vkAllocateCommandBuffers(device_, &ai, &cmd_) != VK_SUCCESS) {
    set_error("command buffer");
    shutdown();
    return false;
  }
  VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
  if (vkCreateFence(device_, &fi, nullptr, &fence_) != VK_SUCCESS) {
    set_error("fence");
    shutdown();
    return false;
  }

  VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  sci.magFilter = VK_FILTER_LINEAR;
  sci.minFilter = VK_FILTER_LINEAR;
  sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
  sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  if (vkCreateSampler(device_, &sci, nullptr, &sampler_) != VK_SUCCESS) {
    set_error("sampler");
    shutdown();
    return false;
  }

  enabled_ = true;
  set_error("ok");
  VRP_LOG("VaapiNv12Texture: dma-buf NV12 import armed");
  return true;
}

void VaapiNv12Texture::reset() {
  if (!device_) return;
  (void)wait_fence();
  destroy_slot(slots_[0]);
  destroy_slot(slots_[1]);
  destroy_slot(slots_[2]);
  display_slot_.store(0);
  prev_slot_ = 0;
  valid_.store(false);
  present_gen_.store(0);
  bound_gen_ = 0;
  width_ = 0;
  height_ = 0;
  import_logged_ = false;
}

void VaapiNv12Texture::shutdown() {
  dying_ = true;
  enabled_ = false;
  if (device_) {
    // The fence is created signaled. Wait only when a layout transition is still
    // in flight. Call this before XrVulkanApp::shutdown(); the loader aborts if
    // vkWaitForFences runs on a device that has already been destroyed.
    if (fence_ && vkGetFenceStatus(device_, fence_) == VK_NOT_READY) {
      (void)wait_fence();
    }
    destroy_slot(slots_[0]);
    destroy_slot(slots_[1]);
    destroy_slot(slots_[2]);
    if (sampler_) vkDestroySampler(device_, sampler_, nullptr);
    if (fence_) vkDestroyFence(device_, fence_, nullptr);
    if (pool_) vkDestroyCommandPool(device_, pool_, nullptr);
  }
  sampler_ = VK_NULL_HANDLE;
  fence_ = VK_NULL_HANDLE;
  cmd_ = VK_NULL_HANDLE;
  pool_ = VK_NULL_HANDLE;
  device_ = VK_NULL_HANDLE;
  phys_ = VK_NULL_HANDLE;
  queue_ = VK_NULL_HANDLE;
  get_fd_props_ = nullptr;
  display_slot_.store(0);
  prev_slot_ = 0;
  valid_.store(false);
  present_gen_.store(0);
  bound_gen_ = 0;
  width_ = 0;
  height_ = 0;
  full_range_ = false;
}

void VaapiNv12Texture::destroy_plane(Plane& p) {
  if (device_) {
    if (p.view) vkDestroyImageView(device_, p.view, nullptr);
    if (p.image) vkDestroyImage(device_, p.image, nullptr);
    if (p.memory) vkFreeMemory(device_, p.memory, nullptr);
  }
  p = {};
}

void VaapiNv12Texture::destroy_slot(Slot& slot) {
  destroy_plane(slot.y);
  destroy_plane(slot.uv);
#if defined(VRP_HAS_FFMPEG)
  if (slot.held) {
    auto* frame = static_cast<AVFrame*>(slot.held);
    av_frame_free(&frame);
  }
#endif
  slot.held = nullptr;
}

bool VaapiNv12Texture::wait_fence() {
  if (!fence_ || !device_) return true;
  return vkWaitForFences(device_, 1, &fence_, VK_TRUE, kFenceTimeoutNs) == VK_SUCCESS;
}

VkImageView VaapiNv12Texture::y_view() const {
  return slots_[display_slot_.load(std::memory_order_acquire)].y.view;
}

VkImageView VaapiNv12Texture::uv_view() const {
  return slots_[display_slot_.load(std::memory_order_acquire)].uv.view;
}

bool VaapiNv12Texture::poll_present() {
  const uint64_t g = present_gen_.load(std::memory_order_acquire);
  if (g == bound_gen_) return false;
  bound_gen_ = g;
  return true;
}

bool VaapiNv12Texture::modifier_ok(uint64_t modifier, VkFormat format, int width, int height) const {
  if (modifier == kDrmModInvalid || width <= 0 || height <= 0) return false;
  VkPhysicalDeviceImageDrmFormatModifierInfoEXT mod{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT};
  mod.drmFormatModifier = modifier;
  mod.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  VkPhysicalDeviceImageFormatInfo2 info{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2};
  info.pNext = &mod;
  info.format = format;
  info.type = VK_IMAGE_TYPE_2D;
  info.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
  info.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
  VkImageFormatProperties2 props{VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};
  return vkGetPhysicalDeviceImageFormatProperties2(phys_, &info, &props) == VK_SUCCESS;
}

bool VaapiNv12Texture::import_plane(Plane& plane, int fd, uint64_t modifier, int width, int height, VkFormat format,
                                    VkDeviceSize offset, VkDeviceSize row_pitch, VkDeviceSize size) {
  destroy_plane(plane);
  if (fd < 0 || row_pitch == 0 || size == 0) {
    set_error("bad plane");
    return false;
  }
  if (!modifier_ok(modifier, format, width, height)) {
    set_error("modifier not sampleable");
    return false;
  }

  VkSubresourceLayout layout{};
  layout.offset = offset;
  layout.size = size;
  layout.rowPitch = row_pitch;
  layout.arrayPitch = 0;
  layout.depthPitch = 0;

  VkImageDrmFormatModifierExplicitCreateInfoEXT expl{
      VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT};
  expl.drmFormatModifier = modifier;
  expl.drmFormatModifierPlaneCount = 1;
  expl.pPlaneLayouts = &layout;

  VkExternalMemoryImageCreateInfo ext{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
  ext.pNext = &expl;
  ext.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

  VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ici.pNext = &ext;
  ici.flags = VK_IMAGE_CREATE_ALIAS_BIT;
  ici.imageType = VK_IMAGE_TYPE_2D;
  ici.format = format;
  ici.extent = {static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1};
  ici.mipLevels = 1;
  ici.arrayLayers = 1;
  ici.samples = VK_SAMPLE_COUNT_1_BIT;
  ici.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
  ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
  ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (vkCreateImage(device_, &ici, nullptr, &plane.image) != VK_SUCCESS) {
    set_error("vkCreateImage");
    return false;
  }

  VkMemoryFdPropertiesKHR fd_props{VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
  if (get_fd_props_(device_, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, fd, &fd_props) != VK_SUCCESS) {
    set_error("vkGetMemoryFdPropertiesKHR");
    destroy_plane(plane);
    return false;
  }
  VkMemoryRequirements req{};
  vkGetImageMemoryRequirements(device_, plane.image, &req);
  const uint32_t bits = req.memoryTypeBits & fd_props.memoryTypeBits;
  VkPhysicalDeviceMemoryProperties mp{};
  vkGetPhysicalDeviceMemoryProperties(phys_, &mp);
  uint32_t mem_type = UINT32_MAX;
  for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
    if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
      mem_type = i;
      break;
    }
  }
  if (mem_type == UINT32_MAX) {
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
      if (bits & (1u << i)) {
        mem_type = i;
        break;
      }
    }
  }
  if (mem_type == UINT32_MAX) {
    set_error("no memory type for dma-buf");
    destroy_plane(plane);
    return false;
  }

  const int dupfd = ::dup(fd);
  if (dupfd < 0) {
    set_error("dup");
    destroy_plane(plane);
    return false;
  }

  VkImportMemoryFdInfoKHR imp{VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR};
  imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
  imp.fd = dupfd;
  VkMemoryDedicatedAllocateInfo ded{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
  ded.pNext = &imp;
  ded.image = plane.image;
  VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  mai.pNext = &ded;
  mai.allocationSize = req.size;
  mai.memoryTypeIndex = mem_type;
  // fd is consumed by vkAllocateMemory, including on failure.
  if (vkAllocateMemory(device_, &mai, nullptr, &plane.memory) != VK_SUCCESS) {
    set_error("vkAllocateMemory");
    destroy_plane(plane);
    return false;
  }
  if (vkBindImageMemory(device_, plane.image, plane.memory, 0) != VK_SUCCESS) {
    set_error("vkBindImageMemory");
    destroy_plane(plane);
    return false;
  }

  VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  vi.image = plane.image;
  vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
  vi.format = format;
  vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  vi.subresourceRange.levelCount = 1;
  vi.subresourceRange.layerCount = 1;
  if (vkCreateImageView(device_, &vi, nullptr, &plane.view) != VK_SUCCESS) {
    set_error("vkCreateImageView");
    destroy_plane(plane);
    return false;
  }
  return true;
}

bool VaapiNv12Texture::transition_slot(const Slot& slot) {
  std::unique_lock<std::mutex> qlock;
  if (queue_mu_) qlock = std::unique_lock<std::mutex>(*queue_mu_);
  vkResetCommandBuffer(cmd_, 0);
  VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (vkBeginCommandBuffer(cmd_, &bi) != VK_SUCCESS) return false;

  auto barrier = [&](VkImage image) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = 1;
    b.srcAccessMask = 0;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0,
                         nullptr, 0, nullptr, 1, &b);
  };
  barrier(slot.y.image);
  barrier(slot.uv.image);
  if (vkEndCommandBuffer(cmd_) != VK_SUCCESS) return false;
  if (vkResetFences(device_, 1, &fence_) != VK_SUCCESS) return false;
  VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  si.commandBufferCount = 1;
  si.pCommandBuffers = &cmd_;
  if (vkQueueSubmit(queue_, 1, &si, fence_) != VK_SUCCESS) {
    vkDestroyFence(device_, fence_, nullptr);
    fence_ = VK_NULL_HANDLE;
    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    (void)vkCreateFence(device_, &fi, nullptr, &fence_);
    return false;
  }
  if (qlock.owns_lock()) qlock.unlock();
  return wait_fence();
}

bool VaapiNv12Texture::try_import(const AVFrame* vaapi_frame, bool full_range_hint) {
#if !defined(VRP_HAS_FFMPEG)
  (void)vaapi_frame;
  (void)full_range_hint;
  return false;
#else
  if (!enabled_ || dying_ || !vaapi_frame) return false;
  if (vaapi_frame->format != AV_PIX_FMT_VAAPI) return false;
  const int w = vaapi_frame->width;
  const int h = vaapi_frame->height;
  if (w <= 0 || h <= 0 || (w & 1) || (h & 1)) return false;
  if (!wait_fence()) return false;

  AVFrame* mapped = av_frame_alloc();
  if (!mapped) return false;
  mapped->format = AV_PIX_FMT_DRM_PRIME;
  const int map_err = av_hwframe_map(mapped, vaapi_frame, AV_HWFRAME_MAP_READ);
  if (map_err < 0 || !mapped->data[0]) {
    av_frame_free(&mapped);
    if (!import_logged_) {
      import_logged_ = true;
      VRP_LOG("VA-API dma-buf map failed (%d) — CPU RGBA", map_err);
    }
    return false;
  }

  auto* desc = reinterpret_cast<const AVDRMFrameDescriptor*>(mapped->data[0]);
  if (!desc || desc->nb_layers < 1 || desc->nb_objects < 1) {
    av_frame_free(&mapped);
    return false;
  }
  const AVDRMLayerDescriptor& layer = desc->layers[0];
  if (layer.nb_planes < 2 || (layer.format != 0 && layer.format != kDrmFormatNv12)) {
    av_frame_free(&mapped);
    if (!import_logged_) {
      import_logged_ = true;
      VRP_LOG("VA-API dma-buf is not NV12 (format=0x%x planes=%d) — CPU RGBA", layer.format, layer.nb_planes);
    }
    return false;
  }

  const AVDRMPlaneDescriptor& ypl = layer.planes[0];
  const AVDRMPlaneDescriptor& uvpl = layer.planes[1];
  if (ypl.object_index < 0 || uvpl.object_index < 0 || ypl.object_index >= desc->nb_objects ||
      uvpl.object_index >= desc->nb_objects) {
    av_frame_free(&mapped);
    return false;
  }
  const AVDRMObjectDescriptor& yobj = desc->objects[ypl.object_index];
  const AVDRMObjectDescriptor& uvobj = desc->objects[uvpl.object_index];
  uint64_t ymod = yobj.format_modifier == kDrmModInvalid ? 0 : yobj.format_modifier;
  uint64_t uvmod = uvobj.format_modifier == kDrmModInvalid ? 0 : uvobj.format_modifier;

  const int uv_w = w / 2;
  const int uv_h = h / 2;
  const VkDeviceSize y_pitch = static_cast<VkDeviceSize>(ypl.pitch > 0 ? ypl.pitch : w);
  const VkDeviceSize uv_pitch = static_cast<VkDeviceSize>(uvpl.pitch > 0 ? uvpl.pitch : w);
  const VkDeviceSize y_off = static_cast<VkDeviceSize>(ypl.offset > 0 ? ypl.offset : 0);
  const VkDeviceSize uv_off = static_cast<VkDeviceSize>(uvpl.offset > 0 ? uvpl.offset : 0);
  VkDeviceSize y_size = y_pitch * static_cast<VkDeviceSize>(h);
  VkDeviceSize uv_size = uv_pitch * static_cast<VkDeviceSize>(uv_h);
  if (yobj.size > y_off && yobj.size - y_off > y_size) y_size = static_cast<VkDeviceSize>(yobj.size - y_off);
  if (uvobj.size > uv_off && uvobj.size - uv_off > uv_size) uv_size = static_cast<VkDeviceSize>(uvobj.size - uv_off);

  if ((width_ != 0 && (w != width_ || h != height_))) {
    destroy_slot(slots_[0]);
    destroy_slot(slots_[1]);
    destroy_slot(slots_[2]);
    valid_.store(false);
    width_ = 0;
    height_ = 0;
    prev_slot_ = 0;
    display_slot_.store(0);
  }

  int write = 0;
  if (valid_.load()) {
    const int cur = display_slot_.load(std::memory_order_relaxed);
    write = (cur + 1) % 3;
    if (write == prev_slot_) write = (write + 1) % 3;
  }
  Slot& slot = slots_[write];
  destroy_slot(slot);

  if (!import_plane(slot.y, yobj.fd, ymod, w, h, VK_FORMAT_R8_UNORM, y_off, y_pitch, y_size) ||
      !import_plane(slot.uv, uvobj.fd, uvmod, uv_w, uv_h, VK_FORMAT_R8G8_UNORM, uv_off, uv_pitch, uv_size)) {
    destroy_slot(slot);
    av_frame_free(&mapped);
    if (!import_logged_) {
      import_logged_ = true;
      VRP_LOG("VA-API dma-buf import failed (%s, modifier=0x%llx) — CPU RGBA", last_error_.c_str(),
              static_cast<unsigned long long>(ymod));
    }
    return false;
  }
  if (!transition_slot(slot)) {
    // Keep the images until the next call's fence wait. Destroying them while the
    // submit is in flight is invalid. The slot is not published.
    slot.held = mapped;
    if (!import_logged_) {
      import_logged_ = true;
      VRP_LOG("VA-API dma-buf layout transition failed — CPU RGBA");
    }
    return false;
  }

  slot.held = mapped;
  width_ = w;
  height_ = h;
  full_range_ = full_range_hint;
  prev_slot_ = display_slot_.load(std::memory_order_relaxed);
  display_slot_.store(write, std::memory_order_release);
  valid_.store(true, std::memory_order_release);
  present_gen_.fetch_add(1, std::memory_order_release);
  if (!import_logged_) {
    import_logged_ = true;
    VRP_LOG("display path: VA-API → Vulkan NV12 zero-copy %dx%d modifier=0x%llx", w, h,
            static_cast<unsigned long long>(ymod));
  }
  return true;
#endif
}
}  // namespace vrp
