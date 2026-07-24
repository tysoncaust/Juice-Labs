#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void check(VkResult result, const char* expression, int line) {
    if (result != VK_SUCCESS) {
        throw std::runtime_error(std::string("Vulkan failure ") + std::to_string(result) +
                                 " at line " + std::to_string(line) + ": " + expression);
    }
}
#define VK_CHECK(expr) check((expr), #expr, __LINE__)

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

std::string json_escape(const char* text) {
    std::ostringstream out;
    for (const unsigned char c : std::string(text ? text : "")) {
        switch (c) {
            case '\\': out << "\\\\"; break;
            case '"': out << "\\\""; break;
            case '\n': out << "\\n"; break;
            case '\r': out << "\\r"; break;
            case '\t': out << "\\t"; break;
            default:
                if (c < 0x20) {
                    out << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                        << static_cast<int>(c) << std::dec;
                } else {
                    out << static_cast<char>(c);
                }
        }
    }
    return out.str();
}

std::string uuid_string(const uint8_t* bytes, size_t count) {
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (size_t i = 0; i < count; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) out << '-';
        out << std::setw(2) << static_cast<unsigned>(bytes[i]);
    }
    return out.str();
}

uint32_t find_memory_type(const VkPhysicalDeviceMemoryProperties& properties,
                          uint32_t bits, VkMemoryPropertyFlags required) {
    for (uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
        if ((bits & (1u << i)) != 0u &&
            (properties.memoryTypes[i].propertyFlags & required) == required) {
            return i;
        }
    }
    throw std::runtime_error("No compatible memory type");
}

