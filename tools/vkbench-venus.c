/*
 * vkbench-venus -- the Venus rows for tools/bench-ledger.txt.
 *
 *   map-write/read   memcpy through a vkMapMemory pointer (HOST_VISIBLE|
 *                    COHERENT memory): the window-arena path end to end.
 *   submit-rtt       vkQueueSubmit(empty) + vkWaitForFences round trip:
 *                    the crossing + vkr ring + parked-fence path, timed.
 *
 * Compiled in the guest: gcc -O2 vkbench-venus.c -o vkbench-venus -lvulkan
 */
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MB (1u << 20)
#define MAP_SZ (64 * MB)
#define MAP_ROUNDS 4
#define RTT_ROUNDS 200

static double now_s(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

#define CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { \
	fprintf(stderr, "%s failed: %d\n", #x, r_); exit(1); } } while (0)

int main(void)
{
	VkInstance inst;
	VkPhysicalDevice all[8], phys = VK_NULL_HANDLE;
	VkDevice dev;
	VkQueue queue;
	uint32_t n = 8, qfam = 0, memtype = ~0u;
	VkPhysicalDeviceProperties props;
	VkPhysicalDeviceMemoryProperties mem;

	VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO,
		.pApplicationName = "vkbench-venus",
		.apiVersion = VK_API_VERSION_1_1 };
	VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
		.pApplicationInfo = &app };
	CHECK(vkCreateInstance(&ici, NULL, &inst));
	CHECK(vkEnumeratePhysicalDevices(inst, &n, all));
	/* The real card, not llvmpipe: venus reports DISCRETE. */
	for (uint32_t i = 0; i < n; i++) {
		vkGetPhysicalDeviceProperties(all[i], &props);
		if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
			phys = all[i];
			break;
		}
	}
	if (phys == VK_NULL_HANDLE) {
		fprintf(stderr, "no discrete GPU enumerated\n");
		return 1;
	}
	vkGetPhysicalDeviceProperties(phys, &props);
	printf("device: %s\n", props.deviceName);

	float prio = 1.0f;
	VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
		.queueFamilyIndex = qfam, .queueCount = 1,
		.pQueuePriorities = &prio };
	VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
		.queueCreateInfoCount = 1, .pQueueCreateInfos = &qci };
	CHECK(vkCreateDevice(phys, &dci, NULL, &dev));
	vkGetDeviceQueue(dev, qfam, 0, &queue);

	vkGetPhysicalDeviceMemoryProperties(phys, &mem);
	for (uint32_t i = 0; i < mem.memoryTypeCount; i++) {
		VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
			VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
		if ((mem.memoryTypes[i].propertyFlags & want) == want) {
			memtype = i;
			break;
		}
	}
	if (memtype == ~0u) {
		fprintf(stderr, "no HOST_VISIBLE|COHERENT memory type\n");
		return 1;
	}

	VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = MAP_SZ, .memoryTypeIndex = memtype };
	VkDeviceMemory dm;
	void *map;
	CHECK(vkAllocateMemory(dev, &mai, NULL, &dm));
	CHECK(vkMapMemory(dev, dm, 0, MAP_SZ, 0, &map));
	printf("vkMapMemory: %u MB mapped at %p (type %u)\n",
	       MAP_SZ >> 20, map, memtype);

	unsigned char *payload = malloc(MAP_SZ);
	unsigned char *sink = malloc(MAP_SZ);
	for (unsigned i = 0; i < MAP_SZ; i++)
		payload[i] = (unsigned char)(i * 2654435761u >> 24);

	double t0 = now_s();
	for (int r = 0; r < MAP_ROUNDS; r++)
		memcpy(map, payload, MAP_SZ);
	double dt = now_s() - t0;
	printf("map-write  %9.1f MiB/s\n", MAP_ROUNDS * (MAP_SZ / (double)MB) / dt);

	t0 = now_s();
	for (int r = 0; r < MAP_ROUNDS; r++)
		memcpy(sink, map, MAP_SZ);
	dt = now_s() - t0;
	printf("map-read   %9.1f MiB/s\n", MAP_ROUNDS * (MAP_SZ / (double)MB) / dt);
	if (memcmp(sink, payload, MAP_SZ) != 0) {
		fprintf(stderr, "READBACK MISMATCH\n");
		return 1;
	}

	/* Submit round trip: empty submit, fence, wait. */
	VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
	VkFence fence;
	CHECK(vkCreateFence(dev, &fci, NULL, &fence));
	/* warm one */
	CHECK(vkQueueSubmit(queue, 0, NULL, fence));
	CHECK(vkWaitForFences(dev, 1, &fence, VK_TRUE, ~0ull));
	CHECK(vkResetFences(dev, 1, &fence));

	t0 = now_s();
	for (int r = 0; r < RTT_ROUNDS; r++) {
		CHECK(vkQueueSubmit(queue, 0, NULL, fence));
		CHECK(vkWaitForFences(dev, 1, &fence, VK_TRUE, ~0ull));
		CHECK(vkResetFences(dev, 1, &fence));
	}
	dt = now_s() - t0;
	printf("submit-rtt %9.1f us median-ish (%d rounds)\n",
	       dt / RTT_ROUNDS * 1e6, RTT_ROUNDS);

	vkDestroyFence(dev, fence, NULL);
	vkUnmapMemory(dev, dm);
	vkFreeMemory(dev, dm, NULL);
	vkDestroyDevice(dev, NULL);
	vkDestroyInstance(inst, NULL);
	printf("PASS\n");
	return 0;
}
