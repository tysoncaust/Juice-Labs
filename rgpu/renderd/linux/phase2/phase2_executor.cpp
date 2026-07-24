#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr uint32_t kWidth = 256;
constexpr uint32_t kHeight = 256;
constexpr uint32_t kTextureWidth = 64;
constexpr uint32_t kTextureHeight = 64;
constexpr VkFormat kFormat = VK_FORMAT_R8G8B8A8_UNORM;

void vk_check(VkResult result, const char* expression, int line) {
    if (result != VK_SUCCESS) {
        throw std::runtime_error(std::string("Vulkan failure ") + std::to_string(result) +
                                 " at line " + std::to_string(line) + ": " + expression);
    }
}

#define VK_CHECK(expr) vk_check((expr), #expr, __LINE__)

std::vector<uint32_t> read_spirv(const std::string& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) {
        throw std::runtime_error("Unable to open SPIR-V file: " + path);
    }
    const auto length = input.tellg();
    if (length <= 0 || (static_cast<size_t>(length) % sizeof(uint32_t)) != 0) {
        throw std::runtime_error("Invalid SPIR-V length: " + path);
    }
    input.seekg(0, std::ios::beg);
    std::vector<uint32_t> words(static_cast<size_t>(length) / sizeof(uint32_t));
    input.read(reinterpret_cast<char*>(words.data()), length);
    if (!input) {
        throw std::runtime_error("Unable to read SPIR-V file: " + path);
    }
    return words;
}

uint32_t crc32(const uint8_t* data, size_t size) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            const uint32_t mask = static_cast<uint32_t>(-(static_cast<int32_t>(crc & 1u)));
            crc = (crc >> 1u) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

struct Buffer {
    VkBuffer handle = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
};

struct Image {
    VkImage handle = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
};

struct VulkanContext {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queue_family = UINT32_MAX;
    VkCommandPool command_pool = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties properties{};
    VkPhysicalDeviceMemoryProperties memory_properties{};
};

uint32_t find_memory_type(const VulkanContext& ctx, uint32_t type_bits,
                          VkMemoryPropertyFlags required) {
    for (uint32_t index = 0; index < ctx.memory_properties.memoryTypeCount; ++index) {
        if ((type_bits & (1u << index)) != 0u &&
            (ctx.memory_properties.memoryTypes[index].propertyFlags & required) == required) {
            return index;
        }
    }
    throw std::runtime_error("No compatible Vulkan memory type");
}

