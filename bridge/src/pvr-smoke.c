/*
 * pvr-smoke - minimal display-independent Vulkan hardware test.
 *
 * Creates a device through the configured ICD, clears a 64x64 R8G8B8A8
 * image, copies it to host-visible memory, and prints the readback pixels.
 * No window system, no swapchain.
 */
#include <vulkan/vulkan.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VK_CHECK(x)                                                                                                  \
   do                                                                                                                \
   {                                                                                                                 \
      VkResult _r = (x);                                                                                             \
      if (_r != VK_SUCCESS)                                                                                          \
      {                                                                                                              \
         fprintf(stderr, "%s failed: %d\n", #x, (int)_r);                                                            \
         exit(1);                                                                                                    \
      }                                                                                                              \
   } while (0)

static uint32_t find_memory_type(VkPhysicalDevice phys, uint32_t type_bits, VkMemoryPropertyFlags props)
{
   VkPhysicalDeviceMemoryProperties mp;
   vkGetPhysicalDeviceMemoryProperties(phys, &mp);
   for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
   {
      if ((type_bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & props) == props)
      {
         return i;
      }
   }
   return UINT32_MAX;
}

int main(void)
{
   VkApplicationInfo app = { 0 };
   app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
   app.pApplicationName = "pvr-smoke";
   app.applicationVersion = 1;
   app.pEngineName = "none";
   app.engineVersion = 1;
   app.apiVersion = VK_API_VERSION_1_0;

   VkInstanceCreateInfo ici = { 0 };
   ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
   ici.pApplicationInfo = &app;

   VkInstance inst;
   VK_CHECK(vkCreateInstance(&ici, NULL, &inst));

   uint32_t count = 0;
   VK_CHECK(vkEnumeratePhysicalDevices(inst, &count, NULL));
   printf("physical devices: %u\n", count);
   if (count == 0)
   {
      fprintf(stderr, "no Vulkan physical devices\n");
      return 2;
   }

   VkPhysicalDevice *phys = calloc(count, sizeof(*phys));
   if (phys == NULL)
   {
      return 1;
   }
   VK_CHECK(vkEnumeratePhysicalDevices(inst, &count, phys));

   for (uint32_t i = 0; i < count; i++)
   {
      VkPhysicalDeviceProperties p;
      vkGetPhysicalDeviceProperties(phys[i], &p);
      printf("device %u: %s\n", i, p.deviceName);
      printf("  api %u.%u.%u driver 0x%08x vendor 0x%04x device 0x%04x type %d\n", VK_VERSION_MAJOR(p.apiVersion),
             VK_VERSION_MINOR(p.apiVersion), VK_VERSION_PATCH(p.apiVersion), p.driverVersion, p.vendorID,
             p.deviceID, (int)p.deviceType);
   }

   VkPhysicalDevice pd = phys[0];
   uint32_t qcount = 0;
   vkGetPhysicalDeviceQueueFamilyProperties(pd, &qcount, NULL);
   VkQueueFamilyProperties *qprops = calloc(qcount, sizeof(*qprops));
   if (qprops == NULL)
   {
      return 1;
   }
   vkGetPhysicalDeviceQueueFamilyProperties(pd, &qcount, qprops);

   uint32_t qfam = UINT32_MAX;
   for (uint32_t i = 0; i < qcount; i++)
   {
      printf("queue family %u: flags 0x%x count %u\n", i, qprops[i].queueFlags, qprops[i].queueCount);
      if ((qprops[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && qfam == UINT32_MAX)
      {
         qfam = i;
      }
   }
   if (qfam == UINT32_MAX)
   {
      fprintf(stderr, "no graphics queue family\n");
      return 3;
   }

   float prio = 1.0f;
   VkDeviceQueueCreateInfo qci = { 0 };
   qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
   qci.queueFamilyIndex = qfam;
   qci.queueCount = 1;
   qci.pQueuePriorities = &prio;

   VkDeviceCreateInfo dci = { 0 };
   dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
   dci.queueCreateInfoCount = 1;
   dci.pQueueCreateInfos = &qci;

   VkDevice dev;
   VK_CHECK(vkCreateDevice(pd, &dci, NULL, &dev));
   VkQueue queue;
   vkGetDeviceQueue(dev, qfam, 0, &queue);

   const uint32_t W = 64, H = 64;
   VkImageCreateInfo imgci = { 0 };
   imgci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
   imgci.imageType = VK_IMAGE_TYPE_2D;
   imgci.format = VK_FORMAT_R8G8B8A8_UNORM;
   imgci.extent.width = W;
   imgci.extent.height = H;
   imgci.extent.depth = 1;
   imgci.mipLevels = 1;
   imgci.arrayLayers = 1;
   imgci.samples = VK_SAMPLE_COUNT_1_BIT;
   imgci.tiling = VK_IMAGE_TILING_OPTIMAL;
   imgci.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
   imgci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
   imgci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

   VkImage image;
   VK_CHECK(vkCreateImage(dev, &imgci, NULL, &image));
   VkMemoryRequirements imgreq;
   vkGetImageMemoryRequirements(dev, image, &imgreq);
   uint32_t imgtype = find_memory_type(pd, imgreq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
   if (imgtype == UINT32_MAX)
   {
      fprintf(stderr, "no device-local memory type\n");
      return 4;
   }
   VkMemoryAllocateInfo mai = { 0 };
   mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
   mai.allocationSize = imgreq.size;
   mai.memoryTypeIndex = imgtype;
   VkDeviceMemory imgmem;
   VK_CHECK(vkAllocateMemory(dev, &mai, NULL, &imgmem));
   VK_CHECK(vkBindImageMemory(dev, image, imgmem, 0));

   VkBufferCreateInfo bci = { 0 };
   bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
   bci.size = (VkDeviceSize)W * H * 4;
   bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
   bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
   VkBuffer buffer;
   VK_CHECK(vkCreateBuffer(dev, &bci, NULL, &buffer));
   VkMemoryRequirements bufreq;
   vkGetBufferMemoryRequirements(dev, buffer, &bufreq);
   uint32_t buftype = find_memory_type(pd, bufreq.memoryTypeBits,
                                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
   if (buftype == UINT32_MAX)
   {
      fprintf(stderr, "no host-visible coherent memory type\n");
      return 5;
   }
   VkMemoryAllocateInfo bmai = { 0 };
   bmai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
   bmai.allocationSize = bufreq.size;
   bmai.memoryTypeIndex = buftype;
   VkDeviceMemory bufmem;
   VK_CHECK(vkAllocateMemory(dev, &bmai, NULL, &bufmem));
   VK_CHECK(vkBindBufferMemory(dev, buffer, bufmem, 0));

   VkCommandPoolCreateInfo pci = { 0 };
   pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
   pci.queueFamilyIndex = qfam;
   VkCommandPool pool;
   VK_CHECK(vkCreateCommandPool(dev, &pci, NULL, &pool));

   VkCommandBufferAllocateInfo cbai = { 0 };
   cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
   cbai.commandPool = pool;
   cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
   cbai.commandBufferCount = 1;
   VkCommandBuffer cb;
   VK_CHECK(vkAllocateCommandBuffers(dev, &cbai, &cb));

   VkCommandBufferBeginInfo cbbi = { 0 };
   cbbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
   cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
   VK_CHECK(vkBeginCommandBuffer(cb, &cbbi));

   VkImageMemoryBarrier b = { 0 };
   b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
   b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
   b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
   b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
   b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
   b.image = image;
   b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
   b.subresourceRange.levelCount = 1;
   b.subresourceRange.layerCount = 1;
   b.srcAccessMask = 0;
   b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
   vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1,
                        &b);

   VkClearColorValue color;
   memset(&color, 0, sizeof(color));
   color.float32[0] = 0.25f;
   color.float32[1] = 0.50f;
   color.float32[2] = 0.75f;
   color.float32[3] = 1.0f;
   VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
   vkCmdClearColorImage(cb, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &range);

   b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
   b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
   b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
   b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
   vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1,
                        &b);

   VkBufferImageCopy region = { 0 };
   region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
   region.imageSubresource.layerCount = 1;
   region.imageExtent.width = W;
   region.imageExtent.height = H;
   region.imageExtent.depth = 1;
   vkCmdCopyImageToBuffer(cb, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &region);

   VK_CHECK(vkEndCommandBuffer(cb));

   VkSubmitInfo si = { 0 };
   si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
   si.commandBufferCount = 1;
   si.pCommandBuffers = &cb;
   VK_CHECK(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE));
   VK_CHECK(vkQueueWaitIdle(queue));

   void *ptr = NULL;
   VK_CHECK(vkMapMemory(dev, bufmem, 0, VK_WHOLE_SIZE, 0, &ptr));
   const uint8_t *px = ptr;
   printf("readback first: %u %u %u %u\n", px[0], px[1], px[2], px[3]);
   printf("readback last:  %u %u %u %u\n", px[(W * H - 1) * 4 + 0], px[(W * H - 1) * 4 + 1],
          px[(W * H - 1) * 4 + 2], px[(W * H - 1) * 4 + 3]);
   vkUnmapMemory(dev, bufmem);

   printf("pvr-smoke: OK\n");

   vkDestroyCommandPool(dev, pool, NULL);
   vkDestroyBuffer(dev, buffer, NULL);
   vkFreeMemory(dev, bufmem, NULL);
   vkDestroyImage(dev, image, NULL);
   vkFreeMemory(dev, imgmem, NULL);
   vkDestroyDevice(dev, NULL);
   vkDestroyInstance(inst, NULL);
   free(qprops);
   free(phys);
   return 0;
}
