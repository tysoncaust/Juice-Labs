/* rgpu Linux Vulkan renderer (rgpu-renderd-linux) — the Colab/remote backend.
 *
 * OFFSCREEN (headless) Vulkan: no window, no swapchain, no VkSurface — renders
 * into ordinary device-local VkImages and copies/measures them. This is the right
 * shape for a Colab T4 (no display), and it consumes rgpu protocol *semantics* so
 * a Vulkan backend implements them independently of the Windows D3D frontend.
 *
 * Two modes:
 *   batch      rgpu_renderd_vk <batch.bin> <frame.raw>
 *              Consume an rgpu COMMAND_BATCH (CREATE_TEXTURE_2D/CLEAR_RTV/PRESENT),
 *              render one frame, read it back, verify pixel == the CLEAR_RTV color
 *              (same check as the Windows D3D11 loopback), write the RGBA frame.
 *   benchmark  rgpu_renderd_vk --headless --width W --height H --frames N \
 *                              --benchmark-json out.json
 *              Render N offscreen frames measuring GPU time with Vulkan TIMESTAMP
 *              queries, and emit a benchmark JSON (incl. software_renderer:false on
 *              a real GPU). Proves the renderer runs on the actual hardware.
 *
 * Build:  g++ -std=c++17 -O2 rgpu_renderd_vk.cpp -o rgpu_renderd_vk -lvulkan
 */
#include <vulkan/vulkan.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <vector>
#include <string>
#include <algorithm>

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
        if (r.op == RGPU_CMD_CREATE_TEXTURE_2D && r.arg_len >= 12) { std::memcpy(&out.width, args, 4); std::memcpy(&out.height, args + 4, 4); }
        else if (r.op == RGPU_CMD_CLEAR_RTV && r.arg_len >= 16) { std::memcpy(out.clear, args, 16); out.have_clear = true; }
        else if (r.op == RGPU_CMD_PRESENT) out.have_present = true;
        pos += r.arg_len;
    }
    return true;
}

#define VKOK(x) do { VkResult _r = (x); if (_r != VK_SUCCESS) { std::printf("  vk fail %d at %s:%d\n", _r, __FILE__, __LINE__); std::exit(2); } } while (0)

static uint32_t find_mem(VkPhysicalDevice pd, uint32_t typeBits, VkMemoryPropertyFlags props) {
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((typeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & props) == props) return i;
    return UINT32_MAX;
}

/* Shared Vulkan objects for offscreen rendering into a WxH color image. */
struct Vk {
    VkInstance inst; VkPhysicalDevice pd; VkDevice dev; VkQueue queue; uint32_t gq;
    VkPhysicalDeviceProperties props; bool software = false;
    VkFormat fmt = VK_FORMAT_R8G8B8A8_UNORM; uint32_t w, h;
    VkImage image; VkDeviceMemory imem; VkImageView view;
    VkRenderPass rp; VkFramebuffer fb; VkCommandPool pool;
};

static void vk_init(Vk &v) {
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO}; app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; ici.pApplicationInfo = &app;
    VKOK(vkCreateInstance(&ici, nullptr, &v.inst));
    uint32_t npd = 0; vkEnumeratePhysicalDevices(v.inst, &npd, nullptr);
    if (!npd) { std::printf("  FAIL: no Vulkan physical device\n"); std::exit(1); }
    std::vector<VkPhysicalDevice> pds(npd); vkEnumeratePhysicalDevices(v.inst, &npd, pds.data());
    v.pd = pds[0];
    for (auto d : pds) { VkPhysicalDeviceProperties p; vkGetPhysicalDeviceProperties(d, &p);
        if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) { v.pd = d; break; } }
    vkGetPhysicalDeviceProperties(v.pd, &v.props);
    std::string name = v.props.deviceName;
    v.software = (v.props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) ||
                 name.find("llvmpipe") != std::string::npos || name.find("lavapipe") != std::string::npos;
    std::printf("  GPU: %s  (Vulkan %u.%u.%u, %s)\n", v.props.deviceName,
                VK_VERSION_MAJOR(v.props.apiVersion), VK_VERSION_MINOR(v.props.apiVersion), VK_VERSION_PATCH(v.props.apiVersion),
                v.software ? "SOFTWARE renderer" : "HARDWARE GPU");
    uint32_t nq = 0; vkGetPhysicalDeviceQueueFamilyProperties(v.pd, &nq, nullptr);
    std::vector<VkQueueFamilyProperties> qf(nq); vkGetPhysicalDeviceQueueFamilyProperties(v.pd, &nq, qf.data());
    v.gq = UINT32_MAX;
    for (uint32_t i = 0; i < nq; i++) if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { v.gq = i; break; }
    if (v.gq == UINT32_MAX) { std::printf("  FAIL: no graphics queue\n"); std::exit(1); }
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO}; qci.queueFamilyIndex = v.gq; qci.queueCount = 1; qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
    VKOK(vkCreateDevice(v.pd, &dci, nullptr, &v.dev));
    vkGetDeviceQueue(v.dev, v.gq, 0, &v.queue);
}

