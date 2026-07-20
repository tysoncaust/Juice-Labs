/* rgpu Linux Vulkan renderer (rgpu-renderd-linux) — the Colab/remote backend.
 *
 * Consumes an rgpu protocol COMMAND_BATCH (the same wire format the Windows
 * D3D11 reference renderer consumes) and executes it on a real Vulkan GPU (the
 * Colab NVIDIA T4), proving the protocol expresses D3D11 *semantics* a Vulkan
 * backend can implement independently — NOT a forwarding of Windows calls.
 *
 * This minimal-but-real backend implements the loopback subset: CREATE_TEXTURE_2D
 * (offscreen color target), CLEAR_RTV (render-pass load-op clear to the batch's
 * color), PRESENT (read the frame back). It renders headless (no swapchain, no
 * shaders — clear via the render pass), copies the image to a host-visible
 * buffer, and writes the RGBA frame to disk for the H.264 encode step. Exit 0
 * iff the read-back top-left pixel equals the requested clear color (the same
 * check the Windows D3D11 loopback test makes).
 *
 * Build (on Colab):  g++ -std=c++17 -O2 rgpu_renderd_vk.cpp -o rgpu_renderd_vk -lvulkan
 * Run:               ./rgpu_renderd_vk batch.bin frame.raw
 */
#include <vulkan/vulkan.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <vector>
#include <string>

/* ---- rgpu protocol (mirrors proto/rgpu_protocol.h + rgpu_cmds.h) ---------- */
static const uint32_t RGPU_MAGIC = 0x52475055u;
static const uint32_t RGPU_CMD_CREATE_TEXTURE_2D = 101;
static const uint32_t RGPU_CMD_CLEAR_RTV = 142;
static const uint32_t RGPU_CMD_PRESENT = 180;

struct FrameHeader { uint32_t magic; uint16_t version; uint16_t type; uint32_t session; uint32_t sequence; uint32_t payload_len; };
struct CmdRecord { uint32_t op; uint32_t handle; uint32_t arg_len; };

struct ParsedBatch { uint32_t width = 64, height = 64; float clear[4] = {0, 0, 0, 1}; bool have_clear = false, have_present = false; };

static bool parse_batch(const std::string &path, ParsedBatch &out) {
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) { std::printf("  cannot open batch %s\n", path.c_str()); return false; }
    std::fseek(f, 0, SEEK_END); long n = std::ftell(f); std::fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> buf(n > 0 ? n : 0);
    if (n <= 0 || std::fread(buf.data(), 1, n, f) != (size_t)n) { std::fclose(f); return false; }
    std::fclose(f);
    if (buf.size() < sizeof(FrameHeader)) return false;
    FrameHeader h; std::memcpy(&h, buf.data(), sizeof(h));
    if (h.magic != RGPU_MAGIC) { std::printf("  bad magic 0x%08X\n", h.magic); return false; }
    size_t pos = sizeof(FrameHeader);
    while (pos + sizeof(CmdRecord) <= buf.size()) {
        CmdRecord r; std::memcpy(&r, buf.data() + pos, sizeof(r)); pos += sizeof(r);
        if (pos + r.arg_len > buf.size()) break;
        const uint8_t *args = buf.data() + pos;
        if (r.op == RGPU_CMD_CREATE_TEXTURE_2D && r.arg_len >= 12) {
            std::memcpy(&out.width, args, 4); std::memcpy(&out.height, args + 4, 4);
        } else if (r.op == RGPU_CMD_CLEAR_RTV && r.arg_len >= 16) {
            std::memcpy(out.clear, args, 16); out.have_clear = true;
        } else if (r.op == RGPU_CMD_PRESENT) {
            out.have_present = true;
        }
        pos += r.arg_len;
    }
    return true;
}

#define VKOK(x) do { VkResult _r = (x); if (_r != VK_SUCCESS) { std::printf("  vk fail %d at %s:%d\n", _r, __FILE__, __LINE__); return 2; } } while (0)

static uint32_t find_mem(VkPhysicalDevice pd, uint32_t typeBits, VkMemoryPropertyFlags props) {
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((typeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & props) == props) return i;
    return UINT32_MAX;
}

