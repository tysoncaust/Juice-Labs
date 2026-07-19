/* rgpu_vk_probe — tiny native headless Vulkan device-create test.
 *
 * vulkaninfo can report a GPU while a real headless vkCreateDevice still fails,
 * so this is the authoritative signal used by rgpu_probe.py: it creates a
 * headless Vulkan instance (no surface/WSI), picks the first physical GPU, and
 * creates a logical device with a graphics+compute-capable queue. Exit 0 = the
 * Colab runtime can genuinely render headlessly; non-zero = reject the runtime.
 *
 * Build on Colab:
 *   sudo apt-get install -y libvulkan-dev
 *   cc rgpu_vk_probe.c -o rgpu_vk_probe -lvulkan
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

static int fail(const char *msg, VkResult r) {
    fprintf(stderr, "rgpu_vk_probe: %s (VkResult=%d)\n", msg, r);
    return 1;
}

int main(void) {
    VkApplicationInfo app = {0};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "rgpu_vk_probe";
    app.apiVersion = VK_API_VERSION_1_1;

    VkInstanceCreateInfo ici = {0};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;

    VkInstance inst;
    VkResult r = vkCreateInstance(&ici, NULL, &inst);
    if (r != VK_SUCCESS) return fail("vkCreateInstance failed (no headless ICD?)", r);

    uint32_t n = 0;
    vkEnumeratePhysicalDevices(inst, &n, NULL);
    if (n == 0) { fprintf(stderr, "rgpu_vk_probe: no physical devices\n"); return 1; }
    VkPhysicalDevice *devs = calloc(n, sizeof(VkPhysicalDevice));
    vkEnumeratePhysicalDevices(inst, &n, devs);

    VkPhysicalDevice phys = devs[0];
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(phys, &props);

    uint32_t qn = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &qn, NULL);
    VkQueueFamilyProperties *qf = calloc(qn, sizeof(VkQueueFamilyProperties));
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &qn, qf);

    int qindex = -1;
    for (uint32_t i = 0; i < qn; i++) {
        if (qf[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) { qindex = (int)i; break; }
    }
    if (qindex < 0) { fprintf(stderr, "rgpu_vk_probe: no graphics/compute queue\n"); return 1; }

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {0};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = (uint32_t)qindex;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;

    VkDeviceCreateInfo dci = {0};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;

    VkDevice dev;
    r = vkCreateDevice(phys, &dci, NULL, &dev);
    if (r != VK_SUCCESS) return fail("vkCreateDevice failed", r);

    printf("OK gpu=\"%s\" api=%u.%u.%u queueFamily=%d\n", props.deviceName,
           VK_VERSION_MAJOR(props.apiVersion), VK_VERSION_MINOR(props.apiVersion),
           VK_VERSION_PATCH(props.apiVersion), qindex);

    vkDestroyDevice(dev, NULL);
    vkDestroyInstance(inst, NULL);
    free(devs); free(qf);
    return 0;
}
