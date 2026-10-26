#include "video/pipewire_dma_texture.hpp"

#include <drm/drm_fourcc.h>
#include <unistd.h>

namespace vrp {
namespace {
constexpr uint64_t kFenceTimeoutNs = 50'000'000ull;
constexpr uint64_t kDrmModInvalid = DRM_FORMAT_MOD_INVALID;
}  // namespace

PipewireDmaTexture::~PipewireDmaTexture() { shutdown(); }

void PipewireDmaTexture::set_error(const std::string& e) { last_error_ = e; }

bool PipewireDmaTexture::map_drm_to_vk(uint32_t drm_fourcc, VkFormat& out_format) {
  switch (drm_fourcc) {
    case DRM_FORMAT_ARGB8888:
    case DRM_FORMAT_XRGB8888:
      out_format = VK_FORMAT_B8G8R8A8_UNORM;
      return true;
    case DRM_FORMAT_ABGR8888:
    case DRM_FORMAT_XBGR8888:
      out_format = VK_FORMAT_R8G8B8A8_UNORM;
      return true;
    default:
      return false;
  }
}

bool PipewireDmaTexture::init(VkPhysicalDevice phys, VkDevice device, VkQueue queue,
                              uint32_t queue_family, std::mutex* queue_mu, bool dma_buf_import) {
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
    VRP_LOG("PipewireDmaTexture: %s", last_error_.c_str());
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
  VRP_LOG("PipewireDmaTexture: dma-buf BGRA import armed");
  return true;
}

void PipewireDmaTexture::destroy_slot(Slot& slot) {
  if (!device_) {
    slot = {};
    return;
  }
  if (slot.view) vkDestroyImageView(device_, slot.view, nullptr);
  if (slot.image) vkDestroyImage(device_, slot.image, nullptr);
  if (slot.memory) vkFreeMemory(device_, slot.memory, nullptr);
  slot = {};
}

void PipewireDmaTexture::reset() {
  if (!device_) return;
  (void)wait_fence();
  destroy_slot(slots_[0]);
  destroy_slot(slots_[1]);
  destroy_slot(slots_[2]);
  display_slot_.store(0);
  prev_slot_ = 0;
  valid_.store(false);
  width_ = 0;
  height_ = 0;
  present_gen_.store(0);
  bound_gen_ = 0;
  import_logged_ = false;
}

void PipewireDmaTexture::shutdown() {
  dying_ = true;
  if (device_) {
    (void)wait_fence();
    reset();
    if (sampler_) vkDestroySampler(device_, sampler_, nullptr);
    if (fence_) vkDestroyFence(device_, fence_, nullptr);
    if (pool_) vkDestroyCommandPool(device_, pool_, nullptr);
  }
  sampler_ = VK_NULL_HANDLE;
  fence_ = VK_NULL_HANDLE;
  cmd_ = VK_NULL_HANDLE;
  pool_ = VK_NULL_HANDLE;
  get_fd_props_ = nullptr;
  device_ = VK_NULL_HANDLE;
  phys_ = VK_NULL_HANDLE;
  queue_ = VK_NULL_HANDLE;
  queue_mu_ = nullptr;
  enabled_ = false;
}

bool PipewireDmaTexture::wait_fence() {
  if (!device_ || !fence_) return true;
  const VkResult r = vkWaitForFences(device_, 1, &fence_, VK_TRUE, kFenceTimeoutNs);
  if (r == VK_TIMEOUT) {
    set_error("fence timeout");
    return false;
  }
  return r == VK_SUCCESS;
}

VkImageView PipewireDmaTexture::view() const {
  if (!valid_.load(std::memory_order_acquire)) return VK_NULL_HANDLE;
  const int i = display_slot_.load(std::memory_order_acquire);
  return slots_[i].view;
}

bool PipewireDmaTexture::poll_present() {
  const uint64_t g = present_gen_.load(std::memory_order_acquire);
  if (g == bound_gen_) return false;
  bound_gen_ = g;
  return valid_.load(std::memory_order_acquire);
}

bool PipewireDmaTexture::modifier_ok(uint64_t modifier, VkFormat format, int width, int height) const {
  if (!phys_) return false;
  uint64_t mod = modifier;
  if (mod == kDrmModInvalid) mod = DRM_FORMAT_MOD_LINEAR;

  VkPhysicalDeviceImageDrmFormatModifierInfoEXT mod_info{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT};
  mod_info.drmFormatModifier = mod;
  mod_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

  VkPhysicalDeviceExternalImageFormatInfo ext{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO};
  ext.pNext = &mod_info;
  ext.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

  VkPhysicalDeviceImageFormatInfo2 info{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2};
  info.pNext = &ext;
  info.format = format;
  info.type = VK_IMAGE_TYPE_2D;
  info.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
  info.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
  VkImageFormatProperties2 props{VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};
  return vkGetPhysicalDeviceImageFormatProperties2(phys_, &info, &props) == VK_SUCCESS;
}

bool PipewireDmaTexture::import_image(Slot& slot, int fd, uint64_t modifier, int width, int height,
                                      VkFormat format, VkDeviceSize offset, VkDeviceSize row_pitch,
                                      VkDeviceSize size) {
  destroy_slot(slot);
  if (fd < 0 || row_pitch == 0 || size == 0) {
    set_error("bad plane");
    return false;
  }
  uint64_t mod = modifier;
  if (mod == kDrmModInvalid) mod = DRM_FORMAT_MOD_LINEAR;
  if (!modifier_ok(mod, format, width, height)) {
    set_error("modifier not sampleable");
    return false;
  }

  VkSubresourceLayout layout{};
  layout.offset = offset;
  layout.size = size;
  layout.rowPitch = row_pitch;

  VkImageDrmFormatModifierExplicitCreateInfoEXT expl{
      VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT};
  expl.drmFormatModifier = mod;
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
  if (vkCreateImage(device_, &ici, nullptr, &slot.image) != VK_SUCCESS) {
    set_error("vkCreateImage");
    return false;
  }

  VkMemoryFdPropertiesKHR fd_props{VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
  if (get_fd_props_(device_, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, fd, &fd_props) !=
      VK_SUCCESS) {
    set_error("vkGetMemoryFdPropertiesKHR");
    destroy_slot(slot);
    return false;
  }
  VkMemoryRequirements req{};
  vkGetImageMemoryRequirements(device_, slot.image, &req);
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
    destroy_slot(slot);
    return false;
  }

  const int dupfd = ::dup(fd);
  if (dupfd < 0) {
    set_error("dup");
    destroy_slot(slot);
    return false;
  }

  VkImportMemoryFdInfoKHR imp{VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR};
  imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
  imp.fd = dupfd;
  VkMemoryDedicatedAllocateInfo ded{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
  ded.pNext = &imp;
  ded.image = slot.image;
  VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  mai.pNext = &ded;
  mai.allocationSize = req.size;
  mai.memoryTypeIndex = mem_type;
  if (vkAllocateMemory(device_, &mai, nullptr, &slot.memory) != VK_SUCCESS) {
    set_error("vkAllocateMemory");
    destroy_slot(slot);
    return false;
  }
  if (vkBindImageMemory(device_, slot.image, slot.memory, 0) != VK_SUCCESS) {
    set_error("vkBindImageMemory");
    destroy_slot(slot);
    return false;
  }

  VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  vi.image = slot.image;
  vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
  vi.format = format;
  vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  vi.subresourceRange.levelCount = 1;
  vi.subresourceRange.layerCount = 1;
  if (vkCreateImageView(device_, &vi, nullptr, &slot.view) != VK_SUCCESS) {
    set_error("vkCreateImageView");
    destroy_slot(slot);
    return false;
  }
  return true;
}

bool PipewireDmaTexture::transition_slot(const Slot& slot) {
  std::unique_lock<std::mutex> qlock;
  if (queue_mu_) qlock = std::unique_lock<std::mutex>(*queue_mu_);
  vkResetCommandBuffer(cmd_, 0);
  VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (vkBeginCommandBuffer(cmd_, &bi) != VK_SUCCESS) return false;

  VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.image = slot.image;
  b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  b.subresourceRange.levelCount = 1;
  b.subresourceRange.layerCount = 1;
  b.srcAccessMask = 0;
  b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
                       0, nullptr, 0, nullptr, 1, &b);
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

bool PipewireDmaTexture::try_import(const PipewireDmaFrame& frame) {
  if (!enabled_ || dying_ || !frame) return false;
  VkFormat vkfmt = VK_FORMAT_UNDEFINED;
  if (!map_drm_to_vk(frame.drm_fourcc, vkfmt)) {
    if (!import_logged_) {
      import_logged_ = true;
      VRP_LOG("PipeWire dma-buf unsupported fourcc=0x%x — CPU RGBA", frame.drm_fourcc);
    }
    set_error("unsupported fourcc");
    return false;
  }
  if (!wait_fence()) return false;

  const int w = frame.width;
  const int h = frame.height;
  if (w <= 0 || h <= 0) return false;

  if (width_ != 0 && (w != width_ || h != height_)) {
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

  const VkDeviceSize pitch =
      frame.stride > 0 ? static_cast<VkDeviceSize>(frame.stride) : static_cast<VkDeviceSize>(w) * 4u;
  const VkDeviceSize off = static_cast<VkDeviceSize>(frame.offset);
  VkDeviceSize size = frame.size > 0 ? static_cast<VkDeviceSize>(frame.size) : pitch * static_cast<VkDeviceSize>(h);
  if (size < pitch * static_cast<VkDeviceSize>(h)) size = pitch * static_cast<VkDeviceSize>(h);

  if (!import_image(slot, frame.fd, frame.modifier, w, h, vkfmt, off, pitch, size)) {
    destroy_slot(slot);
    if (!import_logged_) {
      import_logged_ = true;
      VRP_LOG("PipeWire dma-buf import failed (%s, modifier=0x%llx) — CPU RGBA", last_error_.c_str(),
              static_cast<unsigned long long>(frame.modifier));
    }
    return false;
  }
  if (!transition_slot(slot)) {
    if (!import_logged_) {
      import_logged_ = true;
      VRP_LOG("PipeWire dma-buf layout transition failed — CPU RGBA");
    }
    return false;
  }

  width_ = w;
  height_ = h;
  prev_slot_ = display_slot_.load(std::memory_order_relaxed);
  display_slot_.store(write, std::memory_order_release);
  valid_.store(true, std::memory_order_release);
  present_gen_.fetch_add(1, std::memory_order_release);
  if (!import_logged_) {
    import_logged_ = true;
    VRP_LOG("display path: PipeWire DMA-BUF zero-copy %dx%d modifier=0x%llx", w, h,
            static_cast<unsigned long long>(frame.modifier));
  }
  return true;
}

}  // namespace vrp
