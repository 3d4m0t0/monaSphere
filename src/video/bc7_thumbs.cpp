#include "video/bc7_thumbs.hpp"

#include "common.hpp"
#include "video/video_decoder.hpp"

#include <cstring>
#include <fstream>
#include <vector>

namespace vrp {
namespace {

std::vector<uint32_t> read_spv(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return {};
  in.seekg(0, std::ios::end);
  const auto n = static_cast<size_t>(in.tellg());
  if (n < 4 || (n % 4) != 0) return {};
  in.seekg(0);
  std::vector<uint32_t> code(n / 4);
  in.read(reinterpret_cast<char*>(code.data()), static_cast<std::streamsize>(n));
  return code;
}

}  // namespace

uint32_t Bc7ThumbCache::find_memory_type(uint32_t type_bits, VkMemoryPropertyFlags props) const {
  VkPhysicalDeviceMemoryProperties mp{};
  vkGetPhysicalDeviceMemoryProperties(phys_, &mp);
  for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
    if ((type_bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & props) == props) return i;
  }
  return UINT32_MAX;
}

bool Bc7ThumbCache::init(VkPhysicalDevice phys, VkDevice device, VkQueue queue, VkQueue graphics_queue,
                         uint32_t queue_family, std::mutex* graphics_mu, const std::string& shader_dir) {
  shutdown();
  phys_ = phys;
  device_ = device;
  queue_ = queue;
  graphics_queue_ = graphics_queue ? graphics_queue : queue;
  queue_family_ = queue_family;
  graphics_mu_ = graphics_mu;
  same_queue_ = queue_ == graphics_queue_;
  if (!phys_ || !device_ || !queue_) return false;

  uint32_t qcount = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(phys_, &qcount, nullptr);
  std::vector<VkQueueFamilyProperties> qprops(qcount);
  vkGetPhysicalDeviceQueueFamilyProperties(phys_, &qcount, qprops.data());
  if (queue_family_ >= qcount || !(qprops[queue_family_].queueFlags & VK_QUEUE_COMPUTE_BIT)) {
    VRP_LOG("BC1 thumbs: queue has no compute");
    return false;
  }

  VkFormatProperties fp{};
  vkGetPhysicalDeviceFormatProperties(phys_, VK_FORMAT_BC1_RGB_SRGB_BLOCK, &fp);
  const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
  if ((fp.optimalTilingFeatures & need) != need) {
    VRP_LOG("BC1 thumbs: BC1_RGB_SRGB is not supported");
    return false;
  }

  auto code = read_spv(shader_dir + "/bc1.comp.spv");
  if (code.empty()) {
    VRP_LOG("BC1 thumbs: shader missing");
    return false;
  }
  VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  smci.codeSize = code.size() * 4;
  smci.pCode = code.data();
  if (vkCreateShaderModule(device_, &smci, nullptr, &shader_) != VK_SUCCESS) return false;

  VkDescriptorSetLayoutBinding binds[2]{};
  binds[0].binding = 0;
  binds[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  binds[0].descriptorCount = 1;
  binds[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  binds[1] = binds[0];
  binds[1].binding = 1;
  VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  lci.bindingCount = 2;
  lci.pBindings = binds;
  if (vkCreateDescriptorSetLayout(device_, &lci, nullptr, &dsl_) != VK_SUCCESS) {
    shutdown();
    return false;
  }

  VkDescriptorPoolSize ps{};
  ps.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  ps.descriptorCount = 2;
  VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  pci.maxSets = 1;
  pci.poolSizeCount = 1;
  pci.pPoolSizes = &ps;
  if (vkCreateDescriptorPool(device_, &pci, nullptr, &dset_pool_) != VK_SUCCESS) {
    shutdown();
    return false;
  }
  VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  dai.descriptorPool = dset_pool_;
  dai.descriptorSetCount = 1;
  dai.pSetLayouts = &dsl_;
  if (vkAllocateDescriptorSets(device_, &dai, &dset_) != VK_SUCCESS) {
    shutdown();
    return false;
  }

  VkPushConstantRange pcr{};
  pcr.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  pcr.size = sizeof(uint32_t) * 5;
  VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  plci.setLayoutCount = 1;
  plci.pSetLayouts = &dsl_;
  plci.pushConstantRangeCount = 1;
  plci.pPushConstantRanges = &pcr;
  if (vkCreatePipelineLayout(device_, &plci, nullptr, &layout_) != VK_SUCCESS) {
    shutdown();
    return false;
  }

  VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
  ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
  ci.stage.module = shader_;
  ci.stage.pName = "main";
  ci.layout = layout_;
  if (vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &ci, nullptr, &pipe_) != VK_SUCCESS) {
    shutdown();
    return false;
  }

  VkCommandPoolCreateInfo cpci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  cpci.queueFamilyIndex = queue_family_;
  if (vkCreateCommandPool(device_, &cpci, nullptr, &pool_) != VK_SUCCESS) {
    shutdown();
    return false;
  }
  VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  cai.commandPool = pool_;
  cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cai.commandBufferCount = 2;
  VkCommandBuffer cbs[2] = {};
  if (vkAllocateCommandBuffers(device_, &cai, cbs) != VK_SUCCESS) {
    shutdown();
    return false;
  }
  cmd_ = cbs[0];
  handoff_cmd_ = cbs[1];
  VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
  if (vkCreateFence(device_, &fci, nullptr, &fence_) != VK_SUCCESS ||
      vkCreateFence(device_, &fci, nullptr, &handoff_fence_) != VK_SUCCESS) {
    shutdown();
    return false;
  }
  if (!same_queue_) {
    VkSemaphoreCreateInfo sci_sem{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    if (vkCreateSemaphore(device_, &sci_sem, nullptr, &handoff_sem_) != VK_SUCCESS) {
      shutdown();
      return false;
    }
  }

  VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  sci.magFilter = VK_FILTER_LINEAR;
  sci.minFilter = VK_FILTER_LINEAR;
  sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
  sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  if (vkCreateSampler(device_, &sci, nullptr, &sampler_) != VK_SUCCESS) {
    shutdown();
    return false;
  }

  slots_.assign(static_cast<size_t>(kSeekThumbCountMax), {});
  ok_ = true;
  VRP_LOG("BC1 thumbs: GPU compressor ready");
  return true;
}

void Bc7ThumbCache::destroy_slot(Slot& s) {
  if (s.view) vkDestroyImageView(device_, s.view, nullptr);
  if (s.image) vkDestroyImage(device_, s.image, nullptr);
  if (s.memory) vkFreeMemory(device_, s.memory, nullptr);
  s = {};
}

uint64_t Bc7ThumbCache::begin_build() {
  std::lock_guard<std::mutex> lock(mu_);
  if (device_ && fence_) vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX);
  if (device_ && handoff_fence_) vkWaitForFences(device_, 1, &handoff_fence_, VK_TRUE, UINT64_MAX);
  // Hide slots now so the next frame does not bind them. The images themselves stay
  // until release_images(); an already submitted eye frame may still be sampling them.
  ready_ = 0;
  interval_sec_ = 0;
  return ++build_;
}

void Bc7ThumbCache::release_images() {
  std::lock_guard<std::mutex> lock(mu_);
  if (device_ && fence_) vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX);
  if (device_ && handoff_fence_) vkWaitForFences(device_, 1, &handoff_fence_, VK_TRUE, UINT64_MAX);
  for (Slot& s : slots_) destroy_slot(s);
  ready_ = 0;
}

