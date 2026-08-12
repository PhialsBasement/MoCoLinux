/*
 * vkclear -- first pixels through Venus: render (clear) a 512x512 RGBA8
 * image on the GPU, copy it to a host-visible buffer, read it back through
 * the window path, verify every byte. No shaders; every other stage of
 * real rendering is exercised: image creation, layout transitions, command
 * execution, copy-to-buffer, coherent readback.
 *
 * gcc -O2 vkclear.c -o vkclear -lvulkan
 */
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 512
#define H 512

#define CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { \
	fprintf(stderr, "%s: %d\n", #x, r_); exit(1); } } while (0)


static uint32_t pick_type(VkPhysicalDeviceMemoryProperties *m, uint32_t bits,
			  VkMemoryPropertyFlags want)
{
	uint32_t i;

	for (i = 0; i < m->memoryTypeCount; i++)
		if ((bits & (1u << i)) &&
		    (m->memoryTypes[i].propertyFlags & want) == want)
			return i;
	fprintf(stderr, "no memory type for bits %x want %x\n", bits, want);
	exit(1);
}

int main(void)
{
	setvbuf(stderr, NULL, _IONBF, 0);
#define STAGE(s) fprintf(stderr, "stage: %s\n", s)
	VkInstance inst;
	VkPhysicalDevice all[8], phys = VK_NULL_HANDLE;
	VkDevice dev;
	VkQueue queue;
	uint32_t n = 8, i, host_type = ~0u, dev_type = ~0u;
	VkPhysicalDeviceProperties props;
	VkPhysicalDeviceMemoryProperties mem;

	VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO,
		.apiVersion = VK_API_VERSION_1_1 };
	VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
		.pApplicationInfo = &app };
	STAGE("instance"); CHECK(vkCreateInstance(&ici, NULL, &inst));
	STAGE("enumerate"); CHECK(vkEnumeratePhysicalDevices(inst, &n, all));
	for (i = 0; i < n; i++) {
		vkGetPhysicalDeviceProperties(all[i], &props);
		if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
			phys = all[i];
			break;
		}
	}
	if (phys == VK_NULL_HANDLE)
		return fprintf(stderr, "no discrete GPU\n"), 1;
	vkGetPhysicalDeviceProperties(phys, &props);
	printf("device: %s\n", props.deviceName);

	float prio = 1.0f;
	VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
		.queueCount = 1, .pQueuePriorities = &prio };
	VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
		.queueCreateInfoCount = 1, .pQueueCreateInfos = &qci };
	STAGE("device"); CHECK(vkCreateDevice(phys, &dci, NULL, &dev));
	vkGetDeviceQueue(dev, 0, 0, &queue);

	vkGetPhysicalDeviceMemoryProperties(phys, &mem);
	for (i = 0; i < mem.memoryTypeCount; i++) {
		VkMemoryPropertyFlags f = mem.memoryTypes[i].propertyFlags;

		if (dev_type == ~0u &&
		    (f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
			dev_type = i;
		if (host_type == ~0u &&
		    (f & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
			  VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
		    (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
		     VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
			host_type = i;
	}

	/* The render target, on the card. */
	VkImageCreateInfo imci = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.imageType = VK_IMAGE_TYPE_2D,
		.format = VK_FORMAT_R8G8B8A8_UNORM,
		.extent = { W, H, 1 },
		.mipLevels = 1, .arrayLayers = 1,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.tiling = VK_IMAGE_TILING_OPTIMAL,
		.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
			 VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
	VkImage img;
	STAGE("image"); CHECK(vkCreateImage(dev, &imci, NULL, &img));
	VkMemoryRequirements mr;
	vkGetImageMemoryRequirements(dev, img, &mr);
	VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = mr.size,
		.memoryTypeIndex = pick_type(&mem, mr.memoryTypeBits,
			VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
	VkDeviceMemory imem;
	CHECK(vkAllocateMemory(dev, &mai, NULL, &imem));
	STAGE("bind-image"); CHECK(vkBindImageMemory(dev, img, imem, 0));

	/* The readback buffer, host-visible. */
	VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.size = (VkDeviceSize)W * H * 4,
		.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT };
	VkBuffer buf;
	STAGE("buffer"); CHECK(vkCreateBuffer(dev, &bci, NULL, &buf));
	vkGetBufferMemoryRequirements(dev, buf, &mr);
	mai.allocationSize = mr.size;
	mai.memoryTypeIndex = pick_type(&mem, mr.memoryTypeBits,
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
		VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	VkDeviceMemory bmem;
	CHECK(vkAllocateMemory(dev, &mai, NULL, &bmem));
	STAGE("bind-buffer"); CHECK(vkBindBufferMemory(dev, buf, bmem, 0));

	VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
	VkCommandPool pool;
	STAGE("cmdpool"); CHECK(vkCreateCommandPool(dev, &pci, NULL, &pool));
	VkCommandBufferAllocateInfo cai = {
		VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = pool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = 1 };
	VkCommandBuffer cb;
	CHECK(vkAllocateCommandBuffers(dev, &cai, &cb));

	VkCommandBufferBeginInfo bi = {
		VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	STAGE("begin"); CHECK(vkBeginCommandBuffer(cb, &bi));

	VkImageMemoryBarrier b1 = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		.srcAccessMask = 0,
		.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
		.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = img,
		.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
	vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
			     VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL,
			     0, NULL, 1, &b1);

	/* The "render": a precise color the checksum can insist on. */
	VkClearColorValue color = { .float32 = { 0.25f, 0.5f, 0.75f, 1.0f } };
	VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
	vkCmdClearColorImage(cb, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			     &color, 1, &range);

	VkImageMemoryBarrier b2 = b1;
	b2.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	b2.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	b2.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	b2.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
			     VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL,
			     0, NULL, 1, &b2);

	VkBufferImageCopy region = { 0 };
	region.imageSubresource =
		(VkImageSubresourceLayers){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
	region.imageExtent = (VkExtent3D){ W, H, 1 };
	vkCmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			       buf, 1, &region);
	STAGE("end"); CHECK(vkEndCommandBuffer(cb));

	VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
	VkFence fence;
	CHECK(vkCreateFence(dev, &fci, NULL, &fence));
	VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO,
		.commandBufferCount = 1, .pCommandBuffers = &cb };
	STAGE("submit"); CHECK(vkQueueSubmit(queue, 1, &si, fence));
	STAGE("wait"); CHECK(vkWaitForFences(dev, 1, &fence, VK_TRUE, ~0ull));

	unsigned char *p;
	STAGE("map"); CHECK(vkMapMemory(dev, bmem, 0, (VkDeviceSize)W * H * 4, 0,
			  (void **)&p));

	/* 0.25/0.5/0.75/1.0 in UNORM8: 64, 128, 191, 255. */
	/* 127.5 is a tie; the spec lets either neighbor win. */
	const unsigned char lo[4] = { 64, 127, 191, 255 };
	const unsigned char hi[4] = { 64, 128, 191, 255 };
	unsigned long bad = 0, sum = 0;

	for (i = 0; i < W * H; i++) {
		const unsigned char *px = p + (size_t)i * 4;
		int c, ok = 1;

		sum += px[0] + px[1] + px[2] + px[3];
		for (c = 0; c < 4; c++)
			if (px[c] < lo[c] || px[c] > hi[c])
				ok = 0;
		if (!ok)
			bad++;
	}
	printf("pixels %u, wrong %lu, bytesum %lu (expect %lu)\n",
	       W * H, bad, sum, (unsigned long)(64 + 127 + 191 + 255) * W * H);
	printf(bad == 0 ? "CHECKSUM PASS -- the GPU rendered and we read it"
			  " back intact\n"
			: "CHECKSUM FAIL\n");
	return bad == 0 ? 0 : 1;
}