static void vk_make_target(Vk &v, uint32_t w, uint32_t h) {
    v.w = w; v.h = h;
    VkImageCreateInfo img{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    img.imageType = VK_IMAGE_TYPE_2D; img.format = v.fmt; img.extent = {w, h, 1}; img.mipLevels = 1; img.arrayLayers = 1;
    img.samples = VK_SAMPLE_COUNT_1_BIT; img.tiling = VK_IMAGE_TILING_OPTIMAL;
    img.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT; img.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VKOK(vkCreateImage(v.dev, &img, nullptr, &v.image));
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(v.dev, v.image, &mr);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; mai.allocationSize = mr.size;
    mai.memoryTypeIndex = find_mem(v.pd, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VKOK(vkAllocateMemory(v.dev, &mai, nullptr, &v.imem)); vkBindImageMemory(v.dev, v.image, v.imem, 0);
    VkImageViewCreateInfo ivci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; ivci.image = v.image; ivci.viewType = VK_IMAGE_VIEW_TYPE_2D; ivci.format = v.fmt;
    ivci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VKOK(vkCreateImageView(v.dev, &ivci, nullptr, &v.view));
    VkAttachmentDescription att{}; att.format = v.fmt; att.samples = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED; att.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    VkAttachmentReference ar{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{}; sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS; sub.colorAttachmentCount = 1; sub.pColorAttachments = &ar;
    VkSubpassDependency dep{}; dep.srcSubpass = 0; dep.dstSubpass = VK_SUBPASS_EXTERNAL;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT; dep.dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
    dep.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT; dep.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    VkRenderPassCreateInfo rpci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO}; rpci.attachmentCount = 1; rpci.pAttachments = &att;
    rpci.subpassCount = 1; rpci.pSubpasses = &sub; rpci.dependencyCount = 1; rpci.pDependencies = &dep;
    VKOK(vkCreateRenderPass(v.dev, &rpci, nullptr, &v.rp));
    VkFramebufferCreateInfo fbci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO}; fbci.renderPass = v.rp; fbci.attachmentCount = 1; fbci.pAttachments = &v.view;
    fbci.width = w; fbci.height = h; fbci.layers = 1;
    VKOK(vkCreateFramebuffer(v.dev, &fbci, nullptr, &v.fb));
    VkCommandPoolCreateInfo cpci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; cpci.queueFamilyIndex = v.gq;
    cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    VKOK(vkCreateCommandPool(v.dev, &cpci, nullptr, &v.pool));
}

/* ---------------- batch (loopback) mode ---------------- */
static int run_batch(Vk &v, const ParsedBatch &b, const std::string &outPath) {
    VkDeviceSize rbSize = (VkDeviceSize)b.width * b.height * 4;
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; bci.size = rbSize; bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VkBuffer rb; VKOK(vkCreateBuffer(v.dev, &bci, nullptr, &rb));
    VkMemoryRequirements bmr; vkGetBufferMemoryRequirements(v.dev, rb, &bmr);
    VkMemoryAllocateInfo bmai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; bmai.allocationSize = bmr.size;
    bmai.memoryTypeIndex = find_mem(v.pd, bmr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkDeviceMemory bmem; VKOK(vkAllocateMemory(v.dev, &bmai, nullptr, &bmem)); vkBindBufferMemory(v.dev, rb, bmem, 0);
    VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; cbai.commandPool = v.pool; cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cbai.commandBufferCount = 1;
    VkCommandBuffer cb; VKOK(vkAllocateCommandBuffers(v.dev, &cbai, &cb));
    VkCommandBufferBeginInfo cbbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VKOK(vkBeginCommandBuffer(cb, &cbbi));
    VkClearValue cv{}; cv.color = {{b.clear[0], b.clear[1], b.clear[2], b.clear[3]}};
    VkRenderPassBeginInfo rpbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO}; rpbi.renderPass = v.rp; rpbi.framebuffer = v.fb;
    rpbi.renderArea = {{0, 0}, {b.width, b.height}}; rpbi.clearValueCount = 1; rpbi.pClearValues = &cv;
    vkCmdBeginRenderPass(cb, &rpbi, VK_SUBPASS_CONTENTS_INLINE); vkCmdEndRenderPass(cb);
    VkBufferImageCopy region{}; region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; region.imageExtent = {b.width, b.height, 1};
    vkCmdCopyImageToBuffer(cb, v.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb, 1, &region);
    VKOK(vkEndCommandBuffer(cb));
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO}; si.commandBufferCount = 1; si.pCommandBuffers = &cb;
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; VkFence fence; vkCreateFence(v.dev, &fci, nullptr, &fence);
    VKOK(vkQueueSubmit(v.queue, 1, &si, fence)); vkWaitForFences(v.dev, 1, &fence, VK_TRUE, UINT64_MAX);
    void *mapped = nullptr; vkMapMemory(v.dev, bmem, 0, rbSize, 0, &mapped);
    const uint8_t *px = (const uint8_t *)mapped; uint8_t r = px[0], g = px[1], bl = px[2], a = px[3];
    FILE *of = std::fopen(outPath.c_str(), "wb"); if (of) { std::fwrite(mapped, 1, rbSize, of); std::fclose(of); }
    vkUnmapMemory(v.dev, bmem);
    std::printf("  rendered frame %ux%u; top-left pixel (%u,%u,%u,%u) -> %s\n", b.width, b.height, r, g, bl, a, outPath.c_str());
    uint8_t exp[4] = {(uint8_t)(b.clear[0]*255+0.5f), (uint8_t)(b.clear[1]*255+0.5f), (uint8_t)(b.clear[2]*255+0.5f), (uint8_t)(b.clear[3]*255+0.5f)};
    int ok = b.have_clear && std::abs((int)r-exp[0])<=1 && std::abs((int)g-exp[1])<=1 && std::abs((int)bl-exp[2])<=1 && std::abs((int)a-exp[3])<=1;
    std::printf("--------------------------\n");
    std::printf(ok ? "RESULT: rgpu protocol rendered on %s (pixel matches CLEAR_RTV color)\n"
                   : "RESULT: FAIL\n", v.software ? "SOFTWARE Vulkan" : "the real GPU via Vulkan");
    return ok ? 0 : 1;
}

