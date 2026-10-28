#include <hardware/hardware.h>
#include <hardware/hwvulkan.h>

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The vendor HAL is a Vulkan 1.1-era Android driver.  The host loader may be
 * much newer (1.4.x) and will forward its own instance version unless the ICD
 * advertises and enforces a lower one.  Report 1.1 and clamp CreateInstance. */
#define SYSVK_API_VERSION VK_API_VERSION_1_1

/*
 * VK_EXT_robustness2 shim
 * =======================
 *
 * DDK 1.18 on the GE8322 does not expose VK_EXT_robustness2 at all, and Mesa's
 * Zink refuses to create a screen without its nullDescriptor feature:
 *
 *   Zink requires the nullDescriptor feature of KHR/EXT robustness2.
 *
 * The device already exposes everything else Zink needs (dynamic rendering,
 * synchronization2, timeline semaphores, imageless framebuffers, maintenance
 * 1-4, scalar block layout, push descriptors), so this shim supplies the one
 * missing feature:
 *
 *   - vkEnumerateDeviceExtensionProperties() appends VK_EXT_robustness2.
 *   - vkGetPhysicalDeviceFeatures2() reports nullDescriptor = VK_TRUE.
 *   - vkGetPhysicalDeviceProperties2() fills the matching properties struct.
 *   - vkCreateDevice() removes the extension name before calling the vendor,
 *     which would otherwise fail with VK_ERROR_EXTENSION_NOT_PRESENT.
 */

#define SYSVK_RB2_EXTENSION_NAME "VK_EXT_robustness2"
#define SYSVK_RB2_SPEC_VERSION 1

/* The sysroot headers are new enough for these, but keep the shim buildable
 * against older Android header sets as well. */
#ifndef VK_EXT_robustness2
#define VK_EXT_robustness2 1
#define VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT ((VkStructureType)1000286000)
#define VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_PROPERTIES_EXT ((VkStructureType)1000286001)
typedef struct VkPhysicalDeviceRobustness2FeaturesEXT {
  VkStructureType sType;
  void *pNext;
  VkBool32 robustBufferAccess2;
  VkBool32 robustImageAccess2;
  VkBool32 nullDescriptor;
} VkPhysicalDeviceRobustness2FeaturesEXT;
typedef struct VkPhysicalDeviceRobustness2PropertiesEXT {
  VkStructureType sType;
  void *pNext;
  VkDeviceSize robustStorageBufferAccessSizeAlignment;
  VkDeviceSize robustUniformBufferAccessSizeAlignment;
} VkPhysicalDeviceRobustness2PropertiesEXT;
#endif

static hwvulkan_device_t *sysvk_device = NULL;
static pthread_once_t sysvk_once = PTHREAD_ONCE_INIT;

/* The vendor HAL's dispatch, captured once the first instance exists.  The
 * loader talks to this ICD through the functions returned below, so they have
 * to be able to reach the vendor entry points.  One instance is assumed, which
 * matches every Vulkan consumer on this device (Zink, vulkaninfo, vkmark). */
static VkInstance sysvk_instance = VK_NULL_HANDLE;
static VkDevice sysvk_device_handle = VK_NULL_HANDLE;
static PFN_vkGetInstanceProcAddr sysvk_next_gipa = NULL;
static PFN_vkGetDeviceProcAddr sysvk_next_gdpa = NULL;

static void sysvk_init(void) {
  hw_module_t *module = NULL;
  if (hw_get_module(HWVULKAN_HARDWARE_MODULE_ID, (const hw_module_t **)&module) != 0) {
    return;
  }
  if (module->methods->open(module, HWVULKAN_DEVICE_0, (hw_device_t **)&sysvk_device) != 0) {
    sysvk_device = NULL;
  }
}

static int sysvk_debug = -1;

/* VK_EXT_robustness2 is advertised, not implemented: the vendor has no
 * robustness2 support, so nullDescriptor is only safe for clients that use it
 * the way Zink does (unused bindings, no actual null access).  Advertising it
 * to every client would hand DXVK/vkd3d a capability that does not exist, so
 * the shim is opt-in.  SYSVK_ENABLE_RB2=1 is set by the Zink test environment
 * only; every other application sees the vendor's real extension list. */
static int sysvk_rb2 = -1;

static int sysvk_rb2_on(void) {
  if (sysvk_rb2 < 0) {
    const char *e = getenv("SYSVK_ENABLE_RB2");
    sysvk_rb2 = (e != NULL && *e != '\0' && strcmp(e, "0") != 0) ? 1 : 0;
  }
  return sysvk_rb2;
}

static int sysvk_debug_on(void) {
  if (sysvk_debug < 0) {
    const char *e = getenv("SYSVK_DEBUG");
    sysvk_debug = (e != NULL && *e != '\0' && strcmp(e, "0") != 0) ? 1 : 0;
  }
  return sysvk_debug;
}

#define SYSVK_LOG(...)                          \
  do {                                          \
    if (sysvk_debug_on()) {                     \
      fprintf(stderr, "[sysvk] " __VA_ARGS__);  \
      fputc('\n', stderr);                      \
    }                                           \
  } while (0)

