#include <hardware/hardware.h>
#include <hardware/hwvulkan.h>

#include <pthread.h>
#include <string.h>

/* The vendor HAL is a Vulkan 1.1-era Android driver.  The host loader may be
 * much newer (1.4.x) and will forward its own instance version unless the ICD
 * advertises and enforces a lower one.  Report 1.1 and clamp CreateInstance. */
#define SYSVK_API_VERSION VK_API_VERSION_1_1

static hwvulkan_device_t *sysvk_device = NULL;
static pthread_once_t sysvk_once = PTHREAD_ONCE_INIT;

static void sysvk_init(void) {
  hw_module_t *module = NULL;
  if (hw_get_module(HWVULKAN_HARDWARE_MODULE_ID, (const hw_module_t **)&module) != 0) {
    return;
  }
  if (module->methods->open(module, HWVULKAN_DEVICE_0, (hw_device_t **)&sysvk_device) != 0) {
    sysvk_device = NULL;
  }
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

  return sysvk_device->CreateInstance(&info, pAllocator, pInstance);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vk_icdGetInstanceProcAddr(VkInstance instance, const char *pName) {
  if (pName != NULL) {
    if (strcmp(pName, "vkEnumerateInstanceVersion") == 0) {
      return (PFN_vkVoidFunction)sysvk_EnumerateInstanceVersion;
    }
    if (strcmp(pName, "vkCreateInstance") == 0) {
      return (PFN_vkVoidFunction)sysvk_CreateInstance;
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
