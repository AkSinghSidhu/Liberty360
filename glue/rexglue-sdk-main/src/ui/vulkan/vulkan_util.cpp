/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <atomic>
#include <cstdint>
#include <string>

#include <rex/assert.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/ui/vulkan/device.h>
#include <rex/ui/vulkan/util.h>

namespace rex {
namespace ui {
namespace vulkan {
namespace util {

void FlushMappedMemoryRange(const VulkanDevice* const vulkan_device, const VkDeviceMemory memory,
                            const uint32_t memory_type, const VkDeviceSize offset,
                            const VkDeviceSize memory_size, const VkDeviceSize size) {
  assert_false(size != VK_WHOLE_SIZE && memory_size == VK_WHOLE_SIZE);
  assert_true(memory_size == VK_WHOLE_SIZE || offset <= memory_size);
  assert_true(memory_size == VK_WHOLE_SIZE || size <= memory_size - offset);
  if (!size || (vulkan_device->memory_types().host_coherent & (uint32_t(1) << memory_type))) {
    return;
  }
  VkMappedMemoryRange range;
  range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
  range.pNext = nullptr;
  range.memory = memory;
  range.offset = offset;
  range.size = size;
  const VkDeviceSize non_coherent_atom_size = vulkan_device->properties().nonCoherentAtomSize;
  range.offset = offset / non_coherent_atom_size * non_coherent_atom_size;
  if (size != VK_WHOLE_SIZE) {
    range.size =
        std::min(rex::round_up(offset + size, non_coherent_atom_size), memory_size) - range.offset;
  }
  vulkan_device->functions().vkFlushMappedMemoryRanges(vulkan_device->device(), 1, &range);
}

bool CreateDedicatedAllocationBuffer(const VulkanDevice* const vulkan_device,
                                     const VkDeviceSize size, const VkBufferUsageFlags usage,
                                     const MemoryPurpose memory_purpose, VkBuffer& buffer_out,
                                     VkDeviceMemory& memory_out, uint32_t* const memory_type_out,
                                     VkDeviceSize* const memory_size_out) {
  const VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  VkBufferCreateInfo buffer_create_info;
  buffer_create_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  buffer_create_info.pNext = nullptr;
  buffer_create_info.flags = 0;
  buffer_create_info.size = size;
  buffer_create_info.usage = usage;
  buffer_create_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  buffer_create_info.queueFamilyIndexCount = 0;
  buffer_create_info.pQueueFamilyIndices = nullptr;
  VkBuffer buffer;
  if (dfn.vkCreateBuffer(device, &buffer_create_info, nullptr, &buffer) != VK_SUCCESS) {
    return false;
  }

  VkMemoryRequirements memory_requirements;
  dfn.vkGetBufferMemoryRequirements(device, buffer, &memory_requirements);
  uint32_t memory_type = ChooseMemoryType(vulkan_device->memory_types(),
                                          memory_requirements.memoryTypeBits, memory_purpose);
  if (memory_type == UINT32_MAX) {
    dfn.vkDestroyBuffer(device, buffer, nullptr);
    return false;
  }

  VkMemoryAllocateInfo memory_allocate_info;
  memory_allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  memory_allocate_info.pNext = nullptr;
  memory_allocate_info.allocationSize = memory_requirements.size;
  memory_allocate_info.memoryTypeIndex = memory_type;
  VkMemoryDedicatedAllocateInfo memory_dedicated_allocate_info;
  if (vulkan_device->extensions().ext_1_1_KHR_dedicated_allocation) {
    memory_dedicated_allocate_info.pNext = memory_allocate_info.pNext;
    memory_allocate_info.pNext = &memory_dedicated_allocate_info;
    memory_dedicated_allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
    memory_dedicated_allocate_info.pNext = nullptr;
    memory_dedicated_allocate_info.image = VK_NULL_HANDLE;
    memory_dedicated_allocate_info.buffer = buffer;
  }
  VkDeviceMemory memory;
  if (dfn.vkAllocateMemory(device, &memory_allocate_info, nullptr, &memory) != VK_SUCCESS) {
    dfn.vkDestroyBuffer(device, buffer, nullptr);
    return false;
  }

  if (dfn.vkBindBufferMemory(device, buffer, memory, 0) != VK_SUCCESS) {
    dfn.vkDestroyBuffer(device, buffer, nullptr);
    dfn.vkFreeMemory(device, memory, nullptr);
    return false;
  }

  buffer_out = buffer;
  memory_out = memory;
  if (memory_type_out) {
    *memory_type_out = memory_type;
  }
  if (memory_size_out) {
    *memory_size_out = memory_allocate_info.allocationSize;
  }
  return true;
}

namespace {

const char* VulkanResultText(VkResult result) {
  switch (result) {
    case VK_SUCCESS:
      return "success";
    case VK_ERROR_OUT_OF_HOST_MEMORY:
      return "out-of-host-memory";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY:
      return "out-of-device-memory";
    case VK_ERROR_TOO_MANY_OBJECTS:
      return "too-many-objects";
    case VK_ERROR_FORMAT_NOT_SUPPORTED:
      return "format-not-supported";
    case VK_ERROR_FEATURE_NOT_PRESENT:
      return "feature-not-present";
    case VK_ERROR_INITIALIZATION_FAILED:
      return "initialization-failed";
    case VK_ERROR_UNKNOWN:
      return "unknown";
    default:
      return "other";
  }
}

std::atomic<uint64_t>& ImageAllocationFailureCount() {
  static std::atomic<uint64_t> value{0};
  return value;
}

std::atomic<uint32_t>& ImageFailureProbeCount() {
  static std::atomic<uint32_t> value{0};
  return value;
}

// Scene surfaces failing to allocate used to be silent (the caller only saw a
// boolean), which made "image-allocation" rejections undiagnosable. Dump the
// driver's memory topology once, then log the exact failing step and result.
void LogDeviceMemoryLimitsOnce(const VulkanDevice* vulkan_device) {
  static std::atomic<bool> logged{false};
  if (logged.exchange(true, std::memory_order_relaxed)) {
    return;
  }
  const auto& instance_functions = vulkan_device->vulkan_instance()->functions();
  VkPhysicalDeviceProperties properties{};
  instance_functions.vkGetPhysicalDeviceProperties(vulkan_device->physical_device(), &properties);
  VkPhysicalDeviceMemoryProperties memory_properties{};
  instance_functions.vkGetPhysicalDeviceMemoryProperties(vulkan_device->physical_device(),
                                                         &memory_properties);
  std::string heaps;
  for (uint32_t i = 0; i < memory_properties.memoryHeapCount; ++i) {
    heaps += (heaps.empty() ? "" : ",");
    heaps += std::to_string(i) + ":" +
             std::to_string(uint64_t(memory_properties.memoryHeaps[i].size >> 20)) + "MB:flags" +
             std::to_string(uint32_t(memory_properties.memoryHeaps[i].flags));
  }
  std::string types;
  for (uint32_t i = 0; i < memory_properties.memoryTypeCount; ++i) {
    types += (types.empty() ? "" : ",");
    types += std::to_string(i) + ":heap" +
             std::to_string(uint32_t(memory_properties.memoryTypes[i].heapIndex)) + ":flags" +
             std::to_string(uint32_t(memory_properties.memoryTypes[i].propertyFlags));
  }
  REXLOG_WARN(
      "vulkan-util: image-failure-device-limits maxMemoryAllocationCount={} "
      "maxImageDimension2D={} heaps=[{}] memory-types=[{}]",
      properties.limits.maxMemoryAllocationCount, properties.limits.maxImageDimension2D, heaps,
      types);
}

// Distinguishes failure causes the caller cannot see: a plain retry success
// means a transient failure, an aligned-extent success points at row-pitch
// alignment, a linear-tiling success at tiling constraints, a plain
// allocation success at the dedicated-memory path, and a driver memory result
// at genuine exhaustion or the allocation-count cap.
void ProbeImageCreationFailure(const VulkanDevice* vulkan_device,
                               const VkImageCreateInfo& create_info,
                               const VkMemoryRequirements* memory_requirements,
                               uint32_t memory_type) {
  if (ImageFailureProbeCount().fetch_add(1, std::memory_order_relaxed) >= 40) {
    return;
  }
  const VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  auto try_create = [&](const VkImageCreateInfo& info) {
    VkImage probe_image = VK_NULL_HANDLE;
    const VkResult result = dfn.vkCreateImage(device, &info, nullptr, &probe_image);
    if (result == VK_SUCCESS && probe_image != VK_NULL_HANDLE) {
      dfn.vkDestroyImage(device, probe_image, nullptr);
    }
    return result;
  };
  const VkResult retry_result = try_create(create_info);
  if (retry_result == VK_SUCCESS) {
    REXLOG_WARN(
        "vulkan-util: image-failure-probe plain-retry succeeded (transient) format={} "
        "extent={}x{} samples={} usage={:08X}",
        uint32_t(create_info.format), create_info.extent.width, create_info.extent.height,
        uint32_t(create_info.samples), create_info.usage);
    return;
  }
  VkImageCreateInfo probe_info = create_info;
  probe_info.extent.width = (create_info.extent.width + 3u) & ~3u;
  probe_info.extent.height = (create_info.extent.height + 3u) & ~3u;
  const VkResult aligned_result = try_create(probe_info);
  probe_info = create_info;
  probe_info.tiling = VK_IMAGE_TILING_LINEAR;
  const VkResult linear_result = try_create(probe_info);
  probe_info = create_info;
  probe_info.usage =
      create_info.usage &
      (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);
  const VkResult minimal_usage_result = try_create(probe_info);
  VkResult plain_allocate_result = VK_ERROR_FEATURE_NOT_PRESENT;
  VkDeviceSize plain_allocate_size = 0;
  if (memory_requirements && memory_type != UINT32_MAX) {
    VkMemoryAllocateInfo allocate_info{};
    allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocate_info.pNext = nullptr;
    allocate_info.allocationSize = memory_requirements->size;
    allocate_info.memoryTypeIndex = memory_type;
    plain_allocate_size = memory_requirements->size;
    VkDeviceMemory probe_memory = VK_NULL_HANDLE;
    plain_allocate_result = dfn.vkAllocateMemory(device, &allocate_info, nullptr, &probe_memory);
    if (plain_allocate_result == VK_SUCCESS && probe_memory != VK_NULL_HANDLE) {
      dfn.vkFreeMemory(device, probe_memory, nullptr);
    }
  }
  REXLOG_WARN(
      "vulkan-util: image-failure-probe format={} extent={}x{} samples={} usage={:08X} "
      "retry={}({}) aligned-extent={}({}) linear-tiling={}({}) minimal-usage={}({}) "
      "plain-alloc={}({}) alloc-size={} memory-type={}",
      uint32_t(create_info.format), create_info.extent.width, create_info.extent.height,
      uint32_t(create_info.samples), create_info.usage, VulkanResultText(retry_result),
      int32_t(retry_result), VulkanResultText(aligned_result), int32_t(aligned_result),
      VulkanResultText(linear_result), int32_t(linear_result),
      VulkanResultText(minimal_usage_result), int32_t(minimal_usage_result),
      VulkanResultText(plain_allocate_result), int32_t(plain_allocate_result),
      uint64_t(plain_allocate_size), memory_type);
}

void LogImageAllocationFailure(const VulkanDevice* vulkan_device, const char* step,
                               VkResult result, const VkImageCreateInfo& create_info,
                               uint32_t memory_type, VkDeviceSize memory_size,
                               const VkMemoryRequirements* memory_requirements) {
  LogDeviceMemoryLimitsOnce(vulkan_device);
  const uint64_t count = ImageAllocationFailureCount().fetch_add(1, std::memory_order_relaxed) + 1;
  if (count <= 32 || !(count % 1024)) {
    REXLOG_WARN(
        "vulkan-util: image allocation failed #{} step={} result={}({}) format={} extent={}x{} "
        "samples={} usage={:08X} tiling={} mips={} layers={} create-flags={:08X} memory-type={} "
        "memory-size={}",
        count, step, VulkanResultText(result), int32_t(result), uint32_t(create_info.format),
        create_info.extent.width, create_info.extent.height, uint32_t(create_info.samples),
        create_info.usage, uint32_t(create_info.tiling), create_info.mipLevels,
        create_info.arrayLayers, create_info.flags, memory_type, uint64_t(memory_size));
  }
  if (count <= 8 || !(count % 4096)) {
    ProbeImageCreationFailure(vulkan_device, create_info, memory_requirements, memory_type);
  }
}

}  // namespace

bool CreateDedicatedAllocationImage(const VulkanDevice* const vulkan_device,
                                    const VkImageCreateInfo& create_info,
                                    const MemoryPurpose memory_purpose, VkImage& image_out,
                                    VkDeviceMemory& memory_out, uint32_t* const memory_type_out,
                                    VkDeviceSize* const memory_size_out) {
  // Legacy image-format queries describe ordinary images exactly. Extended
  // creation chains (external memory, format lists, etc.) require their own
  // matching properties2 query and continue through the existing creation path.
  VkResult support_result = VK_SUCCESS;
  if (!create_info.pNext &&
      !IsImageCreateInfoSupported(vulkan_device, create_info, &support_result)) {
    LogImageAllocationFailure(vulkan_device, "support-query", support_result, create_info,
                              UINT32_MAX, 0, nullptr);
    return false;
  }
  const VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  VkImage image;
  const VkResult create_image_result = dfn.vkCreateImage(device, &create_info, nullptr, &image);
  if (create_image_result != VK_SUCCESS) {
    LogImageAllocationFailure(vulkan_device, "create-image", create_image_result, create_info,
                              UINT32_MAX, 0, nullptr);
    return false;
  }

  VkMemoryRequirements memory_requirements;
  dfn.vkGetImageMemoryRequirements(device, image, &memory_requirements);
  uint32_t memory_type = ChooseMemoryType(vulkan_device->memory_types(),
                                          memory_requirements.memoryTypeBits, memory_purpose);
  if (memory_type == UINT32_MAX) {
    dfn.vkDestroyImage(device, image, nullptr);
    LogImageAllocationFailure(vulkan_device, "choose-memory-type", VK_ERROR_UNKNOWN, create_info,
                              UINT32_MAX, memory_requirements.size, &memory_requirements);
    return false;
  }

  VkMemoryAllocateInfo memory_allocate_info;
  memory_allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  memory_allocate_info.pNext = nullptr;
  memory_allocate_info.allocationSize = memory_requirements.size;
  memory_allocate_info.memoryTypeIndex = memory_type;
  VkMemoryDedicatedAllocateInfo memory_dedicated_allocate_info;
  if (vulkan_device->extensions().ext_1_1_KHR_dedicated_allocation) {
    memory_dedicated_allocate_info.pNext = memory_allocate_info.pNext;
    memory_allocate_info.pNext = &memory_dedicated_allocate_info;
    memory_dedicated_allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
    memory_dedicated_allocate_info.pNext = nullptr;
    memory_dedicated_allocate_info.image = image;
    memory_dedicated_allocate_info.buffer = VK_NULL_HANDLE;
  }
  VkDeviceMemory memory;
  const VkResult allocate_result =
      dfn.vkAllocateMemory(device, &memory_allocate_info, nullptr, &memory);
  if (allocate_result != VK_SUCCESS) {
    dfn.vkDestroyImage(device, image, nullptr);
    LogImageAllocationFailure(vulkan_device, "allocate-memory", allocate_result, create_info,
                              memory_type, memory_requirements.size, &memory_requirements);
    return false;
  }

  const VkResult bind_result = dfn.vkBindImageMemory(device, image, memory, 0);
  if (bind_result != VK_SUCCESS) {
    dfn.vkDestroyImage(device, image, nullptr);
    dfn.vkFreeMemory(device, memory, nullptr);
    LogImageAllocationFailure(vulkan_device, "bind-memory", bind_result, create_info, memory_type,
                              memory_requirements.size, &memory_requirements);
    return false;
  }

  image_out = image;
  memory_out = memory;
  if (memory_type_out) {
    *memory_type_out = memory_type;
  }
  if (memory_size_out) {
    *memory_size_out = memory_allocate_info.allocationSize;
  }
  return true;
}

VkSampleCountFlags QueryImageFormatSampleCounts(const VulkanDevice* vulkan_device,
                                                const VkFormat format,
                                                const VkImageType image_type,
                                                const VkImageTiling tiling,
                                                const VkImageUsageFlags usage,
                                                const VkImageCreateFlags flags) {
  if (!vulkan_device) {
    return 0;
  }
  VkImageFormatProperties properties{};
  const VkResult result =
      vulkan_device->vulkan_instance()->functions().vkGetPhysicalDeviceImageFormatProperties(
          vulkan_device->physical_device(), format, image_type, tiling, usage, flags,
          &properties);
  return result == VK_SUCCESS ? properties.sampleCounts : 0;
}

bool IsImageCreateInfoSupported(const VulkanDevice* vulkan_device,
                                const VkImageCreateInfo& create_info,
                                VkResult* const failure_result) {
  if (!vulkan_device || create_info.pNext) {
    if (failure_result) {
      *failure_result = VK_ERROR_FEATURE_NOT_PRESENT;
    }
    return false;
  }
  VkImageFormatProperties properties{};
  const VkResult query_result =
      vulkan_device->vulkan_instance()->functions().vkGetPhysicalDeviceImageFormatProperties(
          vulkan_device->physical_device(), create_info.format, create_info.imageType,
          create_info.tiling, create_info.usage, create_info.flags, &properties);
  if (query_result != VK_SUCCESS) {
    if (failure_result) {
      *failure_result = query_result;
    }
    return false;
  }
  const bool supported =
      create_info.extent.width && create_info.extent.height && create_info.extent.depth &&
      create_info.extent.width <= properties.maxExtent.width &&
      create_info.extent.height <= properties.maxExtent.height &&
      create_info.extent.depth <= properties.maxExtent.depth && create_info.mipLevels &&
      create_info.mipLevels <= properties.maxMipLevels && create_info.arrayLayers &&
      create_info.arrayLayers <= properties.maxArrayLayers &&
      (properties.sampleCounts & create_info.samples) != 0;
  if (!supported) {
    // The query answered, so failure_result alone cannot say which reported
    // limit was exceeded - name the failing predicate and value explicitly.
    static std::atomic<uint64_t> support_rejection_count{0};
    const uint64_t rejection_count =
        support_rejection_count.fetch_add(1, std::memory_order_relaxed) + 1;
    if (rejection_count <= 16 || !(rejection_count % 1024)) {
      const char* predicate = "sample-count";
      if (!create_info.extent.width || !create_info.extent.height || !create_info.extent.depth) {
        predicate = "zero-extent";
      } else if (create_info.extent.width > properties.maxExtent.width ||
                 create_info.extent.height > properties.maxExtent.height ||
                 create_info.extent.depth > properties.maxExtent.depth) {
        predicate = "extent-limit";
      } else if (!create_info.mipLevels || create_info.mipLevels > properties.maxMipLevels) {
        predicate = "mip-limit";
      } else if (!create_info.arrayLayers ||
                 create_info.arrayLayers > properties.maxArrayLayers) {
        predicate = "layer-limit";
      }
      REXLOG_WARN(
          "vulkan-util: support-check rejected #{} predicate={} format={} extent={}x{}x{} "
          "mips={} layers={} samples={:X} usage={:08X} tiling={} create-flags={:08X} driver "
          "maxExtent={}x{}x{} maxMips={} maxLayers={} sampleCounts={:X}",
          rejection_count, predicate, uint32_t(create_info.format), create_info.extent.width,
          create_info.extent.height, create_info.extent.depth, create_info.mipLevels,
          create_info.arrayLayers, uint32_t(create_info.samples), create_info.usage,
          uint32_t(create_info.tiling), create_info.flags, properties.maxExtent.width,
          properties.maxExtent.height, properties.maxExtent.depth, properties.maxMipLevels,
          properties.maxArrayLayers, uint32_t(properties.sampleCounts));
    }
  }
  if (failure_result) {
    *failure_result = supported ? VK_SUCCESS : VK_ERROR_UNKNOWN;
  }
  return supported;
}

VkPipeline CreateComputePipeline(const VulkanDevice* const vulkan_device,
                                 const VkPipelineLayout layout, const VkShaderModule shader,
                                 const VkSpecializationInfo* const specialization_info,
                                 const char* const entry_point) {
  VkComputePipelineCreateInfo pipeline_create_info;
  pipeline_create_info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
  pipeline_create_info.pNext = nullptr;
  pipeline_create_info.flags = 0;
  pipeline_create_info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  pipeline_create_info.stage.pNext = nullptr;
  pipeline_create_info.stage.flags = 0;
  pipeline_create_info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
  pipeline_create_info.stage.module = shader;
  pipeline_create_info.stage.pName = entry_point;
  pipeline_create_info.stage.pSpecializationInfo = specialization_info;
  pipeline_create_info.layout = layout;
  pipeline_create_info.basePipelineHandle = VK_NULL_HANDLE;
  pipeline_create_info.basePipelineIndex = -1;
  VkPipeline pipeline;
  if (vulkan_device->functions().vkCreateComputePipelines(vulkan_device->device(), VK_NULL_HANDLE,
                                                          1, &pipeline_create_info, nullptr,
                                                          &pipeline) != VK_SUCCESS) {
    return VK_NULL_HANDLE;
  }
  return pipeline;
}

VkPipeline CreateComputePipeline(const VulkanDevice* const vulkan_device, VkPipelineLayout layout,
                                 const uint32_t* shader_code, size_t shader_code_size_bytes,
                                 const VkSpecializationInfo* specialization_info,
                                 const char* entry_point) {
  const VkShaderModule shader =
      CreateShaderModule(vulkan_device, shader_code, shader_code_size_bytes);
  if (shader == VK_NULL_HANDLE) {
    return VK_NULL_HANDLE;
  }
  const VkPipeline pipeline =
      CreateComputePipeline(vulkan_device, layout, shader, specialization_info, entry_point);
  vulkan_device->functions().vkDestroyShaderModule(vulkan_device->device(), shader, nullptr);
  return pipeline;
}

}  // namespace util
}  // namespace vulkan
}  // namespace ui
}  // namespace rex
