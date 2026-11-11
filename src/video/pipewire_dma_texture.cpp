#include "video/pipewire_dma_texture.hpp"

#include <drm/drm_fourcc.h>
#include <linux/dma-buf.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>

namespace vrp {
namespace {
constexpr uint64_t kFenceTimeoutNs = 50'000'000ull;
constexpr uint64_t kDrmModInvalid = DRM_FORMAT_MOD_INVALID;
/** NVIDIA: wait this many identical frames before first vkAllocateMemory. */
constexpr int kNvidiaArmHits = 2;
constexpr uint32_t kVendorNvidia = 0x10DEu;

/**
 * Wait until outstanding GPU writers on this dma-buf have signaled.
 * DMA_BUF_IOCTL_SYNC alone does not wait on NVIDIA's implicit GPU timeline;
 * EXPORT_SYNC_FILE (READ) returns a sync_file that does.
 * @return true if waited or ioctl unsupported (caller still uses SYNC ioctl).
 */
enum class DmabufSyncResult { Ok, IoctlFail, Timeout };

DmabufSyncResult wait_dmabuf_writers(int dmabuf_fd, int timeout_ms = 150) {
  if (dmabuf_fd < 0) return DmabufSyncResult::IoctlFail;
  dma_buf_export_sync_file exp{};
  exp.flags = DMA_BUF_SYNC_READ;
  exp.fd = -1;
  if (::ioctl(dmabuf_fd, DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &exp) != 0 || exp.fd < 0) {
    return DmabufSyncResult::IoctlFail;
  }
  pollfd pfd{};
  pfd.fd = exp.fd;
  pfd.events = POLLIN;
  const int pr = ::poll(&pfd, 1, timeout_ms);
  ::close(exp.fd);
  if (pr > 0) return DmabufSyncResult::Ok;
  return DmabufSyncResult::Timeout;
}

void append_mods_for_format(VkPhysicalDevice phys, VkFormat format, std::vector<uint64_t>* out) {
  if (!phys || !out) return;
  VkDrmFormatModifierPropertiesListEXT list{
      VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT};
  VkFormatProperties2 fp{VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2};
  fp.pNext = &list;
  vkGetPhysicalDeviceFormatProperties2(phys, format, &fp);
  if (list.drmFormatModifierCount == 0) return;
  std::vector<VkDrmFormatModifierPropertiesEXT> props(list.drmFormatModifierCount);
  list.pDrmFormatModifierProperties = props.data();
  vkGetPhysicalDeviceFormatProperties2(phys, format, &fp);
  for (const auto& p : props) {
    if (p.drmFormatModifier == kDrmModInvalid) continue;
    if (p.drmFormatModifierPlaneCount != 1) continue;
    if ((p.drmFormatModifierTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) == 0) continue;
    if (std::find(out->begin(), out->end(), p.drmFormatModifier) == out->end()) {
      out->push_back(p.drmFormatModifier);
    }
  }
}
}  // namespace

PipewireDmaTexture::~PipewireDmaTexture() { shutdown(); }

std::vector<uint64_t> PipewireDmaTexture::query_sampleable_modifiers(VkPhysicalDevice phys) {
  std::vector<uint64_t> out;
  out.push_back(DRM_FORMAT_MOD_LINEAR);
  if (!phys) return out;
  // ScreenCast typically offers XRGB/ARGB (= B8G8R8A8) or XBGR/ABGR (= R8G8B8A8).
  append_mods_for_format(phys, VK_FORMAT_B8G8R8A8_UNORM, &out);
  append_mods_for_format(phys, VK_FORMAT_R8G8B8A8_UNORM, &out);
  return out;
}

void PipewireDmaTexture::set_error(const std::string& e) { last_error_ = e; }

void PipewireDmaTexture::abandon(const std::string& e) {
  set_error(e);
  abandoned_.store(true, std::memory_order_release);
  VRP_LOG("PipewireDmaTexture: abandon import (%s) — no further attempts (VkInstance safety)",
          e.c_str());
}

bool PipewireDmaTexture::map_drm_to_vk(uint32_t drm_fourcc, VkFormat& out_format,
                                       bool& force_opaque_alpha) {
  force_opaque_alpha = false;
  switch (drm_fourcc) {
    case DRM_FORMAT_ARGB8888:
      out_format = VK_FORMAT_B8G8R8A8_UNORM;
      return true;
    case DRM_FORMAT_XRGB8888:
      // X channel is not alpha; Flat blends with SRC_ALPHA — force A=1 or the panel vanishes.
      out_format = VK_FORMAT_B8G8R8A8_UNORM;
      force_opaque_alpha = true;
      return true;
    case DRM_FORMAT_ABGR8888:
      out_format = VK_FORMAT_R8G8B8A8_UNORM;
      return true;
    case DRM_FORMAT_XBGR8888:
      out_format = VK_FORMAT_R8G8B8A8_UNORM;
      force_opaque_alpha = true;
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
  abandoned_.store(false, std::memory_order_relaxed);

  VkPhysicalDeviceProperties props{};
  vkGetPhysicalDeviceProperties(phys_, &props);
  nvidia_ = (props.vendorID == kVendorNvidia);

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

  sampleable_mods_ = query_sampleable_modifiers(phys_);
  enabled_ = true;
  set_error("ok");
  if (nvidia_) {
    VRP_LOG("PipewireDmaTexture: dma-buf BGRA import armed (NVIDIA experimental: "
            "OPAQUE_FD↔DMA_BUF, explicit modifiers, arm %d frames, %zu sampleable mods)",
            kNvidiaArmHits, sampleable_mods_.size());
  } else {
    VRP_LOG("PipewireDmaTexture: dma-buf BGRA import armed (%zu sampleable mods)",
            sampleable_mods_.size());
  }
  if (!sampleable_mods_.empty()) {
    std::string line = "PipewireDmaTexture: sampleable DRM modifiers:";
    for (size_t i = 0; i < sampleable_mods_.size() && i < 12; ++i) {
      char buf[32];
      std::snprintf(buf, sizeof(buf), " 0x%llx",
                    static_cast<unsigned long long>(sampleable_mods_[i]));
      line += buf;
    }
    if (sampleable_mods_.size() > 12) line += " …";
    VRP_LOG("%s", line.c_str());
  }
  return true;
}

void PipewireDmaTexture::destroy_slot(Slot& slot) {
  const uint64_t seq = slot.pw_seq;
  if (device_) {
    if (slot.view) vkDestroyImageView(device_, slot.view, nullptr);
    if (slot.image) vkDestroyImage(device_, slot.image, nullptr);
    if (slot.memory) vkFreeMemory(device_, slot.memory, nullptr);
  }
  slot = {};
  // Return the PipeWire buffer only after Vulkan dropped the import — earlier
  // queue_buffer lets Mutter rewrite memory still sampled on the HMD.
  if (seq != 0 && release_seq_) release_seq_(seq);
}

bool PipewireDmaTexture::dma_buf_key(int fd, uint64_t* out_key) {
  if (!out_key || fd < 0) return false;
  struct stat st {};
  if (::fstat(fd, &st) != 0) return false;
  // Dups of the same PipeWire pool buffer share st_dev + st_ino.
  *out_key = (static_cast<uint64_t>(st.st_dev) * 0x9E3779B97F4A7C15ull) ^ static_cast<uint64_t>(st.st_ino);
  return *out_key != 0;
}

void PipewireDmaTexture::reset() {
  if (!device_) return;
  (void)wait_fence();
  destroy_slot(slots_[0]);
  destroy_slot(slots_[1]);
  display_slot_.store(0);
  valid_.store(false);
  width_ = 0;
  height_ = 0;
  present_gen_.store(0);
  bound_gen_ = 0;
  import_logged_ = false;
  last_write_slot_ = 0;
  eye_done_epoch_ = 0;
  present_hold_until_epoch_ = 0;
  slots_[0].busy_until_epoch = 0;
  slots_[1].busy_until_epoch = 0;
  arm_w_ = arm_h_ = 0;
  arm_mod_ = 0;
  arm_fourcc_ = 0;
  arm_hits_ = 0;
  // Keep abandoned_ / dmabuf_sync_ever_ok_ across reset within the same VkDevice.
}

void PipewireDmaTexture::mark_eye_done() {
  // SceneRenderer submit fence signaled — same role as per-slot VkFence for the
  // image that was sampled by that submit (and any slot waiting on this epoch).
  ++eye_done_epoch_;
}

bool PipewireDmaTexture::pending_eye_retire() const {
  if (!valid_.load(std::memory_order_acquire)) return false;
  return eye_done_epoch_ < slots_[0].busy_until_epoch;
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
  nvidia_ = false;
  sampleable_mods_.clear();
  abandoned_.store(false, std::memory_order_relaxed);
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

uint64_t PipewireDmaTexture::display_seq() const {
  if (!valid_.load(std::memory_order_acquire)) return 0;
  const int i = display_slot_.load(std::memory_order_acquire);
  if (i < 0 || i > 1) return 0;
  return slots_[i].content_seq;
}

uint64_t PipewireDmaTexture::slot_content_seq(int slot) const {
  if (slot < 0 || slot > 1) return 0;
  return slots_[slot].content_seq;
}

uint64_t PipewireDmaTexture::slot_content_key(int slot) const {
  if (slot < 0 || slot > 1) return 0;
  return slots_[slot].content_key;
}

uint64_t PipewireDmaTexture::slot_spa_seq(int slot) const {
  if (slot < 0 || slot > 1) return 0;
  return slots_[slot].spa_seq;
}

int64_t PipewireDmaTexture::slot_spa_pts(int slot) const {
  if (slot < 0 || slot > 1) return 0;
  return slots_[slot].spa_pts;
}

bool PipewireDmaTexture::slot_has_spa(int slot) const {
  if (slot < 0 || slot > 1) return false;
  return slots_[slot].has_spa_meta;
}

bool PipewireDmaTexture::frame_ready_to_import(const PipewireDmaFrame& frame,
                                              uint64_t* out_mod) const {
  if (!out_mod) return false;
  if (frame.width < 64 || frame.height < 64) {
    return false;
  }
  if (frame.stride == 0 || frame.size == 0) return false;
  if (frame.drm_fourcc == 0) return false;

  uint64_t mod = frame.modifier;
  if (nvidia_) {
    // Trap 2: NVIDIA rejects modifier-less imports. Never invent LINEAR.
    if (!frame.has_modifier || mod == kDrmModInvalid) return false;
  } else {
    if (!frame.has_modifier || mod == kDrmModInvalid) mod = DRM_FORMAT_MOD_LINEAR;
  }
  *out_mod = mod;
  return true;
}

bool PipewireDmaTexture::modifier_ok(uint64_t modifier, VkFormat format,
                                     VkExternalMemoryHandleTypeFlagBits handle_type) const {
  if (!phys_ || modifier == kDrmModInvalid) return false;

  VkPhysicalDeviceImageDrmFormatModifierInfoEXT mod_info{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT};
  mod_info.drmFormatModifier = modifier;
  mod_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

  VkPhysicalDeviceExternalImageFormatInfo ext{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO};
  ext.pNext = &mod_info;
  ext.handleType = handle_type;

  VkPhysicalDeviceImageFormatInfo2 info{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2};
  info.pNext = &ext;
  info.format = format;
  info.type = VK_IMAGE_TYPE_2D;
  info.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
  info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  VkImageFormatProperties2 props{VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};
  if (vkGetPhysicalDeviceImageFormatProperties2(phys_, &info, &props) == VK_SUCCESS) return true;
  // Some stacks reject TRANSFER_SRC on modifier images — sampled-only still ok for zero-copy.
  info.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
  return vkGetPhysicalDeviceImageFormatProperties2(phys_, &info, &props) == VK_SUCCESS;
}

bool PipewireDmaTexture::probe_handle_type(int fd, VkExternalMemoryHandleTypeFlagBits* out_type,
                                           VkMemoryFdPropertiesKHR* out_props) const {
  if (!get_fd_props_ || !out_type || !out_props || fd < 0) return false;

  // Trap 1: NVIDIA often prefers OPAQUE_FD for the same dma-buf FD.
  VkExternalMemoryHandleTypeFlagBits order[2];
  if (nvidia_) {
    order[0] = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    order[1] = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
  } else {
    order[0] = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    order[1] = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
  }
  for (auto ht : order) {
    VkMemoryFdPropertiesKHR props{VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
    if (get_fd_props_(device_, ht, fd, &props) == VK_SUCCESS) {
      *out_type = ht;
      *out_props = props;
      return true;
    }
  }
  return false;
}

bool PipewireDmaTexture::import_image(Slot& slot, int fd, uint64_t modifier, int width, int height,
                                      VkFormat format, bool force_opaque_alpha, VkDeviceSize offset,
                                      VkDeviceSize row_pitch, VkDeviceSize size,
                                      bool require_transfer_src) {
  destroy_slot(slot);
  if (fd < 0 || row_pitch == 0 || size == 0) {
    set_error("bad plane");
    return false;
  }
  if (modifier == kDrmModInvalid) {
    set_error("modifier missing");
    return false;
  }

  // Probe FD *before* CreateImage / AllocateMemory (Trap 3: avoid poison probes).
  VkExternalMemoryHandleTypeFlagBits handle_type{};
  VkMemoryFdPropertiesKHR fd_props{VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
  if (!probe_handle_type(fd, &handle_type, &fd_props)) {
    set_error("vkGetMemoryFdPropertiesKHR (dma-buf/opaque-fd)");
    return false;
  }
  if (!modifier_ok(modifier, format, handle_type)) {
    set_error("modifier not sampleable");
    return false;
  }

  VkSubresourceLayout layout{};
  layout.offset = offset;
  layout.size = size;
  layout.rowPitch = row_pitch;

  // Trap 2: always pass the producer DRM modifier explicitly.
  VkImageDrmFormatModifierExplicitCreateInfoEXT expl{
      VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT};
  expl.drmFormatModifier = modifier;
  expl.drmFormatModifierPlaneCount = 1;
  expl.pPlaneLayouts = &layout;

  VkExternalMemoryImageCreateInfo ext{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
  ext.pNext = &expl;
  ext.handleTypes = handle_type;

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
  // TRANSFER_SRC enables copy-out into a private OPTIMAL image (NVIDIA flicker fix).
  ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (vkCreateImage(device_, &ici, nullptr, &slot.image) != VK_SUCCESS) {
    if (require_transfer_src) {
      set_error("vkCreateImage (TRANSFER_SRC)");
      return false;
    }
    ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
    if (vkCreateImage(device_, &ici, nullptr, &slot.image) != VK_SUCCESS) {
      abandon("vkCreateImage");
      return false;
    }
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
    destroy_slot(slot);
    abandon("no memory type for dma-buf");
    return false;
  }

  const int dupfd = ::dup(fd);
  if (dupfd < 0) {
    destroy_slot(slot);
    set_error("dup");
    return false;
  }

  VkImportMemoryFdInfoKHR imp{VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR};
  imp.handleType = handle_type;
  imp.fd = dupfd;
  VkMemoryDedicatedAllocateInfo ded{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
  ded.pNext = &imp;
  ded.image = slot.image;
  VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  mai.pNext = &ded;
  mai.allocationSize = req.size;
  mai.memoryTypeIndex = mem_type;
  const VkResult ar = vkAllocateMemory(device_, &mai, nullptr, &slot.memory);
  if (ar != VK_SUCCESS) {
    // dupfd ownership transferred only on success — close on failure.
    ::close(dupfd);
    destroy_slot(slot);
    abandon(std::string("vkAllocateMemory r=") + std::to_string(static_cast<int>(ar)) +
            (handle_type == VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT ? " opaque-fd"
                                                                        : " dma-buf"));
    return false;
  }
  if (vkBindImageMemory(device_, slot.image, slot.memory, 0) != VK_SUCCESS) {
    destroy_slot(slot);
    abandon("vkBindImageMemory");
    return false;
  }

  VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  vi.image = slot.image;
  vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
  vi.format = format;
  vi.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
  vi.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
  vi.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
  vi.components.a = force_opaque_alpha ? VK_COMPONENT_SWIZZLE_ONE : VK_COMPONENT_SWIZZLE_IDENTITY;
  vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  vi.subresourceRange.levelCount = 1;
  vi.subresourceRange.layerCount = 1;
  if (vkCreateImageView(device_, &vi, nullptr, &slot.view) != VK_SUCCESS) {
    destroy_slot(slot);
    abandon("vkCreateImageView");
    return false;
  }

  if (!import_logged_) {
    VRP_LOG("PipewireDmaTexture: import via %s modifier=0x%llx",
            handle_type == VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT ? "OPAQUE_FD" : "DMA_BUF",
            static_cast<unsigned long long>(modifier));
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

  // Acquire from the producer (Mutter/PipeWire) before sampling. Without the
  // EXTERNAL ownership transfer, NVIDIA can keep sampling a buffer the
  // compositor already recycled → flicker / dialog-frame loop / hard hang.
  VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  b.srcQueueFamilyIndex = slot.is_private ? VK_QUEUE_FAMILY_IGNORED : VK_QUEUE_FAMILY_EXTERNAL;
  b.dstQueueFamilyIndex = slot.is_private ? VK_QUEUE_FAMILY_IGNORED : queue_family_;
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

bool PipewireDmaTexture::ensure_private_slot(Slot& slot, int w, int h, VkFormat format,
                                             bool force_opaque_alpha) {
  if (slot.is_private && slot.image && slot.priv_w == w && slot.priv_h == h &&
      slot.priv_fmt == format && slot.force_opaque == force_opaque_alpha && slot.view) {
    return true;
  }
  const uint64_t old_seq = slot.pw_seq;
  slot.pw_seq = 0;
  destroy_slot(slot);
  if (old_seq != 0 && release_seq_) release_seq_(old_seq);

  VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ci.imageType = VK_IMAGE_TYPE_2D;
  ci.format = format;
  ci.extent = {static_cast<uint32_t>(w), static_cast<uint32_t>(h), 1};
  ci.mipLevels = 1;
  ci.arrayLayers = 1;
  ci.samples = VK_SAMPLE_COUNT_1_BIT;
  ci.tiling = VK_IMAGE_TILING_OPTIMAL;
  // TRANSFER_SRC: staging→present promote copy.
  ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
             VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (vkCreateImage(device_, &ci, nullptr, &slot.image) != VK_SUCCESS) {
    set_error("private CreateImage");
    return false;
  }
  VkMemoryRequirements req{};
  vkGetImageMemoryRequirements(device_, slot.image, &req);
  VkPhysicalDeviceMemoryProperties mp{};
  vkGetPhysicalDeviceMemoryProperties(phys_, &mp);
  uint32_t mem_type = UINT32_MAX;
  for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
    if ((req.memoryTypeBits & (1u << i)) == 0) continue;
    if ((mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) == 0) continue;
    mem_type = i;
    break;
  }
  if (mem_type == UINT32_MAX) {
    destroy_slot(slot);
    set_error("private memory type");
    return false;
  }
  VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  mai.allocationSize = req.size;
  mai.memoryTypeIndex = mem_type;
  if (vkAllocateMemory(device_, &mai, nullptr, &slot.memory) != VK_SUCCESS ||
      vkBindImageMemory(device_, slot.image, slot.memory, 0) != VK_SUCCESS) {
    destroy_slot(slot);
    set_error("private Allocate/Bind");
    return false;
  }
  VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  vi.image = slot.image;
  vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
  vi.format = format;
  vi.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
  vi.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
  vi.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
  vi.components.a = force_opaque_alpha ? VK_COMPONENT_SWIZZLE_ONE : VK_COMPONENT_SWIZZLE_IDENTITY;
  vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  vi.subresourceRange.levelCount = 1;
  vi.subresourceRange.layerCount = 1;
  if (vkCreateImageView(device_, &vi, nullptr, &slot.view) != VK_SUCCESS) {
    destroy_slot(slot);
    set_error("private CreateImageView");
    return false;
  }
  slot.is_private = true;
  slot.priv_w = w;
  slot.priv_h = h;
  slot.priv_fmt = format;
  slot.force_opaque = force_opaque_alpha;
  slot.buf_key = 0;
  slot.pw_seq = 0;
  return true;
}

bool PipewireDmaTexture::import_copy_out(Slot& dst, const PipewireDmaFrame& frame, uint64_t mod,
                                         VkFormat format, bool force_opaque_alpha) {
  // Caller passes staging (slots_[1]). Path: dma-buf FD → temporary ext → dst.
  const int w = frame.width;
  const int h = frame.height;
  if (!ensure_private_slot(dst, w, h, format, force_opaque_alpha)) return false;

  Slot ext{};
  const VkDeviceSize pitch =
      frame.stride > 0 ? static_cast<VkDeviceSize>(frame.stride) : static_cast<VkDeviceSize>(w) * 4u;
  const VkDeviceSize off = static_cast<VkDeviceSize>(frame.offset);
  VkDeviceSize size = frame.size > 0 ? static_cast<VkDeviceSize>(frame.size) : pitch * static_cast<VkDeviceSize>(h);
  if (size < pitch * static_cast<VkDeviceSize>(h)) size = pitch * static_cast<VkDeviceSize>(h);
  // Temporary external import — usage must allow TRANSFER_SRC for the blit.
  // import_image currently creates SAMPLED-only; recreate with transfer via a
  // one-shot path: import then copy using general layout acquire.
  if (!import_image(ext, frame.fd, mod, w, h, format, force_opaque_alpha, off, pitch, size,
                    /*require_transfer_src=*/true)) {
    return false;
  }

  // Producer GPU timeline (fallback when PW-thread SyncTimeline wait did not run).
  // Prefer frame.producer_synced from capture; still refuse timeout / late ioctl-fail.
  if (!frame.producer_synced) {
    static int sync_log = 0;
    const DmabufSyncResult sr = wait_dmabuf_writers(frame.fd);
    if (sr == DmabufSyncResult::Ok) dmabuf_sync_ever_ok_ = true;
    if (sync_log < 8) {
      ++sync_log;
      VRP_LOG("dma-buf EXPORT_SYNC_FILE wait (blit): %s",
              sr == DmabufSyncResult::Ok       ? "ok"
              : sr == DmabufSyncResult::Timeout ? "timeout"
                                                : "ioctl-fail (driver/uapi)");
    }
    if (sr == DmabufSyncResult::Timeout ||
        (sr == DmabufSyncResult::IoctlFail && dmabuf_sync_ever_ok_)) {
      destroy_slot(ext);
      set_error(sr == DmabufSyncResult::Timeout ? "skip: dmabuf sync timeout"
                                                : "skip: dmabuf sync ioctl-fail");
      return false;
    }
  }
  // DMA_BUF_IOCTL_SYNC is for CPU mmap coherence — not required for GPU→GPU
  // vkCmdCopyImage. On NVIDIA it can mis-trigger CPU-access paths; keep only
  // EXPORT_SYNC_FILE / SyncTimeline waits (done above or on PW thread).

  std::unique_lock<std::mutex> qlock;
  if (queue_mu_) qlock = std::unique_lock<std::mutex>(*queue_mu_);
  vkResetCommandBuffer(cmd_, 0);
  VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (vkBeginCommandBuffer(cmd_, &bi) != VK_SUCCESS) {
    destroy_slot(ext);
    return false;
  }

  VkImageMemoryBarrier barriers[2]{};
  barriers[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  barriers[0].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  barriers[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  barriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
  barriers[0].dstQueueFamilyIndex = queue_family_;
  barriers[0].image = ext.image;
  barriers[0].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  barriers[0].subresourceRange.levelCount = 1;
  barriers[0].subresourceRange.layerCount = 1;
  barriers[0].srcAccessMask = 0;
  barriers[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;

  // Back slot may already hold the previous generation in SHADER_READ. Prefer
  // that oldLayout over UNDEFINED so a mistaken front write cannot black-flash.
  const bool dst_had_sample =
      dst.is_private && dst.view != VK_NULL_HANDLE && dst.content_seq != 0;
  barriers[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  barriers[1].oldLayout =
      dst_had_sample ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
  barriers[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  barriers[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barriers[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barriers[1].image = dst.image;
  barriers[1].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  barriers[1].subresourceRange.levelCount = 1;
  barriers[1].subresourceRange.layerCount = 1;
  barriers[1].srcAccessMask = dst_had_sample ? VK_ACCESS_SHADER_READ_BIT : 0;
  barriers[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

  const VkPipelineStageFlags src_stages =
      VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT |
      (dst_had_sample ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT : 0);
  vkCmdPipelineBarrier(cmd_, src_stages, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
                       2, barriers);

  VkImageCopy region{};
  region.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  region.srcSubresource.layerCount = 1;
  region.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  region.dstSubresource.layerCount = 1;
  region.extent = {static_cast<uint32_t>(w), static_cast<uint32_t>(h), 1};
  vkCmdCopyImage(cmd_, ext.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst.image,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

  VkImageMemoryBarrier to_sample{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  to_sample.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  to_sample.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  to_sample.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  to_sample.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  to_sample.image = dst.image;
  to_sample.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  to_sample.subresourceRange.levelCount = 1;
  to_sample.subresourceRange.layerCount = 1;
  to_sample.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  to_sample.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
                       0, nullptr, 0, nullptr, 1, &to_sample);

  if (vkEndCommandBuffer(cmd_) != VK_SUCCESS) {
    destroy_slot(ext);
    return false;
  }
  if (vkResetFences(device_, 1, &fence_) != VK_SUCCESS) {
    destroy_slot(ext);
    return false;
  }
  VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  si.commandBufferCount = 1;
  si.pCommandBuffers = &cmd_;
  if (vkQueueSubmit(queue_, 1, &si, fence_) != VK_SUCCESS) {
    destroy_slot(ext);
    set_error("copy-out submit");
    return false;
  }
  if (qlock.owns_lock()) qlock.unlock();
  if (!wait_fence()) {
    destroy_slot(ext);
    return false;
  }

  // Private image owns the pixels. Hold ends here (release_seq → queue_buffer).
  destroy_slot(ext);
  dst.pw_seq = 0;
  dst.is_private = true;
  if (frame.seq != 0 && release_seq_) release_seq_(frame.seq);
  return true;
}

bool PipewireDmaTexture::promote_staging_to_present() {
  // Path: staging slots_[1] → present slots_[0] (view() always samples present).
  Slot& src = slots_[1];
  Slot& dst = slots_[0];
  if (!src.image || !src.view || !src.is_private || src.priv_w <= 0 || src.priv_h <= 0) {
    set_error("promote: empty staging");
    return false;
  }
  // Recreate present if geometry/swizzle diverge from staging (XRGB A=ONE).
  if (dst.is_private && dst.image &&
      (dst.priv_w != src.priv_w || dst.priv_h != src.priv_h || dst.priv_fmt != src.priv_fmt ||
       dst.force_opaque != src.force_opaque)) {
    destroy_slot(dst);
  }
  if (!ensure_private_slot(dst, src.priv_w, src.priv_h, src.priv_fmt, src.force_opaque)) {
    return false;
  }

  const bool dst_had =
      dst.is_private && dst.view != VK_NULL_HANDLE && dst.content_seq != 0;

  std::unique_lock<std::mutex> qlock;
  if (queue_mu_) qlock = std::unique_lock<std::mutex>(*queue_mu_);
  vkResetCommandBuffer(cmd_, 0);
  VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (vkBeginCommandBuffer(cmd_, &bi) != VK_SUCCESS) return false;

  VkImageMemoryBarrier barriers[2]{};
  barriers[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  barriers[0].oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  barriers[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  barriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barriers[0].image = src.image;
  barriers[0].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  barriers[0].subresourceRange.levelCount = 1;
  barriers[0].subresourceRange.layerCount = 1;
  barriers[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
  barriers[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;

  barriers[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  barriers[1].oldLayout =
      dst_had ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
  barriers[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  barriers[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barriers[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barriers[1].image = dst.image;
  barriers[1].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  barriers[1].subresourceRange.levelCount = 1;
  barriers[1].subresourceRange.layerCount = 1;
  barriers[1].srcAccessMask = dst_had ? VK_ACCESS_SHADER_READ_BIT : 0;
  barriers[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

  const VkPipelineStageFlags src_stages =
      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
      (dst_had ? static_cast<VkPipelineStageFlags>(0) : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT);
  vkCmdPipelineBarrier(cmd_, src_stages, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
                       2, barriers);

  VkImageCopy region{};
  region.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  region.srcSubresource.layerCount = 1;
  region.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  region.dstSubresource.layerCount = 1;
  region.extent = {static_cast<uint32_t>(src.priv_w), static_cast<uint32_t>(src.priv_h), 1};
  vkCmdCopyImage(cmd_, src.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst.image,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

  VkImageMemoryBarrier after[2]{};
  after[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  after[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  after[0].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  after[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  after[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  after[0].image = src.image;
  after[0].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  after[0].subresourceRange.levelCount = 1;
  after[0].subresourceRange.layerCount = 1;
  after[0].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  after[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

  after[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  after[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  after[1].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  after[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  after[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  after[1].image = dst.image;
  after[1].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  after[1].subresourceRange.levelCount = 1;
  after[1].subresourceRange.layerCount = 1;
  after[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  after[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
                       0, nullptr, 0, nullptr, 2, after);

  if (vkEndCommandBuffer(cmd_) != VK_SUCCESS) return false;
  if (vkResetFences(device_, 1, &fence_) != VK_SUCCESS) return false;
  VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  si.commandBufferCount = 1;
  si.pCommandBuffers = &cmd_;
  if (vkQueueSubmit(queue_, 1, &si, fence_) != VK_SUCCESS) {
    set_error("promote submit");
    return false;
  }
  if (qlock.owns_lock()) qlock.unlock();
  if (!wait_fence()) return false;

  dst.content_seq = src.content_seq;
  dst.content_key = src.content_key;
  dst.buf_key = src.buf_key;
  dst.spa_seq = src.spa_seq;
  dst.spa_pts = src.spa_pts;
  dst.has_spa_meta = src.has_spa_meta;
  dst.is_private = true;
  return true;
}

bool PipewireDmaTexture::try_import(const PipewireDmaFrame& frame) {
  if (!enabled_ || dying_ || !frame) return false;
  if (abandoned_.load(std::memory_order_acquire)) return false;

  VkFormat vkfmt = VK_FORMAT_UNDEFINED;
  bool force_a = false;
  if (!map_drm_to_vk(frame.drm_fourcc, vkfmt, force_a)) {
    if (!import_logged_) {
      import_logged_ = true;
      VRP_LOG("PipeWire dma-buf unsupported fourcc=0x%x — CPU RGBA", frame.drm_fourcc);
    }
    set_error("unsupported fourcc");
    // Soft: keep waiting for a supported fourcc (do not poison).
    return false;
  }

  uint64_t mod = 0;
  if (!frame_ready_to_import(frame, &mod)) {
    set_error(nvidia_ ? "skip: waiting for explicit DRM modifier" : "skip: frame not ready");
    return false;
  }

  // Trap 3: arm until producer size/modifier is stable (skip dummy first frames).
  const int need_hits = nvidia_ ? kNvidiaArmHits : 1;
  if (arm_w_ != frame.width || arm_h_ != frame.height || arm_mod_ != mod ||
      arm_fourcc_ != frame.drm_fourcc) {
    arm_w_ = frame.width;
    arm_h_ = frame.height;
    arm_mod_ = mod;
    arm_fourcc_ = frame.drm_fourcc;
    arm_hits_ = 1;
    set_error("skip: arming dma-buf import");
    return false;
  }
  if (arm_hits_ < need_hits) {
    ++arm_hits_;
    set_error("skip: arming dma-buf import");
    return false;
  }

  const int w = frame.width;
  const int h = frame.height;

  if (width_ != 0 && (w != width_ || h != height_)) {
    if (!wait_fence()) return false;
    destroy_slot(slots_[0]);
    destroy_slot(slots_[1]);
    valid_.store(false);
    width_ = 0;
    height_ = 0;
    display_slot_.store(0);
    eye_done_epoch_ = 0;
    present_hold_until_epoch_ = 0;
  }

  uint64_t key = 0;
  const bool have_key = dma_buf_key(frame.fd, &key);
  if (!wait_fence()) return false;

  const bool had = valid_.load(std::memory_order_acquire);
  Slot& staging = slots_[1];
  last_write_slot_ = 1;  // dma copy-out destination (present remains slots_[0])

  // Stage any seq newer than staging (present may still show an older held frame).
  const uint64_t stage_seq = staging.content_seq;
  if (frame.seq != 0 && stage_seq != 0 && frame.seq <= stage_seq) {
    set_error("skip: stale seq");
    return false;
  }

  // Do not burn a full 1080p GPU copy-out when promote cannot land yet.
  // Capture keep-latest will hand a fresher BO after the hold/eye gate opens.
  if (had) {
    if (eye_done_epoch_ < slots_[0].busy_until_epoch) {
      set_error("skip: waiting eye fence");
      return false;
    }
    if (eye_done_epoch_ < present_hold_until_epoch_) {
      set_error("skip: present hold");
      return false;
    }
  }

  if (!import_copy_out(staging, frame, mod, vkfmt, force_a)) {
    if (!import_logged_) {
      import_logged_ = true;
      VRP_LOG("PipeWire dma-buf copy-out failed (%s, modifier=0x%llx) — CPU RGBA",
              last_error_.c_str(), static_cast<unsigned long long>(mod));
    }
    return false;
  }
  staging.content_seq = frame.seq;
  staging.content_key = have_key ? key : 0;
  staging.buf_key = staging.content_key;
  staging.has_spa_meta = frame.has_spa_meta;
  staging.spa_seq = frame.spa_seq;
  staging.spa_pts = frame.spa_pts;
  width_ = w;
  height_ = h;

  // Staging updated; present promote is gated so pool BO rotation cannot thrash
  // the sampled image every XR commit.
  (void)had;
  (void)promote_staging_if_allowed();
  if (!valid_.load(std::memory_order_acquire) && !import_logged_) {
    // First frame must promote; surface hard failure once.
    if (last_error_.find("skip:") == std::string::npos) {
      import_logged_ = true;
      VRP_LOG("PipeWire dma-buf promote failed (%s) — CPU RGBA", last_error_.c_str());
    }
  }
  if (!import_logged_ && valid_.load(std::memory_order_acquire)) {
    import_logged_ = true;
    VRP_LOG("display path: PipeWire DMA-BUF staging→present (hold=%llu epochs) %dx%d "
            "modifier=0x%llx%s",
            static_cast<unsigned long long>(kPresentHoldEpochs), w, h,
            static_cast<unsigned long long>(mod), nvidia_ ? " (NVIDIA)" : "");
  }
  // Staging always accepted (dma released in import_copy_out).
  return true;
}

bool PipewireDmaTexture::promote_staging_if_allowed() {
  Slot& staging = slots_[1];
  if (!staging.image || !staging.view || staging.content_seq == 0) {
    set_error("skip: no staging");
    return false;
  }
  const bool had = valid_.load(std::memory_order_acquire);
  if (had && staging.content_seq != 0 && slots_[0].content_seq != 0 &&
      staging.content_seq <= slots_[0].content_seq) {
    set_error("skip: staging not ahead");
    return false;
  }
  if (had && eye_done_epoch_ < slots_[0].busy_until_epoch) {
    set_error("skip: waiting eye fence");
    return false;
  }
  if (had && eye_done_epoch_ < present_hold_until_epoch_) {
    set_error("skip: present hold");
    return false;
  }
  if (!wait_fence()) return false;
  if (!promote_staging_to_present()) return false;

  slots_[0].busy_until_epoch = eye_done_epoch_ + 1;
  present_hold_until_epoch_ = eye_done_epoch_ + kPresentHoldEpochs;
  display_slot_.store(0, std::memory_order_release);
  present_gen_.fetch_add(1, std::memory_order_release);
  valid_.store(true, std::memory_order_release);
  return true;
}

bool PipewireDmaTexture::try_promote_pending() {
  if (!enabled_ || dying_) return false;
  if (abandoned_.load(std::memory_order_acquire)) return false;
  return promote_staging_if_allowed();
}

}  // namespace vrp
