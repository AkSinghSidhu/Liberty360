// Source-built Vulkan dispatch adapter for the unchanged official game engine.
// It neither patches libmain.so nor hooks dlopen/dlsym process-wide.
#include <jni.h>
#include <android/log.h>
#include <adrenotools/driver.h>
#include <vulkan/vulkan.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include <limits.h>
#include <unistd.h>
#include <cstdio>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <ctime>
#include <vector>
#include <unordered_map>

#define EXPORT extern "C" __attribute__((visibility("default")))

EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance, const char*);
EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice, const char*);
EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkCreateInstance(const VkInstanceCreateInfo*, const VkAllocationCallbacks*, VkInstance*);
EXPORT VKAPI_ATTR void VKAPI_CALL vkDestroyInstance(VkInstance, const VkAllocationCallbacks*);
EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkCreateDevice(VkPhysicalDevice, const VkDeviceCreateInfo*, const VkAllocationCallbacks*, VkDevice*);
EXPORT VKAPI_ATTR void VKAPI_CALL vkDestroyDevice(VkDevice, const VkAllocationCallbacks*);
EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkCreateGraphicsPipelines(VkDevice, VkPipelineCache, uint32_t, const VkGraphicsPipelineCreateInfo*, const VkAllocationCallbacks*, VkPipeline*);
EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkCreateComputePipelines(VkDevice, VkPipelineCache, uint32_t, const VkComputePipelineCreateInfo*, const VkAllocationCallbacks*, VkPipeline*);
EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkCreateImage(VkDevice, const VkImageCreateInfo*, const VkAllocationCallbacks*, VkImage*);
EXPORT VKAPI_ATTR void VKAPI_CALL vkDestroyImage(VkDevice, VkImage, const VkAllocationCallbacks*);
EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkCreateImageView(VkDevice, const VkImageViewCreateInfo*, const VkAllocationCallbacks*, VkImageView*);
EXPORT VKAPI_ATTR void VKAPI_CALL vkDestroyImageView(VkDevice, VkImageView, const VkAllocationCallbacks*);
EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateDeviceExtensionProperties(VkPhysicalDevice, const char*, uint32_t*, VkExtensionProperties*);
EXPORT VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFeatures2(VkPhysicalDevice, VkPhysicalDeviceFeatures2*);
EXPORT VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFeatures2KHR(VkPhysicalDevice, VkPhysicalDeviceFeatures2KHR*);
EXPORT VKAPI_ATTR void VKAPI_CALL vkCmdBeginRendering(VkCommandBuffer, const VkRenderingInfo*);
EXPORT VKAPI_ATTR void VKAPI_CALL vkCmdBeginRenderingKHR(VkCommandBuffer, const VkRenderingInfoKHR*);
EXPORT VKAPI_ATTR void VKAPI_CALL vkCmdEndRendering(VkCommandBuffer);
EXPORT VKAPI_ATTR void VKAPI_CALL vkCmdEndRenderingKHR(VkCommandBuffer);
EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkQueuePresentKHR(VkQueue, const VkPresentInfoKHR*);
EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceVersion(uint32_t*);
EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceExtensionProperties(const char*, uint32_t*, VkExtensionProperties*);
EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceLayerProperties(uint32_t*, VkLayerProperties*);

namespace {
constexpr const char* kTag = "LibertyDriver";
constexpr uint32_t kMaxPhysicalDevices = 16;
std::mutex g_init_mutex;
std::atomic<bool> g_ready{false};
std::atomic<uint32_t> g_game_instances{0};
bool g_attempted = false;
bool g_custom = false;
// Identity of physical device 0 under the stock driver, read before a custom
// one is loaded. Empty when it could not be read.
std::string g_system_identity;
void* g_loader = nullptr; // Deliberately retained until process exit.
PFN_vkGetInstanceProcAddr g_get = nullptr;
PFN_vkDestroyInstance g_destroy = nullptr;
std::string g_arguments;
std::string g_status = "{\"ok\":false,\"error\":\"Driver bridge not initialized\"}";

VkInstance g_last_instance = VK_NULL_HANDLE;

template<typename Fn>
Fn LoaderSymbol(const char* name) {
  if (g_get && g_last_instance) {
    Fn fn = reinterpret_cast<Fn>(g_get(g_last_instance, name));
    if (fn) return fn;
  }
  if (g_get) {
    Fn fn = reinterpret_cast<Fn>(g_get(VK_NULL_HANDLE, name));
    if (fn) return fn;
  }
  return reinterpret_cast<Fn>(dlsym(g_loader, name));
}
VkDevice g_device = VK_NULL_HANDLE;
VkPipelineCache g_pipeline_cache = VK_NULL_HANDLE;
std::mutex g_cache_mutex;
std::atomic<uint32_t> g_pipelines_since_last_save{0};
std::string g_cache_file_path;

PFN_vkCreateDevice g_real_create_device = nullptr;
PFN_vkDestroyDevice g_real_destroy_device = nullptr;
PFN_vkCreateGraphicsPipelines g_real_create_graphics_pipelines = nullptr;
PFN_vkCreateComputePipelines g_real_create_compute_pipelines = nullptr;
PFN_vkQueuePresentKHR g_real_queue_present = nullptr;
PFN_vkCreatePipelineCache g_create_pipeline_cache = nullptr;
PFN_vkDestroyPipelineCache g_destroy_pipeline_cache = nullptr;
PFN_vkGetPipelineCacheData g_get_pipeline_cache_data = nullptr;

PFN_vkCreateRenderPass g_real_create_render_pass = nullptr;
PFN_vkDestroyRenderPass g_real_destroy_render_pass = nullptr;
PFN_vkCreateFramebuffer g_real_create_framebuffer = nullptr;
PFN_vkDestroyFramebuffer g_real_destroy_framebuffer = nullptr;
PFN_vkCmdBeginRenderPass g_real_cmd_begin_render_pass = nullptr;
PFN_vkCmdEndRenderPass g_real_cmd_end_render_pass = nullptr;
PFN_vkCreateImageView g_real_create_image_view = nullptr;
PFN_vkDestroyImageView g_real_destroy_image_view = nullptr;
PFN_vkCreateImage g_real_create_image = nullptr;
PFN_vkDestroyImage g_real_destroy_image = nullptr;
PFN_vkEnumerateDeviceExtensionProperties g_real_enum_device_ext_props = nullptr;
PFN_vkGetPhysicalDeviceFeatures2 g_real_get_pd_features2 = nullptr;
PFN_vkGetPhysicalDeviceFeatures2KHR g_real_get_pd_features2_khr = nullptr;

struct GenericNode {
  VkStructureType sType;
  const void* pNext;
};

struct ViewInfo {
  VkFormat format;
  VkSampleCountFlagBits samples;
};

std::mutex g_view_mutex;
std::unordered_map<VkImage, VkSampleCountFlagBits> g_image_samples;
std::unordered_map<VkImageView, ViewInfo> g_views;

struct RenderPassKey {
  uint32_t color_count;
  VkFormat color_formats[8];
  VkAttachmentLoadOp color_load_ops[8];
  VkAttachmentStoreOp color_store_ops[8];
  VkImageLayout color_layouts[8];
  VkFormat depth_format;
  VkAttachmentLoadOp depth_load_op;
  VkAttachmentStoreOp depth_store_op;
  VkImageLayout depth_layout;
  VkFormat stencil_format;
  VkAttachmentLoadOp stencil_load_op;
  VkAttachmentStoreOp stencil_store_op;
  VkImageLayout stencil_layout;
  VkSampleCountFlagBits samples;

  bool operator==(const RenderPassKey& o) const {
    return std::memcmp(this, &o, sizeof(RenderPassKey)) == 0;
  }
};

struct RenderPassKeyHash {
  size_t operator()(const RenderPassKey& k) const {
    const uint8_t* p = reinterpret_cast<const uint8_t*>(&k);
    size_t h = 14695981039346656037ULL;
    for (size_t i = 0; i < sizeof(RenderPassKey); ++i) {
      h = (h ^ p[i]) * 1099511628211ULL;
    }
    return h;
  }
};

struct FramebufferKey {
  VkRenderPass render_pass;
  uint32_t width;
  uint32_t height;
  uint32_t layers;
  uint32_t attachment_count;
  VkImageView attachments[9];

