#include "scene/fsr1_upscaler.hpp"

#include "common.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <mutex>
#include <vector>

#define A_CPU
#include "ffx_a.h"
#include "ffx_fsr1.h"

namespace vrp {
namespace {

constexpr int kMaxEdge = 7680;

struct BlitUbo {
  uint32_t width;
  uint32_t height;
  uint32_t nv12;
  uint32_t pad;
};

float rcas_sharpness_for_mode(FsrMode mode) {
  // FsrRcasCon: 0 = max sharp, larger = softer.
  switch (mode) {
    case FsrMode::UltraQuality: return 0.6f;
    case FsrMode::Quality: return 0.25f;
    case FsrMode::Performance: return 0.0f;
    case FsrMode::Off:
    default: return 0.2f;
  }
}

}  // namespace

void Fsr1Upscaler::init(VkPhysicalDevice phys, VkDevice device, VkQueue queue, uint32_t queue_family,
                        std::mutex* queue_mu, const std::string& shader_dir) {
  shutdown();
  phys_ = phys;
  device_ = device;
  queue_ = queue;
  queue_family_ = queue_family;
  queue_mu_ = queue_mu;
  shader_dir_ = shader_dir;

  VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  sci.magFilter = VK_FILTER_LINEAR;
  sci.minFilter = VK_FILTER_LINEAR;
  sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
  sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sci.maxLod = 1.f;
  VRP_CHECK(vkCreateSampler(device_, &sci, nullptr, &sampler_) == VK_SUCCESS, "fsr sampler");

  VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  pci.queueFamilyIndex = queue_family_;
  VRP_CHECK(vkCreateCommandPool(device_, &pci, nullptr, &cmd_pool_) == VK_SUCCESS, "fsr cmd pool");

  VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  cai.commandPool = cmd_pool_;
  cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cai.commandBufferCount = 1;
  VRP_CHECK(vkAllocateCommandBuffers(device_, &cai, &cmd_) == VK_SUCCESS, "fsr cmd");

  VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
  VRP_CHECK(vkCreateFence(device_, &fci, nullptr, &fence_) == VK_SUCCESS, "fsr fence");

  auto make_ubo = [&](VkBuffer& buf, VkDeviceMemory& mem, void*& mapped, VkDeviceSize size) {
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size;
    bi.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VRP_CHECK(vkCreateBuffer(device_, &bi, nullptr, &buf) == VK_SUCCESS, "fsr ubo");
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(device_, buf, &req);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = find_memory(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                              VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VRP_CHECK(vkAllocateMemory(device_, &mai, nullptr, &mem) == VK_SUCCESS, "fsr ubo mem");
    VRP_CHECK(vkBindBufferMemory(device_, buf, mem, 0) == VK_SUCCESS, "fsr ubo bind");
    VRP_CHECK(vkMapMemory(device_, mem, 0, size, 0, &mapped) == VK_SUCCESS, "fsr ubo map");
  };
  make_ubo(blit_ubo_, blit_ubo_mem_, blit_ubo_map_, sizeof(BlitUbo));
  make_ubo(easu_ubo_, easu_ubo_mem_, easu_ubo_map_, sizeof(FsrConsts));
  make_ubo(rcas_ubo_, rcas_ubo_mem_, rcas_ubo_map_, sizeof(FsrConsts));

  create_pipelines();
}

void Fsr1Upscaler::shutdown() {
  if (!device_) return;
  vkDeviceWaitIdle(device_);
  destroy_pipelines();
  destroy_rt(src_rt_);
  destroy_rt(easu_rt_);
  destroy_rt(out_rt_);
  out_view_ = VK_NULL_HANDLE;
  out_w_ = out_h_ = 0;

  auto free_ubo = [&](VkBuffer& b, VkDeviceMemory& m, void*& map) {
    if (map && m) {
      vkUnmapMemory(device_, m);
      map = nullptr;
    }
    if (b) vkDestroyBuffer(device_, b, nullptr);
    if (m) vkFreeMemory(device_, m, nullptr);
    b = VK_NULL_HANDLE;
    m = VK_NULL_HANDLE;
  };
  free_ubo(blit_ubo_, blit_ubo_mem_, blit_ubo_map_);
  free_ubo(easu_ubo_, easu_ubo_mem_, easu_ubo_map_);
  free_ubo(rcas_ubo_, rcas_ubo_mem_, rcas_ubo_map_);

  if (fence_) vkDestroyFence(device_, fence_, nullptr);
  if (cmd_pool_) vkDestroyCommandPool(device_, cmd_pool_, nullptr);
  if (sampler_) vkDestroySampler(device_, sampler_, nullptr);
  fence_ = VK_NULL_HANDLE;
  cmd_ = VK_NULL_HANDLE;
  cmd_pool_ = VK_NULL_HANDLE;
  sampler_ = VK_NULL_HANDLE;
  device_ = VK_NULL_HANDLE;
}

uint32_t Fsr1Upscaler::find_memory(uint32_t bits, VkMemoryPropertyFlags flags) const {
  VkPhysicalDeviceMemoryProperties props{};
  vkGetPhysicalDeviceMemoryProperties(phys_, &props);
  for (uint32_t i = 0; i < props.memoryTypeCount; ++i) {
    if ((bits & (1u << i)) && (props.memoryTypes[i].propertyFlags & flags) == flags) return i;
  }
  vrp_fatal("fsr: no memory type");
  return 0;
}

VkShaderModule Fsr1Upscaler::load_shader(const std::string& path) const {
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f) vrp_fatal("Cannot open shader: " + path);
  const auto size = f.tellg();
  f.seekg(0);
  std::vector<uint32_t> words(static_cast<size_t>(size) / 4);
  f.read(reinterpret_cast<char*>(words.data()), size);
  VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  ci.codeSize = words.size() * 4;
  ci.pCode = words.data();
  VkShaderModule mod = VK_NULL_HANDLE;
  VRP_CHECK(vkCreateShaderModule(device_, &ci, nullptr, &mod) == VK_SUCCESS, "fsr shader");
  return mod;
}

void Fsr1Upscaler::destroy_rt(Rt& rt) {
  if (!device_) return;
  if (rt.view) vkDestroyImageView(device_, rt.view, nullptr);
  if (rt.image) vkDestroyImage(device_, rt.image, nullptr);
  if (rt.memory) vkFreeMemory(device_, rt.memory, nullptr);
  rt = {};
}

bool Fsr1Upscaler::ensure_rt(Rt& rt, int w, int h, VkImageUsageFlags usage) {
  if (rt.image && rt.w == w && rt.h == h) return true;
  destroy_rt(rt);
  VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ii.imageType = VK_IMAGE_TYPE_2D;
  ii.format = VK_FORMAT_R8G8B8A8_UNORM;
  ii.extent = {static_cast<uint32_t>(w), static_cast<uint32_t>(h), 1};
  ii.mipLevels = 1;
  ii.arrayLayers = 1;
  ii.samples = VK_SAMPLE_COUNT_1_BIT;
  ii.tiling = VK_IMAGE_TILING_OPTIMAL;
  ii.usage = usage;
  ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  VRP_CHECK(vkCreateImage(device_, &ii, nullptr, &rt.image) == VK_SUCCESS, "fsr rt image");
  VkMemoryRequirements req{};
  vkGetImageMemoryRequirements(device_, rt.image, &req);
  VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  mai.allocationSize = req.size;
  mai.memoryTypeIndex = find_memory(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  VRP_CHECK(vkAllocateMemory(device_, &mai, nullptr, &rt.memory) == VK_SUCCESS, "fsr rt mem");
  VRP_CHECK(vkBindImageMemory(device_, rt.image, rt.memory, 0) == VK_SUCCESS, "fsr rt bind");
  VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  vi.image = rt.image;
  vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
  vi.format = VK_FORMAT_R8G8B8A8_UNORM;
  vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  vi.subresourceRange.levelCount = 1;
  vi.subresourceRange.layerCount = 1;
  VRP_CHECK(vkCreateImageView(device_, &vi, nullptr, &rt.view) == VK_SUCCESS, "fsr rt view");
  rt.w = w;
  rt.h = h;
  return true;
}

void Fsr1Upscaler::create_pipelines() {
  // Blit set: UBO + 2 combined samplers + storage image
  {
    VkDescriptorSetLayoutBinding b[4]{};
    b[0].binding = 0;
    b[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    b[0].descriptorCount = 1;
    b[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    b[1].binding = 1;
    b[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b[1].descriptorCount = 1;
    b[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    b[2] = b[1];
    b[2].binding = 2;
    b[3].binding = 3;
    b[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    b[3].descriptorCount = 1;
    b[3].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    lci.bindingCount = 4;
    lci.pBindings = b;
    VRP_CHECK(vkCreateDescriptorSetLayout(device_, &lci, nullptr, &blit_dsl_) == VK_SUCCESS, "blit dsl");
  }
  // FSR set: UBO + sampled image + storage image + sampler
  {
    VkDescriptorSetLayoutBinding b[4]{};
    b[0].binding = 0;
    b[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    b[0].descriptorCount = 1;
    b[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    b[1].binding = 1;
    b[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    b[1].descriptorCount = 1;
    b[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    b[2].binding = 2;
    b[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    b[2].descriptorCount = 1;
    b[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    b[3].binding = 3;
    b[3].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    b[3].descriptorCount = 1;
    b[3].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    b[3].pImmutableSamplers = &sampler_;
    VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    lci.bindingCount = 4;
    lci.pBindings = b;
    VRP_CHECK(vkCreateDescriptorSetLayout(device_, &lci, nullptr, &fsr_dsl_) == VK_SUCCESS, "fsr dsl");
  }

  VkDescriptorPoolSize sizes[4]{};
  sizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  sizes[0].descriptorCount = 8;
  sizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  sizes[1].descriptorCount = 4;
  sizes[2].type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  sizes[2].descriptorCount = 4;
  sizes[3].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  sizes[3].descriptorCount = 8;
  // sampler descriptors come from immutable; still need pool room for sets
  VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  pci.maxSets = 8;
  pci.poolSizeCount = 4;
  pci.pPoolSizes = sizes;
  VRP_CHECK(vkCreateDescriptorPool(device_, &pci, nullptr, &pool_) == VK_SUCCESS, "fsr pool");

  VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  ai.descriptorPool = pool_;
  ai.descriptorSetCount = 1;
  ai.pSetLayouts = &blit_dsl_;
  VRP_CHECK(vkAllocateDescriptorSets(device_, &ai, &blit_set_) == VK_SUCCESS, "blit set");
  ai.pSetLayouts = &fsr_dsl_;
  VRP_CHECK(vkAllocateDescriptorSets(device_, &ai, &easu_set_) == VK_SUCCESS, "easu set");
  VRP_CHECK(vkAllocateDescriptorSets(device_, &ai, &rcas_set_) == VK_SUCCESS, "rcas set");

  VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  plci.setLayoutCount = 1;
  plci.pSetLayouts = &blit_dsl_;
  VRP_CHECK(vkCreatePipelineLayout(device_, &plci, nullptr, &blit_layout_) == VK_SUCCESS, "blit layout");
  plci.pSetLayouts = &fsr_dsl_;
  VRP_CHECK(vkCreatePipelineLayout(device_, &plci, nullptr, &fsr_layout_) == VK_SUCCESS, "fsr layout");

  blit_cs_ = load_shader(shader_dir_ + "/fsr_src_blit.comp.spv");
  easu_cs_ = load_shader(shader_dir_ + "/fsr_easu.comp.spv");
  rcas_cs_ = load_shader(shader_dir_ + "/fsr_rcas.comp.spv");

  auto make_pipe = [&](VkShaderModule cs, VkPipelineLayout layout, VkPipeline& pipe) {
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    ci.stage.module = cs;
    ci.stage.pName = "main";
    ci.layout = layout;
    VRP_CHECK(vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &ci, nullptr, &pipe) == VK_SUCCESS,
              "fsr pipe");
  };
  make_pipe(blit_cs_, blit_layout_, blit_pipe_);
  make_pipe(easu_cs_, fsr_layout_, easu_pipe_);
  make_pipe(rcas_cs_, fsr_layout_, rcas_pipe_);
}

void Fsr1Upscaler::destroy_pipelines() {
  if (!device_) return;
  if (blit_pipe_) vkDestroyPipeline(device_, blit_pipe_, nullptr);
  if (easu_pipe_) vkDestroyPipeline(device_, easu_pipe_, nullptr);
  if (rcas_pipe_) vkDestroyPipeline(device_, rcas_pipe_, nullptr);
  if (blit_layout_) vkDestroyPipelineLayout(device_, blit_layout_, nullptr);
  if (fsr_layout_) vkDestroyPipelineLayout(device_, fsr_layout_, nullptr);
  if (blit_cs_) vkDestroyShaderModule(device_, blit_cs_, nullptr);
  if (easu_cs_) vkDestroyShaderModule(device_, easu_cs_, nullptr);
  if (rcas_cs_) vkDestroyShaderModule(device_, rcas_cs_, nullptr);
  if (pool_) vkDestroyDescriptorPool(device_, pool_, nullptr);
  if (blit_dsl_) vkDestroyDescriptorSetLayout(device_, blit_dsl_, nullptr);
  if (fsr_dsl_) vkDestroyDescriptorSetLayout(device_, fsr_dsl_, nullptr);
  blit_pipe_ = easu_pipe_ = rcas_pipe_ = VK_NULL_HANDLE;
  blit_layout_ = fsr_layout_ = VK_NULL_HANDLE;
  blit_cs_ = easu_cs_ = rcas_cs_ = VK_NULL_HANDLE;
  pool_ = VK_NULL_HANDLE;
  blit_dsl_ = fsr_dsl_ = VK_NULL_HANDLE;
  blit_set_ = easu_set_ = rcas_set_ = VK_NULL_HANDLE;
}

void Fsr1Upscaler::update_blit_set(VkImageView y, VkImageView uv, VkSampler samp, int nv12) {
  VkDescriptorBufferInfo bi{};
  bi.buffer = blit_ubo_;
  bi.range = sizeof(BlitUbo);

  VkDescriptorImageInfo yi{};
  yi.sampler = samp;
  yi.imageView = y;
  yi.imageLayout = (nv12 != 0) ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

  VkDescriptorImageInfo uvi = yi;
  uvi.imageView = (nv12 != 0 && uv) ? uv : y;

  VkDescriptorImageInfo di{};
  di.imageView = src_rt_.view;
  di.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

  VkWriteDescriptorSet w[4]{};
  w[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  w[0].dstSet = blit_set_;
  w[0].dstBinding = 0;
  w[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  w[0].descriptorCount = 1;
  w[0].pBufferInfo = &bi;
  w[1] = w[0];
  w[1].dstBinding = 1;
  w[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  w[1].pImageInfo = &yi;
  w[1].pBufferInfo = nullptr;
  w[2] = w[1];
  w[2].dstBinding = 2;
  w[2].pImageInfo = &uvi;
  w[3] = w[0];
  w[3].dstBinding = 3;
  w[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  w[3].pBufferInfo = nullptr;
  w[3].pImageInfo = &di;
  vkUpdateDescriptorSets(device_, 4, w, 0, nullptr);
}

void Fsr1Upscaler::update_fsr_set(VkDescriptorSet set, VkBuffer ubo, VkImageView in_view,
                                  VkImageView out_view) {
  VkDescriptorBufferInfo bi{};
  bi.buffer = ubo;
  bi.range = sizeof(FsrConsts);
  VkDescriptorImageInfo ii{};
  ii.imageView = in_view;
  ii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  VkDescriptorImageInfo oi{};
  oi.imageView = out_view;
  oi.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
  VkWriteDescriptorSet w[3]{};
  w[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  w[0].dstSet = set;
  w[0].dstBinding = 0;
  w[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  w[0].descriptorCount = 1;
  w[0].pBufferInfo = &bi;
  w[1] = w[0];
  w[1].dstBinding = 1;
  w[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  w[1].pBufferInfo = nullptr;
  w[1].pImageInfo = &ii;
  w[2] = w[0];
  w[2].dstBinding = 2;
  w[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  w[2].pBufferInfo = nullptr;
  w[2].pImageInfo = &oi;
  // binding 3 is immutable sampler
  vkUpdateDescriptorSets(device_, 3, w, 0, nullptr);
}

void Fsr1Upscaler::barrier_rw(VkCommandBuffer cmd, VkImage image, VkAccessFlags src, VkAccessFlags dst,
                              VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage,
                              VkImageLayout layout) {
  VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  b.srcAccessMask = src;
  b.dstAccessMask = dst;
  b.oldLayout = layout;
  b.newLayout = layout;
  b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.image = image;
  b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  b.subresourceRange.levelCount = 1;
  b.subresourceRange.layerCount = 1;
  vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

bool Fsr1Upscaler::process(VkImageView src_y, VkImageView src_uv, VkSampler src_sampler, int src_w,
                           int src_h, int nv12) {
  if (!enabled() || !device_ || !src_y || !src_sampler || src_w < 16 || src_h < 16) return false;

  const float scale = fsr_mode_scale(mode_);
  int dst_w = static_cast<int>(std::lround(static_cast<float>(src_w) * scale));
  int dst_h = static_cast<int>(std::lround(static_cast<float>(src_h) * scale));
  // Cap long edge to limit VRAM / dispatch cost (keep relative scale when possible).
  const int long_edge = std::max(dst_w, dst_h);
  if (long_edge > kMaxEdge) {
    const float s = static_cast<float>(kMaxEdge) / static_cast<float>(long_edge);
    dst_w = std::max(16, static_cast<int>(std::lround(static_cast<float>(dst_w) * s)));
    dst_h = std::max(16, static_cast<int>(std::lround(static_cast<float>(dst_h) * s)));
  }
  dst_w &= ~1;
  dst_h &= ~1;
  // Never downscale below source — RCAS-only path still varies sharpness by mode.
  if (dst_w < src_w) dst_w = src_w;
  if (dst_h < src_h) dst_h = src_h;
  dst_w &= ~1;
  dst_h &= ~1;

  const VkImageUsageFlags usage =
      VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  ensure_rt(src_rt_, src_w, src_h, usage);
  ensure_rt(easu_rt_, dst_w, dst_h, usage);
  ensure_rt(out_rt_, dst_w, dst_h, usage);

  BlitUbo blit{static_cast<uint32_t>(src_w), static_cast<uint32_t>(src_h),
               static_cast<uint32_t>(nv12), 0};
  std::memcpy(blit_ubo_map_, &blit, sizeof(blit));

  FsrConsts easu{};
  FsrEasuCon(easu.const0, easu.const1, easu.const2, easu.const3, static_cast<AF1>(src_w),
             static_cast<AF1>(src_h), static_cast<AF1>(src_w), static_cast<AF1>(src_h),
             static_cast<AF1>(dst_w), static_cast<AF1>(dst_h));
  easu.sample[0] = 0;
  std::memcpy(easu_ubo_map_, &easu, sizeof(easu));

  FsrConsts rcas{};
  FsrRcasCon(rcas.const0, rcas_sharpness_for_mode(mode_));
  rcas.sample[0] = 0;
  std::memcpy(rcas_ubo_map_, &rcas, sizeof(rcas));

  static FsrMode last_logged_mode = FsrMode::Off;
  static int last_logged_sw = 0, last_logged_sh = 0, last_logged_dw = 0, last_logged_dh = 0;
  if (mode_ != last_logged_mode || src_w != last_logged_sw || src_h != last_logged_sh ||
      dst_w != last_logged_dw || dst_h != last_logged_dh) {
    VRP_LOG("FSR1 %s: %dx%d → %dx%d (scale %.2f)", fsr_mode_name(mode_), src_w, src_h, dst_w, dst_h,
            scale);
    last_logged_mode = mode_;
    last_logged_sw = src_w;
    last_logged_sh = src_h;
    last_logged_dw = dst_w;
    last_logged_dh = dst_h;
  }

  update_blit_set(src_y, src_uv, src_sampler, nv12);
  update_fsr_set(easu_set_, easu_ubo_, src_rt_.view, easu_rt_.view);
  update_fsr_set(rcas_set_, rcas_ubo_, easu_rt_.view, out_rt_.view);

  vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX);
  vkResetFences(device_, 1, &fence_);
  vkResetCommandBuffer(cmd_, 0);

  VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(cmd_, &bi);

  auto transition = [&](VkImage img, VkImageLayout old_l, VkImageLayout new_l, VkAccessFlags src_a,
                        VkAccessFlags dst_a, VkPipelineStageFlags src_s, VkPipelineStageFlags dst_s) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = src_a;
    b.dstAccessMask = dst_a;
    b.oldLayout = old_l;
    b.newLayout = new_l;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(cmd_, src_s, dst_s, 0, 0, nullptr, 0, nullptr, 1, &b);
  };

  // Undefined → GENERAL for storage targets
  transition(src_rt_.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0,
             VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
  transition(easu_rt_.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0,
             VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
  transition(out_rt_.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0,
             VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

  // 1) Blit video → src_rt
  vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, blit_pipe_);
  vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, blit_layout_, 0, 1, &blit_set_, 0,
                          nullptr);
  vkCmdDispatch(cmd_, (src_w + 7) / 8, (src_h + 7) / 8, 1);

  transition(src_rt_.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
             VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

  // 2) EASU
  vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, easu_pipe_);
  vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, fsr_layout_, 0, 1, &easu_set_, 0,
                          nullptr);
  const int groups_x = (dst_w + 15) / 16;
  const int groups_y = (dst_h + 15) / 16;
  vkCmdDispatch(cmd_, groups_x, groups_y, 1);

  transition(easu_rt_.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
             VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

  // 3) RCAS
  vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, rcas_pipe_);
  vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, fsr_layout_, 0, 1, &rcas_set_, 0,
                          nullptr);
  vkCmdDispatch(cmd_, groups_x, groups_y, 1);

  transition(out_rt_.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
             VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

  vkEndCommandBuffer(cmd_);

  VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  si.commandBufferCount = 1;
  si.pCommandBuffers = &cmd_;
  if (queue_mu_) {
    std::lock_guard<std::mutex> lock(*queue_mu_);
    VRP_CHECK(vkQueueSubmit(queue_, 1, &si, fence_) == VK_SUCCESS, "fsr submit");
  } else {
    VRP_CHECK(vkQueueSubmit(queue_, 1, &si, fence_) == VK_SUCCESS, "fsr submit");
  }
  // Bound wait — scene will sample after fence in next prepare, but we need completion before
  // descriptor bind this frame.
  vkWaitForFences(device_, 1, &fence_, VK_TRUE, 50'000'000ull);
  if (vkGetFenceStatus(device_, fence_) != VK_SUCCESS) {
    vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX);
  }

  out_view_ = out_rt_.view;
  out_w_ = dst_w;
  out_h_ = dst_h;
  return true;
}

}  // namespace vrp