void Bc7ThumbCache::shutdown() {
  {
    std::lock_guard<std::mutex> lock(mu_);
    ok_ = false;
    if (device_ && fence_) vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX);
    if (device_ && handoff_fence_) vkWaitForFences(device_, 1, &handoff_fence_, VK_TRUE, UINT64_MAX);
    for (Slot& s : slots_) destroy_slot(s);
    slots_.clear();
    ready_ = 0;
    if (src_mapped_ && src_mem_) vkUnmapMemory(device_, src_mem_);
    src_mapped_ = nullptr;
    if (src_) vkDestroyBuffer(device_, src_, nullptr);
    if (src_mem_) vkFreeMemory(device_, src_mem_, nullptr);
    if (dst_) vkDestroyBuffer(device_, dst_, nullptr);
    if (dst_mem_) vkFreeMemory(device_, dst_mem_, nullptr);
    src_ = dst_ = VK_NULL_HANDLE;
    src_mem_ = dst_mem_ = VK_NULL_HANDLE;
    src_size_ = dst_size_ = 0;
    if (pipe_) vkDestroyPipeline(device_, pipe_, nullptr);
    if (layout_) vkDestroyPipelineLayout(device_, layout_, nullptr);
    if (dset_pool_) vkDestroyDescriptorPool(device_, dset_pool_, nullptr);
    if (dsl_) vkDestroyDescriptorSetLayout(device_, dsl_, nullptr);
    if (shader_) vkDestroyShaderModule(device_, shader_, nullptr);
    if (handoff_sem_) vkDestroySemaphore(device_, handoff_sem_, nullptr);
    if (handoff_fence_) vkDestroyFence(device_, handoff_fence_, nullptr);
    if (fence_) vkDestroyFence(device_, fence_, nullptr);
    if (pool_) vkDestroyCommandPool(device_, pool_, nullptr);
    if (sampler_) vkDestroySampler(device_, sampler_, nullptr);
    pipe_ = VK_NULL_HANDLE;
    layout_ = VK_NULL_HANDLE;
    dset_pool_ = VK_NULL_HANDLE;
    dsl_ = VK_NULL_HANDLE;
    dset_ = VK_NULL_HANDLE;
    shader_ = VK_NULL_HANDLE;
    fence_ = VK_NULL_HANDLE;
    handoff_fence_ = VK_NULL_HANDLE;
    handoff_sem_ = VK_NULL_HANDLE;
    pool_ = VK_NULL_HANDLE;
    cmd_ = VK_NULL_HANDLE;
    handoff_cmd_ = VK_NULL_HANDLE;
    sampler_ = VK_NULL_HANDLE;
    ok_ = false;
  }
  device_ = VK_NULL_HANDLE;
}

