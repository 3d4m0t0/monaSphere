#include "scene/scene_renderer.hpp"

#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>
#include <fstream>
#include <vector>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace vrp {
namespace {

struct Mat4 {
  float m[16]{};
  static Mat4 identity() {
    Mat4 r{};
    r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.f;
    return r;
  }
  static Mat4 mul(const Mat4& a, const Mat4& b) {
    Mat4 r{};
    for (int c = 0; c < 4; ++c)
      for (int row = 0; row < 4; ++row)
        r.m[c * 4 + row] = a.m[0 * 4 + row] * b.m[c * 4 + 0] + a.m[1 * 4 + row] * b.m[c * 4 + 1] +
                           a.m[2 * 4 + row] * b.m[c * 4 + 2] + a.m[3 * 4 + row] * b.m[c * 4 + 3];
    return r;
  }
};

Mat4 pose_to_matrix(const XrPosef& pose) {
  const auto& q = pose.orientation;
  const auto& p = pose.position;
  const float x2 = q.x + q.x, y2 = q.y + q.y, z2 = q.z + q.z;
  const float xx = q.x * x2, yy = q.y * y2, zz = q.z * z2;
  const float xy = q.x * y2, xz = q.x * z2, yz = q.y * z2;
  const float wx = q.w * x2, wy = q.w * y2, wz = q.w * z2;
  Mat4 r = Mat4::identity();
  r.m[0] = 1.f - (yy + zz);
  r.m[1] = xy + wz;
  r.m[2] = xz - wy;
  r.m[4] = xy - wz;
  r.m[5] = 1.f - (xx + zz);
  r.m[6] = yz + wx;
  r.m[8] = xz + wy;
  r.m[9] = yz - wx;
  r.m[10] = 1.f - (xx + yy);
  r.m[12] = p.x;
  r.m[13] = p.y;
  r.m[14] = p.z;
  return r;
}

Mat4 invert_rigid(const Mat4& m) {
  Mat4 r = Mat4::identity();
  // transpose rotation
  r.m[0] = m.m[0];
  r.m[1] = m.m[4];
  r.m[2] = m.m[8];
  r.m[4] = m.m[1];
  r.m[5] = m.m[5];
  r.m[6] = m.m[9];
  r.m[8] = m.m[2];
  r.m[9] = m.m[6];
  r.m[10] = m.m[10];
  // translation
  const float tx = m.m[12], ty = m.m[13], tz = m.m[14];
  r.m[12] = -(r.m[0] * tx + r.m[4] * ty + r.m[8] * tz);
  r.m[13] = -(r.m[1] * tx + r.m[5] * ty + r.m[9] * tz);
  r.m[14] = -(r.m[2] * tx + r.m[6] * ty + r.m[10] * tz);
  return r;
}

Mat4 projection_fov(const XrFovf& fov, float near_z, float far_z) {
  // OpenXR FOV → Vulkan clip space (Y down, Z in [0, 1]).
  // Matching Khronos xr_linear.h GRAPHICS_VULKAN.
  const float l = std::tan(fov.angleLeft);
  const float r = std::tan(fov.angleRight);
  const float u = std::tan(fov.angleUp);
  const float d = std::tan(fov.angleDown);
  const float w = r - l;
  const float h = u - d;
  Mat4 m{};
  m.m[0] = 2.f / w;
  m.m[5] = -2.f / h;  // Vulkan: invert Y vs OpenGL
  m.m[8] = (r + l) / w;
  m.m[9] = (u + d) / h;
  m.m[10] = far_z / (near_z - far_z);
  m.m[11] = -1.f;
  m.m[14] = (far_z * near_z) / (near_z - far_z);
  return m;
}

struct PushConstants {
  Mat4 mvp;
  int32_t viewIndex;
  int32_t stereoMode;
  int32_t is180;
  int32_t nv12;
};

std::vector<uint32_t> read_spv(const std::string& path) {
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f) vrp_fatal("Cannot open shader: " + path);
  auto size = f.tellg();
  f.seekg(0);
  std::vector<uint32_t> data(static_cast<size_t>(size) / 4);
  f.read(reinterpret_cast<char*>(data.data()), size);
  return data;
}

}  // namespace