  bool operator==(const FramebufferKey& o) const {
    return std::memcmp(this, &o, sizeof(FramebufferKey)) == 0;
  }
};

struct FramebufferKeyHash {
  size_t operator()(const FramebufferKey& k) const {
    const uint8_t* p = reinterpret_cast<const uint8_t*>(&k);
    size_t h = 14695981039346656037ULL;
    for (size_t i = 0; i < sizeof(FramebufferKey); ++i) {
      h = (h ^ p[i]) * 1099511628211ULL;
    }
    return h;
  }
};

std::mutex g_rp_mutex;
std::unordered_map<RenderPassKey, VkRenderPass, RenderPassKeyHash> g_render_passes;
std::unordered_map<FramebufferKey, VkFramebuffer, FramebufferKeyHash> g_framebuffers;

void InvalidateFramebuffersWithViewLocked(VkDevice device, VkImageView imageView) {
  std::lock_guard<std::mutex> lock(g_rp_mutex);
  const auto destroy_fb = g_real_destroy_framebuffer ? g_real_destroy_framebuffer :
      LoaderSymbol<PFN_vkDestroyFramebuffer>("vkDestroyFramebuffer");
  for (auto it = g_framebuffers.begin(); it != g_framebuffers.end(); ) {
    bool uses_view = false;
    for (uint32_t i = 0; i < it->first.attachment_count; ++i) {
      if (it->first.attachments[i] == imageView) {
        uses_view = true;
        break;
      }
    }
    if (uses_view) {
      if (destroy_fb && it->second != VK_NULL_HANDLE) {
        destroy_fb(device, it->second, nullptr);
      }
      it = g_framebuffers.erase(it);
    } else {
      ++it;
    }
  }
}

void CleanupDynamicRendering(VkDevice device) {
  std::lock_guard<std::mutex> lock(g_rp_mutex);
  const auto destroy_fb = g_real_destroy_framebuffer ? g_real_destroy_framebuffer :
      LoaderSymbol<PFN_vkDestroyFramebuffer>("vkDestroyFramebuffer");
  for (const auto& pair : g_framebuffers) {
    if (destroy_fb && pair.second != VK_NULL_HANDLE) {
      destroy_fb(device, pair.second, nullptr);
    }
  }
  g_framebuffers.clear();

  const auto destroy_rp = g_real_destroy_render_pass ? g_real_destroy_render_pass :
      LoaderSymbol<PFN_vkDestroyRenderPass>("vkDestroyRenderPass");
  for (const auto& pair : g_render_passes) {
    if (destroy_rp && pair.second != VK_NULL_HANDLE) {
      destroy_rp(device, pair.second, nullptr);
    }
  }
  g_render_passes.clear();

  std::lock_guard<std::mutex> view_lock(g_view_mutex);
  g_views.clear();
  g_image_samples.clear();
}

VkRenderPass GetOrCreateRenderPass(VkDevice device, const RenderPassKey& key) {
  std::lock_guard<std::mutex> lock(g_rp_mutex);
  auto it = g_render_passes.find(key);
  if (it != g_render_passes.end()) {
    return it->second;
  }

  std::vector<VkAttachmentDescription> attachments;
  std::vector<VkAttachmentReference> color_refs;
  VkAttachmentReference depth_ref{};
  bool has_depth_or_stencil = false;

  for (uint32_t i = 0; i < key.color_count; ++i) {
    if (key.color_formats[i] == VK_FORMAT_UNDEFINED) {
      VkAttachmentReference ref{};
      ref.attachment = VK_ATTACHMENT_UNUSED;
      ref.layout = VK_IMAGE_LAYOUT_UNDEFINED;
      color_refs.push_back(ref);
      continue;
    }
    VkAttachmentDescription desc{};
    desc.format = key.color_formats[i];
    desc.samples = key.samples ? key.samples : VK_SAMPLE_COUNT_1_BIT;
    desc.loadOp = key.color_load_ops[i];
    desc.storeOp = key.color_store_ops[i];
    desc.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    desc.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    desc.initialLayout = key.color_layouts[i] != VK_IMAGE_LAYOUT_UNDEFINED ?
        key.color_layouts[i] : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    desc.finalLayout = key.color_layouts[i] != VK_IMAGE_LAYOUT_UNDEFINED ?
        key.color_layouts[i] : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkAttachmentReference ref{};
    ref.attachment = static_cast<uint32_t>(attachments.size());
    ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color_refs.push_back(ref);
    attachments.push_back(desc);
  }

  VkFormat ds_format = key.depth_format != VK_FORMAT_UNDEFINED ?
      key.depth_format : key.stencil_format;
  if (ds_format != VK_FORMAT_UNDEFINED) {
    has_depth_or_stencil = true;
    VkAttachmentDescription desc{};
    desc.format = ds_format;
    desc.samples = key.samples ? key.samples : VK_SAMPLE_COUNT_1_BIT;
    desc.loadOp = key.depth_load_op;
    desc.storeOp = key.depth_store_op;
    desc.stencilLoadOp = key.stencil_load_op;
    desc.stencilStoreOp = key.stencil_store_op;
    desc.initialLayout = key.depth_layout != VK_IMAGE_LAYOUT_UNDEFINED ?
        key.depth_layout : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    desc.finalLayout = key.depth_layout != VK_IMAGE_LAYOUT_UNDEFINED ?
        key.depth_layout : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    depth_ref.attachment = static_cast<uint32_t>(attachments.size());
    depth_ref.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    attachments.push_back(desc);
  }

  VkSubpassDescription subpass{};
  subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  subpass.colorAttachmentCount = static_cast<uint32_t>(color_refs.size());
  subpass.pColorAttachments = color_refs.empty() ? nullptr : color_refs.data();
  subpass.pDepthStencilAttachment = has_depth_or_stencil ? &depth_ref : nullptr;

  VkRenderPassCreateInfo rp_info{};
  rp_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
  rp_info.attachmentCount = static_cast<uint32_t>(attachments.size());
  rp_info.pAttachments = attachments.empty() ? nullptr : attachments.data();
  rp_info.subpassCount = 1;
  rp_info.pSubpasses = &subpass;

  VkRenderPass rp = VK_NULL_HANDLE;
  const auto create_rp = g_real_create_render_pass ? g_real_create_render_pass :
      LoaderSymbol<PFN_vkCreateRenderPass>("vkCreateRenderPass");
  if (!create_rp || create_rp(device, &rp_info, nullptr, &rp) != VK_SUCCESS) {
    __android_log_print(ANDROID_LOG_ERROR, kTag, "Failed to create emulated VkRenderPass");
    return VK_NULL_HANDLE;
  }

  g_render_passes[key] = rp;
  return rp;
}

std::string Quote(const std::string& value) {
  std::string out = "\"";
  for (unsigned char c : value) {
    switch (c) {
      case '\\': out += "\\\\"; break;
      case '"': out += "\\\""; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          constexpr char hex[] = "0123456789abcdef";
          out += "\\u00";
          out += hex[c >> 4];
          out += hex[c & 15];
        } else {
          out += static_cast<char>(c);
        }
    }
  }
  return out + "\"";
}

std::string JavaString(JNIEnv* env, jstring value) {
  if (!value) return {};
  const char* bytes = env->GetStringUTFChars(value, nullptr);
  if (!bytes) return {};
  std::string result(bytes);
  env->ReleaseStringUTFChars(value, bytes);
  return result;
}

std::string CanonicalDirectory(const std::string& value) {
  char resolved[PATH_MAX];
  struct stat st{};
  if (value.empty() || value.front() != '/' || !realpath(value.c_str(), resolved) ||
      stat(resolved, &st) != 0 || !S_ISDIR(st.st_mode)) return {};
  return std::string(resolved) + "/";
}