VulkanContext create_context() {
    VulkanContext ctx;
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "rgpu-phase2-executor";
    app.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    app.pEngineName = "rgpu";
    app.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    app.apiVersion = VK_API_VERSION_1_1;

    VkInstanceCreateInfo instance_info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instance_info.pApplicationInfo = &app;
    VK_CHECK(vkCreateInstance(&instance_info, nullptr, &ctx.instance));

    uint32_t physical_count = 0;
    VK_CHECK(vkEnumeratePhysicalDevices(ctx.instance, &physical_count, nullptr));
    if (physical_count == 0) {
        throw std::runtime_error("No Vulkan physical device available");
    }
    std::vector<VkPhysicalDevice> physical_devices(physical_count);
    VK_CHECK(vkEnumeratePhysicalDevices(ctx.instance, &physical_count, physical_devices.data()));
    ctx.physical = physical_devices.front();
    for (VkPhysicalDevice candidate : physical_devices) {
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(candidate, &properties);
        if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            ctx.physical = candidate;
            break;
        }
    }
    vkGetPhysicalDeviceProperties(ctx.physical, &ctx.properties);
    vkGetPhysicalDeviceMemoryProperties(ctx.physical, &ctx.memory_properties);

    uint32_t queue_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(ctx.physical, &queue_count, nullptr);
    std::vector<VkQueueFamilyProperties> queue_properties(queue_count);
    vkGetPhysicalDeviceQueueFamilyProperties(ctx.physical, &queue_count, queue_properties.data());
    for (uint32_t i = 0; i < queue_count; ++i) {
        if ((queue_properties[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0u) {
            ctx.queue_family = i;
            break;
        }
    }
    if (ctx.queue_family == UINT32_MAX) {
        throw std::runtime_error("No graphics queue family available");
    }

    const float queue_priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queue_info.queueFamilyIndex = ctx.queue_family;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &queue_priority;

    VkDeviceCreateInfo device_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    device_info.queueCreateInfoCount = 1;
    device_info.pQueueCreateInfos = &queue_info;
    VK_CHECK(vkCreateDevice(ctx.physical, &device_info, nullptr, &ctx.device));
    vkGetDeviceQueue(ctx.device, ctx.queue_family, 0, &ctx.queue);

    VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool_info.queueFamilyIndex = ctx.queue_family;
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    VK_CHECK(vkCreateCommandPool(ctx.device, &pool_info, nullptr, &ctx.command_pool));
    return ctx;
}

Buffer create_buffer(const VulkanContext& ctx, VkDeviceSize size, VkBufferUsageFlags usage,
                     VkMemoryPropertyFlags memory_flags) {
    Buffer buffer;
    buffer.size = size;
    VkBufferCreateInfo buffer_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    buffer_info.size = size;
    buffer_info.usage = usage;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VK_CHECK(vkCreateBuffer(ctx.device, &buffer_info, nullptr, &buffer.handle));

    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(ctx.device, buffer.handle, &requirements);
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = find_memory_type(ctx, requirements.memoryTypeBits, memory_flags);
    VK_CHECK(vkAllocateMemory(ctx.device, &allocation, nullptr, &buffer.memory));
    VK_CHECK(vkBindBufferMemory(ctx.device, buffer.handle, buffer.memory, 0));
    return buffer;
}

Image create_image(const VulkanContext& ctx, uint32_t width, uint32_t height,
                   VkImageUsageFlags usage) {
    Image image;
    VkImageCreateInfo image_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = kFormat;
    image_info.extent = {width, height, 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = usage;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VK_CHECK(vkCreateImage(ctx.device, &image_info, nullptr, &image.handle));

    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(ctx.device, image.handle, &requirements);
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = find_memory_type(
        ctx, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VK_CHECK(vkAllocateMemory(ctx.device, &allocation, nullptr, &image.memory));
    VK_CHECK(vkBindImageMemory(ctx.device, image.handle, image.memory, 0));

    VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view_info.image = image.handle;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = kFormat;
    view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view_info.subresourceRange.baseMipLevel = 0;
    view_info.subresourceRange.levelCount = 1;
    view_info.subresourceRange.baseArrayLayer = 0;
    view_info.subresourceRange.layerCount = 1;
    VK_CHECK(vkCreateImageView(ctx.device, &view_info, nullptr, &image.view));
    return image;
}

VkShaderModule create_shader_module(const VulkanContext& ctx,
                                    const std::vector<uint32_t>& code) {
    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize = code.size() * sizeof(uint32_t);
    info.pCode = code.data();
    VkShaderModule module = VK_NULL_HANDLE;
    VK_CHECK(vkCreateShaderModule(ctx.device, &info, nullptr, &module));
    return module;
}

void image_barrier(VkCommandBuffer command_buffer, VkImage image,
                   VkImageLayout old_layout, VkImageLayout new_layout,
                   VkAccessFlags source_access, VkAccessFlags destination_access,
                   VkPipelineStageFlags source_stage, VkPipelineStageFlags destination_stage) {
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.oldLayout = old_layout;
    barrier.newLayout = new_layout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;
    barrier.srcAccessMask = source_access;
    barrier.dstAccessMask = destination_access;
    vkCmdPipelineBarrier(command_buffer, source_stage, destination_stage, 0,
                         0, nullptr, 0, nullptr, 1, &barrier);
}

void destroy_buffer(const VulkanContext& ctx, Buffer& buffer) {
    if (buffer.handle != VK_NULL_HANDLE) vkDestroyBuffer(ctx.device, buffer.handle, nullptr);
    if (buffer.memory != VK_NULL_HANDLE) vkFreeMemory(ctx.device, buffer.memory, nullptr);
    buffer = {};
}

void destroy_image(const VulkanContext& ctx, Image& image) {
    if (image.view != VK_NULL_HANDLE) vkDestroyImageView(ctx.device, image.view, nullptr);
    if (image.handle != VK_NULL_HANDLE) vkDestroyImage(ctx.device, image.handle, nullptr);
    if (image.memory != VK_NULL_HANDLE) vkFreeMemory(ctx.device, image.memory, nullptr);
    image = {};
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 5) {
        std::cerr << "usage: phase2_executor <vert.spv> <frag.spv> <frame.rgba> <evidence.json>\n";
        return 64;
    }

    const std::string vertex_path = argv[1];
    const std::string fragment_path = argv[2];
    const std::string frame_path = argv[3];
    const std::string evidence_path = argv[4];

    VulkanContext ctx{};
    Buffer upload{};
    Buffer readback{};
    Image texture{};
    Image target{};
    VkSampler sampler = VK_NULL_HANDLE;
    VkRenderPass render_pass = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptor_layout = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
    VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkShaderModule vertex_module = VK_NULL_HANDLE;
    VkShaderModule fragment_module = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkCommandBuffer command_buffer = VK_NULL_HANDLE;

    try {
        const auto started = std::chrono::steady_clock::now();
        ctx = create_context();
        std::cout << "GPU=" << ctx.properties.deviceName << "\n";

        const size_t upload_bytes = static_cast<size_t>(kTextureWidth) * kTextureHeight * 4u;
        upload = create_buffer(ctx, upload_bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                   VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        void* upload_map = nullptr;
        VK_CHECK(vkMapMemory(ctx.device, upload.memory, 0, upload.size, 0, &upload_map));
        auto* upload_pixels = static_cast<uint8_t*>(upload_map);
        for (uint32_t y = 0; y < kTextureHeight; ++y) {
            for (uint32_t x = 0; x < kTextureWidth; ++x) {
                const size_t offset = (static_cast<size_t>(y) * kTextureWidth + x) * 4u;
                const bool checker = (((x / 8u) + (y / 8u)) & 1u) != 0u;
                upload_pixels[offset + 0] = checker ? 240u : static_cast<uint8_t>((x * 255u) / (kTextureWidth - 1u));
                upload_pixels[offset + 1] = checker ? static_cast<uint8_t>((y * 255u) / (kTextureHeight - 1u)) : 48u;
                upload_pixels[offset + 2] = checker ? 32u : 220u;
                upload_pixels[offset + 3] = 255u;
            }
        }
        vkUnmapMemory(ctx.device, upload.memory);

        texture = create_image(ctx, kTextureWidth, kTextureHeight,
                               VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
        target = create_image(ctx, kWidth, kHeight,
                              VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        readback = create_buffer(ctx, static_cast<VkDeviceSize>(kWidth) * kHeight * 4u,
                                 VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                     VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

        VkSamplerCreateInfo sampler_info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sampler_info.magFilter = VK_FILTER_NEAREST;
        sampler_info.minFilter = VK_FILTER_NEAREST;
        sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        sampler_info.maxLod = 0.0f;
        VK_CHECK(vkCreateSampler(ctx.device, &sampler_info, nullptr, &sampler));

        VkAttachmentDescription attachment{};
        attachment.format = kFormat;
        attachment.samples = VK_SAMPLE_COUNT_1_BIT;
        attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        attachment.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        VkAttachmentReference color_reference{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &color_reference;
        VkSubpassDependency dependency{};
        dependency.srcSubpass = 0;
        dependency.dstSubpass = VK_SUBPASS_EXTERNAL;
        dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependency.dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
        dependency.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dependency.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        VkRenderPassCreateInfo render_pass_info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        render_pass_info.attachmentCount = 1;
        render_pass_info.pAttachments = &attachment;
        render_pass_info.subpassCount = 1;
        render_pass_info.pSubpasses = &subpass;
        render_pass_info.dependencyCount = 1;
        render_pass_info.pDependencies = &dependency;
        VK_CHECK(vkCreateRenderPass(ctx.device, &render_pass_info, nullptr, &render_pass));

        VkFramebufferCreateInfo framebuffer_info{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        framebuffer_info.renderPass = render_pass;
        framebuffer_info.attachmentCount = 1;
        framebuffer_info.pAttachments = &target.view;
        framebuffer_info.width = kWidth;
        framebuffer_info.height = kHeight;
        framebuffer_info.layers = 1;
        VK_CHECK(vkCreateFramebuffer(ctx.device, &framebuffer_info, nullptr, &framebuffer));

        VkDescriptorSetLayoutBinding descriptor_binding{};
        descriptor_binding.binding = 0;
        descriptor_binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        descriptor_binding.descriptorCount = 1;
        descriptor_binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo descriptor_layout_info{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        descriptor_layout_info.bindingCount = 1;
        descriptor_layout_info.pBindings = &descriptor_binding;
        VK_CHECK(vkCreateDescriptorSetLayout(ctx.device, &descriptor_layout_info, nullptr,
                                             &descriptor_layout));

        VkDescriptorPoolSize descriptor_pool_size{};
        descriptor_pool_size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        descriptor_pool_size.descriptorCount = 1;
        VkDescriptorPoolCreateInfo descriptor_pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        descriptor_pool_info.maxSets = 1;
        descriptor_pool_info.poolSizeCount = 1;
        descriptor_pool_info.pPoolSizes = &descriptor_pool_size;
        VK_CHECK(vkCreateDescriptorPool(ctx.device, &descriptor_pool_info, nullptr,
                                        &descriptor_pool));

        VkDescriptorSetAllocateInfo descriptor_allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        descriptor_allocate.descriptorPool = descriptor_pool;
        descriptor_allocate.descriptorSetCount = 1;
        descriptor_allocate.pSetLayouts = &descriptor_layout;
        VK_CHECK(vkAllocateDescriptorSets(ctx.device, &descriptor_allocate, &descriptor_set));
        VkDescriptorImageInfo descriptor_image{};
        descriptor_image.sampler = sampler;
        descriptor_image.imageView = texture.view;
        descriptor_image.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkWriteDescriptorSet descriptor_write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        descriptor_write.dstSet = descriptor_set;
        descriptor_write.dstBinding = 0;
        descriptor_write.descriptorCount = 1;
        descriptor_write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        descriptor_write.pImageInfo = &descriptor_image;
        vkUpdateDescriptorSets(ctx.device, 1, &descriptor_write, 0, nullptr);

        const auto vertex_code = read_spirv(vertex_path);
        const auto fragment_code = read_spirv(fragment_path);
        vertex_module = create_shader_module(ctx, vertex_code);
        fragment_module = create_shader_module(ctx, fragment_code);

        VkPipelineLayoutCreateInfo pipeline_layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pipeline_layout_info.setLayoutCount = 1;
        pipeline_layout_info.pSetLayouts = &descriptor_layout;
        VK_CHECK(vkCreatePipelineLayout(ctx.device, &pipeline_layout_info, nullptr,
                                        &pipeline_layout));

        std::array<VkPipelineShaderStageCreateInfo, 2> shader_stages{};
        shader_stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        shader_stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        shader_stages[0].module = vertex_module;
        shader_stages[0].pName = "main";
        shader_stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        shader_stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        shader_stages[1].module = fragment_module;
        shader_stages[1].pName = "main";

        VkPipelineVertexInputStateCreateInfo vertex_input{
            VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo input_assembly{
            VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkViewport viewport{};
        viewport.width = static_cast<float>(kWidth);
        viewport.height = static_cast<float>(kHeight);
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;
        VkRect2D scissor{{0, 0}, {kWidth, kHeight}};
        VkPipelineViewportStateCreateInfo viewport_state{
            VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        viewport_state.viewportCount = 1;
        viewport_state.pViewports = &viewport;
        viewport_state.scissorCount = 1;
        viewport_state.pScissors = &scissor;
        VkPipelineRasterizationStateCreateInfo rasterization{
            VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rasterization.polygonMode = VK_POLYGON_MODE_FILL;
        rasterization.cullMode = VK_CULL_MODE_NONE;
        rasterization.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rasterization.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo multisample{
            VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState blend_attachment{};
        blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                           VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo blend{
            VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        blend.attachmentCount = 1;
        blend.pAttachments = &blend_attachment;
        VkGraphicsPipelineCreateInfo pipeline_info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        pipeline_info.stageCount = static_cast<uint32_t>(shader_stages.size());
        pipeline_info.pStages = shader_stages.data();
        pipeline_info.pVertexInputState = &vertex_input;
        pipeline_info.pInputAssemblyState = &input_assembly;
        pipeline_info.pViewportState = &viewport_state;
        pipeline_info.pRasterizationState = &rasterization;
        pipeline_info.pMultisampleState = &multisample;
        pipeline_info.pColorBlendState = &blend;
        pipeline_info.layout = pipeline_layout;
        pipeline_info.renderPass = render_pass;
        pipeline_info.subpass = 0;
        VK_CHECK(vkCreateGraphicsPipelines(ctx.device, VK_NULL_HANDLE, 1, &pipeline_info,
                                           nullptr, &pipeline));

        VkCommandBufferAllocateInfo command_allocate{
            VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        command_allocate.commandPool = ctx.command_pool;
        command_allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        command_allocate.commandBufferCount = 1;
        VK_CHECK(vkAllocateCommandBuffers(ctx.device, &command_allocate, &command_buffer));
        VkCommandBufferBeginInfo command_begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        command_begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        VK_CHECK(vkBeginCommandBuffer(command_buffer, &command_begin));

        image_barrier(command_buffer, texture.handle, VK_IMAGE_LAYOUT_UNDEFINED,
                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                      VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy upload_region{};
        upload_region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        upload_region.imageExtent = {kTextureWidth, kTextureHeight, 1};
        vkCmdCopyBufferToImage(command_buffer, upload.handle, texture.handle,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &upload_region);
        image_barrier(command_buffer, texture.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                      VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

        VkClearValue clear{};
        clear.color = {{0.01f, 0.01f, 0.01f, 1.0f}};
        VkRenderPassBeginInfo render_begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        render_begin.renderPass = render_pass;
        render_begin.framebuffer = framebuffer;
        render_begin.renderArea = {{0, 0}, {kWidth, kHeight}};
        render_begin.clearValueCount = 1;
        render_begin.pClearValues = &clear;
        vkCmdBeginRenderPass(command_buffer, &render_begin, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                pipeline_layout, 0, 1, &descriptor_set, 0, nullptr);
        vkCmdDraw(command_buffer, 3, 1, 0, 0);
        vkCmdEndRenderPass(command_buffer);

        VkBufferImageCopy readback_region{};
        readback_region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        readback_region.imageExtent = {kWidth, kHeight, 1};
        vkCmdCopyImageToBuffer(command_buffer, target.handle,
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.handle, 1,
                               &readback_region);
        VK_CHECK(vkEndCommandBuffer(command_buffer));

        VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        VK_CHECK(vkCreateFence(ctx.device, &fence_info, nullptr, &fence));
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &command_buffer;
        VK_CHECK(vkQueueSubmit(ctx.queue, 1, &submit, fence));
        const VkResult fence_result = vkWaitForFences(ctx.device, 1, &fence, VK_TRUE,
                                                      10'000'000'000ull);
        if (fence_result != VK_SUCCESS) {
            throw std::runtime_error("Fence completion failed or timed out: " +
                                     std::to_string(fence_result));
        }

        void* readback_map = nullptr;
        VK_CHECK(vkMapMemory(ctx.device, readback.memory, 0, readback.size, 0,
                             &readback_map));
        const auto* frame_bytes = static_cast<const uint8_t*>(readback_map);
        std::ofstream frame_output(frame_path, std::ios::binary);
        frame_output.write(reinterpret_cast<const char*>(frame_bytes),
                           static_cast<std::streamsize>(readback.size));
        frame_output.close();
        if (!frame_output) {
            throw std::runtime_error("Unable to write frame: " + frame_path);
        }
        const uint32_t frame_crc = crc32(frame_bytes, static_cast<size_t>(readback.size));
        const size_t corner = 0;
        const size_t center = (static_cast<size_t>(kHeight / 2u) * kWidth + kWidth / 2u) * 4u;
        const bool non_uniform = std::memcmp(frame_bytes + corner, frame_bytes + center, 4) != 0;
        const std::array<uint8_t, 4> corner_pixel = {
            frame_bytes[corner + 0], frame_bytes[corner + 1], frame_bytes[corner + 2],
            frame_bytes[corner + 3]};
        const std::array<uint8_t, 4> center_pixel = {
            frame_bytes[center + 0], frame_bytes[center + 1], frame_bytes[center + 2],
            frame_bytes[center + 3]};
        vkUnmapMemory(ctx.device, readback.memory);
        if (!non_uniform || frame_crc == 0u) {
            throw std::runtime_error("Rendered frame validation failed");
        }

        const auto completed = std::chrono::steady_clock::now();
        const double elapsed_ms =
            std::chrono::duration<double, std::milli>(completed - started).count();
        std::ofstream evidence(evidence_path);
        evidence << "{\n"
                 << "  \"phase\": 2,\n"
                 << "  \"backend\": \"Vulkan\",\n"
                 << "  \"gpu\": \"" << ctx.properties.deviceName << "\",\n"
                 << "  \"physical_device_type\": " << static_cast<int>(ctx.properties.deviceType) << ",\n"
                 << "  \"hardware_vulkan\": " << (ctx.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU ? "false" : "true") << ",\n"
                 << "  \"resource_creation\": true,\n"
                 << "  \"shader_modules\": 2,\n"
                 << "  \"graphics_pipeline_created\": true,\n"
                 << "  \"upload_heap_bytes\": " << upload_bytes << ",\n"
                 << "  \"descriptor_sets_reconstructed\": 1,\n"
                 << "  \"command_buffers_submitted\": 1,\n"
                 << "  \"draw_calls\": 1,\n"
                 << "  \"fence_completed\": true,\n"
                 << "  \"frame_width\": " << kWidth << ",\n"
                 << "  \"frame_height\": " << kHeight << ",\n"
                 << "  \"frame_crc32\": \"" << std::hex << frame_crc << std::dec << "\",\n"
                 << "  \"frame_non_uniform\": true,\n"
                 << "  \"corner_pixel_rgba\": [" << static_cast<int>(corner_pixel[0]) << ", "
                 << static_cast<int>(corner_pixel[1]) << ", "
                 << static_cast<int>(corner_pixel[2]) << ", "
                 << static_cast<int>(corner_pixel[3]) << "],\n"
                 << "  \"center_pixel_rgba\": [" << static_cast<int>(center_pixel[0]) << ", "
                 << static_cast<int>(center_pixel[1]) << ", "
                 << static_cast<int>(center_pixel[2]) << ", "
                 << static_cast<int>(center_pixel[3]) << "],\n"
                 << "  \"elapsed_ms\": " << elapsed_ms << "\n"
                 << "}\n";
        evidence.close();
        if (!evidence) {
            throw std::runtime_error("Unable to write evidence: " + evidence_path);
        }

        std::cout << "PHASE2_VULKAN_EXECUTOR=PASS"
                  << " crc32=" << std::hex << frame_crc << std::dec
                  << " upload_bytes=" << upload_bytes
                  << " descriptor_sets=1 command_buffers=1 fence=complete\n";

        vkDeviceWaitIdle(ctx.device);
        if (fence != VK_NULL_HANDLE) vkDestroyFence(ctx.device, fence, nullptr);
        if (pipeline != VK_NULL_HANDLE) vkDestroyPipeline(ctx.device, pipeline, nullptr);
        if (pipeline_layout != VK_NULL_HANDLE)
            vkDestroyPipelineLayout(ctx.device, pipeline_layout, nullptr);
        if (fragment_module != VK_NULL_HANDLE)
            vkDestroyShaderModule(ctx.device, fragment_module, nullptr);
        if (vertex_module != VK_NULL_HANDLE)
            vkDestroyShaderModule(ctx.device, vertex_module, nullptr);
        if (descriptor_pool != VK_NULL_HANDLE)
            vkDestroyDescriptorPool(ctx.device, descriptor_pool, nullptr);
        if (descriptor_layout != VK_NULL_HANDLE)
            vkDestroyDescriptorSetLayout(ctx.device, descriptor_layout, nullptr);
        if (framebuffer != VK_NULL_HANDLE)
            vkDestroyFramebuffer(ctx.device, framebuffer, nullptr);
        if (render_pass != VK_NULL_HANDLE)
            vkDestroyRenderPass(ctx.device, render_pass, nullptr);
        if (sampler != VK_NULL_HANDLE) vkDestroySampler(ctx.device, sampler, nullptr);
        destroy_buffer(ctx, readback);
        destroy_buffer(ctx, upload);
        destroy_image(ctx, target);
        destroy_image(ctx, texture);
        if (ctx.command_pool != VK_NULL_HANDLE)
            vkDestroyCommandPool(ctx.device, ctx.command_pool, nullptr);
        if (ctx.device != VK_NULL_HANDLE) vkDestroyDevice(ctx.device, nullptr);
        if (ctx.instance != VK_NULL_HANDLE) vkDestroyInstance(ctx.instance, nullptr);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "PHASE2_VULKAN_EXECUTOR=FAIL error=" << error.what() << "\n";
        if (ctx.device != VK_NULL_HANDLE) vkDeviceWaitIdle(ctx.device);
        if (fence != VK_NULL_HANDLE) vkDestroyFence(ctx.device, fence, nullptr);
        if (pipeline != VK_NULL_HANDLE) vkDestroyPipeline(ctx.device, pipeline, nullptr);
        if (pipeline_layout != VK_NULL_HANDLE)
            vkDestroyPipelineLayout(ctx.device, pipeline_layout, nullptr);
        if (fragment_module != VK_NULL_HANDLE)
            vkDestroyShaderModule(ctx.device, fragment_module, nullptr);
        if (vertex_module != VK_NULL_HANDLE)
            vkDestroyShaderModule(ctx.device, vertex_module, nullptr);
        if (descriptor_pool != VK_NULL_HANDLE)
            vkDestroyDescriptorPool(ctx.device, descriptor_pool, nullptr);
        if (descriptor_layout != VK_NULL_HANDLE)
            vkDestroyDescriptorSetLayout(ctx.device, descriptor_layout, nullptr);
        if (framebuffer != VK_NULL_HANDLE)
            vkDestroyFramebuffer(ctx.device, framebuffer, nullptr);
        if (render_pass != VK_NULL_HANDLE)
            vkDestroyRenderPass(ctx.device, render_pass, nullptr);
        if (sampler != VK_NULL_HANDLE) vkDestroySampler(ctx.device, sampler, nullptr);
        if (ctx.device != VK_NULL_HANDLE) {
            destroy_buffer(ctx, readback);
            destroy_buffer(ctx, upload);
            destroy_image(ctx, target);
            destroy_image(ctx, texture);
            if (ctx.command_pool != VK_NULL_HANDLE)
                vkDestroyCommandPool(ctx.device, ctx.command_pool, nullptr);
            vkDestroyDevice(ctx.device, nullptr);
        }
        if (ctx.instance != VK_NULL_HANDLE) vkDestroyInstance(ctx.instance, nullptr);
        return 1;
    }
}