int main(int argc, char **argv) {
    std::printf("rgpu linux vulkan renderer\n--------------------------\n");
    ParsedBatch b;
    std::string batchPath = argc > 1 ? argv[1] : "batch.bin";
    std::string outPath = argc > 2 ? argv[2] : "frame.raw";
    if (!parse_batch(batchPath, b)) { std::printf("  FAIL: parse batch\n"); return 1; }
    std::printf("  batch: %ux%u clear=(%.2f,%.2f,%.2f,%.2f) present=%d\n",
                b.width, b.height, b.clear[0], b.clear[1], b.clear[2], b.clear[3], b.have_present);

    /* ---- Vulkan instance + physical device ---- */
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO}; app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; ici.pApplicationInfo = &app;
    VkInstance inst; VKOK(vkCreateInstance(&ici, nullptr, &inst));
    uint32_t npd = 0; vkEnumeratePhysicalDevices(inst, &npd, nullptr);
    if (!npd) { std::printf("  FAIL: no Vulkan physical device\n"); return 1; }
    std::vector<VkPhysicalDevice> pds(npd); vkEnumeratePhysicalDevices(inst, &npd, pds.data());
    VkPhysicalDevice pd = pds[0];
    for (auto d : pds) { VkPhysicalDeviceProperties p; vkGetPhysicalDeviceProperties(d, &p);
        if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) { pd = d; break; } }
    VkPhysicalDeviceProperties props; vkGetPhysicalDeviceProperties(pd, &props);
    std::printf("  GPU: %s (Vulkan %u.%u.%u)\n", props.deviceName,
                VK_VERSION_MAJOR(props.apiVersion), VK_VERSION_MINOR(props.apiVersion), VK_VERSION_PATCH(props.apiVersion));

    /* ---- graphics queue + logical device ---- */
    uint32_t nq = 0; vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, nullptr);
    std::vector<VkQueueFamilyProperties> qf(nq); vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qf.data());
    uint32_t gq = UINT32_MAX;
    for (uint32_t i = 0; i < nq; i++) if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { gq = i; break; }
    if (gq == UINT32_MAX) { std::printf("  FAIL: no graphics queue\n"); return 1; }
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = gq; qci.queueCount = 1; qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
    VkDevice dev; VKOK(vkCreateDevice(pd, &dci, nullptr, &dev));
    VkQueue queue; vkGetDeviceQueue(dev, gq, 0, &queue);

    /* ---- offscreen color target (CREATE_TEXTURE_2D) ---- */
    const VkFormat fmt = VK_FORMAT_R8G8B8A8_UNORM;
    VkImageCreateInfo img{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    img.imageType = VK_IMAGE_TYPE_2D; img.format = fmt; img.extent = {b.width, b.height, 1};
    img.mipLevels = 1; img.arrayLayers = 1; img.samples = VK_SAMPLE_COUNT_1_BIT; img.tiling = VK_IMAGE_TILING_OPTIMAL;
    img.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    img.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImage image; VKOK(vkCreateImage(dev, &img, nullptr, &image));
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(dev, image, &mr);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = mr.size; mai.memoryTypeIndex = find_mem(pd, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkDeviceMemory imem; VKOK(vkAllocateMemory(dev, &mai, nullptr, &imem)); vkBindImageMemory(dev, image, imem, 0);
    VkImageViewCreateInfo ivci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    ivci.image = image; ivci.viewType = VK_IMAGE_VIEW_TYPE_2D; ivci.format = fmt;
    ivci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageView view; VKOK(vkCreateImageView(dev, &ivci, nullptr, &view));

    /* ---- render pass (load-op clear = CLEAR_RTV) + framebuffer ---- */
    VkAttachmentDescription att{}; att.format = fmt; att.samples = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED; att.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    VkAttachmentReference ar{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{}; sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS; sub.colorAttachmentCount = 1; sub.pColorAttachments = &ar;
    VkSubpassDependency dep{}; dep.srcSubpass = 0; dep.dstSubpass = VK_SUBPASS_EXTERNAL;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT; dep.dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
    dep.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT; dep.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    VkRenderPassCreateInfo rpci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rpci.attachmentCount = 1; rpci.pAttachments = &att; rpci.subpassCount = 1; rpci.pSubpasses = &sub;
    rpci.dependencyCount = 1; rpci.pDependencies = &dep;
    VkRenderPass rp; VKOK(vkCreateRenderPass(dev, &rpci, nullptr, &rp));
    VkFramebufferCreateInfo fbci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fbci.renderPass = rp; fbci.attachmentCount = 1; fbci.pAttachments = &view; fbci.width = b.width; fbci.height = b.height; fbci.layers = 1;
    VkFramebuffer fb; VKOK(vkCreateFramebuffer(dev, &fbci, nullptr, &fb));

    /* ---- host-visible readback buffer (PRESENT) ---- */
    VkDeviceSize rbSize = (VkDeviceSize)b.width * b.height * 4;
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; bci.size = rbSize; bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VkBuffer rb; VKOK(vkCreateBuffer(dev, &bci, nullptr, &rb));
    VkMemoryRequirements bmr; vkGetBufferMemoryRequirements(dev, rb, &bmr);
    VkMemoryAllocateInfo bmai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; bmai.allocationSize = bmr.size;
    bmai.memoryTypeIndex = find_mem(pd, bmr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkDeviceMemory bmem; VKOK(vkAllocateMemory(dev, &bmai, nullptr, &bmem)); vkBindBufferMemory(dev, rb, bmem, 0);

    /* ---- record + submit: clear (render pass) then copy image -> buffer ---- */
    VkCommandPoolCreateInfo cpci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; cpci.queueFamilyIndex = gq;
    VkCommandPool pool; VKOK(vkCreateCommandPool(dev, &cpci, nullptr, &pool));
    VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbai.commandPool = pool; cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cbai.commandBufferCount = 1;
    VkCommandBuffer cb; VKOK(vkAllocateCommandBuffers(dev, &cbai, &cb));
    VkCommandBufferBeginInfo cbbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VKOK(vkBeginCommandBuffer(cb, &cbbi));
    VkClearValue cv{}; cv.color = {{b.clear[0], b.clear[1], b.clear[2], b.clear[3]}};
    VkRenderPassBeginInfo rpbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rpbi.renderPass = rp; rpbi.framebuffer = fb; rpbi.renderArea = {{0, 0}, {b.width, b.height}}; rpbi.clearValueCount = 1; rpbi.pClearValues = &cv;
    vkCmdBeginRenderPass(cb, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdEndRenderPass(cb); /* load-op clear already wrote the color; finalLayout = TRANSFER_SRC */
    VkBufferImageCopy region{}; region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; region.imageExtent = {b.width, b.height, 1};
    vkCmdCopyImageToBuffer(cb, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb, 1, &region);
    VKOK(vkEndCommandBuffer(cb));
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO}; si.commandBufferCount = 1; si.pCommandBuffers = &cb;
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; VkFence fence; vkCreateFence(dev, &fci, nullptr, &fence);
    VKOK(vkQueueSubmit(queue, 1, &si, fence));
    vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX);

    /* ---- read back the frame + verify + write to disk for the encoder ---- */
    void *mapped = nullptr; vkMapMemory(dev, bmem, 0, rbSize, 0, &mapped);
    const uint8_t *px = (const uint8_t *)mapped;
    uint8_t r = px[0], g = px[1], bl = px[2], a = px[3];
    FILE *of = std::fopen(outPath.c_str(), "wb"); if (of) { std::fwrite(mapped, 1, rbSize, of); std::fclose(of); }
    vkUnmapMemory(dev, bmem);
    std::printf("  rendered frame %ux%u; top-left pixel (%u,%u,%u,%u) -> %s\n", b.width, b.height, r, g, bl, a, outPath.c_str());

    uint8_t exp[4] = {(uint8_t)(b.clear[0] * 255 + 0.5f), (uint8_t)(b.clear[1] * 255 + 0.5f),
                      (uint8_t)(b.clear[2] * 255 + 0.5f), (uint8_t)(b.clear[3] * 255 + 0.5f)};
    int ok = b.have_clear && std::abs((int)r - exp[0]) <= 1 && std::abs((int)g - exp[1]) <= 1 &&
             std::abs((int)bl - exp[2]) <= 1 && std::abs((int)a - exp[3]) <= 1;
    std::printf("--------------------------\n");
    std::printf(ok ? "RESULT: rgpu protocol rendered on the real Vulkan GPU (pixel matches CLEAR_RTV color)\n"
                   : "RESULT: FAIL\n");
    return ok ? 0 : 1;
}