void SceneRenderer::init(XrVulkanApp& app, const std::string& shader_dir) {
  app_ = &app;
  device_ = app.device();
  phys_ = app.physical_device();
  shader_dir_ = shader_dir;
  create_descriptors();
  build_meshes();
  if (!app.swapchains().empty()) {
    create_pipelines(app.swapchains()[0].format);
  }

  cmds_.resize(2);
  VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  ai.commandPool = app.command_pool();
  ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  ai.commandBufferCount = 2;
  VRP_CHECK(vkAllocateCommandBuffers(device_, &ai, cmds_.data()) == VK_SUCCESS, "scene cmds");

  VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
  VRP_CHECK(vkCreateFence(device_, &fci, nullptr, &submit_fence_) == VK_SUCCESS, "scene fence");
}

void SceneRenderer::shutdown() {
  if (!device_) return;
  vkDeviceWaitIdle(device_);
  if (submit_fence_) {
    vkDestroyFence(device_, submit_fence_, nullptr);
    submit_fence_ = VK_NULL_HANDLE;
  }
  if (pipeline_) vkDestroyPipeline(device_, pipeline_, nullptr);
  if (layout_) vkDestroyPipelineLayout(device_, layout_, nullptr);
  if (vert_) vkDestroyShaderModule(device_, vert_, nullptr);
  if (frag_) vkDestroyShaderModule(device_, frag_, nullptr);
  if (dset_) {
    // freed with pool
  }
  if (pool_) vkDestroyDescriptorPool(device_, pool_, nullptr);
  if (dsl_) vkDestroyDescriptorSetLayout(device_, dsl_, nullptr);
  auto destroy_buf = [&](VkBuffer& b, VkDeviceMemory& m) {
    if (b) vkDestroyBuffer(device_, b, nullptr);
    if (m) vkFreeMemory(device_, m, nullptr);
    b = VK_NULL_HANDLE;
    m = VK_NULL_HANDLE;
  };
  destroy_buf(vbo_quad_, vbo_quad_mem_);
  destroy_buf(vbo_sphere_, vbo_sphere_mem_);
  destroy_buf(ibo_sphere_, ibo_sphere_mem_);
  pipeline_ = VK_NULL_HANDLE;
  layout_ = VK_NULL_HANDLE;
}

void SceneRenderer::set_projection(ProjectionMode mode, StereoLayout stereo, float flat_fov_deg,
                                   float distance) {
  mode_ = mode;
  stereo_ = stereo;
  flat_fov_deg_ = flat_fov_deg;
  distance_ = distance;
}

void SceneRenderer::set_content_aspect(float width_over_height) {
  if (width_over_height > 0.05f && width_over_height < 50.f) {
    content_aspect_ = width_over_height;
  }
}