bool Bc7ThumbCache::ensure_src(VkDeviceSize bytes) {
  if (src_ && src_size_ >= bytes && src_mapped_) return true;
  if (src_mapped_ && src_mem_) vkUnmapMemory(device_, src_mem_);
  src_mapped_ = nullptr;
  if (src_) vkDestroyBuffer(device_, src_, nullptr);
  if (src_mem_) vkFreeMemory(device_, src_mem_, nullptr);
  src_ = VK_NULL_HANDLE;
  src_mem_ = VK_NULL_HANDLE;
  src_size_ = 0;

  VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  bci.size = bytes;
  bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
  if (vkCreateBuffer(device_, &bci, nullptr, &src_) != VK_SUCCESS) return false;
  VkMemoryRequirements req{};
  vkGetBufferMemoryRequirements(device_, src_, &req);
  VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  mai.allocationSize = req.size;
  mai.memoryTypeIndex = find_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                                  VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  if (mai.memoryTypeIndex == UINT32_MAX ||
      vkAllocateMemory(device_, &mai, nullptr, &src_mem_) != VK_SUCCESS ||
      vkBindBufferMemory(device_, src_, src_mem_, 0) != VK_SUCCESS ||
      vkMapMemory(device_, src_mem_, 0, req.size, 0, &src_mapped_) != VK_SUCCESS) {
    return false;
  }
  src_size_ = bytes;
  write_descriptors();
  return true;
}

bool Bc7ThumbCache::ensure_dst(VkDeviceSize bytes) {
  if (dst_ && dst_size_ >= bytes) return true;
  if (dst_) vkDestroyBuffer(device_, dst_, nullptr);
  if (dst_mem_) vkFreeMemory(device_, dst_mem_, nullptr);
  dst_ = VK_NULL_HANDLE;
  dst_mem_ = VK_NULL_HANDLE;
  dst_size_ = 0;

  VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  bci.size = bytes;
  bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
  if (vkCreateBuffer(device_, &bci, nullptr, &dst_) != VK_SUCCESS) return false;
  VkMemoryRequirements req{};
  vkGetBufferMemoryRequirements(device_, dst_, &req);
  VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  mai.allocationSize = req.size;
  mai.memoryTypeIndex = find_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (mai.memoryTypeIndex == UINT32_MAX) {
    mai.memoryTypeIndex = find_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
  }
  if (mai.memoryTypeIndex == UINT32_MAX ||
      vkAllocateMemory(device_, &mai, nullptr, &dst_mem_) != VK_SUCCESS ||
      vkBindBufferMemory(device_, dst_, dst_mem_, 0) != VK_SUCCESS) {
    return false;
  }
  dst_size_ = bytes;
  write_descriptors();
  return true;
}