/* ---------------- benchmark mode (GPU timestamp queries) ---------------- */
static int run_benchmark(Vk &v, uint32_t frames, const std::string &jsonPath) {
    double period = v.props.limits.timestampPeriod;   /* ns per tick */
    VkQueryPoolCreateInfo qpci{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO}; qpci.queryType = VK_QUERY_TYPE_TIMESTAMP; qpci.queryCount = 2;
    VkQueryPool qpool; VKOK(vkCreateQueryPool(v.dev, &qpci, nullptr, &qpool));
    VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; cbai.commandPool = v.pool; cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cbai.commandBufferCount = 1;
    VkCommandBuffer cb; VKOK(vkAllocateCommandBuffers(v.dev, &cbai, &cb));
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; VkFence fence; vkCreateFence(v.dev, &fci, nullptr, &fence);
    std::vector<double> ms; ms.reserve(frames);
    for (uint32_t i = 0; i < frames; i++) {
        vkResetCommandBuffer(cb, 0);
        VkCommandBufferBeginInfo cbbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cb, &cbbi);
        vkCmdResetQueryPool(cb, qpool, 0, 2);
        vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, qpool, 0);
        float t = (i % 256) / 255.0f; VkClearValue cv{}; cv.color = {{t, 0.4f, 1.0f - t, 1.0f}};
        VkRenderPassBeginInfo rpbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO}; rpbi.renderPass = v.rp; rpbi.framebuffer = v.fb;
        rpbi.renderArea = {{0, 0}, {v.w, v.h}}; rpbi.clearValueCount = 1; rpbi.pClearValues = &cv;
        vkCmdBeginRenderPass(cb, &rpbi, VK_SUBPASS_CONTENTS_INLINE); vkCmdEndRenderPass(cb);
        vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, qpool, 1);
        vkEndCommandBuffer(cb);
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO}; si.commandBufferCount = 1; si.pCommandBuffers = &cb;
        vkQueueSubmit(v.queue, 1, &si, fence); vkWaitForFences(v.dev, 1, &fence, VK_TRUE, UINT64_MAX); vkResetFences(v.dev, 1, &fence);
        uint64_t ts[2] = {0, 0}; vkGetQueryPoolResults(v.dev, qpool, 0, 2, sizeof(ts), ts, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
        ms.push_back((double)(ts[1] - ts[0]) * period / 1e6);
    }
    std::vector<double> sorted = ms; std::sort(sorted.begin(), sorted.end());
    double sum = 0; int over = 0; for (double x : ms) { sum += x; if (x > 16.67) over++; }
    double avg = sum / ms.size(); double p95 = sorted[(size_t)(0.95 * (sorted.size() - 1))];
    double avg_fps = avg > 0 ? 1000.0 / avg : 0;
    std::printf("  benchmark: %u frames @ %ux%u  avg_gpu=%.3f ms  p95=%.3f ms  avg_fps=%.1f  over16.67ms=%d\n",
                frames, v.w, v.h, avg, p95, avg_fps, over);
    FILE *j = std::fopen(jsonPath.c_str(), "w");
    if (j) {
        std::fprintf(j,
            "{\n  \"gpu\": \"%s\",\n  \"renderer\": \"Vulkan\",\n  \"software_renderer\": %s,\n"
            "  \"resolution\": \"%ux%u\",\n  \"frames\": %u,\n  \"average_gpu_ms\": %.3f,\n"
            "  \"p95_gpu_ms\": %.3f,\n  \"average_fps\": %.1f,\n  \"frames_over_16_67_ms\": %d,\n"
            "  \"timestamp_period_ns\": %.4f\n}\n",
            v.props.deviceName, v.software ? "true" : "false", v.w, v.h, frames, avg, p95, avg_fps, over, period);
        std::fclose(j);
        std::printf("  wrote %s\n", jsonPath.c_str());
    }
    std::printf("--------------------------\n");
    std::printf(v.software ? "RESULT: benchmark ran on SOFTWARE Vulkan (install libnvidia-gl-<branch> for the GPU)\n"
                           : "RESULT: benchmark ran on the real GPU via Vulkan (software_renderer=false)\n");
    return v.software ? 1 : 0;
}