static PFN_vkVoidFunction sysvk_next_instance_proc(const char *name) {
  if (sysvk_next_gipa == NULL) {
    return NULL;
  }
  return sysvk_next_gipa(sysvk_instance, name);
}

static void sysvk_dump_gipa(void);

__attribute__((constructor)) static void sysvk_loaded(void) {
  SYSVK_LOG("libsysvk loaded");
}

VKAPI_ATTR VkResult VKAPI_CALL
sysvk_EnumerateInstanceVersion(uint32_t *pApiVersion) {
  *pApiVersion = SYSVK_API_VERSION;
  return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
sysvk_CreateInstance(const VkInstanceCreateInfo *pCreateInfo,
                     const VkAllocationCallbacks *pAllocator,
                     VkInstance *pInstance) {
  if (pCreateInfo == NULL || pInstance == NULL) {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  pthread_once(&sysvk_once, sysvk_init);
  if (!sysvk_device) {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  VkInstanceCreateInfo info = *pCreateInfo;
  VkApplicationInfo app = {0};
  app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  app.apiVersion = VK_API_VERSION_1_0;
  if (pCreateInfo->pApplicationInfo != NULL) {
    app = *pCreateInfo->pApplicationInfo;
    if (app.apiVersion > SYSVK_API_VERSION) {
      app.apiVersion = SYSVK_API_VERSION;
    }
  }
  info.pApplicationInfo = &app;

  VkResult result = sysvk_device->CreateInstance(&info, pAllocator, pInstance);
  if (result == VK_SUCCESS && *pInstance != VK_NULL_HANDLE) {
    sysvk_instance = *pInstance;
    sysvk_next_gipa = sysvk_device->GetInstanceProcAddr;
    sysvk_next_gdpa =
        (PFN_vkGetDeviceProcAddr)sysvk_next_gipa(*pInstance, "vkGetDeviceProcAddr");
  }
  SYSVK_LOG("CreateInstance r=%d instance=%p gdpa=%p", result,
            (void *)(uintptr_t)*pInstance, (void *)sysvk_next_gdpa);
  if (result == VK_SUCCESS) {
    sysvk_dump_gipa();
  }
  return result;
}

/*
 * Device extension enumeration: pass the vendor list through and append
 * VK_EXT_robustness2 when it is missing.
 */
VKAPI_ATTR VkResult VKAPI_CALL
sysvk_EnumerateDeviceExtensionProperties(VkPhysicalDevice physicalDevice,
                                         const char *pLayerName,
                                         uint32_t *pPropertyCount,
                                         VkExtensionProperties *pProperties) {
  PFN_vkEnumerateDeviceExtensionProperties next =
      (PFN_vkEnumerateDeviceExtensionProperties)
          sysvk_next_instance_proc("vkEnumerateDeviceExtensionProperties");

  if (next == NULL || pPropertyCount == NULL) {
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  if (pLayerName != NULL || !sysvk_rb2_on()) {
    return next(physicalDevice, pLayerName, pPropertyCount, pProperties);
  }

  uint32_t count = 0;
  VkResult result = next(physicalDevice, NULL, &count, NULL);
  if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
    return result;
  }

  VkExtensionProperties *vendor = NULL;
  if (count > 0) {
    vendor = calloc(count, sizeof(*vendor));
    if (vendor == NULL) {
      return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    uint32_t written = count;
    result = next(physicalDevice, NULL, &written, vendor);
    if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
      free(vendor);
      return result;
    }
    count = written;
  }

  bool have_rb2 = false;
  for (uint32_t i = 0; i < count; i++) {
    if (strcmp(vendor[i].extensionName, SYSVK_RB2_EXTENSION_NAME) == 0) {
      have_rb2 = true;
      break;
    }
  }

  uint32_t total = count + (have_rb2 ? 0u : 1u);

  if (pProperties == NULL) {
    *pPropertyCount = total;
    free(vendor);
    return VK_SUCCESS;
  }

  uint32_t copy = *pPropertyCount < total ? *pPropertyCount : total;
  for (uint32_t i = 0; i < copy && i < count; i++) {
    pProperties[i] = vendor[i];
  }
  if (!have_rb2 && copy > count) {
    VkExtensionProperties *added = &pProperties[count];
    memset(added, 0, sizeof(*added));
    memcpy(added->extensionName, SYSVK_RB2_EXTENSION_NAME,
           sizeof(SYSVK_RB2_EXTENSION_NAME));
    added->specVersion = SYSVK_RB2_SPEC_VERSION;
  }
  *pPropertyCount = copy;
  free(vendor);
  return copy < total ? VK_INCOMPLETE : VK_SUCCESS;
}

static void sysvk_patch_features2(void *pNext) {
  if (!sysvk_rb2_on()) {
    return;
  }
  for (VkBaseOutStructure *s = (VkBaseOutStructure *)pNext; s != NULL; s = s->pNext) {
    if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT) {
      VkPhysicalDeviceRobustness2FeaturesEXT *rb2 =
          (VkPhysicalDeviceRobustness2FeaturesEXT *)s;

      /* The vendor has neither robustness2 mode; only null descriptors are
       * emulated, so do not claim more than that. */
      rb2->robustBufferAccess2 = VK_FALSE;
      rb2->robustImageAccess2 = VK_FALSE;
      rb2->nullDescriptor = VK_TRUE;
    }
  }
}

VKAPI_ATTR void VKAPI_CALL
sysvk_GetPhysicalDeviceFeatures2(VkPhysicalDevice physicalDevice,
                                 VkPhysicalDeviceFeatures2 *pFeatures) {
  PFN_vkGetPhysicalDeviceFeatures2 next =
      (PFN_vkGetPhysicalDeviceFeatures2)
          sysvk_next_instance_proc("vkGetPhysicalDeviceFeatures2");
  if (next == NULL) {
    next = (PFN_vkGetPhysicalDeviceFeatures2)
        sysvk_next_instance_proc("vkGetPhysicalDeviceFeatures2KHR");
  }
  if (next == NULL || pFeatures == NULL) {
    return;
  }

  next(physicalDevice, pFeatures);
  sysvk_patch_features2(pFeatures->pNext);

  if (sysvk_debug_on()) {
    SYSVK_LOG("GetPhysicalDeviceFeatures2 robustBufferAccess=%d shaderInt64=%d "
              "independentBlend=%d samplerAnisotropy=%d imageCubeArray=%d",
              pFeatures->features.robustBufferAccess, pFeatures->features.shaderInt64,
              pFeatures->features.independentBlend, pFeatures->features.samplerAnisotropy,
              pFeatures->features.imageCubeArray);
    for (VkBaseOutStructure *s = (VkBaseOutStructure *)pFeatures->pNext; s != NULL; s = s->pNext) {
      SYSVK_LOG("  features2 chain sType=%d", (int)s->sType);
    }
  }
}

static void sysvk_patch_properties2(void *pNext) {
  if (!sysvk_rb2_on()) {
    return;
  }
  for (VkBaseOutStructure *s = (VkBaseOutStructure *)pNext; s != NULL; s = s->pNext) {
    if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_PROPERTIES_EXT) {
      VkPhysicalDeviceRobustness2PropertiesEXT *rb2 =
          (VkPhysicalDeviceRobustness2PropertiesEXT *)s;

      /* Null descriptors are replaced by one dummy resource, so no special
       * alignment is required from the application. */
      rb2->robustStorageBufferAccessSizeAlignment = 1;
      rb2->robustUniformBufferAccessSizeAlignment = 1;
    }
  }
}

VKAPI_ATTR void VKAPI_CALL
sysvk_GetPhysicalDeviceProperties2(VkPhysicalDevice physicalDevice,
                                   VkPhysicalDeviceProperties2 *pProperties) {
  PFN_vkGetPhysicalDeviceProperties2 next =
      (PFN_vkGetPhysicalDeviceProperties2)
          sysvk_next_instance_proc("vkGetPhysicalDeviceProperties2");
  if (next == NULL) {
    next = (PFN_vkGetPhysicalDeviceProperties2)
        sysvk_next_instance_proc("vkGetPhysicalDeviceProperties2KHR");
  }
  if (next == NULL || pProperties == NULL) {
    return;
  }

  next(physicalDevice, pProperties);
  sysvk_patch_properties2(pProperties->pNext);
}

VKAPI_ATTR VkResult VKAPI_CALL
sysvk_CreateDevice(VkPhysicalDevice physicalDevice,
                   const VkDeviceCreateInfo *pCreateInfo,
                   const VkAllocationCallbacks *pAllocator, VkDevice *pDevice) {
  PFN_vkCreateDevice next =
      (PFN_vkCreateDevice)sysvk_next_instance_proc("vkCreateDevice");

  if (next == NULL || pCreateInfo == NULL || pDevice == NULL) {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  SYSVK_LOG("CreateDevice ext=%u pNext=%p", pCreateInfo->enabledExtensionCount,
            pCreateInfo->pNext);

  bool enable_rb2 = false;
  if (sysvk_rb2_on()) {
    for (uint32_t i = 0; i < pCreateInfo->enabledExtensionCount; i++) {
      const char *name = pCreateInfo->ppEnabledExtensionNames[i];
      if (name != NULL && strcmp(name, SYSVK_RB2_EXTENSION_NAME) == 0) {
        enable_rb2 = true;
        break;
      }
    }
  }

  if (!enable_rb2) {
    VkResult result = next(physicalDevice, pCreateInfo, pAllocator, pDevice);
    if (result == VK_SUCCESS && pDevice != NULL && *pDevice != VK_NULL_HANDLE) {
      sysvk_device_handle = *pDevice;
    }
    SYSVK_LOG("CreateDevice r=%d device=%p rb2=0", result,
              (void *)(uintptr_t)(pDevice ? *pDevice : VK_NULL_HANDLE));
    return result;
  }

  /* Strip only the extension name.  Feature structs in the pNext chain that
   * the vendor does not recognise are ignored by the driver, which is what the
   * specification requires for unknown extension structures. */
  VkDeviceCreateInfo info = *pCreateInfo;
  const char **names = NULL;
  uint32_t kept = 0;

  if (pCreateInfo->enabledExtensionCount > 0) {
    names = calloc(pCreateInfo->enabledExtensionCount, sizeof(*names));
    if (names == NULL) {
      return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    for (uint32_t i = 0; i < pCreateInfo->enabledExtensionCount; i++) {
      const char *name = pCreateInfo->ppEnabledExtensionNames[i];
      if (name != NULL && strcmp(name, SYSVK_RB2_EXTENSION_NAME) == 0) {
        continue;
      }
      names[kept++] = name;
    }
  }

  info.enabledExtensionCount = kept;
  info.ppEnabledExtensionNames = names;

  VkResult result = next(physicalDevice, &info, pAllocator, pDevice);
  free(names);
  if (result == VK_SUCCESS && pDevice != NULL && *pDevice != VK_NULL_HANDLE) {
    sysvk_device_handle = *pDevice;
  }
  SYSVK_LOG("CreateDevice r=%d device=%p rb2=%d", result,
            (void *)(uintptr_t)(pDevice ? *pDevice : VK_NULL_HANDLE), (int)enable_rb2);
  return result;
}

/* Resolves a device-level entry point on the vendor driver.  The device handle
 * is recorded for every client, not just the robustness2 ones: without it the
 * submit fix below cannot reach the vendor at all. */
static PFN_vkVoidFunction sysvk_next_device_proc(const char *name) {
  if (sysvk_next_gdpa == NULL || sysvk_device_handle == VK_NULL_HANDLE) {
    return NULL;
  }
  return sysvk_next_gdpa(sysvk_device_handle, name);
}

/* Maps a pointer inside the vendor module to its ELF virtual address so it can
 * be matched against a disassembly.  The module is mapped by the Android linker
 * and is not in the glibc link map, so /proc/self/maps is the reliable source. */
static unsigned long sysvk_module_vaddr(const void *ptr) {
  if (ptr == NULL) {
    return 0;
  }
  FILE *f = fopen("/proc/self/maps", "r");
  if (f == NULL) {
    return 0;
  }
  unsigned long want = (unsigned long)(uintptr_t)ptr;
  unsigned long vaddr = 0;
  char line[512];
  while (fgets(line, sizeof(line), f) != NULL) {
    unsigned long start = 0, end = 0, off = 0;
    if (sscanf(line, "%lx-%lx %*s %lx", &start, &end, &off) != 3) {
      continue;
    }
    if (want >= start && want < end) {
      vaddr = want - start + off;
      break;
    }
  }
  fclose(f);
  return vaddr;
}

static const char *const sysvk_gipa_names[] = {
    "vkDestroyInstance", "vkEnumeratePhysicalDevices", "vkGetPhysicalDeviceFeatures",
    "vkGetPhysicalDeviceFormatProperties", "vkGetPhysicalDeviceImageFormatProperties",
    "vkGetPhysicalDeviceProperties", "vkGetPhysicalDeviceQueueFamilyProperties",
    "vkGetPhysicalDeviceMemoryProperties", "vkGetInstanceProcAddr",
    "vkGetDeviceProcAddr", "vkCreateDevice", "vkEnumerateDeviceExtensionProperties",
    "vkEnumerateDeviceLayerProperties", "vkGetPhysicalDeviceSparseImageFormatProperties",
    "vkGetPhysicalDeviceFeatures2", "vkGetPhysicalDeviceProperties2",
    "vkGetPhysicalDeviceFormatProperties2", "vkGetPhysicalDeviceImageFormatProperties2",
    "vkGetPhysicalDeviceQueueFamilyProperties2",
    "vkGetPhysicalDeviceMemoryProperties2",
    "vkGetPhysicalDeviceSparseImageFormatProperties2", "vkGetPhysicalDeviceExternalBufferProperties",
    "vkGetPhysicalDeviceExternalFenceProperties",
    "vkGetPhysicalDeviceExternalSemaphoreProperties",
    "vkDestroyDevice", "vkGetDeviceQueue", "vkGetDeviceQueue2", "vkQueueSubmit", "vkQueueWaitIdle",
    "vkDeviceWaitIdle", "vkAllocateMemory", "vkFreeMemory", "vkMapMemory", "vkUnmapMemory",
    "vkFlushMappedMemoryRanges", "vkInvalidateMappedMemoryRanges", "vkGetDeviceMemoryCommitment",
    "vkBindBufferMemory", "vkBindImageMemory", "vkGetBufferMemoryRequirements",
    "vkGetImageMemoryRequirements", "vkGetImageSparseMemoryRequirements",
    "vkGetPhysicalDeviceSparseImageFormatProperties2", "vkQueueBindSparse",
    "vkCreateFence", "vkDestroyFence", "vkResetFences", "vkGetFenceStatus", "vkWaitForFences",
    "vkCreateSemaphore", "vkDestroySemaphore", "vkCreateEvent", "vkDestroyEvent", "vkGetEventStatus",
    "vkSetEvent", "vkResetEvent", "vkCreateQueryPool", "vkDestroyQueryPool",
    "vkGetQueryPoolResults", "vkCreateBuffer", "vkDestroyBuffer", "vkCreateBufferView",
    "vkDestroyBufferView", "vkCreateImage", "vkDestroyImage", "vkGetImageSubresourceLayout",
    "vkCreateImageView", "vkDestroyImageView", "vkCreateShaderModule", "vkDestroyShaderModule",
    "vkCreatePipelineCache", "vkDestroyPipelineCache", "vkGetPipelineCacheData",
    "vkMergePipelineCaches", "vkCreateGraphicsPipelines", "vkCreateComputePipelines",
    "vkDestroyPipeline", "vkCreatePipelineLayout", "vkDestroyPipelineLayout",
    "vkCreateSampler", "vkDestroySampler", "vkCreateDescriptorSetLayout",
    "vkDestroyDescriptorSetLayout", "vkCreateDescriptorPool", "vkDestroyDescriptorPool",
    "vkResetDescriptorPool", "vkAllocateDescriptorSets", "vkFreeDescriptorSets",
    "vkUpdateDescriptorSets", "vkCreateFramebuffer", "vkDestroyFramebuffer",
    "vkCreateRenderPass", "vkDestroyRenderPass", "vkGetRenderAreaGranularity",
    "vkCreateCommandPool", "vkDestroyCommandPool", "vkResetCommandPool",
    "vkAllocateCommandBuffers", "vkFreeCommandBuffers", "vkBeginCommandBuffer",
    "vkEndCommandBuffer", "vkResetCommandBuffer", "vkCmdBindPipeline",
    "vkCmdSetViewport", "vkCmdSetScissor", "vkCmdBindDescriptorSets", "vkCmdBindVertexBuffers",
    "vkCmdBindIndexBuffer", "vkCmdDraw", "vkCmdDrawIndexed", "vkCmdDrawIndirect",
    "vkCmdDrawIndexedIndirect", "vkCmdDispatch", "vkCmdDispatchIndirect",
    "vkCmdCopyBuffer", "vkCmdCopyImage", "vkCmdBlitImage", "vkCmdCopyBufferToImage",
    "vkCmdCopyImageToBuffer", "vkCmdUpdateBuffer", "vkCmdFillBuffer", "vkCmdClearColorImage",
    "vkCmdClearDepthStencilImage", "vkCmdClearAttachments", "vkCmdResolveImage",
    "vkCmdSetEvent", "vkCmdResetEvent", "vkCmdWaitEvents", "vkCmdPipelineBarrier",
    "vkCmdBeginQuery", "vkCmdEndQuery", "vkCmdResetQueryPool", "vkCmdWriteTimestamp",
    "vkCmdCopyQueryPoolResults", "vkCmdPushConstants", "vkCmdBeginRenderPass",
    "vkCmdNextSubpass", "vkCmdEndRenderPass", "vkCmdExecuteCommands",
    "vkCreateDescriptorUpdateTemplate", "vkDestroyDescriptorUpdateTemplate",
    "vkUpdateDescriptorSetWithTemplate", "vkCmdPushDescriptorSetKHR",
    "vkCmdPushDescriptorSetWithTemplateKHR", "vkCreateSwapchainKHR",
    "vkDestroySwapchainKHR", "vkGetSwapchainImagesKHR", "vkAcquireNextImageKHR",
    "vkQueuePresentKHR", "vkCreateRenderPass2", "vkCmdBeginRenderPass2",
    "vkCmdNextSubpass2", "vkCmdEndRenderPass2", "vkGetSemaphoreCounterValue",
    "vkWaitSemaphores", "vkSignalSemaphore", "vkGetBufferDeviceAddress",
    "vkCmdBeginRendering", "vkCmdEndRendering", "vkCmdPipelineBarrier2",
    "vkCmdSetCullMode", "vkCmdSetFrontFace", "vkCmdSetPrimitiveTopology",
    "vkCmdSetDepthTestEnable", "vkCmdSetDepthWriteEnable", "vkCmdSetStencilTestEnable",
    "vkCmdSetDepthCompareOp", "vkCmdSetDepthBoundsTestEnable",
    "vkCmdSetRasterizerDiscardEnable", "vkCmdSetVertexInputEXT",
    "vkCmdBindVertexBuffers2", "vkCmdSetScissorWithCount",
    "vkCmdSetViewportWithCount", "vkCmdDrawIndirectCount",
    "vkCmdDrawIndexedIndirectCount", "vkCreateGraphicsPipelines2",
};

static void sysvk_dump_gipa(void) {
  if (!sysvk_debug_on() || sysvk_next_gipa == NULL) {
    return;
  }
  for (size_t i = 0; i < sizeof(sysvk_gipa_names) / sizeof(sysvk_gipa_names[0]); i++) {
    PFN_vkVoidFunction p = sysvk_next_gipa(sysvk_instance, sysvk_gipa_names[i]);
    if (p == NULL) {
      continue;
    }
    SYSVK_LOG("gipa %-46s %p vaddr=0x%lx", sysvk_gipa_names[i], (void *)p,
              sysvk_module_vaddr((const void *)p));
  }
}

/*
 * Empty submits
 * =============
 *
 * The vendor's vkQueueSubmit dereferences the first command buffer of every
 * submit entry, so a submit with commandBufferCount == 0 (which Zink uses to
 * signal a timeline semaphore without work) crashes it.  Vulkan allows such a
 * submit, and it is semantically a no-op apart from the semaphore/fence
 * operations, so this ICD substitutes a recorded empty command buffer.  That
 * keeps the semaphore signalling identical instead of dropping it.
 *
 * The queue family is needed to create a matching command pool; it is recorded
 * from vkGetDeviceQueue()/vkGetDeviceQueue2().
 */

#define SYSVK_MAX_QUEUES 32
#define SYSVK_MAX_FAMILIES 8

static VkQueue sysvk_queue_handles[SYSVK_MAX_QUEUES];
static uint32_t sysvk_queue_families[SYSVK_MAX_QUEUES];
static uint32_t sysvk_queue_map_count = 0;
static VkCommandPool sysvk_empty_pools[SYSVK_MAX_FAMILIES];
static VkCommandBuffer sysvk_empty_cmds[SYSVK_MAX_FAMILIES];
static pthread_mutex_t sysvk_queue_lock = PTHREAD_MUTEX_INITIALIZER;

static void sysvk_record_queue(VkQueue queue, uint32_t family) {
  if (queue == VK_NULL_HANDLE) {
    return;
  }
  pthread_mutex_lock(&sysvk_queue_lock);
  for (uint32_t i = 0; i < sysvk_queue_map_count; i++) {
    if (sysvk_queue_handles[i] == queue) {
      sysvk_queue_families[i] = family;
      pthread_mutex_unlock(&sysvk_queue_lock);
      return;
    }
  }
  if (sysvk_queue_map_count < SYSVK_MAX_QUEUES) {
    sysvk_queue_handles[sysvk_queue_map_count] = queue;
    sysvk_queue_families[sysvk_queue_map_count] = family;
    sysvk_queue_map_count++;
  }
  pthread_mutex_unlock(&sysvk_queue_lock);
}

static uint32_t sysvk_queue_family_of(VkQueue queue) {
  uint32_t family = 0;
  pthread_mutex_lock(&sysvk_queue_lock);
  for (uint32_t i = 0; i < sysvk_queue_map_count; i++) {
    if (sysvk_queue_handles[i] == queue) {
      family = sysvk_queue_families[i];
      break;
    }
  }
  pthread_mutex_unlock(&sysvk_queue_lock);
  return family;
}

static VkCommandBuffer sysvk_empty_cmdbuf(VkQueue queue) {
  uint32_t family = sysvk_queue_family_of(queue);
  if (family >= SYSVK_MAX_FAMILIES) {
    family = 0;
  }

  pthread_mutex_lock(&sysvk_queue_lock);
  if (sysvk_empty_cmds[family] == VK_NULL_HANDLE) {
    PFN_vkCreateCommandPool create_pool =
        (PFN_vkCreateCommandPool)sysvk_next_device_proc("vkCreateCommandPool");
    PFN_vkAllocateCommandBuffers alloc_cmds =
        (PFN_vkAllocateCommandBuffers)sysvk_next_device_proc("vkAllocateCommandBuffers");
    PFN_vkBeginCommandBuffer begin_cmd =
        (PFN_vkBeginCommandBuffer)sysvk_next_device_proc("vkBeginCommandBuffer");
    PFN_vkEndCommandBuffer end_cmd =
        (PFN_vkEndCommandBuffer)sysvk_next_device_proc("vkEndCommandBuffer");

    if (create_pool != NULL && alloc_cmds != NULL && begin_cmd != NULL && end_cmd != NULL) {
      VkCommandPoolCreateInfo pool_info = {0};
      pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
      pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
      pool_info.queueFamilyIndex = family;

      VkCommandPool pool = VK_NULL_HANDLE;
      if (create_pool(sysvk_device_handle, &pool_info, NULL, &pool) == VK_SUCCESS) {
        VkCommandBufferAllocateInfo alloc_info = {0};
        alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        alloc_info.commandPool = pool;
        alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc_info.commandBufferCount = 1;

        VkCommandBuffer cmd = VK_NULL_HANDLE;
        if (alloc_cmds(sysvk_device_handle, &alloc_info, &cmd) == VK_SUCCESS) {
          VkCommandBufferBeginInfo begin_info = {0};
          begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
          begin_info.flags = VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT;
          if (begin_cmd(cmd, &begin_info) == VK_SUCCESS && end_cmd(cmd) == VK_SUCCESS) {
            sysvk_empty_pools[family] = pool;
            sysvk_empty_cmds[family] = cmd;
            SYSVK_LOG("created empty command buffer %p for queue family %u",
                      (void *)(uintptr_t)cmd, family);
          }
        }
      }
    }
  }
  VkCommandBuffer cmd = sysvk_empty_cmds[family];
  pthread_mutex_unlock(&sysvk_queue_lock);
  return cmd;
}

VKAPI_ATTR void VKAPI_CALL
sysvk_GetDeviceQueue(VkDevice device, uint32_t queueFamilyIndex, uint32_t queueIndex,
                     VkQueue *pQueue) {
  PFN_vkGetDeviceQueue next =
      (PFN_vkGetDeviceQueue)sysvk_next_device_proc("vkGetDeviceQueue");
  if (next == NULL) {
    return;
  }
  next(device, queueFamilyIndex, queueIndex, pQueue);
  if (pQueue != NULL) {
    sysvk_record_queue(*pQueue, queueFamilyIndex);
  }
}

VKAPI_ATTR void VKAPI_CALL
sysvk_GetDeviceQueue2(VkDevice device, const VkDeviceQueueInfo2 *pQueueInfo, VkQueue *pQueue) {
  PFN_vkGetDeviceQueue2 next =
      (PFN_vkGetDeviceQueue2)sysvk_next_device_proc("vkGetDeviceQueue2");
  if (next == NULL) {
    return;
  }
  next(device, pQueueInfo, pQueue);
  if (pQueue != NULL && pQueueInfo != NULL) {
    sysvk_record_queue(*pQueue, pQueueInfo->queueFamilyIndex);
  }
}

VKAPI_ATTR VkResult VKAPI_CALL
sysvk_QueueSubmit(VkQueue queue, uint32_t submitCount, const VkSubmitInfo *pSubmits,
                  VkFence fence) {
  PFN_vkQueueSubmit next = (PFN_vkQueueSubmit)sysvk_next_device_proc("vkQueueSubmit");
  if (next == NULL) {
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  if (sysvk_debug_on()) {
    SYSVK_LOG("QueueSubmit submits=%u fence=%p", submitCount, (void *)(uintptr_t)fence);
    for (uint32_t i = 0; i < submitCount; i++) {
      const VkSubmitInfo *s = &pSubmits[i];
      SYSVK_LOG("  submit[%u] cmds=%u waits=%u signals=%u pNext=%p sType=%u", i,
                s->commandBufferCount, s->waitSemaphoreCount, s->signalSemaphoreCount,
                s->pNext, s->pNext != NULL ? *(const uint32_t *)s->pNext : 0u);
      for (uint32_t j = 0; j < s->commandBufferCount; j++) {
        SYSVK_LOG("    cmdbuf %p", (void *)(uintptr_t)s->pCommandBuffers[j]);
      }
      for (uint32_t j = 0; j < s->waitSemaphoreCount; j++) {
        SYSVK_LOG("    wait %p stage=0x%x", (void *)(uintptr_t)s->pWaitSemaphores[j],
                  s->pWaitDstStageMask[j]);
      }
      for (uint32_t j = 0; j < s->signalSemaphoreCount; j++) {
        SYSVK_LOG("    signal %p", (void *)(uintptr_t)s->pSignalSemaphores[j]);
      }
    }
  }

  /* Nothing to repair: hand the submit straight to the vendor so ordinary
   * clients never touch the copy path below. */
  bool need_fix = false;
  for (uint32_t i = 0; i < submitCount; i++) {
    if (pSubmits[i].commandBufferCount == 0) {
      need_fix = true;
      break;
    }
  }
  if (!need_fix) {
    return next(queue, submitCount, pSubmits, fence);
  }

  VkSubmitInfo stack_subs[8];
  VkCommandBuffer stack_cmds[8];
  VkSubmitInfo *subs = stack_subs;
  VkCommandBuffer *cmds = stack_cmds;
  if (submitCount > 8) {
    subs = calloc(submitCount, sizeof(*subs));
    cmds = calloc(submitCount, sizeof(*cmds));
    if (subs == NULL || cmds == NULL) {
      free(subs);
      free(cmds);
      return next(queue, submitCount, pSubmits, fence);
    }
  }

  uint32_t out_count = 0;
  for (uint32_t i = 0; i < submitCount; i++) {
    VkSubmitInfo s = pSubmits[i];
    if (s.commandBufferCount == 0) {
      VkCommandBuffer empty = sysvk_empty_cmdbuf(queue);
      if (empty == VK_NULL_HANDLE) {
        SYSVK_LOG("  dropping empty submit[%u]: no placeholder command buffer", i);
        continue;
      }
      cmds[out_count] = empty;
      s.commandBufferCount = 1;
      s.pCommandBuffers = &cmds[out_count];
    }
    subs[out_count++] = s;
  }

  VkResult result = VK_SUCCESS;
  if (out_count > 0) {
    result = next(queue, out_count, subs, fence);
  }
  if (subs != stack_subs) {
    free(subs);
    free(cmds);
  }
  return result;
}

VKAPI_ATTR VkResult VKAPI_CALL
sysvk_QueueSubmit2(VkQueue queue, uint32_t submitCount, const VkSubmitInfo2 *pSubmits,
                   VkFence fence) {
  PFN_vkQueueSubmit2 next = (PFN_vkQueueSubmit2)sysvk_next_device_proc("vkQueueSubmit2");
  if (next == NULL) {
    next = (PFN_vkQueueSubmit2)sysvk_next_device_proc("vkQueueSubmit2KHR");
  }
  if (next == NULL) {
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  SYSVK_LOG("QueueSubmit2 submits=%u fence=%p", submitCount, (void *)(uintptr_t)fence);

  bool need_fix = false;
  for (uint32_t i = 0; i < submitCount; i++) {
    if (pSubmits[i].commandBufferInfoCount == 0) {
      need_fix = true;
      break;
    }
  }
  if (!need_fix) {
    return next(queue, submitCount, pSubmits, fence);
  }

  VkSubmitInfo2 stack_subs[8];
  VkCommandBufferSubmitInfo stack_cmds[8];
  VkSubmitInfo2 *subs = stack_subs;
  VkCommandBufferSubmitInfo *cmds = stack_cmds;
  if (submitCount > 8) {
    subs = calloc(submitCount, sizeof(*subs));
    cmds = calloc(submitCount, sizeof(*cmds));
    if (subs == NULL || cmds == NULL) {
      free(subs);
      free(cmds);
      return next(queue, submitCount, pSubmits, fence);
    }
  }

  uint32_t out_count = 0;
  for (uint32_t i = 0; i < submitCount; i++) {
    VkSubmitInfo2 s = pSubmits[i];
    if (s.commandBufferInfoCount == 0) {
      VkCommandBuffer empty = sysvk_empty_cmdbuf(queue);
      if (empty == VK_NULL_HANDLE) {
        continue;
      }
      cmds[out_count].sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
      cmds[out_count].pNext = NULL;
      cmds[out_count].commandBuffer = empty;
      cmds[out_count].deviceMask = 0;
      s.commandBufferInfoCount = 1;
      s.pCommandBufferInfos = &cmds[out_count];
    }
    subs[out_count++] = s;
  }

  VkResult result = VK_SUCCESS;
  if (out_count > 0) {
    result = next(queue, out_count, subs, fence);
  }
  if (subs != stack_subs) {
    free(subs);
    free(cmds);
  }
  return result;
}

static const struct {
  const char *name;
  PFN_vkVoidFunction function;
} sysvk_device_overrides[] = {
    {"vkGetDeviceQueue", (PFN_vkVoidFunction)sysvk_GetDeviceQueue},
    {"vkGetDeviceQueue2", (PFN_vkVoidFunction)sysvk_GetDeviceQueue2},
    {"vkQueueSubmit", (PFN_vkVoidFunction)sysvk_QueueSubmit},
    {"vkQueueSubmit2", (PFN_vkVoidFunction)sysvk_QueueSubmit2},
    {"vkQueueSubmit2KHR", (PFN_vkVoidFunction)sysvk_QueueSubmit2},
};
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
sysvk_GetDeviceProcAddr(VkDevice device, const char *pName) {
  if (pName == NULL) {
    return NULL;
  }
  if (strcmp(pName, "vkGetDeviceProcAddr") == 0) {
    return (PFN_vkVoidFunction)sysvk_GetDeviceProcAddr;
  }
  for (size_t i = 0; i < sizeof(sysvk_device_overrides) / sizeof(sysvk_device_overrides[0]); i++) {
    if (strcmp(pName, sysvk_device_overrides[i].name) == 0) {
      SYSVK_LOG("GDPA override %s", pName);
      return sysvk_device_overrides[i].function;
    }
  }
  if (sysvk_next_gdpa == NULL) {
    return NULL;
  }
  return sysvk_next_gdpa(device, pName);
}

static const struct {
  const char *name;
  PFN_vkVoidFunction function;
} sysvk_instance_overrides[] = {
    {"vkCreateDevice", (PFN_vkVoidFunction)sysvk_CreateDevice},
    {"vkGetDeviceProcAddr", (PFN_vkVoidFunction)sysvk_GetDeviceProcAddr},
    {"vkEnumerateDeviceExtensionProperties",
     (PFN_vkVoidFunction)sysvk_EnumerateDeviceExtensionProperties},
    {"vkGetPhysicalDeviceFeatures2", (PFN_vkVoidFunction)sysvk_GetPhysicalDeviceFeatures2},
    {"vkGetPhysicalDeviceFeatures2KHR",
     (PFN_vkVoidFunction)sysvk_GetPhysicalDeviceFeatures2},
    {"vkGetPhysicalDeviceProperties2",
     (PFN_vkVoidFunction)sysvk_GetPhysicalDeviceProperties2},
    {"vkGetPhysicalDeviceProperties2KHR",
     (PFN_vkVoidFunction)sysvk_GetPhysicalDeviceProperties2},
};

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vk_icdGetInstanceProcAddr(VkInstance instance, const char *pName) {
  if (pName != NULL) {
    if (strcmp(pName, "vkEnumerateInstanceVersion") == 0) {
      return (PFN_vkVoidFunction)sysvk_EnumerateInstanceVersion;
    }
    if (strcmp(pName, "vkCreateInstance") == 0) {
      return (PFN_vkVoidFunction)sysvk_CreateInstance;
    }
    for (size_t i = 0; i < sizeof(sysvk_instance_overrides) / sizeof(sysvk_instance_overrides[0]); i++) {
      if (strcmp(pName, sysvk_instance_overrides[i].name) == 0) {
        return sysvk_instance_overrides[i].function;
      }
    }
  }

  pthread_once(&sysvk_once, sysvk_init);
  if (!sysvk_device) {
    return NULL;
  }
  return sysvk_device->GetInstanceProcAddr(instance, pName);
}

PFN_vkVoidFunction vk_icdGetPhysicalDeviceProcAddr(VkInstance _instance,
                                                   const char *pName) {
  return NULL;
}

VKAPI_ATTR VkResult VKAPI_CALL
vk_icdNegotiateLoaderICDInterfaceVersion(uint32_t *pSupportedVersion) {
#define MIN(i, j) (((i) < (j)) ? (i) : (j))
  *pSupportedVersion = MIN(*pSupportedVersion, 5u);
  return VK_SUCCESS;
}