bool RegularFile(const std::string& path) {
  struct stat st{};
  return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

std::string LoaderPath() {
  Dl_info info{};
  if (g_get && dladdr(reinterpret_cast<void*>(g_get), &info) && info.dli_fname)
    return info.dli_fname;
  return {};
}

std::string Failure(const std::string& why) {
  __android_log_print(ANDROID_LOG_ERROR, kTag, "%s", why.c_str());
  return "{\"ok\":false,\"mode\":" + Quote(g_custom ? "turnip" : "system") +
         ",\"error\":" + Quote(why) + ",\"loader_path\":" + Quote(LoaderPath()) + "}";
}

template<typename Fn>
Fn Get(VkInstance instance, const char* name) {
  return reinterpret_cast<Fn>(g_get(instance, name));
}

std::string GetCachePath() {
  const char* xdg = getenv("XDG_DATA_HOME");
  if (xdg && *xdg) {
    std::string candidate = std::string(xdg) + "/LibertyRecomp";
    struct stat st{};
    if (stat(candidate.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
      return candidate + "/xenos_vk_pipeline_cache_v2.bin";
    }
    return std::string(xdg) + "/xenos_vk_pipeline_cache_v2.bin";
  }
  const char* home = getenv("HOME");
  if (home && *home) {
    std::string candidate = std::string(home) + "/LibertyRecomp";
    struct stat st{};
    if (stat(candidate.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
      return candidate + "/xenos_vk_pipeline_cache_v2.bin";
    }
    return std::string(home) + "/xenos_vk_pipeline_cache_v2.bin";
  }
  return "/sdcard/Android/data/com.libertyrecomp/files/LibertyRecomp/xenos_vk_pipeline_cache_v2.bin";
}

void SavePipelineCache(VkDevice device) {
  if (device == VK_NULL_HANDLE) return;
  std::lock_guard<std::mutex> lock(g_cache_mutex);
  if (g_pipeline_cache == VK_NULL_HANDLE || !g_get_pipeline_cache_data || g_cache_file_path.empty()) {
    return;
  }

  size_t data_size = 0;
  VkResult res = g_get_pipeline_cache_data(device, g_pipeline_cache, &data_size, nullptr);
  if (res != VK_SUCCESS || data_size <= sizeof(VkPipelineCacheHeaderVersionOne)) {
    return;
  }

  std::vector<uint8_t> data(data_size);
  res = g_get_pipeline_cache_data(device, g_pipeline_cache, &data_size, data.data());
  if (res != VK_SUCCESS) return;

  std::string tmp_path = g_cache_file_path + ".tmp";
  FILE* fp = fopen(tmp_path.c_str(), "wb");
  if (!fp) return;
  size_t written = fwrite(data.data(), 1, data_size, fp);
  fclose(fp);
  if (written == data_size) {
    rename(tmp_path.c_str(), g_cache_file_path.c_str());
    g_pipelines_since_last_save.store(0, std::memory_order_relaxed);
    __android_log_print(ANDROID_LOG_INFO, kTag,
        "Saved Vulkan persistent pipeline cache (%zu bytes) to %s",
        data_size, g_cache_file_path.c_str());
  } else {
    unlink(tmp_path.c_str());
  }
}

static std::atomic<uint64_t> s_last_save_time_ms{0};
static std::atomic<bool> s_saving{false};

void AsyncSavePipelineCache(VkDevice device) {
  if (device == VK_NULL_HANDLE || s_saving.exchange(true)) return;
  std::thread([device]() {
    SavePipelineCache(device);
    s_saving.store(false);
  }).detach();
}

// What makes one driver distinguishable from another.
//
// Not the GPU name alone - that is the same physical chip either way - and not
// apiVersion, which two builds of the same driver family often share.
// driverVersion, driverName and driverInfo are what actually move between a
// stock driver and one taken from somewhere else.
std::string DeviceIdentity(const VkPhysicalDeviceProperties& basic,
                           const VkPhysicalDeviceDriverProperties& driver) {
  return std::to_string(driver.driverID) + "|" + std::to_string(basic.driverVersion) + "|" +
         std::string(driver.driverName) + "|" + std::string(driver.driverInfo) + "|" +
         std::string(basic.deviceName);
}

// Reads the stock driver's identity, using its own handle and nothing global.
//
// This is what makes supporting a non-Turnip driver possible at all.
// libadrenotools is documented to return a valid pointer and then quietly fall
// back to the original driver when its hook does not take, so something has to
// prove the swap really happened. For Turnip the driver ID alone proves it -
// the stock Adreno driver can never report Mesa. A custom QUALCOMM driver
// reports exactly what the stock one reports, so the ID proves nothing, and
// the only honest evidence is that the identity CHANGED.
// The apiVersion to ask an instance for. The platform loader of Android 10 to
// 12 reports Vulkan 1.1 even when the driver behind it (Turnip on an Adreno
// 6xx) offers 1.3; a 1.1 loader accepts a higher apiVersion, and the device
// decides which 1.2 functionality exists. So 1.2 is requested from any 1.1+
// loader, which is what the game's own instance does as well.
uint32_t ProbeApiVersion(uint32_t loader_api) {
  return loader_api >= VK_API_VERSION_1_1 ? VK_API_VERSION_1_2 : VK_API_VERSION_1_0;
}

bool ProbeSystemIdentity(std::string& identity) {
  void* handle = dlopen("/system/lib64/libvulkan.so", RTLD_NOW | RTLD_LOCAL);
  if (!handle) return false;
  const auto get = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
      dlsym(handle, "vkGetInstanceProcAddr"));
  if (!get) { dlclose(handle); return false; }
  const auto create = reinterpret_cast<PFN_vkCreateInstance>(get(VK_NULL_HANDLE, "vkCreateInstance"));
  const auto enumerate_version = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(
      get(VK_NULL_HANDLE, "vkEnumerateInstanceVersion"));
  if (!create) { dlclose(handle); return false; }
  uint32_t api = VK_API_VERSION_1_0;
  if (enumerate_version && enumerate_version(&api) != VK_SUCCESS) api = VK_API_VERSION_1_0;
  api = ProbeApiVersion(api);

  VkApplicationInfo app{};
  app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  app.pApplicationName = "Liberty System Driver Probe";
  app.apiVersion = api;
  VkInstanceCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  info.pApplicationInfo = &app;
  VkInstance instance = VK_NULL_HANDLE;
  if (create(&info, nullptr, &instance) != VK_SUCCESS || instance == VK_NULL_HANDLE) {
    dlclose(handle);
    return false;
  }

  bool found = false;
  const auto enumerate = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(
      get(instance, "vkEnumeratePhysicalDevices"));
  const auto properties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(
      get(instance, "vkGetPhysicalDeviceProperties"));
  const auto properties2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
      get(instance, "vkGetPhysicalDeviceProperties2"));
  uint32_t count = 0;
  if (enumerate && properties && enumerate(instance, &count, nullptr) == VK_SUCCESS &&
      count > 0 && count <= kMaxPhysicalDevices) {
    std::vector<VkPhysicalDevice> devices(count);
    if (enumerate(instance, &count, devices.data()) == VK_SUCCESS && count > 0) {
      VkPhysicalDeviceProperties basic{};
      properties(devices[0], &basic);
      VkPhysicalDeviceDriverProperties driver{};
      driver.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
      if (api >= VK_API_VERSION_1_1 && basic.apiVersion >= VK_API_VERSION_1_2 && properties2) {
        VkPhysicalDeviceProperties2 extended{};
        extended.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        extended.pNext = &driver;
        properties2(devices[0], &extended);
      }
      identity = DeviceIdentity(basic, driver);
      found = true;
    }
  }
  const auto destroy = reinterpret_cast<PFN_vkDestroyInstance>(get(instance, "vkDestroyInstance"));
  if (destroy) destroy(instance, nullptr);
  dlclose(handle);
  return found;
}

// The capabilities the native renderer refuses to start without; the check
// that enforces them is in ui/vulkan/vulkan_device.cpp (with_native_shader_support).
// Reported per device so that a GPU lacking one - an unfamiliar vendor driver,
// say - produces a readable message on the launcher instead of a renderer that
// fails inside the game. Each value is 1 (present), 0 (absent) or -1 (could not
// be queried); only an explicit 0 is ever treated as a problem.
std::string CapabilityJson(VkInstance instance, VkPhysicalDevice device, uint32_t requested_api,
                           uint32_t device_api) {
  const auto features = Get<PFN_vkGetPhysicalDeviceFeatures>(instance, "vkGetPhysicalDeviceFeatures");
  const auto features2 = Get<PFN_vkGetPhysicalDeviceFeatures2>(instance, "vkGetPhysicalDeviceFeatures2");
  auto flag = [](int value) { return std::to_string(value); };
  int int64 = -1, clamp = -1, clip = -1, sample_rate = -1, blend = -1;
  int runtime_array = -1, partially_bound = -1, device_address = -1;
  if (features) {
    VkPhysicalDeviceFeatures base{};
    features(device, &base);
    int64 = base.shaderInt64 ? 1 : 0;
    clamp = base.depthClamp ? 1 : 0;
    clip = base.shaderClipDistance ? 1 : 0;
    sample_rate = base.sampleRateShading ? 1 : 0;
    blend = base.independentBlend ? 1 : 0;
  }
  if (features2) {
    if (requested_api >= VK_API_VERSION_1_2 && device_api >= VK_API_VERSION_1_2) {
      VkPhysicalDeviceVulkan12Features v12{};
      v12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
      VkPhysicalDeviceFeatures2 extended{};
      extended.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
      extended.pNext = &v12;
      features2(device, &extended);
      runtime_array = v12.runtimeDescriptorArray ? 1 : 0;
      partially_bound = v12.descriptorBindingPartiallyBound ? 1 : 0;
      device_address = v12.bufferDeviceAddress ? 1 : 0;
    } else if (device_api >= VK_API_VERSION_1_1) {
      VkPhysicalDeviceBufferDeviceAddressFeaturesKHR bda{};
      bda.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES_KHR;
      VkPhysicalDeviceDescriptorIndexingFeaturesEXT desc{};
      desc.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES_EXT;
      bda.pNext = &desc;
      VkPhysicalDeviceFeatures2 extended{};
      extended.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
      extended.pNext = &bda;
      features2(device, &extended);
      runtime_array = desc.runtimeDescriptorArray ? 1 : 0;
      partially_bound = desc.descriptorBindingPartiallyBound ? 1 : 0;
      device_address = bda.bufferDeviceAddress ? 1 : 0;
    }
  }
  return "{\"shaderInt64\":" + flag(int64) + ",\"depthClamp\":" + flag(clamp) +
         ",\"shaderClipDistance\":" + flag(clip) + ",\"sampleRateShading\":" + flag(sample_rate) +
         ",\"independentBlend\":" + flag(blend) + ",\"runtimeDescriptorArray\":" + flag(runtime_array) +
         ",\"descriptorBindingPartiallyBound\":" + flag(partially_bound) +
         ",\"bufferDeviceAddress\":" + flag(device_address) + ",\"dynamicRendering\":1}";
}

// Uses only functions from the selected loader, never this proxy's exports.
// A custom selection requires Vulkan 1.2 driver identity support. RP6/T30
// supports this; failing explicitly is safer than guessing from a GPU name.
bool VerifyInstance(VkInstance instance, uint32_t requested_api,
                    std::string& devices_json, std::string& error) {
  const auto enumerate = Get<PFN_vkEnumeratePhysicalDevices>(instance, "vkEnumeratePhysicalDevices");
  const auto properties = Get<PFN_vkGetPhysicalDeviceProperties>(instance, "vkGetPhysicalDeviceProperties");
  const auto properties2 = Get<PFN_vkGetPhysicalDeviceProperties2>(instance, "vkGetPhysicalDeviceProperties2");
  if (!enumerate || !properties) {
    error = "Selected loader is missing physical-device query functions";
    return false;
  }
  if (g_custom && (requested_api < VK_API_VERSION_1_1 || !properties2)) {
    error = "Custom driver validation requires a Vulkan 1.1 instance and the properties2 query";
    return false;
  }
  uint32_t count = 0;
  VkResult result = enumerate(instance, &count, nullptr);
  if (result != VK_SUCCESS || count == 0 || count > kMaxPhysicalDevices) {
    error = "Physical-device enumeration failed or returned an unsupported count (result=" +
            std::to_string(result) + ", count=" + std::to_string(count) + ")";
    return false;
  }
  std::vector<VkPhysicalDevice> devices(count);
  result = enumerate(instance, &count, devices.data());
  if (result != VK_SUCCESS || count == 0 || count > devices.size()) {
    error = "Physical-device list changed or could not be read";
    return false;
  }
  devices_json = "[";
  for (uint32_t i = 0; i < count; ++i) {
    VkPhysicalDeviceProperties basic{};
    properties(devices[i], &basic);
    VkPhysicalDeviceDriverProperties driver{};
    driver.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
    if (requested_api >= VK_API_VERSION_1_1 && basic.apiVersion >= VK_API_VERSION_1_2 && properties2) {
      VkPhysicalDeviceProperties2 extended{};
      extended.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
      extended.pNext = &driver;
      properties2(devices[i], &extended);
    }
    // Did the swap actually happen? Two ways to know, and the cheap one first:
    // a driver that is not Qualcomm's own cannot be the stock Adreno driver,
    // which settles every Turnip case without needing a baseline at all.
    //
    // Otherwise fall back to comparing identities. This used to demand Mesa
    // Turnip outright, which made a proprietary Qualcomm driver - the kind
    // extracted from another device - impossible to use however good it was,
    // because it reports the same driverID as the driver it replaces. An
    // identity that has not moved is the real evidence of libadrenotools
    // having fallen back, and that is what is reported now.
    // Every custom driver is checked the same way, with no shortcut on driver
    // ID. A shortcut was tried - trust anything not reporting as Qualcomm -
    // and it passed a silent fallback as success on the very first device it
    // met: this phone's stock driver reports Vulkan 1.1, so the 1.2 driver
    // properties are never filled in and driverID reads 0 rather than
    // QUALCOMM_PROPRIETARY. The old Turnip-only test happened to catch that
    // (0 is not Mesa either); an ID-based test that is trying to ACCEPT
    // drivers cannot. The identity comparison does not care, because a
    // fallback produces the stock identity whatever the ID says.
    if (g_custom) {
      const std::string identity = DeviceIdentity(basic, driver);
      if (g_system_identity.empty()) {
        error = "A custom driver was selected, but this device's own driver could not be read "
                "first, so there is no way to tell whether the custom one actually loaded";
        return false;
      }
      if (identity == g_system_identity) {
        error = "The custom driver did not take effect - the loaded driver is identical to this "
                "device's own (" + std::string(basic.deviceName) + ", driverVersion " +
                std::to_string(basic.driverVersion) + ")";
        return false;
      }
    }
    if (i) devices_json += ",";
    devices_json += "{\"name\":" + Quote(basic.deviceName) +
                    ",\"api_version\":" + std::to_string(basic.apiVersion) +
                    ",\"driver_version\":" + std::to_string(basic.driverVersion) +
                    ",\"vendor_id\":" + std::to_string(basic.vendorID) +
                    ",\"device_id\":" + std::to_string(basic.deviceID) +
                    ",\"driver_id\":" + std::to_string(driver.driverID) +
                    ",\"driver_name\":" + Quote(driver.driverName) +
                    ",\"driver_info\":" + Quote(driver.driverInfo) +
                    ",\"features\":" + CapabilityJson(instance, devices[i], requested_api, basic.apiVersion) + "}";
  }
  devices_json += "]";
  return true;
}

bool Probe(std::string& devices, std::string& error) {
  const auto create = Get<PFN_vkCreateInstance>(VK_NULL_HANDLE, "vkCreateInstance");
  const auto version = Get<PFN_vkEnumerateInstanceVersion>(VK_NULL_HANDLE, "vkEnumerateInstanceVersion");
  uint32_t api = VK_API_VERSION_1_0;
  if (!create || (version && version(&api) != VK_SUCCESS)) {
    error = "Selected Vulkan loader could not provide instance creation/version";
    return false;
  }
  if (g_custom && api < VK_API_VERSION_1_1) {
    error = "The Vulkan loader of this Android version is older than Vulkan 1.1";
    return false;
  }
  api = ProbeApiVersion(api);
  VkApplicationInfo app{};
  app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  app.pApplicationName = "Liberty Driver Validation";
  app.apiVersion = api;
  VkInstanceCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  info.pApplicationInfo = &app;
  VkInstance instance = VK_NULL_HANDLE;
  const VkResult result = create(&info, nullptr, &instance);
  if (result != VK_SUCCESS || instance == VK_NULL_HANDLE) {
    error = "Selected Vulkan driver failed the startup probe (VkResult=" + std::to_string(result) + ")";
    return false;
  }
  const bool valid = VerifyInstance(instance, api, devices, error);
  g_destroy(instance, nullptr);
  return valid;
}
} // namespace