int main(int argc, char **argv) {
    std::printf("rgpu linux vulkan renderer (offscreen)\n--------------------------\n");
    /* arg parse */
    uint32_t width = 0, height = 0, frames = 0; std::string benchJson, batchPath, outPath = "frame.raw";
    std::vector<std::string> pos;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&](uint32_t &d) { if (i + 1 < argc) d = (uint32_t)std::strtoul(argv[++i], nullptr, 10); };
        if (a == "--width") next(width);
        else if (a == "--height") next(height);
        else if (a == "--frames") next(frames);
        else if (a == "--benchmark-json") { if (i + 1 < argc) benchJson = argv[++i]; }
        else if (a == "--headless" || a == "--backend" || a == "vulkan") { if (a == "--backend" && i + 1 < argc) i++; }
        else pos.push_back(a);
    }

    Vk v; vk_init(v);
    if (frames > 0) {
        if (!width) width = 1280; if (!height) height = 720; if (benchJson.empty()) benchJson = "benchmark.json";
        vk_make_target(v, width, height);
        return run_benchmark(v, frames, benchJson);
    }
    /* batch/loopback mode */
    ParsedBatch b;
    batchPath = pos.size() > 0 ? pos[0] : "batch.bin";
    outPath   = pos.size() > 1 ? pos[1] : "frame.raw";
    if (!parse_batch(batchPath, b)) { std::printf("  FAIL: parse batch\n"); return 1; }
    std::printf("  batch: %ux%u clear=(%.2f,%.2f,%.2f,%.2f) present=%d\n", b.width, b.height, b.clear[0], b.clear[1], b.clear[2], b.clear[3], b.have_present);
    vk_make_target(v, b.width, b.height);
    return run_batch(v, b, outPath);
}