void SceneRenderer::set_texture(VkImageView view, VkSampler sampler) {
  if (!view || !sampler || !dset_) return;
  nv12_ = false;
  nv12_full_range_ = false;
  rgba_gamma_ = false;
  VkDescriptorImageInfo ii[2]{};
  for (int i = 0; i < 2; ++i) {
    ii[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    ii[i].imageView = view;
    ii[i].sampler = sampler;
  }
  VkWriteDescriptorSet w[2]{};
  for (int i = 0; i < 2; ++i) {
    w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w[i].dstSet = dset_;
    w[i].dstBinding = static_cast<uint32_t>(i);
    w[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w[i].descriptorCount = 1;
    w[i].pImageInfo = &ii[i];
  }
  vkUpdateDescriptorSets(device_, 2, w, 0, nullptr);
}

void SceneRenderer::set_texture_gamma(VkImageView view, VkSampler sampler) {
  set_texture(view, sampler);
  rgba_gamma_ = true;
}

void SceneRenderer::set_nv12_texture(VkImageView y, VkImageView uv, VkSampler sampler, bool full_range) {
  if (!y || !uv || !sampler || !dset_) return;
  nv12_ = true;
  nv12_full_range_ = full_range;
  rgba_gamma_ = false;
  VkDescriptorImageInfo ii[2]{};
  // CUDA LINEAR path keeps images in GENERAL after the one-time transition.
  ii[0].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
  ii[0].imageView = y;
  ii[0].sampler = sampler;
  ii[1].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
  ii[1].imageView = uv;
  ii[1].sampler = sampler;
  VkWriteDescriptorSet w[2]{};
  for (int i = 0; i < 2; ++i) {
    w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w[i].dstSet = dset_;
    w[i].dstBinding = static_cast<uint32_t>(i);
    w[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w[i].descriptorCount = 1;
    w[i].pImageInfo = &ii[i];
  }
  vkUpdateDescriptorSets(device_, 2, w, 0, nullptr);
}

void SceneRenderer::set_hud_texture(VkImageView view, VkSampler sampler, bool visible) {
  hud_visible_ = visible && view && sampler && hud_dset_;
  if (!hud_visible_) return;
  VkDescriptorImageInfo ii[2]{};
  for (int i = 0; i < 2; ++i) {
    ii[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    ii[i].imageView = view;
    ii[i].sampler = sampler;
  }
  VkWriteDescriptorSet w[2]{};
  for (int i = 0; i < 2; ++i) {
    w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w[i].dstSet = hud_dset_;
    w[i].dstBinding = static_cast<uint32_t>(i);
    w[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w[i].descriptorCount = 1;
    w[i].pImageInfo = &ii[i];
  }
  vkUpdateDescriptorSets(device_, 2, w, 0, nullptr);
}

void SceneRenderer::set_hud_layout(float half_width_m, float distance_m, float aspect_h_over_w) {
  if (half_width_m > 0.05f) hud_half_w_ = half_width_m;
  if (distance_m > 0.3f) hud_distance_ = distance_m;
  if (aspect_h_over_w > 0.2f) hud_aspect_ = aspect_h_over_w;
}

bool SceneRenderer::wait_previous_submit(uint64_t timeout_ns) {
  if (!submit_fence_ || !device_) return true;
  const VkResult r = vkWaitForFences(device_, 1, &submit_fence_, VK_TRUE, timeout_ns);
  return r == VK_SUCCESS;
}

uint32_t SceneRenderer::find_memory(uint32_t bits, VkMemoryPropertyFlags flags) const {
  VkPhysicalDeviceMemoryProperties mp{};
  vkGetPhysicalDeviceMemoryProperties(phys_, &mp);
  for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
    if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & flags) == flags) return i;
  }
  vrp_fatal("memory type");
}

VkShaderModule SceneRenderer::load_shader(const std::string& path) {
  auto code = read_spv(path);
  VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  ci.codeSize = code.size() * 4;
  ci.pCode = code.data();
  VkShaderModule mod = VK_NULL_HANDLE;
  VRP_CHECK(vkCreateShaderModule(device_, &ci, nullptr, &mod) == VK_SUCCESS, path);
  return mod;
}

void SceneRenderer::create_descriptors() {
  VkDescriptorSetLayoutBinding b[2]{};
  for (int i = 0; i < 2; ++i) {
    b[i].binding = static_cast<uint32_t>(i);
    b[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b[i].descriptorCount = 1;
    b[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  }
  VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  lci.bindingCount = 2;
  lci.pBindings = b;
  VRP_CHECK(vkCreateDescriptorSetLayout(device_, &lci, nullptr, &dsl_) == VK_SUCCESS, "dsl");

  VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 16};
  VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  pci.poolSizeCount = 1;
  pci.pPoolSizes = &ps;
  pci.maxSets = 4;
  VRP_CHECK(vkCreateDescriptorPool(device_, &pci, nullptr, &pool_) == VK_SUCCESS, "dpool");

  VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  ai.descriptorPool = pool_;
  ai.descriptorSetCount = 1;
  ai.pSetLayouts = &dsl_;
  VRP_CHECK(vkAllocateDescriptorSets(device_, &ai, &dset_) == VK_SUCCESS, "dset");
  VRP_CHECK(vkAllocateDescriptorSets(device_, &ai, &hud_dset_) == VK_SUCCESS, "hud dset");
}

void SceneRenderer::build_meshes() {
  // Quad in XY, facing -Z (OpenXR forward). +X = right.
  const float aspect = 16.f / 9.f;
  const float h = 1.0f;
  const float w = h * aspect;
  Vertex quad[4] = {
      {-w, -h, 0, 0, 1},
      {w, -h, 0, 1, 1},
      {w, h, 0, 1, 0},
      {-w, h, 0, 0, 0},
  };
  // two triangles as triangle list with duplicated verts via indexless draw of 6
  Vertex quad_tris[6] = {quad[0], quad[1], quad[2], quad[0], quad[2], quad[3]};

  auto upload = [&](const void* data, VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer& buf,
                    VkDeviceMemory& mem) {
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = size;
    bci.usage = usage;
    VRP_CHECK(vkCreateBuffer(device_, &bci, nullptr, &buf) == VK_SUCCESS, "mesh buffer");
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(device_, buf, &req);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex =
        find_memory(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VRP_CHECK(vkAllocateMemory(device_, &mai, nullptr, &mem) == VK_SUCCESS, "mesh mem");
    vkBindBufferMemory(device_, buf, mem, 0);
    void* mapped = nullptr;
    vkMapMemory(device_, mem, 0, size, 0, &mapped);
    std::memcpy(mapped, data, static_cast<size_t>(size));
    vkUnmapMemory(device_, mem);
  };

  upload(quad_tris, sizeof(quad_tris), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, vbo_quad_, vbo_quad_mem_);

  // Equirect sphere: Y-up, -Z forward at u=0.5 (video center in front at session start).
  const int stacks = 48, slices = 96;
  const float radius = 10.f;
  std::vector<Vertex> sphere_v;
  std::vector<uint32_t> sphere_i;
  for (int i = 0; i <= stacks; ++i) {
    float v = static_cast<float>(i) / stacks;
    float phi = v * static_cast<float>(M_PI);  // 0..π (north pole → south)
    for (int j = 0; j <= slices; ++j) {
      float u = static_cast<float>(j) / slices;
      float theta = (u - 0.5f) * static_cast<float>(2.0 * M_PI);  // -π..π, 0 at center
      const float sp = std::sin(phi);
      const float x = radius * std::sin(theta) * sp;   // +X = right
      const float y = radius * std::cos(phi);
      const float z = -radius * std::cos(theta) * sp;  // u=0.5 → -Z forward
      sphere_v.push_back({x, y, z, u, v});
    }
  }
  for (int i = 0; i < stacks; ++i) {
    for (int j = 0; j < slices; ++j) {
      uint32_t a = i * (slices + 1) + j;
      uint32_t b = a + slices + 1;
      // Inward-facing winding (viewed from origin)
      sphere_i.push_back(a);
      sphere_i.push_back(b);
      sphere_i.push_back(a + 1);
      sphere_i.push_back(b);
      sphere_i.push_back(b + 1);
      sphere_i.push_back(a + 1);
    }
  }
  sphere_index_count_ = static_cast<uint32_t>(sphere_i.size());
  upload(sphere_v.data(), sphere_v.size() * sizeof(Vertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, vbo_sphere_,
         vbo_sphere_mem_);
  upload(sphere_i.data(), sphere_i.size() * sizeof(uint32_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT, ibo_sphere_,
         ibo_sphere_mem_);
}

void SceneRenderer::create_pipelines(VkFormat /*format*/) {
  vert_ = load_shader(shader_dir_ + "/scene.vert.spv");
  frag_ = load_shader(shader_dir_ + "/scene.frag.spv");

  VkPushConstantRange pcr{};
  pcr.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
  pcr.size = sizeof(PushConstants);

  VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  plci.setLayoutCount = 1;
  plci.pSetLayouts = &dsl_;
  plci.pushConstantRangeCount = 1;
  plci.pPushConstantRanges = &pcr;
  VRP_CHECK(vkCreatePipelineLayout(device_, &plci, nullptr, &layout_) == VK_SUCCESS, "pipeline layout");

  VkPipelineShaderStageCreateInfo stages[2]{};
  stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[0].module = vert_;
  stages[0].pName = "main";
  stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  stages[1].module = frag_;
  stages[1].pName = "main";

  VkVertexInputBindingDescription bind{};
  bind.binding = 0;
  bind.stride = sizeof(Vertex);
  bind.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
  VkVertexInputAttributeDescription attrs[2]{};
  attrs[0].location = 0;
  attrs[0].binding = 0;
  attrs[0].format = VK_FORMAT_R32G32B32_SFLOAT;
  attrs[0].offset = offsetof(Vertex, x);
  attrs[1].location = 1;
  attrs[1].binding = 0;
  attrs[1].format = VK_FORMAT_R32G32_SFLOAT;
  attrs[1].offset = offsetof(Vertex, u);

  VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  vi.vertexBindingDescriptionCount = 1;
  vi.pVertexBindingDescriptions = &bind;
  vi.vertexAttributeDescriptionCount = 2;
  vi.pVertexAttributeDescriptions = attrs;

  VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

  VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  vp.viewportCount = 1;
  vp.scissorCount = 1;

  VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  rs.polygonMode = VK_POLYGON_MODE_FILL;
  rs.cullMode = VK_CULL_MODE_NONE;
  rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  rs.lineWidth = 1.f;

  VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

  VkPipelineColorBlendAttachmentState blend{};
  blend.blendEnable = VK_TRUE;
  blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
  blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
  blend.colorBlendOp = VK_BLEND_OP_ADD;
  blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
  blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
  blend.alphaBlendOp = VK_BLEND_OP_ADD;
  blend.colorWriteMask =
      VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  cb.attachmentCount = 1;
  cb.pAttachments = &blend;

  VkDynamicState dyn_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dyn.dynamicStateCount = 2;
  dyn.pDynamicStates = dyn_states;

  VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  gp.stageCount = 2;
  gp.pStages = stages;
  gp.pVertexInputState = &vi;
  gp.pInputAssemblyState = &ia;
  gp.pViewportState = &vp;
  gp.pRasterizationState = &rs;
  gp.pMultisampleState = &ms;
  gp.pColorBlendState = &cb;
  gp.pDynamicState = &dyn;
  gp.layout = layout_;
  gp.renderPass = app_->color_pass();
  gp.subpass = 0;
  VRP_CHECK(vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &gp, nullptr, &pipeline_) == VK_SUCCESS,
            "pipeline");
}

void SceneRenderer::draw_view(uint32_t view_index, const XrView& view, ViewSwapchain& sc,
                              uint32_t image_index, VkCommandBuffer cmd) {
  auto& img = sc.images[image_index];

  VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(cmd, &bi);

  VkClearValue clear{};
  clear.color = {{0.02f, 0.02f, 0.05f, 1.f}};
  VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
  rp.renderPass = app_->color_pass();
  rp.framebuffer = img.framebuffer;
  rp.renderArea.extent = {static_cast<uint32_t>(sc.width), static_cast<uint32_t>(sc.height)};
  rp.clearValueCount = 1;
  rp.pClearValues = &clear;
  vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);

  VkViewport viewport{};
  viewport.width = static_cast<float>(sc.width);
  viewport.height = static_cast<float>(sc.height);
  viewport.maxDepth = 1.f;
  VkRect2D scissor{};
  scissor.extent = rp.renderArea.extent;
  vkCmdSetViewport(cmd, 0, 1, &viewport);
  vkCmdSetScissor(cmd, 0, 1, &scissor);

  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1, &dset_, 0, nullptr);

  Mat4 view_mat = invert_rigid(pose_to_matrix(view.pose));
  Mat4 proj = projection_fov(view.fov, 0.05f, 100.f);
  Mat4 model = Mat4::identity();

  if (mode_ == ProjectionMode::Flat) {
    // FOV defines horizontal half-extent; height follows content aspect.
    const float half_w =
        std::tan(flat_fov_deg_ * 0.5f * static_cast<float>(M_PI) / 180.f) * distance_;
    const float aspect = std::max(0.1f, content_aspect_);
    const float half_h = half_w / aspect;
    // Mesh is built as half-size (aspect×1) with aspect=16/9.
    constexpr float mesh_half_w = 16.f / 9.f;
    constexpr float mesh_half_h = 1.f;
    model.m[0] = half_w / mesh_half_w;
    model.m[5] = half_h / mesh_half_h;
    model.m[14] = -distance_;
  }

  PushConstants pc{};
  pc.mvp = Mat4::mul(proj, Mat4::mul(view_mat, model));
  pc.viewIndex = static_cast<int32_t>(view_index);
  pc.stereoMode = static_cast<int32_t>(stereo_);
  pc.is180 = (mode_ == ProjectionMode::Deg180) ? 1 : 0;
  if (!nv12_) {
    pc.nv12 = rgba_gamma_ ? 3 : 0;  // 3 = gamma RGBA (FSR) → linearize in shader
  } else {
    pc.nv12 = nv12_full_range_ ? 2 : 1;
  }
  vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                     sizeof(pc), &pc);

  if (mode_ == ProjectionMode::Flat) {
    VkDeviceSize off = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vbo_quad_, &off);
    vkCmdDraw(cmd, 6, 1, 0, 0);
  } else {
    VkDeviceSize off = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vbo_sphere_, &off);
    vkCmdBindIndexBuffer(cmd, ibo_sphere_, 0, VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(cmd, sphere_index_count_, 1, 0, 0, 0);
  }

  // Head-locked-ish HUD: fixed in app space in front of origin (after recenter = forward).
  if (hud_visible_ && hud_dset_) {
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1, &hud_dset_, 0, nullptr);
    Mat4 hud_model = Mat4::identity();
    const float half_h = hud_half_w_ * hud_aspect_;
    hud_model.m[0] = hud_half_w_;
    hud_model.m[5] = half_h;
    hud_model.m[14] = -hud_distance_;
    PushConstants hpc{};
    hpc.mvp = Mat4::mul(proj, Mat4::mul(view_mat, hud_model));
    hpc.viewIndex = 0;
    hpc.stereoMode = 0;
    hpc.is180 = 0;
    hpc.nv12 = 0;
    vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof(hpc), &hpc);
    VkDeviceSize off = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vbo_quad_, &off);
    vkCmdDraw(cmd, 6, 1, 0, 0);
  }

  vkCmdEndRenderPass(cmd);
  vkEndCommandBuffer(cmd);
}