bool contains_software_marker(const std::string& name) {
    const std::string value = lower(name);
    const std::array<const char*, 5> markers = {
        "llvmpipe", "lavapipe", "software", "swiftshader", "cpu"
    };
    for (const char* marker : markers) {
        if (value.find(marker) != std::string::npos) return true;
    }
    return false;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: phase2_hardware_probe <evidence.json>\n";
        return 64;
    }
    const std::string evidence_path = argv[1];
    VkInstance instance = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkCommandPool command_pool = VK_NULL_HANDLE;
    VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    VkQueryPool query_pool = VK_NULL_HANDLE;
    VkBuffer workload_buffer = VK_NULL_HANDLE;
    VkDeviceMemory workload_memory = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;

    try {
        uint32_t loader_version = VK_API_VERSION_1_0;
        VK_CHECK(vkEnumerateInstanceVersion(&loader_version));

        VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        application.pApplicationName = "rgpu-phase2-hardware-probe";
        application.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
        application.pEngineName = "rgpu";
        application.engineVersion = VK_MAKE_VERSION(1, 0, 0);
        application.apiVersion = std::min(loader_version, VK_API_VERSION_1_2);
        VkInstanceCreateInfo instance_info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        instance_info.pApplicationInfo = &application;
        VK_CHECK(vkCreateInstance(&instance_info, nullptr, &instance));

        uint32_t physical_count = 0;
        VK_CHECK(vkEnumeratePhysicalDevices(instance, &physical_count, nullptr));
        if (physical_count == 0) throw std::runtime_error("No Vulkan devices");
        std::vector<VkPhysicalDevice> physical_devices(physical_count);
        VK_CHECK(vkEnumeratePhysicalDevices(instance, &physical_count, physical_devices.data()));

        const std::string expected_name = lower(std::getenv("RGPU_EXPECT_DEVICE") ?
            std::getenv("RGPU_EXPECT_DEVICE") : "nvidia");
        const uint32_t expected_vendor = std::getenv("RGPU_EXPECT_VENDOR_ID") ?
            static_cast<uint32_t>(std::strtoul(std::getenv("RGPU_EXPECT_VENDOR_ID"), nullptr, 0)) :
            0x10deu;

        VkPhysicalDevice physical = VK_NULL_HANDLE;
        VkPhysicalDeviceProperties basic_properties{};
        for (VkPhysicalDevice candidate : physical_devices) {
            VkPhysicalDeviceProperties properties{};
            vkGetPhysicalDeviceProperties(candidate, &properties);
            const std::string device_name = lower(properties.deviceName);
            if (properties.vendorID == expected_vendor &&
                device_name.find(expected_name) != std::string::npos &&
                properties.deviceType != VK_PHYSICAL_DEVICE_TYPE_CPU &&
                !contains_software_marker(properties.deviceName)) {
                physical = candidate;
                basic_properties = properties;
                break;
            }
        }
        if (physical == VK_NULL_HANDLE) {
            std::ostringstream message;
            message << "No matching hardware Vulkan device vendor=0x" << std::hex
                    << expected_vendor << " name_contains=" << expected_name;
            throw std::runtime_error(message.str());
        }

        uint32_t extension_count = 0;
        VK_CHECK(vkEnumerateDeviceExtensionProperties(physical, nullptr, &extension_count, nullptr));
        std::vector<VkExtensionProperties> extensions(extension_count);
        VK_CHECK(vkEnumerateDeviceExtensionProperties(physical, nullptr, &extension_count,
                                                       extensions.data()));
        const bool has_pci = std::any_of(extensions.begin(), extensions.end(),
            [](const VkExtensionProperties& extension) {
                return std::string(extension.extensionName) == VK_EXT_PCI_BUS_INFO_EXTENSION_NAME;
            });

        VkPhysicalDeviceDriverProperties driver{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
        VkPhysicalDeviceIDProperties identifiers{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
        VkPhysicalDevicePCIBusInfoPropertiesEXT pci{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PCI_BUS_INFO_PROPERTIES_EXT};
        driver.pNext = &identifiers;
        identifiers.pNext = has_pci ? &pci : nullptr;
        VkPhysicalDeviceProperties2 properties2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        properties2.pNext = &driver;
        vkGetPhysicalDeviceProperties2(physical, &properties2);

        VkPhysicalDeviceFeatures features{};
        vkGetPhysicalDeviceFeatures(physical, &features);
        VkPhysicalDeviceMemoryProperties memory_properties{};
        vkGetPhysicalDeviceMemoryProperties(physical, &memory_properties);

        uint32_t queue_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &queue_count, nullptr);
        std::vector<VkQueueFamilyProperties> queues(queue_count);
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &queue_count, queues.data());
        uint32_t queue_family = UINT32_MAX;
        for (uint32_t i = 0; i < queue_count; ++i) {
            if ((queues[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0u &&
                queues[i].timestampValidBits > 0u) {
                queue_family = i;
                break;
            }
        }
        if (queue_family == UINT32_MAX) {
            throw std::runtime_error("No graphics queue with timestamp support");
        }

        const float priority = 1.0f;
        VkDeviceQueueCreateInfo queue_info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queue_info.queueFamilyIndex = queue_family;
        queue_info.queueCount = 1;
        queue_info.pQueuePriorities = &priority;
        VkDeviceCreateInfo device_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        device_info.queueCreateInfoCount = 1;
        device_info.pQueueCreateInfos = &queue_info;
        VK_CHECK(vkCreateDevice(physical, &device_info, nullptr, &device));
        VkQueue queue = VK_NULL_HANDLE;
        vkGetDeviceQueue(device, queue_family, 0, &queue);

        constexpr VkDeviceSize kBufferBytes = 64ull * 1024ull * 1024ull;
        VkBufferCreateInfo buffer_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        buffer_info.size = kBufferBytes;
        buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VK_CHECK(vkCreateBuffer(device, &buffer_info, nullptr, &workload_buffer));
        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(device, workload_buffer, &requirements);
        VkMemoryAllocateInfo memory_info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        memory_info.allocationSize = requirements.size;
        memory_info.memoryTypeIndex = find_memory_type(
            memory_properties, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VK_CHECK(vkAllocateMemory(device, &memory_info, nullptr, &workload_memory));
        VK_CHECK(vkBindBufferMemory(device, workload_buffer, workload_memory, 0));

        VkQueryPoolCreateInfo query_info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        query_info.queryType = VK_QUERY_TYPE_TIMESTAMP;
        query_info.queryCount = 2;
        VK_CHECK(vkCreateQueryPool(device, &query_info, nullptr, &query_pool));

        VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pool_info.queueFamilyIndex = queue_family;
        VK_CHECK(vkCreateCommandPool(device, &pool_info, nullptr, &command_pool));
        VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocate.commandPool = command_pool;
        allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocate.commandBufferCount = 1;
        VK_CHECK(vkAllocateCommandBuffers(device, &allocate, &command_buffer));

        const uint32_t iterations = std::getenv("RGPU_TIMESTAMP_ITERATIONS") ?
            std::max(1u, static_cast<uint32_t>(std::strtoul(
                std::getenv("RGPU_TIMESTAMP_ITERATIONS"), nullptr, 10))) : 4096u;
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        VK_CHECK(vkBeginCommandBuffer(command_buffer, &begin));
        vkCmdResetQueryPool(command_buffer, query_pool, 0, 2);
        vkCmdWriteTimestamp(command_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, query_pool, 0);
        for (uint32_t i = 0; i < iterations; ++i) {
            vkCmdFillBuffer(command_buffer, workload_buffer, 0, kBufferBytes,
                            0xA5000000u ^ i);
        }
        vkCmdWriteTimestamp(command_buffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, query_pool, 1);
        VK_CHECK(vkEndCommandBuffer(command_buffer));

        VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        VK_CHECK(vkCreateFence(device, &fence_info, nullptr, &fence));
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &command_buffer;
        VK_CHECK(vkQueueSubmit(queue, 1, &submit, fence));
        VK_CHECK(vkWaitForFences(device, 1, &fence, VK_TRUE, 30'000'000'000ull));
        std::array<uint64_t, 2> timestamps{};
        VK_CHECK(vkGetQueryPoolResults(device, query_pool, 0, 2, sizeof(timestamps),
                                       timestamps.data(), sizeof(uint64_t),
                                       VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT));
        const uint64_t timestamp_delta = timestamps[1] - timestamps[0];
        const double timestamp_ns = static_cast<double>(timestamp_delta) *
                                    basic_properties.limits.timestampPeriod;
        if (timestamp_delta == 0 || timestamp_ns <= 0.0) {
            throw std::runtime_error("GPU timestamps did not advance");
        }

        std::ofstream evidence(evidence_path);
        if (!evidence) throw std::runtime_error("Unable to create evidence file");
        evidence << "{\n"
                 << "  \"hardware_vulkan\": true,\n"
                 << "  \"loader_api_version\": \""
                 << VK_VERSION_MAJOR(loader_version) << '.'
                 << VK_VERSION_MINOR(loader_version) << '.'
                 << VK_VERSION_PATCH(loader_version) << "\",\n"
                 << "  \"api_version\": \""
                 << VK_VERSION_MAJOR(properties2.properties.apiVersion) << '.'
                 << VK_VERSION_MINOR(properties2.properties.apiVersion) << '.'
                 << VK_VERSION_PATCH(properties2.properties.apiVersion) << "\",\n"
                 << "  \"device_name\": \"" << json_escape(properties2.properties.deviceName) << "\",\n"
                 << "  \"device_type\": " << static_cast<int>(properties2.properties.deviceType) << ",\n"
                 << "  \"vendor_id\": " << properties2.properties.vendorID << ",\n"
                 << "  \"device_id\": " << properties2.properties.deviceID << ",\n"
                 << "  \"driver_name\": \"" << json_escape(driver.driverName) << "\",\n"
                 << "  \"driver_info\": \"" << json_escape(driver.driverInfo) << "\",\n"
                 << "  \"driver_id\": " << static_cast<int>(driver.driverID) << ",\n"
                 << "  \"device_uuid\": \"" << uuid_string(identifiers.deviceUUID, VK_UUID_SIZE) << "\",\n"
                 << "  \"driver_uuid\": \"" << uuid_string(identifiers.driverUUID, VK_UUID_SIZE) << "\",\n"
                 << "  \"pci_available\": " << (has_pci ? "true" : "false") << ",\n"
                 << "  \"pci_domain\": " << (has_pci ? pci.pciDomain : 0) << ",\n"
                 << "  \"pci_bus\": " << (has_pci ? pci.pciBus : 0) << ",\n"
                 << "  \"pci_device\": " << (has_pci ? pci.pciDevice : 0) << ",\n"
                 << "  \"pci_function\": " << (has_pci ? pci.pciFunction : 0) << ",\n"
                 << "  \"selected_queue_family\": " << queue_family << ",\n"
                 << "  \"timestamp_valid_bits\": " << queues[queue_family].timestampValidBits << ",\n"
                 << "  \"timestamp_period_ns\": " << basic_properties.limits.timestampPeriod << ",\n"
                 << "  \"timestamp_start\": " << timestamps[0] << ",\n"
                 << "  \"timestamp_end\": " << timestamps[1] << ",\n"
                 << "  \"timestamp_delta_ticks\": " << timestamp_delta << ",\n"
                 << "  \"timestamp_elapsed_ns\": " << std::fixed << timestamp_ns << ",\n"
                 << "  \"workload_buffer_bytes\": " << kBufferBytes << ",\n"
                 << "  \"workload_fill_iterations\": " << iterations << ",\n"
                 << "  \"enabled_device_extensions\": [],\n"
                 << "  \"enabled_optional_features\": [],\n"
                 << "  \"available_device_extensions\": [\n";
        for (size_t i = 0; i < extensions.size(); ++i) {
            evidence << "    \"" << json_escape(extensions[i].extensionName) << "\""
                     << (i + 1 == extensions.size() ? "\n" : ",\n");
        }
        evidence << "  ],\n  \"queue_families\": [\n";
        for (size_t i = 0; i < queues.size(); ++i) {
            evidence << "    {\"index\": " << i
                     << ", \"flags\": " << queues[i].queueFlags
                     << ", \"count\": " << queues[i].queueCount
                     << ", \"timestamp_valid_bits\": " << queues[i].timestampValidBits
                     << ", \"min_image_transfer_granularity\": ["
                     << queues[i].minImageTransferGranularity.width << ','
                     << queues[i].minImageTransferGranularity.height << ','
                     << queues[i].minImageTransferGranularity.depth << "]}"
                     << (i + 1 == queues.size() ? "\n" : ",\n");
        }
        evidence << "  ]\n}\n";
        evidence.close();

        std::cout << "PHASE2_HARDWARE_VULKAN_PROBE=PASS device=\""
                  << properties2.properties.deviceName << "\" driver=\""
                  << driver.driverName << "\" timestamp_ticks=" << timestamp_delta
                  << " elapsed_ns=" << std::fixed << timestamp_ns << '\n';

        vkDeviceWaitIdle(device);
        vkDestroyFence(device, fence, nullptr);
        vkDestroyQueryPool(device, query_pool, nullptr);
        vkDestroyBuffer(device, workload_buffer, nullptr);
        vkFreeMemory(device, workload_memory, nullptr);
        vkDestroyCommandPool(device, command_pool, nullptr);
        vkDestroyDevice(device, nullptr);
        vkDestroyInstance(instance, nullptr);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "PHASE2_HARDWARE_VULKAN_PROBE=FAIL error=" << error.what() << '\n';
        if (device != VK_NULL_HANDLE) vkDeviceWaitIdle(device);
        if (fence != VK_NULL_HANDLE) vkDestroyFence(device, fence, nullptr);
        if (query_pool != VK_NULL_HANDLE) vkDestroyQueryPool(device, query_pool, nullptr);
        if (workload_buffer != VK_NULL_HANDLE) vkDestroyBuffer(device, workload_buffer, nullptr);
        if (workload_memory != VK_NULL_HANDLE) vkFreeMemory(device, workload_memory, nullptr);
        if (command_pool != VK_NULL_HANDLE) vkDestroyCommandPool(device, command_pool, nullptr);
        if (device != VK_NULL_HANDLE) vkDestroyDevice(device, nullptr);
        if (instance != VK_NULL_HANDLE) vkDestroyInstance(instance, nullptr);
        return 1;
    }
}