void Bc7ThumbCache::write_descriptors() {
  if (!dset_ || !src_ || !dst_) return;
  VkDescriptorBufferInfo src_i{};
  src_i.buffer = src_;
  src_i.range = VK_WHOLE_SIZE;
  VkDescriptorBufferInfo dst_i{};
  dst_i.buffer = dst_;
  dst_i.range = VK_WHOLE_SIZE;
  VkWriteDescriptorSet w[2]{};
  w[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  w[0].dstSet = dset_;
  w[0].dstBinding = 0;
  w[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  w[0].descriptorCount = 1;
  w[0].pBufferInfo = &src_i;
  w[1] = w[0];
  w[1].dstBinding = 1;
  w[1].pBufferInfo = &dst_i;
  vkUpdateDescriptorSets(device_, 2, w, 0, nullptr);
}

bool Bc7ThumbCache::ensure_slot(int index, int blocks_x, int blocks_y) {
  if (index < 0 || index >= static_cast<int>(slots_.size())) return false;
  const int w = blocks_x * 4;
  const int h = blocks_y * 4;
  Slot& s = slots_[static_cast<size_t>(index)];
  if (s.image && s.width == w && s.height == h && s.view) return true;
  destroy_slot(s);

  VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ici.imageType = VK_IMAGE_TYPE_2D;
  ici.format = VK_FORMAT_BC1_RGB_SRGB_BLOCK;
  ici.extent = {static_cast<uint32_t>(w), static_cast<uint32_t>(h), 1};
  ici.mipLevels = 1;
  ici.arrayLayers = 1;
  ici.samples = VK_SAMPLE_COUNT_1_BIT;
  ici.tiling = VK_IMAGE_TILING_OPTIMAL;
  ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (vkCreateImage(device_, &ici, nullptr, &s.image) != VK_SUCCESS) return false;
  VkMemoryRequirements req{};
  vkGetImageMemoryRequirements(device_, s.image, &req);
  VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  mai.allocationSize = req.size;
  mai.memoryTypeIndex = find_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (mai.memoryTypeIndex == UINT32_MAX ||
      vkAllocateMemory(device_, &mai, nullptr, &s.memory) != VK_SUCCESS ||
      vkBindImageMemory(device_, s.image, s.memory, 0) != VK_SUCCESS) {
    destroy_slot(s);
    return false;
  }
  VkImageViewCreateInfo ivci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  ivci.image = s.image;
  ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
  ivci.format = VK_FORMAT_BC1_RGB_SRGB_BLOCK;
  ivci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  ivci.subresourceRange.levelCount = 1;
  ivci.subresourceRange.layerCount = 1;
  if (vkCreateImageView(device_, &ivci, nullptr, &s.view) != VK_SUCCESS) {
    destroy_slot(s);
    return false;
  }
  s.width = w;
  s.height = h;
  return true;
}

bool Bc7ThumbCache::compress(int index, const uint8_t* rgba, int width, int height, double interval_sec,
                              uint64_t build) {
  if (!rgba || width < 2 || height < 2) return false;
  const int blocks_x = (width + 3) / 4;
  const int blocks_y = (height + 3) / 4;
  const VkDeviceSize src_bytes = static_cast<VkDeviceSize>(width) * static_cast<VkDeviceSize>(height) * 4;
  const VkDeviceSize dst_bytes = static_cast<VkDeviceSize>(blocks_x) * static_cast<VkDeviceSize>(blocks_y) * 8;

  std::lock_guard<std::mutex> lock(mu_);
  if (!ok_ || build != build_ || index < 0 || index >= static_cast<int>(slots_.size())) return false;
  if (!ensure_src(src_bytes) || !ensure_dst(dst_bytes) || !ensure_slot(index, blocks_x, blocks_y)) return false;
  std::memcpy(src_mapped_, rgba, static_cast<size_t>(src_bytes));

  // One submission: every block is written, then copied. Splitting the copy across
  // submits left 8-byte BC1 blocks half-visible, which sample as saturated colors.
  constexpr uint32_t kChunkBlocks = 64u * 1024u;
  const uint32_t total = static_cast<uint32_t>(blocks_x) * static_cast<uint32_t>(blocks_y);
  Slot& slot = slots_[static_cast<size_t>(index)];
  auto fail = [&](const char* why, VkResult result) {
    VRP_LOG("BC1 compress failed: %s (%d) index=%d", why, static_cast<int>(result), index);
    return false;
  };

  if (vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX) != VK_SUCCESS) return fail("wait", VK_ERROR_DEVICE_LOST);
  if (vkResetCommandBuffer(cmd_, 0) != VK_SUCCESS) return fail("reset", VK_ERROR_DEVICE_LOST);

  VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (vkBeginCommandBuffer(cmd_, &bi) != VK_SUCCESS) return fail("begin", VK_ERROR_DEVICE_LOST);

  VkBufferMemoryBarrier src_bar{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
  src_bar.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
  src_bar.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  src_bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  src_bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  src_bar.buffer = src_;
  src_bar.size = src_bytes;
  vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 1,
                       &src_bar, 0, nullptr);

  vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, pipe_);
  vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, layout_, 0, 1, &dset_, 0, nullptr);
  for (uint32_t base = 0; base < total;) {
    const uint32_t n = std::min(kChunkBlocks, total - base);
    const uint32_t pc[5] = {static_cast<uint32_t>(width), static_cast<uint32_t>(height),
                            static_cast<uint32_t>(blocks_x), static_cast<uint32_t>(blocks_y), base};
    vkCmdPushConstants(cmd_, layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), pc);
    vkCmdDispatch(cmd_, (n + 63u) / 64u, 1, 1);
    base += n;
  }

  VkBufferMemoryBarrier dst_bar{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
  dst_bar.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
  dst_bar.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  dst_bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  dst_bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  dst_bar.buffer = dst_;
  dst_bar.size = dst_bytes;
  vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 1,
                       &dst_bar, 0, nullptr);

  VkImageMemoryBarrier to_copy{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  to_copy.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  to_copy.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  to_copy.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  to_copy.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  to_copy.image = slot.image;
  to_copy.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  to_copy.subresourceRange.levelCount = 1;
  to_copy.subresourceRange.layerCount = 1;
  to_copy.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                       nullptr, 1, &to_copy);

  VkBufferImageCopy region{};
  region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  region.imageSubresource.layerCount = 1;
  region.imageExtent = {static_cast<uint32_t>(blocks_x * 4), static_cast<uint32_t>(blocks_y * 4), 1};
  vkCmdCopyBufferToImage(cmd_, dst_, slot.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

  VkImageMemoryBarrier to_sample = to_copy;
  to_sample.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  to_sample.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  to_sample.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  to_sample.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0,
                       nullptr, 1, &to_sample);

  if (vkEndCommandBuffer(cmd_) != VK_SUCCESS) return fail("end", VK_ERROR_DEVICE_LOST);

  VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  si.commandBufferCount = 1;
  si.pCommandBuffers = &cmd_;
  if (vkResetFences(device_, 1, &fence_) != VK_SUCCESS) return fail("reset fence", VK_ERROR_DEVICE_LOST);
  VkResult submit = VK_SUCCESS;
  {
    std::unique_lock<std::mutex> qlock;
    if (same_queue_ && graphics_mu_) qlock = std::unique_lock<std::mutex>(*graphics_mu_);
    submit = vkQueueSubmit(queue_, 1, &si, fence_);
    if (submit != VK_SUCCESS) {
      vkResetCommandBuffer(cmd_, 0);
      VkCommandBufferBeginInfo empty_bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
      empty_bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
      if (vkBeginCommandBuffer(cmd_, &empty_bi) == VK_SUCCESS && vkEndCommandBuffer(cmd_) == VK_SUCCESS) {
        si.signalSemaphoreCount = 0;
        si.pSignalSemaphores = nullptr;
        (void)vkQueueSubmit(queue_, 1, &si, fence_);
      }
    }
  }
  if (submit != VK_SUCCESS) return fail("submit", submit);
  if (vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX) != VK_SUCCESS) return fail("fence", VK_ERROR_DEVICE_LOST);

  // Do not submit a follow-up wait on the eye queue. That wait sits ahead of the
  // next xrEndFrame and the client timeline semaphore never advances.

  if (interval_sec > 0) interval_sec_ = interval_sec;
  if (index == ready_) ready_ = index + 1;
  return true;
}

int Bc7ThumbCache::index_for(double sec) const {
  const int n = ready_;
  if (n <= 0) return -1;
  int idx = 0;
  // The thumb at index i is the frame at i * interval. Floor keeps that frame at or
  // behind the bar, so the picture does not jump ahead and wait for the bar.
  if (interval_sec_ > 0) idx = static_cast<int>(sec / interval_sec_);
  if (idx < 0) idx = 0;
  if (idx >= n) idx = n - 1;
  return idx;
}

VkImageView Bc7ThumbCache::view(int index) const {
  if (index < 0 || index >= ready_) return VK_NULL_HANDLE;
  return slots_[static_cast<size_t>(index)].view;
}

}  // namespace vrp