EXPORT jint JNICALL JNI_OnLoad(JavaVM*, void*) { return JNI_VERSION_1_6; }

EXPORT jstring JNICALL Java_com_libertyrecomp_DriverBridge_nativeInit(
    JNIEnv* env, jclass, jstring native_dir_value, jstring driver_dir_value,
    jstring filename_value, jboolean custom_value, jboolean fallback_value) {
  std::lock_guard<std::mutex> lock(g_init_mutex);
  const std::string native_dir_arg = JavaString(env, native_dir_value);
  const std::string driver_dir_arg = JavaString(env, driver_dir_value);
  const std::string filename = JavaString(env, filename_value);
  if (env->ExceptionCheck()) return nullptr;
  const bool custom = custom_value == JNI_TRUE;
  const bool fallback = fallback_value == JNI_TRUE;
  const std::string arguments = Quote(native_dir_arg) + Quote(driver_dir_arg) + Quote(filename) +
                                (custom ? "1" : "0") + (fallback ? "1" : "0");
  if (g_attempted) {
    const std::string response = arguments == g_arguments ? g_status :
        "{\"ok\":false,\"error\":\"Restart the application process before changing drivers\"}";
    return env->NewStringUTF(response.c_str());
  }
  g_attempted = true;
  g_arguments = arguments;
  g_custom = custom;
  const std::string native_dir = CanonicalDirectory(native_dir_arg);
  std::string error;
  if (native_dir.empty()) {
    error = "Native library directory is missing or not an absolute directory";
  } else {
    // Test the exact bare-name lookup that the unchanged engine will make.
    // Absolute preloading is insufficient if another libvulkan was inserted
    // earlier in this same application namespace. Do not guess in that case.
    void* selected = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL | RTLD_NOLOAD);
    const auto selected_get = selected ? reinterpret_cast<PFN_vkGetInstanceProcAddr>(
        dlsym(selected, "vkGetInstanceProcAddr")) : nullptr;
    if (selected_get != &vkGetInstanceProcAddr) {
      error = "The application Vulkan lookup does not resolve to the driver proxy; restart before loading the game";
    }
    if (selected) dlclose(selected);
  }
  const bool proxy_ok = error.empty();
  if (error.empty() && custom) {
    const std::string driver_dir = CanonicalDirectory(driver_dir_arg);
    char file_path[PATH_MAX];
    if (driver_dir.empty() || driver_dir.rfind("/data/", 0) != 0) {
      error = "Custom Vulkan driver must be in app-private /data storage";
    } else if (filename.empty() || filename.find('/') != std::string::npos ||
               filename == "." || filename == "..") {
      error = "Custom Vulkan driver filename is invalid";
    } else if (!realpath((driver_dir + filename).c_str(), file_path) ||
               std::string(file_path).rfind(driver_dir, 0) != 0 || !RegularFile(file_path)) {
      error = "Custom Vulkan driver file is missing or escapes its private directory";
    } else if (!RegularFile(native_dir + "libhook_impl.so") || !RegularFile(native_dir + "libmain_hook.so")) {
      error = "Custom driver support libraries are missing; native libraries must be extracted";
    } else {
      // Read the stock driver BEFORE the hook is installed - afterwards there
      // is no longer an unhooked loader to ask. Not fatal on its own: a Turnip
      // driver is recognised by its driver ID and needs no baseline, so only a
      // same-vendor driver is refused for the lack of one.
      if (!ProbeSystemIdentity(g_system_identity)) {
        g_system_identity.clear();
        __android_log_print(ANDROID_LOG_WARN, kTag,
                            "could not read this device's own driver identity before loading the custom one");
      }
      g_loader = adrenotools_open_libvulkan(RTLD_NOW | RTLD_LOCAL, ADRENOTOOLS_DRIVER_CUSTOM,
          driver_dir.c_str(), native_dir.c_str(), driver_dir.c_str(), filename.c_str(), nullptr, nullptr);
      if (!g_loader) error = "libadrenotools could not initialize the selected custom driver";
    }
  } else if (error.empty()) {
    // An absolute system path cannot resolve back to this app's proxy by SONAME.
    g_loader = dlopen("/system/lib64/libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    if (!g_loader) {
      const char* detail = dlerror();
      error = std::string("System Vulkan loader could not open: ") + (detail ? detail : "unknown error");
    }
  }
  std::string devices;
  if (error.empty()) {
    g_get = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(g_loader, "vkGetInstanceProcAddr"));
    g_destroy = reinterpret_cast<PFN_vkDestroyInstance>(dlsym(g_loader, "vkDestroyInstance"));
    if (!g_get || !g_destroy || g_get == &vkGetInstanceProcAddr || g_destroy == &vkDestroyInstance) {
      error = "Vulkan loader entrypoints are missing or resolve recursively to the proxy";
    } else if (!Probe(devices, error)) {
      // Keep the exact failure for the status below.
    }
  }
  // A custom driver that cannot run this GPU (Turnip on an Adreno 8xx or a
  // non-Adreno GPU) falls back to the device's own driver when the caller
  // allows it; whether that driver is good enough is decided by the caller.
  std::string fallback_from;
  if (!error.empty() && custom && fallback && proxy_ok) {
    __android_log_print(ANDROID_LOG_WARN, kTag, "custom driver failed (%s); trying the system driver",
                        error.c_str());
    fallback_from = error;
    error.clear();
    devices.clear();
    g_custom = false;
    g_get = nullptr;
    g_destroy = nullptr;
    g_loader = dlopen("/system/lib64/libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    if (!g_loader) {
      const char* detail = dlerror();
      error = std::string("System Vulkan loader could not open: ") + (detail ? detail : "unknown error");
    } else {
      g_get = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(g_loader, "vkGetInstanceProcAddr"));
      g_destroy = reinterpret_cast<PFN_vkDestroyInstance>(dlsym(g_loader, "vkDestroyInstance"));
      if (!g_get || !g_destroy || g_get == &vkGetInstanceProcAddr || g_destroy == &vkDestroyInstance) {
        error = "Vulkan loader entrypoints are missing or resolve recursively to the proxy";
      } else {
        Probe(devices, error);
      }
    }
    if (!error.empty()) error = fallback_from + "; the system driver failed too: " + error;
  }
  if (!error.empty()) {
    g_status = Failure(error);
  } else {
    g_status = "{\"ok\":true,\"mode\":" + Quote(g_custom ? "turnip" : "system") +
        ",\"error\":\"\",\"fallback_from\":" + Quote(fallback_from) +
        ",\"loader_path\":" + Quote(LoaderPath()) +
        ",\"devices\":" + devices + "}";
    g_ready.store(true, std::memory_order_release);
    __android_log_print(ANDROID_LOG_INFO, kTag, "Driver initialized: %s", g_status.c_str());
  }
  return env->NewStringUTF(g_status.c_str());
}

EXPORT jstring JNICALL Java_com_libertyrecomp_DriverBridge_nativeStatus(JNIEnv* env, jclass) {
  std::lock_guard<std::mutex> lock(g_init_mutex);
  return env->NewStringUTF(g_status.c_str());
}

EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkCreateInstance(
    const VkInstanceCreateInfo* info, const VkAllocationCallbacks* allocator, VkInstance* instance) {
  if (!g_ready.load(std::memory_order_acquire)) return VK_ERROR_INITIALIZATION_FAILED;
  const auto create = Get<PFN_vkCreateInstance>(VK_NULL_HANDLE, "vkCreateInstance");
  const VkResult result = create(info, allocator, instance);
  if (result != VK_SUCCESS) return result;
  g_last_instance = *instance;
  const uint32_t api = info && info->pApplicationInfo && info->pApplicationInfo->apiVersion ?
      info->pApplicationInfo->apiVersion : VK_API_VERSION_1_0;
  std::string devices, error;
  if (!VerifyInstance(*instance, api, devices, error)) {
    __android_log_print(ANDROID_LOG_ERROR, kTag, "Game Vulkan instance rejected: %s", error.c_str());
    g_destroy(*instance, allocator);
    if (g_last_instance == *instance) g_last_instance = VK_NULL_HANDLE;
    *instance = VK_NULL_HANDLE;
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  const uint32_t count = g_game_instances.fetch_add(1, std::memory_order_relaxed) + 1;
  if (count <= 8) __android_log_print(ANDROID_LOG_INFO, kTag,
      "Game Vulkan instance %u verified: mode=%s loader=%s devices=%s", count,
      g_custom ? "turnip" : "system", LoaderPath().c_str(), devices.c_str());
  return VK_SUCCESS;
}

EXPORT VKAPI_ATTR void VKAPI_CALL vkDestroyInstance(VkInstance instance, const VkAllocationCallbacks* allocator) {
  if (instance == g_last_instance) g_last_instance = VK_NULL_HANDLE;
  if (g_ready.load(std::memory_order_acquire)) g_destroy(instance, allocator);
}

EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkCreateDevice(
    VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator, VkDevice* pDevice) {
  if (!g_ready.load(std::memory_order_acquire) || !pCreateInfo) return VK_ERROR_INITIALIZATION_FAILED;

  const auto real_create = g_real_create_device ? g_real_create_device :
      LoaderSymbol<PFN_vkCreateDevice>("vkCreateDevice");
  if (!real_create) return VK_ERROR_INITIALIZATION_FAILED;

  std::vector<const char*> filtered_extensions;
  if (pCreateInfo->ppEnabledExtensionNames) {
    for (uint32_t i = 0; i < pCreateInfo->enabledExtensionCount; ++i) {
      const char* ext_name = pCreateInfo->ppEnabledExtensionNames[i];
      if (std::strcmp(ext_name, "VK_KHR_dynamic_rendering") != 0 &&
          std::strcmp(ext_name, "VK_EXT_depth_clip_control") != 0) {
        filtered_extensions.push_back(ext_name);
      }
    }
  }

  VkDeviceCreateInfo modified_info = *pCreateInfo;
  modified_info.enabledExtensionCount = static_cast<uint32_t>(filtered_extensions.size());
  modified_info.ppEnabledExtensionNames = filtered_extensions.data();

  // Temporarily unlink emulated feature structures from pNext
  struct UnlinkRecord {
    VkBaseOutStructure* node;
    VkBaseOutStructure* prev;
  };
  std::vector<UnlinkRecord> unlinked_records;

  auto* head = const_cast<VkBaseOutStructure*>(static_cast<const VkBaseOutStructure*>(pCreateInfo->pNext));
  VkBaseOutStructure* prev = nullptr;
  for (auto* curr = head; curr; ) {
    if (curr->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES_KHR ||
        curr->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_CONTROL_FEATURES_EXT) {
      unlinked_records.push_back({curr, prev});
      if (prev) {
        prev->pNext = curr->pNext;
      } else {
        modified_info.pNext = curr->pNext;
      }
      curr = curr->pNext;
    } else {
      prev = curr;
      curr = curr->pNext;
    }
  }

  VkResult result = real_create(physicalDevice, &modified_info, pAllocator, pDevice);

  // Restore pNext links
  for (auto it = unlinked_records.rbegin(); it != unlinked_records.rend(); ++it) {
    if (it->prev) {
      it->prev->pNext = it->node;
    } else {
      const_cast<VkDeviceCreateInfo*>(pCreateInfo)->pNext = it->node;
    }
  }

  if (result != VK_SUCCESS || !pDevice || *pDevice == VK_NULL_HANDLE) {
    return result;
  }

  VkDevice device = *pDevice;
  PFN_vkGetDeviceProcAddr dev_get = LoaderSymbol<PFN_vkGetDeviceProcAddr>("vkGetDeviceProcAddr");
  if (!dev_get) {
    dev_get = reinterpret_cast<PFN_vkGetDeviceProcAddr>(dlsym(g_loader, "vkGetDeviceProcAddr"));
  }

  if (dev_get) {
    std::lock_guard<std::mutex> lock(g_cache_mutex);
    g_device = device;
    g_real_create_graphics_pipelines = reinterpret_cast<PFN_vkCreateGraphicsPipelines>(
        dev_get(device, "vkCreateGraphicsPipelines"));
    g_real_create_compute_pipelines = reinterpret_cast<PFN_vkCreateComputePipelines>(
        dev_get(device, "vkCreateComputePipelines"));
    g_real_queue_present = reinterpret_cast<PFN_vkQueuePresentKHR>(
        dev_get(device, "vkQueuePresentKHR"));
    g_real_destroy_device = reinterpret_cast<PFN_vkDestroyDevice>(
        dev_get(device, "vkDestroyDevice"));
    g_create_pipeline_cache = reinterpret_cast<PFN_vkCreatePipelineCache>(
        dev_get(device, "vkCreatePipelineCache"));
    g_destroy_pipeline_cache = reinterpret_cast<PFN_vkDestroyPipelineCache>(
        dev_get(device, "vkDestroyPipelineCache"));
    g_get_pipeline_cache_data = reinterpret_cast<PFN_vkGetPipelineCacheData>(
        dev_get(device, "vkGetPipelineCacheData"));

    g_real_create_render_pass = reinterpret_cast<PFN_vkCreateRenderPass>(
        dev_get(device, "vkCreateRenderPass"));
    g_real_destroy_render_pass = reinterpret_cast<PFN_vkDestroyRenderPass>(
        dev_get(device, "vkDestroyRenderPass"));
    g_real_create_framebuffer = reinterpret_cast<PFN_vkCreateFramebuffer>(
        dev_get(device, "vkCreateFramebuffer"));
    g_real_destroy_framebuffer = reinterpret_cast<PFN_vkDestroyFramebuffer>(
        dev_get(device, "vkDestroyFramebuffer"));
    g_real_cmd_begin_render_pass = reinterpret_cast<PFN_vkCmdBeginRenderPass>(
        dev_get(device, "vkCmdBeginRenderPass"));
    g_real_cmd_end_render_pass = reinterpret_cast<PFN_vkCmdEndRenderPass>(
        dev_get(device, "vkCmdEndRenderPass"));
    g_real_create_image_view = reinterpret_cast<PFN_vkCreateImageView>(
        dev_get(device, "vkCreateImageView"));
    g_real_destroy_image_view = reinterpret_cast<PFN_vkDestroyImageView>(
        dev_get(device, "vkDestroyImageView"));
    g_real_create_image = reinterpret_cast<PFN_vkCreateImage>(
        dev_get(device, "vkCreateImage"));
    g_real_destroy_image = reinterpret_cast<PFN_vkDestroyImage>(
        dev_get(device, "vkDestroyImage"));

    if (g_create_pipeline_cache) {
      g_cache_file_path = GetCachePath();
      VkPhysicalDeviceProperties dev_props{};
      PFN_vkGetPhysicalDeviceProperties get_props = LoaderSymbol<PFN_vkGetPhysicalDeviceProperties>(
          "vkGetPhysicalDeviceProperties");
      if (get_props) {
        get_props(physicalDevice, &dev_props);
      }

      std::vector<uint8_t> initial_data;
      FILE* fp = fopen(g_cache_file_path.c_str(), "rb");
      if (fp) {
        fseek(fp, 0, SEEK_END);
        long sz = ftell(fp);
        fseek(fp, 0, SEEK_SET);
        if (sz >= static_cast<long>(sizeof(VkPipelineCacheHeaderVersionOne))) {
          initial_data.resize(sz);
          if (fread(initial_data.data(), 1, sz, fp) == static_cast<size_t>(sz)) {
            const auto* hdr = reinterpret_cast<const VkPipelineCacheHeaderVersionOne*>(initial_data.data());
            if (hdr->headerSize != sizeof(VkPipelineCacheHeaderVersionOne) ||
                hdr->headerVersion != VK_PIPELINE_CACHE_HEADER_VERSION_ONE ||
                (dev_props.vendorID != 0 && hdr->vendorID != dev_props.vendorID) ||
                (dev_props.deviceID != 0 && hdr->deviceID != dev_props.deviceID) ||
                memcmp(hdr->pipelineCacheUUID, dev_props.pipelineCacheUUID, VK_UUID_SIZE) != 0) {
              __android_log_print(ANDROID_LOG_WARN, kTag,
                  "Stale or mismatched pipeline cache detected, resetting cache");
              initial_data.clear();
            } else {
              __android_log_print(ANDROID_LOG_INFO, kTag,
                  "Loaded valid pipeline cache: %ld bytes from %s", sz, g_cache_file_path.c_str());
            }
          } else {
            initial_data.clear();
          }
        }
        fclose(fp);
      }

      VkPipelineCacheCreateInfo cache_info{};
      cache_info.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
      cache_info.initialDataSize = initial_data.size();
      cache_info.pInitialData = initial_data.empty() ? nullptr : initial_data.data();

      VkResult c_res = g_create_pipeline_cache(device, &cache_info, nullptr, &g_pipeline_cache);
      if (c_res != VK_SUCCESS && !initial_data.empty()) {
        __android_log_print(ANDROID_LOG_WARN, kTag,
            "vkCreatePipelineCache with initial data failed (%d), retrying empty", c_res);
        cache_info.initialDataSize = 0;
        cache_info.pInitialData = nullptr;
        c_res = g_create_pipeline_cache(device, &cache_info, nullptr, &g_pipeline_cache);
      }
      if (c_res == VK_SUCCESS) {
        __android_log_print(ANDROID_LOG_INFO, kTag,
            "Persistent VkPipelineCache initialized successfully (handle: %p)",
            reinterpret_cast<void*>(g_pipeline_cache));
      } else {
        __android_log_print(ANDROID_LOG_ERROR, kTag,
            "Failed to initialize VkPipelineCache (%d)", c_res);
        g_pipeline_cache = VK_NULL_HANDLE;
      }
    }
  }

  return result;
}

EXPORT VKAPI_ATTR void VKAPI_CALL vkDestroyDevice(VkDevice device, const VkAllocationCallbacks* pAllocator) {
  SavePipelineCache(device);
  CleanupDynamicRendering(device);
  {
    std::lock_guard<std::mutex> lock(g_cache_mutex);
    if (device == g_device && g_pipeline_cache != VK_NULL_HANDLE && g_destroy_pipeline_cache) {
      g_destroy_pipeline_cache(device, g_pipeline_cache, nullptr);
      g_pipeline_cache = VK_NULL_HANDLE;
      g_device = VK_NULL_HANDLE;
    }
  }
  const auto real_fn = g_real_destroy_device ? g_real_destroy_device :
      LoaderSymbol<PFN_vkDestroyDevice>("vkDestroyDevice");
  if (real_fn) real_fn(device, pAllocator);
}

EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkCreateImage(
    VkDevice device, const VkImageCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator, VkImage* pImage) {
  const auto real_fn = g_real_create_image ? g_real_create_image :
      LoaderSymbol<PFN_vkCreateImage>("vkCreateImage");
  if (!real_fn) return VK_ERROR_INITIALIZATION_FAILED;
  VkResult res = real_fn(device, pCreateInfo, pAllocator, pImage);
  if (res == VK_SUCCESS && pImage && *pImage != VK_NULL_HANDLE && pCreateInfo) {
    std::lock_guard<std::mutex> lock(g_view_mutex);
    g_image_samples[*pImage] = pCreateInfo->samples;
  }
  return res;
}

EXPORT VKAPI_ATTR void VKAPI_CALL vkDestroyImage(
    VkDevice device, VkImage image, const VkAllocationCallbacks* pAllocator) {
  {
    std::lock_guard<std::mutex> lock(g_view_mutex);
    g_image_samples.erase(image);
  }
  const auto real_fn = g_real_destroy_image ? g_real_destroy_image :
      LoaderSymbol<PFN_vkDestroyImage>("vkDestroyImage");
  if (real_fn) real_fn(device, image, pAllocator);
}

EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkCreateImageView(
    VkDevice device, const VkImageViewCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator, VkImageView* pView) {
  const auto real_fn = g_real_create_image_view ? g_real_create_image_view :
      LoaderSymbol<PFN_vkCreateImageView>("vkCreateImageView");
  if (!real_fn) return VK_ERROR_INITIALIZATION_FAILED;
  VkResult res = real_fn(device, pCreateInfo, pAllocator, pView);
  if (res == VK_SUCCESS && pView && *pView != VK_NULL_HANDLE && pCreateInfo) {
    std::lock_guard<std::mutex> lock(g_view_mutex);
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    auto it = g_image_samples.find(pCreateInfo->image);
    if (it != g_image_samples.end()) {
      samples = it->second;
    }
    g_views[*pView] = {pCreateInfo->format, samples};
  }
  return res;
}

EXPORT VKAPI_ATTR void VKAPI_CALL vkDestroyImageView(
    VkDevice device, VkImageView imageView, const VkAllocationCallbacks* pAllocator) {
  {
    std::lock_guard<std::mutex> lock(g_view_mutex);
    g_views.erase(imageView);
    InvalidateFramebuffersWithViewLocked(device, imageView);
  }
  const auto real_fn = g_real_destroy_image_view ? g_real_destroy_image_view :
      LoaderSymbol<PFN_vkDestroyImageView>("vkDestroyImageView");
  if (real_fn) real_fn(device, imageView, pAllocator);
}

EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateDeviceExtensionProperties(
    VkPhysicalDevice physicalDevice, const char* pLayerName,
    uint32_t* pPropertyCount, VkExtensionProperties* pProperties) {
  if (!g_ready.load(std::memory_order_acquire) || !pPropertyCount) return VK_ERROR_INITIALIZATION_FAILED;

  const auto real_fn = g_real_enum_device_ext_props ? g_real_enum_device_ext_props :
      LoaderSymbol<PFN_vkEnumerateDeviceExtensionProperties>("vkEnumerateDeviceExtensionProperties");
  if (!real_fn) return VK_ERROR_INITIALIZATION_FAILED;

  if (pLayerName != nullptr) {
    return real_fn(physicalDevice, pLayerName, pPropertyCount, pProperties);
  }

  uint32_t real_count = 0;
  VkResult res = real_fn(physicalDevice, nullptr, &real_count, nullptr);
  if (res != VK_SUCCESS) return res;

  std::vector<VkExtensionProperties> extensions(real_count);
  if (real_count > 0) {
    res = real_fn(physicalDevice, nullptr, &real_count, extensions.data());
    if (res != VK_SUCCESS && res != VK_INCOMPLETE) return res;
    extensions.resize(real_count);
  }

  bool has_dynamic_rendering = false;
  bool has_depth_clip_control = false;
  for (const auto& ext : extensions) {
    if (std::strcmp(ext.extensionName, "VK_KHR_dynamic_rendering") == 0) {
      has_dynamic_rendering = true;
    } else if (std::strcmp(ext.extensionName, "VK_EXT_depth_clip_control") == 0) {
      has_depth_clip_control = true;
    }
  }

  if (!has_dynamic_rendering) {
    VkExtensionProperties dyn_ext{};
    std::strncpy(dyn_ext.extensionName, "VK_KHR_dynamic_rendering", VK_MAX_EXTENSION_NAME_SIZE - 1);
    dyn_ext.specVersion = 1;
    extensions.push_back(dyn_ext);
  }

  if (!has_depth_clip_control) {
    VkExtensionProperties dcc_ext{};
    std::strncpy(dcc_ext.extensionName, "VK_EXT_depth_clip_control", VK_MAX_EXTENSION_NAME_SIZE - 1);
    dcc_ext.specVersion = 1;
    extensions.push_back(dcc_ext);
  }

  if (!pProperties) {
    *pPropertyCount = static_cast<uint32_t>(extensions.size());
    return VK_SUCCESS;
  }

  uint32_t copy_count = std::min(*pPropertyCount, static_cast<uint32_t>(extensions.size()));
  std::memcpy(pProperties, extensions.data(), copy_count * sizeof(VkExtensionProperties));
  VkResult return_res = copy_count < extensions.size() ? VK_INCOMPLETE : VK_SUCCESS;
  *pPropertyCount = copy_count;
  return return_res;
}

namespace {
void InterceptFeatures(void* pNext) {
  for (auto* header = static_cast<VkBaseOutStructure*>(pNext); header; header = header->pNext) {
    if (header->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES_KHR) {
      auto* dyn_feat = reinterpret_cast<VkPhysicalDeviceDynamicRenderingFeaturesKHR*>(header);
      dyn_feat->dynamicRendering = VK_TRUE;
      __android_log_print(ANDROID_LOG_INFO, kTag, "Enabled dynamicRendering feature via driver proxy");
    } else if (header->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_CONTROL_FEATURES_EXT) {
      auto* dcc_feat = reinterpret_cast<VkPhysicalDeviceDepthClipControlFeaturesEXT*>(header);
      dcc_feat->depthClipControl = VK_TRUE;
      __android_log_print(ANDROID_LOG_INFO, kTag, "Enabled depthClipControl feature via driver proxy");
    }
  }
}
} // namespace

EXPORT VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFeatures2(
    VkPhysicalDevice physicalDevice, VkPhysicalDeviceFeatures2* pFeatures) {
  PFN_vkGetPhysicalDeviceFeatures2 real_fn = g_real_get_pd_features2 ?
      g_real_get_pd_features2 : LoaderSymbol<PFN_vkGetPhysicalDeviceFeatures2>("vkGetPhysicalDeviceFeatures2");
  if (real_fn) real_fn(physicalDevice, pFeatures);
  if (pFeatures) InterceptFeatures(pFeatures->pNext);
}

EXPORT VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFeatures2KHR(
    VkPhysicalDevice physicalDevice, VkPhysicalDeviceFeatures2KHR* pFeatures) {
  PFN_vkGetPhysicalDeviceFeatures2KHR real_fn = g_real_get_pd_features2_khr ?
      g_real_get_pd_features2_khr : LoaderSymbol<PFN_vkGetPhysicalDeviceFeatures2KHR>("vkGetPhysicalDeviceFeatures2KHR");
  if (real_fn) {
    real_fn(physicalDevice, pFeatures);
  } else {
    vkGetPhysicalDeviceFeatures2(physicalDevice, pFeatures);
  }
  if (pFeatures) InterceptFeatures(pFeatures->pNext);
}

EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkCreateGraphicsPipelines(
    VkDevice device, VkPipelineCache pipelineCache, uint32_t createInfoCount,
    const VkGraphicsPipelineCreateInfo* pCreateInfos,
    const VkAllocationCallbacks* pAllocator, VkPipeline* pPipelines) {
  VkPipelineCache cache_to_use = pipelineCache;
  if (cache_to_use == VK_NULL_HANDLE) {
    std::lock_guard<std::mutex> lock(g_cache_mutex);
    if (g_pipeline_cache != VK_NULL_HANDLE && device == g_device) {
      cache_to_use = g_pipeline_cache;
    }
  }

  const auto real_fn = g_real_create_graphics_pipelines ? g_real_create_graphics_pipelines :
      LoaderSymbol<PFN_vkCreateGraphicsPipelines>("vkCreateGraphicsPipelines");
  if (!real_fn) return VK_ERROR_INITIALIZATION_FAILED;

  std::vector<VkGraphicsPipelineCreateInfo> modified_infos(pCreateInfos, pCreateInfos + createInfoCount);
  std::vector<VkPipelineMultisampleStateCreateInfo> modified_ms_states(createInfoCount);
  std::vector<VkPipelineViewportStateCreateInfo> modified_vp_states(createInfoCount);
  bool modified = false;

  for (uint32_t i = 0; i < createInfoCount; ++i) {
    // 1. Emulate dynamic rendering for pipelines without renderPass
    if (modified_infos[i].renderPass == VK_NULL_HANDLE) {
      const VkPipelineRenderingCreateInfo* rendering_info = nullptr;
      for (const auto* h = static_cast<const VkBaseInStructure*>(modified_infos[i].pNext); h; h = h->pNext) {
        if (h->sType == VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR) {
          rendering_info = reinterpret_cast<const VkPipelineRenderingCreateInfo*>(h);
          break;
        }
      }

      if (rendering_info) {
        RenderPassKey key{};
        std::memset(&key, 0, sizeof(key));
        key.color_count = rendering_info->colorAttachmentCount;
        for (uint32_t j = 0; j < key.color_count; ++j) {
          key.color_formats[j] = rendering_info->pColorAttachmentFormats ?
              rendering_info->pColorAttachmentFormats[j] : VK_FORMAT_UNDEFINED;
          key.color_load_ops[j] = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
          key.color_store_ops[j] = VK_ATTACHMENT_STORE_OP_STORE;
          key.color_layouts[j] = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        }
        key.depth_format = rendering_info->depthAttachmentFormat;
        key.depth_load_op = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        key.depth_store_op = VK_ATTACHMENT_STORE_OP_STORE;
        key.depth_layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        key.stencil_format = rendering_info->stencilAttachmentFormat;
        key.stencil_load_op = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        key.stencil_store_op = VK_ATTACHMENT_STORE_OP_STORE;
        key.stencil_layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        key.samples = modified_infos[i].pMultisampleState ?
            modified_infos[i].pMultisampleState->rasterizationSamples : VK_SAMPLE_COUNT_1_BIT;

        VkRenderPass comp_rp = GetOrCreateRenderPass(device, key);
        if (comp_rp != VK_NULL_HANDLE) {
          modified_infos[i].renderPass = comp_rp;
          // Unlink VkPipelineRenderingCreateInfo from pNext
          auto* curr = const_cast<VkBaseOutStructure*>(static_cast<const VkBaseOutStructure*>(modified_infos[i].pNext));
          VkBaseOutStructure* prev_node = nullptr;
          while (curr) {
            if (curr->sType == VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR) {
              if (prev_node) {
                prev_node->pNext = curr->pNext;
              } else {
                modified_infos[i].pNext = curr->pNext;
              }
              break;
            }
            prev_node = curr;
            curr = curr->pNext;
          }
          modified = true;
        }
      }
    }

    // 2. Sanitize multisample state: disable sample-rate shading (SSAA) on Mali.
    if (modified_infos[i].pMultisampleState && modified_infos[i].pMultisampleState->sampleShadingEnable) {
      modified_ms_states[i] = *modified_infos[i].pMultisampleState;
      modified_ms_states[i].sampleShadingEnable = VK_FALSE;
      modified_ms_states[i].minSampleShading = 0.0f;
      modified_infos[i].pMultisampleState = &modified_ms_states[i];
      modified = true;
    }

    // 3. Strip depth clip control create info from pViewportState->pNext
    if (modified_infos[i].pViewportState && modified_infos[i].pViewportState->pNext) {
      bool has_dcc = false;
      for (const auto* h = static_cast<const VkBaseInStructure*>(modified_infos[i].pViewportState->pNext); h; h = h->pNext) {
        if (h->sType == VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_DEPTH_CLIP_CONTROL_CREATE_INFO_EXT) {
          has_dcc = true;
          break;
        }
      }
      if (has_dcc) {
        modified_vp_states[i] = *modified_infos[i].pViewportState;
        auto* curr = const_cast<VkBaseOutStructure*>(static_cast<const VkBaseOutStructure*>(modified_vp_states[i].pNext));
        VkBaseOutStructure* prev_node = nullptr;
        while (curr) {
          if (curr->sType == VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_DEPTH_CLIP_CONTROL_CREATE_INFO_EXT) {
            if (prev_node) {
              prev_node->pNext = curr->pNext;
            } else {
              modified_vp_states[i].pNext = curr->pNext;
            }
            break;
          }
          prev_node = curr;
          curr = curr->pNext;
        }
        modified_infos[i].pViewportState = &modified_vp_states[i];
        modified = true;
      }
    }
  }

  const VkGraphicsPipelineCreateInfo* infos_to_pass = modified ? modified_infos.data() : pCreateInfos;
  VkResult res = real_fn(device, cache_to_use, createInfoCount, infos_to_pass, pAllocator, pPipelines);
  if (res == VK_SUCCESS && cache_to_use != VK_NULL_HANDLE && cache_to_use == g_pipeline_cache) {
    g_pipelines_since_last_save.fetch_add(createInfoCount, std::memory_order_relaxed);
  }
  return res;
}

EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkCreateComputePipelines(
    VkDevice device, VkPipelineCache pipelineCache, uint32_t createInfoCount,
    const VkComputePipelineCreateInfo* pCreateInfos,
    const VkAllocationCallbacks* pAllocator, VkPipeline* pPipelines) {
  VkPipelineCache cache_to_use = pipelineCache;
  if (cache_to_use == VK_NULL_HANDLE) {
    std::lock_guard<std::mutex> lock(g_cache_mutex);
    if (g_pipeline_cache != VK_NULL_HANDLE && device == g_device) {
      cache_to_use = g_pipeline_cache;
    }
  }

  const auto real_fn = g_real_create_compute_pipelines ? g_real_create_compute_pipelines :
      LoaderSymbol<PFN_vkCreateComputePipelines>("vkCreateComputePipelines");
  if (!real_fn) return VK_ERROR_INITIALIZATION_FAILED;

  VkResult res = real_fn(device, cache_to_use, createInfoCount, pCreateInfos, pAllocator, pPipelines);
  if (res == VK_SUCCESS && cache_to_use != VK_NULL_HANDLE && cache_to_use == g_pipeline_cache) {
    g_pipelines_since_last_save.fetch_add(createInfoCount, std::memory_order_relaxed);
  }
  return res;
}

EXPORT VKAPI_ATTR void VKAPI_CALL vkCmdBeginRendering(
    VkCommandBuffer commandBuffer, const VkRenderingInfo* pRenderingInfo) {
  if (!pRenderingInfo || !g_real_cmd_begin_render_pass) return;

  RenderPassKey rp_key{};
  std::memset(&rp_key, 0, sizeof(rp_key));
  rp_key.color_count = pRenderingInfo->colorAttachmentCount;
  rp_key.samples = VK_SAMPLE_COUNT_1_BIT;
  rp_key.depth_load_op = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  rp_key.depth_store_op = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  rp_key.depth_layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
  rp_key.stencil_load_op = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  rp_key.stencil_store_op = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  rp_key.stencil_layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
  for (uint32_t i = 0; i < 8; ++i) {
    rp_key.color_load_ops[i] = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    rp_key.color_store_ops[i] = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    rp_key.color_layouts[i] = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  }

  std::vector<VkImageView> fb_views;
  std::vector<VkClearValue> clear_values;

  {
    std::lock_guard<std::mutex> lock(g_view_mutex);
    for (uint32_t i = 0; i < pRenderingInfo->colorAttachmentCount && i < 8; ++i) {
      const auto& att = pRenderingInfo->pColorAttachments[i];
      if (att.imageView != VK_NULL_HANDLE) {
        auto it = g_views.find(att.imageView);
        if (it != g_views.end()) {
          rp_key.color_formats[i] = it->second.format;
          if (it->second.samples > rp_key.samples) rp_key.samples = it->second.samples;
          rp_key.color_load_ops[i] = att.loadOp;
          rp_key.color_store_ops[i] = att.storeOp;
          rp_key.color_layouts[i] = att.imageLayout;
          fb_views.push_back(att.imageView);
          clear_values.push_back(att.clearValue);
        } else {
          __android_log_print(ANDROID_LOG_WARN, kTag, "Color attachment %u uses unknown imageView %p", i, att.imageView);
          rp_key.color_formats[i] = VK_FORMAT_UNDEFINED;
        }
      } else {
        rp_key.color_formats[i] = VK_FORMAT_UNDEFINED;
      }
    }

    if (pRenderingInfo->pDepthAttachment && pRenderingInfo->pDepthAttachment->imageView != VK_NULL_HANDLE) {
      const auto& att = *pRenderingInfo->pDepthAttachment;
      auto it = g_views.find(att.imageView);
      if (it != g_views.end()) {
        rp_key.depth_format = it->second.format;
        if (it->second.samples > rp_key.samples) rp_key.samples = it->second.samples;
        rp_key.depth_load_op = att.loadOp;
        rp_key.depth_store_op = att.storeOp;
        rp_key.depth_layout = att.imageLayout;
        fb_views.push_back(att.imageView);
        clear_values.push_back(att.clearValue);
      } else {
        __android_log_print(ANDROID_LOG_WARN, kTag, "Depth attachment uses unknown imageView %p", att.imageView);
      }
    }

    if (pRenderingInfo->pStencilAttachment && pRenderingInfo->pStencilAttachment->imageView != VK_NULL_HANDLE) {
      const auto& att = *pRenderingInfo->pStencilAttachment;
      auto it = g_views.find(att.imageView);
      if (it != g_views.end()) {
        rp_key.stencil_format = it->second.format;
        rp_key.stencil_load_op = att.loadOp;
        rp_key.stencil_store_op = att.storeOp;
        rp_key.stencil_layout = att.imageLayout;
        if (!pRenderingInfo->pDepthAttachment ||
            pRenderingInfo->pDepthAttachment->imageView != att.imageView) {
          fb_views.push_back(att.imageView);
          clear_values.push_back(att.clearValue);
        }
      } else {
        __android_log_print(ANDROID_LOG_WARN, kTag, "Stencil attachment uses unknown imageView %p", att.imageView);
      }
    }
  }

  VkDevice device = g_device;
  VkRenderPass rp = GetOrCreateRenderPass(device, rp_key);
  if (rp == VK_NULL_HANDLE) return;

  uint32_t width = std::max(1u, pRenderingInfo->renderArea.offset.x + pRenderingInfo->renderArea.extent.width);
  uint32_t height = std::max(1u, pRenderingInfo->renderArea.offset.y + pRenderingInfo->renderArea.extent.height);
  uint32_t layers = std::max(1u, pRenderingInfo->layerCount);

  FramebufferKey fb_key{};
  std::memset(&fb_key, 0, sizeof(fb_key));
  fb_key.render_pass = rp;
  fb_key.width = width;
  fb_key.height = height;
  fb_key.layers = layers;
  fb_key.attachment_count = static_cast<uint32_t>(std::min(fb_views.size(), static_cast<size_t>(9)));
  for (size_t i = 0; i < fb_key.attachment_count; ++i) {
    fb_key.attachments[i] = fb_views[i];
  }

  VkFramebuffer fb = VK_NULL_HANDLE;
  {
    std::lock_guard<std::mutex> lock(g_rp_mutex);
    auto it = g_framebuffers.find(fb_key);
    if (it != g_framebuffers.end()) {
      fb = it->second;
    } else {
      VkFramebufferCreateInfo fb_info{};
      fb_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
      fb_info.renderPass = rp;
      fb_info.attachmentCount = fb_key.attachment_count;
      fb_info.pAttachments = fb_key.attachments;
      fb_info.width = width;
      fb_info.height = height;
      fb_info.layers = layers;

      const auto create_fb = g_real_create_framebuffer ? g_real_create_framebuffer :
          LoaderSymbol<PFN_vkCreateFramebuffer>("vkCreateFramebuffer");
      if (create_fb && create_fb(device, &fb_info, nullptr, &fb) == VK_SUCCESS) {
        g_framebuffers[fb_key] = fb;
      } else {
        __android_log_print(ANDROID_LOG_ERROR, kTag, "Failed to create emulated VkFramebuffer (%ux%u)", width, height);
      }
    }
  }

  if (fb == VK_NULL_HANDLE) return;

  VkRenderPassBeginInfo begin_info{};
  begin_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
  begin_info.renderPass = rp;
  begin_info.framebuffer = fb;
  begin_info.renderArea = pRenderingInfo->renderArea;
  begin_info.clearValueCount = static_cast<uint32_t>(clear_values.size());
  begin_info.pClearValues = clear_values.empty() ? nullptr : clear_values.data();

  g_real_cmd_begin_render_pass(commandBuffer, &begin_info, VK_SUBPASS_CONTENTS_INLINE);
}

EXPORT VKAPI_ATTR void VKAPI_CALL vkCmdBeginRenderingKHR(
    VkCommandBuffer commandBuffer, const VkRenderingInfoKHR* pRenderingInfo) {
  vkCmdBeginRendering(commandBuffer, pRenderingInfo);
}

EXPORT VKAPI_ATTR void VKAPI_CALL vkCmdEndRendering(VkCommandBuffer commandBuffer) {
  if (g_real_cmd_end_render_pass) {
    g_real_cmd_end_render_pass(commandBuffer);
  }
}

EXPORT VKAPI_ATTR void VKAPI_CALL vkCmdEndRenderingKHR(VkCommandBuffer commandBuffer) {
  vkCmdEndRendering(commandBuffer);
}

EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* pPresentInfo) {
  const auto real_fn = g_real_queue_present ? g_real_queue_present :
      LoaderSymbol<PFN_vkQueuePresentKHR>("vkQueuePresentKHR");
  if (!real_fn) return VK_ERROR_INITIALIZATION_FAILED;

  VkResult res = real_fn(queue, pPresentInfo);

  struct timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  uint64_t now_ms = static_cast<uint64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
  uint64_t last = s_last_save_time_ms.load(std::memory_order_relaxed);
  uint32_t pending = g_pipelines_since_last_save.load(std::memory_order_relaxed);

  // Rate-limit cache persistence to background thread at most once per 30 seconds
  if (pending >= 32 && (now_ms - last >= 30000)) {
    s_last_save_time_ms.store(now_ms, std::memory_order_relaxed);
    AsyncSavePipelineCache(g_device);
  }

  return res;
}

EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char* name) {
  if (!g_ready.load(std::memory_order_acquire) || !name) return nullptr;
  if (std::strcmp(name, "vkGetInstanceProcAddr") == 0)
    return reinterpret_cast<PFN_vkVoidFunction>(&vkGetInstanceProcAddr);
  if (std::strcmp(name, "vkGetDeviceProcAddr") == 0)
    return reinterpret_cast<PFN_vkVoidFunction>(&vkGetDeviceProcAddr);
  if (std::strcmp(name, "vkCreateInstance") == 0)
    return reinterpret_cast<PFN_vkVoidFunction>(&vkCreateInstance);
  if (std::strcmp(name, "vkDestroyInstance") == 0)
    return reinterpret_cast<PFN_vkVoidFunction>(&vkDestroyInstance);
  if (std::strcmp(name, "vkEnumerateInstanceVersion") == 0)
    return reinterpret_cast<PFN_vkVoidFunction>(&vkEnumerateInstanceVersion);
  if (std::strcmp(name, "vkEnumerateInstanceExtensionProperties") == 0)
    return reinterpret_cast<PFN_vkVoidFunction>(&vkEnumerateInstanceExtensionProperties);
  if (std::strcmp(name, "vkEnumerateInstanceLayerProperties") == 0)
    return reinterpret_cast<PFN_vkVoidFunction>(&vkEnumerateInstanceLayerProperties);
  if (std::strcmp(name, "vkEnumerateDeviceExtensionProperties") == 0)
    return reinterpret_cast<PFN_vkVoidFunction>(&vkEnumerateDeviceExtensionProperties);
  if (std::strcmp(name, "vkGetPhysicalDeviceFeatures2") == 0)
    return reinterpret_cast<PFN_vkVoidFunction>(&vkGetPhysicalDeviceFeatures2);
  if (std::strcmp(name, "vkGetPhysicalDeviceFeatures2KHR") == 0)
    return reinterpret_cast<PFN_vkVoidFunction>(&vkGetPhysicalDeviceFeatures2KHR);
  if (std::strcmp(name, "vkCreateDevice") == 0)
    return reinterpret_cast<PFN_vkVoidFunction>(&vkCreateDevice);
  if (std::strcmp(name, "vkDestroyDevice") == 0)
    return reinterpret_cast<PFN_vkVoidFunction>(&vkDestroyDevice);
  if (std::strcmp(name, "vkCreateGraphicsPipelines") == 0)
    return reinterpret_cast<PFN_vkVoidFunction>(&vkCreateGraphicsPipelines);
  if (std::strcmp(name, "vkCreateComputePipelines") == 0)
    return reinterpret_cast<PFN_vkVoidFunction>(&vkCreateComputePipelines);
  if (std::strcmp(name, "vkCreateImage") == 0)
    return reinterpret_cast<PFN_vkVoidFunction>(&vkCreateImage);
  if (std::strcmp(name, "vkDestroyImage") == 0)
    return reinterpret_cast<PFN_vkVoidFunction>(&vkDestroyImage);
  if (std::strcmp(name, "vkCreateImageView") == 0)
    return reinterpret_cast<PFN_vkVoidFunction>(&vkCreateImageView);
  if (std::strcmp(name, "vkDestroyImageView") == 0)
    return reinterpret_cast<PFN_vkVoidFunction>(&vkDestroyImageView);
  if (std::strcmp(name, "vkCmdBeginRendering") == 0)
    return reinterpret_cast<PFN_vkVoidFunction>(&vkCmdBeginRendering);
  if (std::strcmp(name, "vkCmdBeginRenderingKHR") == 0)
    return reinterpret_cast<PFN_vkVoidFunction>(&vkCmdBeginRenderingKHR);
  if (std::strcmp(name, "vkCmdEndRendering") == 0)
    return reinterpret_cast<PFN_vkVoidFunction>(&vkCmdEndRendering);
  if (std::strcmp(name, "vkCmdEndRenderingKHR") == 0)
    return reinterpret_cast<PFN_vkVoidFunction>(&vkCmdEndRenderingKHR);
  if (std::strcmp(name, "vkQueuePresentKHR") == 0)
    return reinterpret_cast<PFN_vkVoidFunction>(&vkQueuePresentKHR);

  const auto real = g_get(instance, name);
  return real;
}

EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device, const char* name) {
  if (!g_ready.load(std::memory_order_acquire) || !name) return nullptr;
  if (std::strcmp(name, "vkGetDeviceProcAddr") == 0)
    return reinterpret_cast<PFN_vkVoidFunction>(&vkGetDeviceProcAddr);
  if (std::strcmp(name, "vkCreateGraphicsPipelines") == 0)
    return reinterpret_cast<PFN_vkVoidFunction>(&vkCreateGraphicsPipelines);
  if (std::strcmp(name, "vkCreateComputePipelines") == 0)
    return reinterpret_cast<PFN_vkVoidFunction>(&vkCreateComputePipelines);
  if (std::strcmp(name, "vkCreateImage") == 0)
    return reinterpret_cast<PFN_vkVoidFunction>(&vkCreateImage);
  if (std::strcmp(name, "vkDestroyImage") == 0)
    return reinterpret_cast<PFN_vkVoidFunction>(&vkDestroyImage);
  if (std::strcmp(name, "vkCreateImageView") == 0)
    return reinterpret_cast<PFN_vkVoidFunction>(&vkCreateImageView);
  if (std::strcmp(name, "vkDestroyImageView") == 0)
    return reinterpret_cast<PFN_vkVoidFunction>(&vkDestroyImageView);
  if (std::strcmp(name, "vkCmdBeginRendering") == 0)
    return reinterpret_cast<PFN_vkVoidFunction>(&vkCmdBeginRendering);
  if (std::strcmp(name, "vkCmdBeginRenderingKHR") == 0)
    return reinterpret_cast<PFN_vkVoidFunction>(&vkCmdBeginRenderingKHR);
  if (std::strcmp(name, "vkCmdEndRendering") == 0)
    return reinterpret_cast<PFN_vkVoidFunction>(&vkCmdEndRendering);
  if (std::strcmp(name, "vkCmdEndRenderingKHR") == 0)
    return reinterpret_cast<PFN_vkVoidFunction>(&vkCmdEndRenderingKHR);
  if (std::strcmp(name, "vkDestroyDevice") == 0)
    return reinterpret_cast<PFN_vkVoidFunction>(&vkDestroyDevice);
  if (std::strcmp(name, "vkQueuePresentKHR") == 0)
    return reinterpret_cast<PFN_vkVoidFunction>(&vkQueuePresentKHR);

  const auto fn = reinterpret_cast<PFN_vkGetDeviceProcAddr>(dlsym(g_loader, "vkGetDeviceProcAddr"));
  return fn ? fn(device, name) : nullptr;
}

EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceVersion(uint32_t* version) {
  if (!g_ready.load(std::memory_order_acquire)) return VK_ERROR_INITIALIZATION_FAILED;
  const auto fn = Get<PFN_vkEnumerateInstanceVersion>(VK_NULL_HANDLE, "vkEnumerateInstanceVersion");
  if (fn) return fn(version);
  if (version) *version = VK_API_VERSION_1_0;
  return VK_SUCCESS;
}

EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceExtensionProperties(
    const char* layer, uint32_t* count, VkExtensionProperties* properties) {
  if (!g_ready.load(std::memory_order_acquire)) return VK_ERROR_INITIALIZATION_FAILED;
  const auto fn = Get<PFN_vkEnumerateInstanceExtensionProperties>(VK_NULL_HANDLE, "vkEnumerateInstanceExtensionProperties");
  return fn ? fn(layer, count, properties) : VK_ERROR_INITIALIZATION_FAILED;
}

EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceLayerProperties(uint32_t* count, VkLayerProperties* properties) {
  if (!g_ready.load(std::memory_order_acquire)) return VK_ERROR_INITIALIZATION_FAILED;
  const auto fn = Get<PFN_vkEnumerateInstanceLayerProperties>(VK_NULL_HANDLE, "vkEnumerateInstanceLayerProperties");
  return fn ? fn(count, properties) : VK_ERROR_INITIALIZATION_FAILED;
}
