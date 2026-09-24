#include "video/cuda_nv12_texture.hpp"

#include "video/cuda_minimal.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <unistd.h>

#if defined(VRP_HAS_FFMPEG)
extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}
#endif

namespace vrp {
namespace {

constexpr uint64_t kFenceTimeoutNs = 50'000'000ull;

#if defined(VRP_HAS_FFMPEG)
bool frame_is_full_range(const AVFrame* f, bool hint) {
  if (!f) return hint;
  if (f->color_range == AVCOL_RANGE_JPEG) return true;
  if (f->color_range == AVCOL_RANGE_MPEG) return false;
  return hint;
}
#endif

}  // namespace

void CudaNv12Texture::set_error(const std::string& e) {
  std::lock_guard<std::mutex> lock(err_mu_);
  last_error_ = e;
}

std::string CudaNv12Texture::last_error() const {
  std::lock_guard<std::mutex> lock(err_mu_);
  return last_error_;
}

bool CudaNv12Texture::init(VkPhysicalDevice phys, VkDevice device, VkQueue queue, uint32_t queue_family,
                           std::mutex* queue_mu, bool external_memory_fd) {
  phys_ = phys;
  device_ = device;
  queue_ = queue;
  queue_family_ = queue_family;
  queue_mu_ = queue_mu;
  dying_.store(false);
  enabled_ = false;
  create_failed_ = false;

  if (!external_memory_fd || !device_) {
    set_error("external memory FD unavailable");
    VRP_LOG("CudaNv12Texture: %s — CPU fallback", last_error_.c_str());
    return false;
  }
  if (!cuda::available()) {
    set_error("CUDA driver API unavailable");
    VRP_LOG("CudaNv12Texture: %s — CPU fallback", last_error_.c_str());
    return false;
  }

  VkPhysicalDeviceIDProperties id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
  VkPhysicalDeviceProperties2 props2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
  props2.pNext = &id;
  vkGetPhysicalDeviceProperties2(phys_, &props2);
  std::memcpy(vk_uuid_, id.deviceUUID, 16);

  if (!match_cuda_uuid()) {
    set_error("Vulkan/CUDA UUID mismatch");
    VRP_LOG("CudaNv12Texture: %s — CPU fallback", last_error_.c_str());
    return false;
  }

  get_memory_fd_ = reinterpret_cast<PFN_vkGetMemoryFdKHR>(vkGetDeviceProcAddr(device_, "vkGetMemoryFdKHR"));
  if (!get_memory_fd_) {
    set_error("vkGetMemoryFdKHR missing");
    return false;
  }

  VkCommandPoolCreateInfo cpci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  cpci.queueFamilyIndex = queue_family_;
  if (vkCreateCommandPool(device_, &cpci, nullptr, &pool_) != VK_SUCCESS) return false;

  VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  ai.commandPool = pool_;
  ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  ai.commandBufferCount = 1;
  if (vkAllocateCommandBuffers(device_, &ai, &cmd_) != VK_SUCCESS) return false;

  VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
  if (vkCreateFence(device_, &fci, nullptr, &fence_) != VK_SUCCESS) return false;

  VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  sci.magFilter = VK_FILTER_LINEAR;
  sci.minFilter = VK_FILTER_LINEAR;
  sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
  sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  if (vkCreateSampler(device_, &sci, nullptr, &sampler_) != VK_SUCCESS) return false;

  enabled_ = true;
  set_error("ok");
  VRP_LOG("CudaNv12Texture: LINEAR NV12 CUDA path enabled (no per-frame Vk copy), device %d", cuda_dev_);
  return true;
}

void CudaNv12Texture::drain_copies() {
  for (int i = 0; i < 200 && copy_active_.load(); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  (void)wait_fence_timeout(kFenceTimeoutNs);
}

bool CudaNv12Texture::push_cuda_ctx(CtxScope& scope, void* preferred) {
  scope = {};
  auto& cu = cuda::api();
  if (!cu.ok || !cu.cuCtxPushCurrent) return false;
  if (preferred) {
    if (cu.cuCtxPushCurrent(reinterpret_cast<cuda::CUcontext>(preferred)) == cuda::CUDA_SUCCESS) {
      scope.pushed = true;
      return true;
    }
  }
  if (cu.cuDevicePrimaryCtxRetain && cu.cuDeviceGet && cuda_dev_ >= 0) {
    cuda::CUdevice dev = 0;
    if (cu.cuDeviceGet(&dev, cuda_dev_) == cuda::CUDA_SUCCESS) {
      cuda::CUcontext primary = nullptr;
      if (cu.cuDevicePrimaryCtxRetain(&primary, dev) == cuda::CUDA_SUCCESS && primary) {
        scope.primary_retained = true;
        scope.cuda_dev = cuda_dev_;
        if (cu.cuCtxPushCurrent(primary) == cuda::CUDA_SUCCESS) {
          scope.pushed = true;
          return true;
        }
        if (cu.cuDevicePrimaryCtxRelease) {
          (void)cu.cuDevicePrimaryCtxRelease(dev);
        }
        scope.primary_retained = false;
      }
    }
  }
  return false;
}

void CudaNv12Texture::pop_cuda_ctx(CtxScope& scope) {
  auto& cu = cuda::api();
  if (scope.pushed && cu.ok && cu.cuCtxPopCurrent) {
    cuda::CUcontext popped = nullptr;
    (void)cu.cuCtxPopCurrent(&popped);
  }
  scope.pushed = false;
  if (scope.primary_retained && cu.ok && cu.cuDevicePrimaryCtxRelease && cu.cuDeviceGet &&
      scope.cuda_dev >= 0) {
    cuda::CUdevice dev = 0;
    if (cu.cuDeviceGet(&dev, scope.cuda_dev) == cuda::CUDA_SUCCESS) {
      (void)cu.cuDevicePrimaryCtxRelease(dev);
    }
  }
  scope.primary_retained = false;
}

void CudaNv12Texture::reset_slots(void* cu_context) {
  drain_copies();
  std::lock_guard<std::mutex> lock(slots_mu_);
  CtxScope scope;
  const bool ctx = push_cuda_ctx(scope, cu_context);
  if (ctx && cuda::api().cuCtxSynchronize) {
    (void)cuda::api().cuCtxSynchronize();
  }
  destroy_slots();
  if (ctx) pop_cuda_ctx(scope);
  create_failed_ = false;
  layouts_ready_ = false;
  present_gen_.store(0);
  bound_gen_ = 0;
  display_slot_.store(0);
  valid_.store(false);
  width_ = 0;
  height_ = 0;
}

void CudaNv12Texture::prepare_shutdown() {
  dying_.store(true);
  drain_copies();
}

void CudaNv12Texture::shutdown() {
  prepare_shutdown();
  {
    std::lock_guard<std::mutex> lock(slots_mu_);
    const bool has_import =
        (slots_[0].y.cuda_ptr || slots_[0].y.cuda_ext_mem || slots_[1].y.cuda_ptr ||
         slots_[1].y.cuda_ext_mem || slots_[0].uv.cuda_ptr || slots_[0].uv.cuda_ext_mem ||
         slots_[1].uv.cuda_ptr || slots_[1].uv.cuda_ext_mem);
    if (has_import) {
      VRP_ERR("CudaNv12Texture::shutdown: CUDA imports still present — call reset_slots(ctx) "
              "before decoder.close()");
      CtxScope scope;
      if (push_cuda_ctx(scope)) {
        if (cuda::api().cuCtxSynchronize) (void)cuda::api().cuCtxSynchronize();
        destroy_slots();
        pop_cuda_ctx(scope);
      } else {
        // Last resort: clear Vk handles without CUDA free (may leak driver objects).
        for (Slot& s : slots_) {
          auto scrub = [&](Plane& p) {
            p.cuda_ptr = 0;
            p.cuda_ext_mem = nullptr;
            if (device_) {
              if (p.view) vkDestroyImageView(device_, p.view, nullptr);
              if (p.image) vkDestroyImage(device_, p.image, nullptr);
              if (p.memory) vkFreeMemory(device_, p.memory, nullptr);
            }
            p = {};
          };
          scrub(s.y);
          scrub(s.uv);
        }
      }
    } else {
      destroy_slots();
    }
  }
  if (sampler_) vkDestroySampler(device_, sampler_, nullptr);
  sampler_ = VK_NULL_HANDLE;
  if (fence_) vkDestroyFence(device_, fence_, nullptr);
  fence_ = VK_NULL_HANDLE;
  if (cmd_ && pool_) vkFreeCommandBuffers(device_, pool_, 1, &cmd_);
  cmd_ = VK_NULL_HANDLE;
  if (pool_) vkDestroyCommandPool(device_, pool_, nullptr);
  pool_ = VK_NULL_HANDLE;
  enabled_ = false;
  valid_.store(false);
  device_ = VK_NULL_HANDLE;
}

bool CudaNv12Texture::match_cuda_uuid() {
  auto& cu = cuda::api();
  if (!cu.ok || !cu.cuDeviceGetUuid || !cu.cuDeviceGetCount || !cu.cuDeviceGet) return false;
  int count = 0;
  if (cu.cuDeviceGetCount(&count) != cuda::CUDA_SUCCESS || count <= 0) return false;
  for (int i = 0; i < count; ++i) {
    cuda::CUdevice dev = 0;
    if (cu.cuDeviceGet(&dev, i) != cuda::CUDA_SUCCESS) continue;
    uint8_t uuid[16]{};
    if (cu.cuDeviceGetUuid(uuid, dev) != cuda::CUDA_SUCCESS) continue;
    if (std::memcmp(uuid, vk_uuid_, 16) == 0) {
      cuda_dev_ = i;
      return true;
    }
  }
  return false;
}

uint32_t CudaNv12Texture::find_memory_type(uint32_t type_bits, VkMemoryPropertyFlags props) const {
  VkPhysicalDeviceMemoryProperties mp{};
  vkGetPhysicalDeviceMemoryProperties(phys_, &mp);
  for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
    if ((type_bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & props) == props) return i;
  }
  for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
    if ((type_bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
      return i;
    }
  }
  vrp_fatal("CudaNv12Texture: no suitable memory type");
}

void CudaNv12Texture::destroy_plane(Plane& p) {
  auto& cu = cuda::api();
  // Mapped buffers must be freed before destroying the external memory object.
  // Doing this after the owning CUcontext is gone (decoder.close) can hang in the driver.
  if (p.cuda_ptr && cu.ok && cu.cuMemFree) {
    (void)cu.cuMemFree(p.cuda_ptr);
  }
  p.cuda_ptr = 0;
  if (p.cuda_ext_mem && cu.ok && cu.cuDestroyExternalMemory) {
    (void)cu.cuDestroyExternalMemory(reinterpret_cast<cuda::CUexternalMemory>(p.cuda_ext_mem));
  }
  p.cuda_ext_mem = nullptr;
  if (device_) {
    if (p.view) vkDestroyImageView(device_, p.view, nullptr);
    if (p.image) vkDestroyImage(device_, p.image, nullptr);
    if (p.memory) vkFreeMemory(device_, p.memory, nullptr);
  }
  p = {};
}

void CudaNv12Texture::destroy_slots() {
  for (Slot& s : slots_) {
    destroy_plane(s.y);
    destroy_plane(s.uv);
  }
  display_slot_.store(0);
  write_ = 1;
  width_ = 0;
  height_ = 0;
  valid_.store(false);
  layouts_ready_ = false;
}

bool CudaNv12Texture::create_plane(Plane& p, int width, int height, VkFormat format) {
  destroy_plane(p);

  VkExternalMemoryImageCreateInfo ext_img{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
  ext_img.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;

  VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ici.pNext = &ext_img;
  ici.imageType = VK_IMAGE_TYPE_2D;
  ici.format = format;
  ici.extent = {static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1};
  ici.mipLevels = 1;
  ici.arrayLayers = 1;
  ici.samples = VK_SAMPLE_COUNT_1_BIT;
  ici.tiling = VK_IMAGE_TILING_LINEAR;
  ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  VkResult vr = vkCreateImage(device_, &ici, nullptr, &p.image);
  if (vr != VK_SUCCESS) {
    set_error("vkCreateImage LINEAR failed " + std::to_string(vr));
    return false;
  }

  VkMemoryDedicatedRequirements ded_req{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
  VkMemoryRequirements2 req2{VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
  req2.pNext = &ded_req;
  VkImageMemoryRequirementsInfo2 iri{VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2};
  iri.image = p.image;
  vkGetImageMemoryRequirements2(device_, &iri, &req2);
  const VkMemoryRequirements& req = req2.memoryRequirements;

  VkExportMemoryAllocateInfo export_ai{VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO};
  export_ai.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;

  VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
  dedicated.pNext = &export_ai;
  dedicated.image = p.image;

  VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  mai.pNext = &dedicated;
  mai.allocationSize = req.size;
  mai.memoryTypeIndex = find_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  vr = vkAllocateMemory(device_, &mai, nullptr, &p.memory);
  if (vr != VK_SUCCESS) {
    set_error("vkAllocateMemory image failed " + std::to_string(vr));
    destroy_plane(p);
    return false;
  }
  if (vkBindImageMemory(device_, p.image, p.memory, 0) != VK_SUCCESS) {
    set_error("vkBindImageMemory failed");
    destroy_plane(p);
    return false;
  }

  VkImageSubresource sub{};
  sub.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  VkSubresourceLayout layout{};
  vkGetImageSubresourceLayout(device_, p.image, &sub, &layout);
  p.row_pitch = layout.rowPitch;
  p.offset = layout.offset;
  p.alloc_size = req.size;
  p.width = width;
  p.height = height;

  VkImageViewCreateInfo ivci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  ivci.image = p.image;
  ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
  ivci.format = format;
  ivci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  ivci.subresourceRange.levelCount = 1;
  ivci.subresourceRange.layerCount = 1;
  if (vkCreateImageView(device_, &ivci, nullptr, &p.view) != VK_SUCCESS) {
    set_error("vkCreateImageView failed");
    destroy_plane(p);
    return false;
  }

  VRP_LOG("CudaNv12Texture: LINEAR plane fmt=0x%x %dx%d pitch=%llu offset=%llu alloc=%llu",
          static_cast<unsigned>(format), width, height, static_cast<unsigned long long>(p.row_pitch),
          static_cast<unsigned long long>(p.offset), static_cast<unsigned long long>(p.alloc_size));
  return true;
}

bool CudaNv12Texture::import_cuda_plane(Plane& p) {
  if (p.cuda_ptr) return true;
  if (!p.memory || !get_memory_fd_) {
    set_error("import: no image memory");
    return false;
  }

  VkMemoryGetFdInfoKHR fd_info{VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR};
  fd_info.memory = p.memory;
  fd_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
  int fd = -1;
  VkResult vr = get_memory_fd_(device_, &fd_info, &fd);
  if (vr != VK_SUCCESS || fd < 0) {
    set_error("vkGetMemoryFdKHR failed " + std::to_string(vr));
    return false;
  }

  auto& cu = cuda::api();
  cuda::CUDA_EXTERNAL_MEMORY_HANDLE_DESC hdesc{};
  hdesc.type = cuda::CU_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD;
  hdesc.handle.fd = fd;
  hdesc.size = p.alloc_size;
  hdesc.flags = cuda::CUDA_EXTERNAL_MEMORY_DEDICATED;
  cuda::CUexternalMemory ext = nullptr;
  cuda::CUresult cr = cu.cuImportExternalMemory(&ext, &hdesc);
  if (cr != cuda::CUDA_SUCCESS) {
    set_error(std::string("cuImportExternalMemory ") + cuda::err_name(cr));
    ::close(fd);
    return false;
  }
  p.cuda_ext_mem = ext;

  cuda::CUDA_EXTERNAL_MEMORY_BUFFER_DESC bdesc{};
  bdesc.offset = p.offset;
  bdesc.size = p.alloc_size - p.offset;
  cuda::CUdeviceptr ptr = 0;
  cr = cu.cuExternalMemoryGetMappedBuffer(&ptr, ext, &bdesc);
  if (cr != cuda::CUDA_SUCCESS) {
    set_error(std::string("cuExternalMemoryGetMappedBuffer ") + cuda::err_name(cr));
    return false;
  }
  p.cuda_ptr = ptr;
  return true;
}

bool CudaNv12Texture::ensure_cuda_imports() {
  for (Slot& s : slots_) {
    if (s.y.image && !import_cuda_plane(s.y)) return false;
    if (s.uv.image && !import_cuda_plane(s.uv)) return false;
  }
  return true;
}

bool CudaNv12Texture::create_slot(Slot& slot, int width, int height) {
  const int uv_w = std::max(1, width / 2);
  const int uv_h = std::max(1, height / 2);
  if (!create_plane(slot.y, width, height, VK_FORMAT_R8_UNORM)) return false;
  if (!create_plane(slot.uv, uv_w, uv_h, VK_FORMAT_R8G8_UNORM)) {
    destroy_plane(slot.y);
    return false;
  }
  return true;
}

bool CudaNv12Texture::transition_all_to_shader_read() {
  std::unique_lock<std::mutex> qlock;
  if (queue_mu_) qlock = std::unique_lock<std::mutex>(*queue_mu_);

  vkResetCommandBuffer(cmd_, 0);
  VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (vkBeginCommandBuffer(cmd_, &bi) != VK_SUCCESS) return false;

  // GENERAL: CUDA can write while Vulkan samples after cuStreamSynchronize (same device).
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
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &b);
  };
  for (Slot& s : slots_) {
    barrier(s.y.image);
    barrier(s.uv.image);
  }
  if (vkEndCommandBuffer(cmd_) != VK_SUCCESS) return false;
  if (vkResetFences(device_, 1, &fence_) != VK_SUCCESS) return false;
  VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  si.commandBufferCount = 1;
  si.pCommandBuffers = &cmd_;
  if (vkQueueSubmit(queue_, 1, &si, fence_) != VK_SUCCESS) return false;
  if (qlock.owns_lock()) qlock.unlock();
  return wait_fence_timeout(kFenceTimeoutNs);
}

bool CudaNv12Texture::ensure_size(int width, int height) {
  // Caller must hold slots_mu_ (try_copy) or be the only user (reset already drained).
  if (width == width_ && height == height_ && slots_[0].y.image && slots_[1].y.image) return true;
  if (create_failed_) return false;

  VRP_LOG("CudaNv12Texture: ensure_size %dx%d → %dx%d", width_, height_, width, height);
  if (!wait_fence_timeout(kFenceTimeoutNs)) {
    set_error("fence timeout on resize");
    return false;
  }
  // CUDA context should already be current (decode thread) for external memory destroy.
  destroy_slots();
  width_ = width;
  height_ = height;
  if (!create_slot(slots_[0], width, height) || !create_slot(slots_[1], width, height)) {
    VRP_ERR("CudaNv12Texture: create_slot failed — %s", last_error().c_str());
    destroy_slots();
    create_failed_ = true;
    return false;
  }
  if (!transition_all_to_shader_read()) {
    set_error("layout transition failed");
    destroy_slots();
    create_failed_ = true;
    return false;
  }
  layouts_ready_ = true;
  valid_.store(false);
  present_gen_.store(0);
  // bound_gen left; XR will see new present_gen
  set_error("ok");
  VRP_LOG("CudaNv12Texture: LINEAR slots ready %dx%d", width, height);
  return true;
}

bool CudaNv12Texture::fence_ready() const {
  if (!fence_ || !device_) return true;
  return vkGetFenceStatus(device_, fence_) == VK_SUCCESS;
}

bool CudaNv12Texture::wait_fence_timeout(uint64_t ns) {
  if (!fence_ || !device_) return true;
  return vkWaitForFences(device_, 1, &fence_, VK_TRUE, ns) == VK_SUCCESS;
}

bool CudaNv12Texture::poll_present() {
  const uint64_t g = present_gen_.load(std::memory_order_acquire);
  if (g == bound_gen_) return false;
  bound_gen_ = g;
  present_flips_.fetch_add(1);
  return true;
}

bool CudaNv12Texture::try_copy_from_cuda(const AVFrame* cuda_frame, void* cu_context, void* cu_stream,
                                         bool full_range_hint) {
#if !defined(VRP_HAS_FFMPEG)
  (void)cuda_frame;
  (void)cu_context;
  (void)cu_stream;
  (void)full_range_hint;
  return false;
#else
  auto fail = [&](const char* why) {
    if (!(create_failed_ && why && std::strcmp(why, "create_failed") == 0)) set_error(why);
    copy_fail_.fetch_add(1);
    return false;
  };

  if (!enabled_ || dying_.load() || !cuda_frame || !cu_context) {
    return fail(create_failed_ ? "create_failed" : "not ready");
  }
  if (create_failed_) {
    copy_fail_.fetch_add(1);
    return false;
  }
  if (cuda_frame->format != AV_PIX_FMT_CUDA) return fail("not AV_PIX_FMT_CUDA");
  if (!cuda_frame->data[0] || !cuda_frame->data[1]) return fail("null plane ptr");

  const int w = cuda_frame->width;
  const int h = cuda_frame->height;
  if (w <= 0 || h <= 0) return fail("bad size");

  copy_active_.store(true);
  struct ClearFlag {
    std::atomic<bool>& f;
    ~ClearFlag() { f.store(false); }
  } clear{copy_active_};
  if (dying_.load()) return fail("dying");

  auto& cu = cuda::api();
  auto ctx = reinterpret_cast<cuda::CUcontext>(cu_context);
  auto stream = reinterpret_cast<cuda::CUstream>(cu_stream);

  CtxScope scope;
  cuda::CUresult push = cu.cuCtxPushCurrent(ctx);
  if (push == cuda::CUDA_SUCCESS) {
    scope.pushed = true;
  } else if (!push_cuda_ctx(scope, nullptr)) {
    return fail(cuda::err_name(push));
  }
  struct PopGuard {
    CudaNv12Texture* self;
    CtxScope* scope;
    ~PopGuard() { self->pop_cuda_ctx(*scope); }
  } pop_guard{this, &scope};

  std::lock_guard<std::mutex> slot_lock(slots_mu_);
  if (!ensure_size(w, h)) {
    create_failed_ = true;
    return fail(last_error().empty() ? "ensure_size" : last_error().c_str());
  }
  if (!ensure_cuda_imports()) {
    create_failed_ = true;
    return fail(last_error().empty() ? "cuda import" : last_error().c_str());
  }

  write_ = 1 - display_slot_.load(std::memory_order_relaxed);
  Slot& slot = slots_[write_];

  auto memcpy_plane = [&](const Plane& plane, const uint8_t* src, int src_pitch, int width_bytes, int rows,
                          const char* name) -> bool {
    int pitch = src_pitch < 0 ? -src_pitch : src_pitch;
    if (pitch < width_bytes) pitch = width_bytes;
    if (plane.row_pitch < static_cast<VkDeviceSize>(width_bytes)) {
      set_error(std::string(name) + " dst pitch too small");
      return false;
    }
    cuda::CUDA_MEMCPY2D m{};
    m.srcMemoryType = cuda::CU_MEMORYTYPE_DEVICE;
    m.srcDevice = reinterpret_cast<cuda::CUdeviceptr>(src);
    m.srcPitch = static_cast<size_t>(pitch);
    m.dstMemoryType = cuda::CU_MEMORYTYPE_DEVICE;
    m.dstDevice = plane.cuda_ptr;
    m.dstPitch = static_cast<size_t>(plane.row_pitch);
    m.WidthInBytes = static_cast<size_t>(width_bytes);
    m.Height = static_cast<size_t>(rows);
    cuda::CUresult r = cu.cuMemcpy2D(&m);
    if (r != cuda::CUDA_SUCCESS) {
      set_error(std::string(name) + " cuMemcpy2D " + cuda::err_name(r));
      return false;
    }
    return true;
  };

  if (!memcpy_plane(slot.y, cuda_frame->data[0], cuda_frame->linesize[0], w, h, "Y")) {
    copy_fail_.fetch_add(1);
    return false;
  }
  if (!memcpy_plane(slot.uv, cuda_frame->data[1], cuda_frame->linesize[1], w, h / 2, "UV")) {
    copy_fail_.fetch_add(1);
    return false;
  }
  // Finish CUDA work on the decode thread only — do NOT vkQueueSubmit here.
  // Per-frame Vulkan barriers on the XR graphics queue caused Monado wait_semaphore timeouts.
  if (stream) {
    if (cu.cuStreamSynchronize(stream) != cuda::CUDA_SUCCESS) {
      if (cu.cuStreamSynchronize(nullptr) != cuda::CUDA_SUCCESS) { /* ignore */ }
    }
  } else if (cu.cuStreamSynchronize) {
    (void)cu.cuStreamSynchronize(nullptr);
  }

  display_slot_.store(write_, std::memory_order_release);
  full_range_.store(frame_is_full_range(cuda_frame, full_range_hint), std::memory_order_release);
  valid_.store(true, std::memory_order_release);
  present_gen_.fetch_add(1, std::memory_order_release);
  copy_ok_.fetch_add(1);
  set_error("ok");
  return true;
#endif
}

}  // namespace vrp