void SceneRenderer::render_frame(XrVulkanApp& app, const XrVulkanApp::FrameInfo& frame,
                                 std::vector<XrCompositionLayerProjectionView>& proj_views,
                                 XrCompositionLayerProjection& layer) {
  if (submit_fence_) {
    vkWaitForFences(device_, 1, &submit_fence_, VK_TRUE, 25'000'000ull);
    // If still busy, wait a bit more but never skip the eye submit after begin_frame.
    if (vkGetFenceStatus(device_, submit_fence_) != VK_SUCCESS) {
      vkWaitForFences(device_, 1, &submit_fence_, VK_TRUE, 50'000'000ull);
    }
    if (vkGetFenceStatus(device_, submit_fence_) != VK_SUCCESS) {
      vkWaitForFences(device_, 1, &submit_fence_, VK_TRUE, UINT64_MAX);
    }
    vkResetFences(device_, 1, &submit_fence_);
  }

  proj_views.resize(frame.view_count);
  auto& swapchains = app.swapchains();
  std::array<uint32_t, 2> image_indices{};

  for (uint32_t i = 0; i < frame.view_count; ++i) {
    auto& sc = swapchains[i];
    XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    VRP_CHECK(XR_SUCCEEDED(xrAcquireSwapchainImage(sc.handle, &ai, &image_indices[i])), "acquire");
    XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = XR_INFINITE_DURATION;
    VRP_CHECK(XR_SUCCEEDED(xrWaitSwapchainImage(sc.handle, &wi)), "wait");
  }

  for (uint32_t i = 0; i < frame.view_count; ++i) {
    draw_view(i, frame.views[i], swapchains[i], image_indices[i], cmds_[i]);
  }

  // One submit for both eyes. Do NOT vkQueueWaitIdle here — that misses HMD
  // predictedDisplayTime and causes mid-frame tearing / reprojection artifacts.
  VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  si.commandBufferCount = frame.view_count;
  si.pCommandBuffers = cmds_.data();
  {
    std::lock_guard<std::mutex> qlock(app.queue_mutex());
    VRP_CHECK(vkQueueSubmit(app.queue(), 1, &si, submit_fence_) == VK_SUCCESS, "eye submit");
  }

  for (uint32_t i = 0; i < frame.view_count; ++i) {
    XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    VRP_CHECK(XR_SUCCEEDED(xrReleaseSwapchainImage(swapchains[i].handle, &ri)), "release");

    proj_views[i] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
    proj_views[i].pose = frame.views[i].pose;
    proj_views[i].fov = frame.views[i].fov;
    proj_views[i].subImage.swapchain = swapchains[i].handle;
    proj_views[i].subImage.imageRect.offset = {0, 0};
    proj_views[i].subImage.imageRect.extent = {swapchains[i].width, swapchains[i].height};
  }

  layer = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
  layer.space = app.app_space();
  layer.viewCount = static_cast<uint32_t>(proj_views.size());
  layer.views = proj_views.data();
}

}  // namespace vrp
