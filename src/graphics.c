#include <string.h>

#include "library.h"

alignas(uint32_t) static const unsigned char draw_shader[] = {
#embed "draw.spv"
};

static bool instance_create(Graphics *graphics) {
    const char *extensions[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME};
    return vkCreateInstance(&(VkInstanceCreateInfo){
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &(VkApplicationInfo){
            .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
            .apiVersion = VK_API_VERSION_1_3,
        },
        .enabledExtensionCount = 2,
        .ppEnabledExtensionNames = extensions,
    }, nullptr, &graphics->instance) == VK_SUCCESS;
}

static bool queue_family_choose(VkPhysicalDevice device, VkSurfaceKHR surface, uint32_t *family) {
    VkQueueFamilyProperties families[16];
    uint32_t family_count = 16;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &family_count, families);
    for (uint32_t index = 0; index < family_count; index++) {
        VkBool32 presents = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(device, index, surface, &presents);
        if (families[index].queueFlags & VK_QUEUE_COMPUTE_BIT && presents) {
            *family = index;
            return true;
        }
    }
    return false;
}

// Vulkan 1.3 and one queue family that computes and presents to the window. A GPU before a CPU.
static bool device_choose(Graphics *graphics) {
    VkPhysicalDevice devices[16];
    uint32_t device_count = 16;
    vkEnumeratePhysicalDevices(graphics->instance, &device_count, devices);
    for (int pass = 0; pass < 2; pass++) {
        for (uint32_t index = 0; index < device_count; index++) {
            VkPhysicalDeviceProperties properties;
            vkGetPhysicalDeviceProperties(devices[index], &properties);
            bool cpu = properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU;
            if (properties.apiVersion < VK_API_VERSION_1_3 || cpu != (pass == 1)
                || !queue_family_choose(devices[index], graphics->surface, &graphics->queue_family))
                continue;
            graphics->physical_device = devices[index];
            return true;
        }
    }
    return false;
}

static bool extension_supported(VkPhysicalDevice device, const char *name) {
    VkExtensionProperties extensions[512];
    uint32_t extension_count = 512;
    vkEnumerateDeviceExtensionProperties(device, nullptr, &extension_count, extensions);
    for (uint32_t index = 0; index < extension_count; index++)
        if (strcmp(extensions[index].extensionName, name) == 0)
            return true;
    return false;
}

// Turns on pixel import where the device has VK_EXT_external_memory_host.
static void host_import_prepare(Graphics *graphics) {
    if (!graphics->host_import)
        return;
    graphics->memory_host_pointer_properties = (PFN_vkGetMemoryHostPointerPropertiesEXT)
        vkGetDeviceProcAddr(graphics->device, "vkGetMemoryHostPointerPropertiesEXT");
    VkPhysicalDeviceExternalMemoryHostPropertiesEXT host_properties = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT,
    };
    vkGetPhysicalDeviceProperties2(graphics->physical_device, &(VkPhysicalDeviceProperties2){
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
        .pNext = &host_properties,
    });
    graphics->import_alignment = host_properties.minImportedHostPointerAlignment;
    graphics->host_import = graphics->memory_host_pointer_properties != nullptr;
}

static bool device_create(Graphics *graphics) {
    graphics->host_import = extension_supported(graphics->physical_device, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME);
    const char *extensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME};
    VkPhysicalDeviceVulkan12Features vulkan12_features = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
        .bufferDeviceAddress = VK_TRUE,
    };
    VkPhysicalDeviceFeatures2 features = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
        .pNext = &vulkan12_features,
        .features.shaderStorageImageWriteWithoutFormat = VK_TRUE,
    };
    float priority = 1;
    if (vkCreateDevice(graphics->physical_device, &(VkDeviceCreateInfo){
            .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
            .pNext = &features,
            .queueCreateInfoCount = 1,
            .pQueueCreateInfos = &(VkDeviceQueueCreateInfo){
                .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                .queueFamilyIndex = graphics->queue_family,
                .queueCount = 1,
                .pQueuePriorities = &priority,
            },
            .enabledExtensionCount = graphics->host_import ? 2 : 1,
            .ppEnabledExtensionNames = extensions,
        }, nullptr, &graphics->device) != VK_SUCCESS)
        return false;
    vkGetDeviceQueue(graphics->device, graphics->queue_family, 0, &graphics->queue);
    host_import_prepare(graphics);
    return true;
}

// 8-bit UNORM: palette colors are already sRGB, so they reach the screen unchanged.
static bool format_choose(Graphics *graphics) {
    VkSurfaceFormatKHR formats[64];
    uint32_t format_count = 64;
    vkGetPhysicalDeviceSurfaceFormatsKHR(graphics->physical_device, graphics->surface, &format_count, formats);
    for (uint32_t index = 0; index < format_count; index++) {
        VkFormat format = formats[index].format;
        VkFormatProperties properties;
        vkGetPhysicalDeviceFormatProperties(graphics->physical_device, format, &properties);
        if ((format == VK_FORMAT_B8G8R8A8_UNORM || format == VK_FORMAT_R8G8B8A8_UNORM)
            && formats[index].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR
            && properties.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) {
            graphics->format = format;
            return true;
        }
    }
    return false;
}

// One storage image, the swapchain image; everything else the shader reaches by address.
static bool pipeline_create(Graphics *graphics) {
    VkDevice device = graphics->device;
    VkShaderModule shader;
    if (vkCreateShaderModule(device, &(VkShaderModuleCreateInfo){
            .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .codeSize = sizeof draw_shader,
            .pCode = (const uint32_t *)draw_shader,
        }, nullptr, &shader) != VK_SUCCESS)
        return false;

    VkDescriptorSetLayout layouts[SWAPCHAIN_IMAGE_LIMIT];
    bool created = vkCreateDescriptorSetLayout(device, &(VkDescriptorSetLayoutCreateInfo){
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .bindingCount = 1,
            .pBindings = &(VkDescriptorSetLayoutBinding){
                .binding = 0,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                .descriptorCount = 1,
                .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
            },
        }, nullptr, &graphics->descriptor_layout) == VK_SUCCESS
        && vkCreatePipelineLayout(device, &(VkPipelineLayoutCreateInfo){
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .setLayoutCount = 1,
            .pSetLayouts = &graphics->descriptor_layout,
            .pushConstantRangeCount = 1,
            .pPushConstantRanges = &(VkPushConstantRange){
                .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
                .size = sizeof(VkDeviceAddress),
            },
        }, nullptr, &graphics->pipeline_layout) == VK_SUCCESS
        && vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &(VkComputePipelineCreateInfo){
            .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
            .stage = {
                .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                .module = shader,
                .pName = "main",
            },
            .layout = graphics->pipeline_layout,
        }, nullptr, &graphics->pipeline) == VK_SUCCESS
        && vkCreateDescriptorPool(device, &(VkDescriptorPoolCreateInfo){
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
            .maxSets = SWAPCHAIN_IMAGE_LIMIT,
            .poolSizeCount = 1,
            .pPoolSizes = &(VkDescriptorPoolSize){VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, SWAPCHAIN_IMAGE_LIMIT},
        }, nullptr, &graphics->descriptor_pool) == VK_SUCCESS;
    vkDestroyShaderModule(device, shader, nullptr);
    if (!created)
        return false;

    for (int index = 0; index < SWAPCHAIN_IMAGE_LIMIT; index++)
        layouts[index] = graphics->descriptor_layout;
    return vkAllocateDescriptorSets(device, &(VkDescriptorSetAllocateInfo){
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = graphics->descriptor_pool,
        .descriptorSetCount = SWAPCHAIN_IMAGE_LIMIT,
        .pSetLayouts = layouts,
    }, graphics->descriptor_sets) == VK_SUCCESS;
}

static bool commands_create(Graphics *graphics) {
    VkDevice device = graphics->device;
    return vkCreateCommandPool(device, &(VkCommandPoolCreateInfo){
            .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
            .queueFamilyIndex = graphics->queue_family,
        }, nullptr, &graphics->command_pool) == VK_SUCCESS
        && vkAllocateCommandBuffers(device, &(VkCommandBufferAllocateInfo){
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = graphics->command_pool,
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = 1,
        }, &graphics->commands) == VK_SUCCESS
        && vkCreateSemaphore(device, &(VkSemaphoreCreateInfo){
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
        }, nullptr, &graphics->acquired) == VK_SUCCESS
        && vkCreateFence(device, &(VkFenceCreateInfo){
            .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
            .flags = VK_FENCE_CREATE_SIGNALED_BIT,
        }, nullptr, &graphics->frame_done) == VK_SUCCESS;
}

// The swapchain comes later, from graphics_resize, once the window has a size.
bool graphics_open(Library *library) {
    Graphics *graphics = &library->graphics;
    if (!instance_create(graphics))
        return failed("cannot make a Vulkan 1.3 instance");
    if (vkCreateWaylandSurfaceKHR(graphics->instance, &(VkWaylandSurfaceCreateInfoKHR){
            .sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR,
            .display = library->display,
            .surface = library->window.surface,
        }, nullptr, &graphics->surface) != VK_SUCCESS)
        return failed("cannot make a Vulkan surface for the window");
    if (!device_choose(graphics))
        return failed("no Vulkan 1.3 device can draw to the window");
    if (!device_create(graphics))
        return failed("the Vulkan device lacks buffer device address or storage image writes without format");
    if (!format_choose(graphics))
        return failed("the window offers no 8-bit format the shader can write");
    if (!pipeline_create(graphics) || !commands_create(graphics))
        return failed("cannot make the draw pipeline");
    return true;
}

static void swapchain_images_destroy(Graphics *graphics) {
    for (uint32_t index = 0; index < graphics->image_count; index++) {
        vkDestroyImageView(graphics->device, graphics->views[index], nullptr);
        vkDestroySemaphore(graphics->device, graphics->rendered[index], nullptr);
    }
    graphics->image_count = 0;
}

void graphics_close(Graphics *graphics) {
    VkDevice device = graphics->device;
    if (device) {
        vkDeviceWaitIdle(device);
        swapchain_images_destroy(graphics);
        vkDestroySwapchainKHR(device, graphics->swapchain, nullptr);
        vkDestroyFence(device, graphics->frame_done, nullptr);
        vkDestroySemaphore(device, graphics->acquired, nullptr);
        vkDestroyCommandPool(device, graphics->command_pool, nullptr);
        vkDestroyPipeline(device, graphics->pipeline, nullptr);
        vkDestroyPipelineLayout(device, graphics->pipeline_layout, nullptr);
        vkDestroyDescriptorPool(device, graphics->descriptor_pool, nullptr);
        vkDestroyDescriptorSetLayout(device, graphics->descriptor_layout, nullptr);
        vkDestroyDevice(device, nullptr);
    }
    if (graphics->instance) {
        vkDestroySurfaceKHR(graphics->instance, graphics->surface, nullptr);
        vkDestroyInstance(graphics->instance, nullptr);
    }
}

// Remakes the swapchain at the window's size. Waits for the GPU, since it may still use the old images.
bool graphics_resize(Graphics *graphics, uint32_t width, uint32_t height) {
    VkDevice device = graphics->device;
    vkDeviceWaitIdle(device);
    swapchain_images_destroy(graphics);

    VkSurfaceCapabilitiesKHR capabilities;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(graphics->physical_device, graphics->surface, &capabilities);
    if (!(capabilities.supportedUsageFlags & VK_IMAGE_USAGE_STORAGE_BIT))
        return failed("the window cannot take a storage image");
    if (capabilities.currentExtent.width != UINT32_MAX) {
        width = capabilities.currentExtent.width;
        height = capabilities.currentExtent.height;
    }
    uint32_t image_count = capabilities.minImageCount + 1;
    if (capabilities.maxImageCount && image_count > capabilities.maxImageCount)
        image_count = capabilities.maxImageCount;

    VkSwapchainKHR old_swapchain = graphics->swapchain;
    VkResult result = vkCreateSwapchainKHR(device, &(VkSwapchainCreateInfoKHR){
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = graphics->surface,
        .minImageCount = image_count,
        .imageFormat = graphics->format,
        .imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
        .imageExtent = {width, height},
        .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_STORAGE_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform = capabilities.currentTransform,
        .compositeAlpha = capabilities.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
            ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR : VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
        .presentMode = VK_PRESENT_MODE_FIFO_KHR,
        .clipped = VK_TRUE,
        .oldSwapchain = old_swapchain,
    }, nullptr, &graphics->swapchain);
    vkDestroySwapchainKHR(device, old_swapchain, nullptr);
    if (result != VK_SUCCESS) {
        graphics->swapchain = VK_NULL_HANDLE;
        return failed("cannot make the swapchain");
    }

    graphics->image_count = SWAPCHAIN_IMAGE_LIMIT;
    if (vkGetSwapchainImagesKHR(device, graphics->swapchain, &graphics->image_count, graphics->images) != VK_SUCCESS)
        return failed("the swapchain has more images than SWAPCHAIN_IMAGE_LIMIT");
    for (uint32_t index = 0; index < graphics->image_count; index++) {
        if (vkCreateImageView(device, &(VkImageViewCreateInfo){
                .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                .image = graphics->images[index],
                .viewType = VK_IMAGE_VIEW_TYPE_2D,
                .format = graphics->format,
                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
            }, nullptr, &graphics->views[index]) != VK_SUCCESS
            || vkCreateSemaphore(device, &(VkSemaphoreCreateInfo){
                .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
            }, nullptr, &graphics->rendered[index]) != VK_SUCCESS)
            return failed("cannot make the swapchain image views");
        vkUpdateDescriptorSets(device, 1, &(VkWriteDescriptorSet){
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = graphics->descriptor_sets[index],
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            .pImageInfo = &(VkDescriptorImageInfo){
                .imageView = graphics->views[index],
                .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
            },
        }, 0, nullptr);
    }
    graphics->extent = (VkExtent2D){width, height};
    return true;
}

// Waits until the GPU is done with the last frame, so its buffers may be written.
void graphics_wait(Graphics *graphics) {
    if (graphics->frame_done)
        vkWaitForFences(graphics->device, 1, &graphics->frame_done, VK_TRUE, UINT64_MAX);
}

static void commands_record(Graphics *graphics, uint32_t image, VkDeviceAddress frame) {
    VkCommandBuffer commands = graphics->commands;
    VkImageSubresourceRange whole_image = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkBeginCommandBuffer(commands, &(VkCommandBufferBeginInfo){
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    });
    vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &(VkImageMemoryBarrier){
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = graphics->images[image],
        .subresourceRange = whole_image,
    });
    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, graphics->pipeline);
    vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_COMPUTE, graphics->pipeline_layout,
                            0, 1, &graphics->descriptor_sets[image], 0, nullptr);
    vkCmdPushConstants(commands, graphics->pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof frame, &frame);
    vkCmdDispatch(commands, (graphics->extent.width + 7) / 8, (graphics->extent.height + 7) / 8, 1);
    // The image goes to the window, the miss table back to the CPU.
    vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT | VK_PIPELINE_STAGE_HOST_BIT, 0,
                         1, &(VkMemoryBarrier){
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
    }, 0, nullptr, 1, &(VkImageMemoryBarrier){
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
        .newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = graphics->images[image],
        .subresourceRange = whole_image,
    });
    vkEndCommandBuffer(commands);
}

// Runs the shader over the next swapchain image and presents it. On false nothing was shown,
// and the swapchain is marked to be remade.
bool graphics_present(Graphics *graphics, VkDeviceAddress frame) {
    uint32_t image;
    VkResult result = vkAcquireNextImageKHR(graphics->device, graphics->swapchain, UINT64_MAX,
                                            graphics->acquired, VK_NULL_HANDLE, &image);
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
        graphics->extent = (VkExtent2D){};
        return false;
    }
    vkResetFences(graphics->device, 1, &graphics->frame_done);
    commands_record(graphics, image, frame);
    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    vkQueueSubmit(graphics->queue, 1, &(VkSubmitInfo){
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &graphics->acquired,
        .pWaitDstStageMask = &wait_stage,
        .commandBufferCount = 1,
        .pCommandBuffers = &graphics->commands,
        .signalSemaphoreCount = 1,
        .pSignalSemaphores = &graphics->rendered[image],
    }, graphics->frame_done);
    result = vkQueuePresentKHR(graphics->queue, &(VkPresentInfoKHR){
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &graphics->rendered[image],
        .swapchainCount = 1,
        .pSwapchains = &graphics->swapchain,
        .pImageIndices = &image,
    });
    if (result != VK_SUCCESS)
        graphics->extent = (VkExtent2D){};
    return result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR;
}

// Host visible and coherent memory; device local too where the GPU has both.
static bool memory_type_choose(Graphics *graphics, uint32_t allowed_types, uint32_t *type) {
    VkPhysicalDeviceMemoryProperties properties;
    vkGetPhysicalDeviceMemoryProperties(graphics->physical_device, &properties);
    VkMemoryPropertyFlags host = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    VkMemoryPropertyFlags preferences[] = {host | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, host};
    for (int preference = 0; preference < 2; preference++) {
        VkMemoryPropertyFlags wanted = preferences[preference];
        for (uint32_t index = 0; index < properties.memoryTypeCount; index++) {
            if (allowed_types & (1u << index) && (properties.memoryTypes[index].propertyFlags & wanted) == wanted) {
                *type = index;
                return true;
            }
        }
    }
    return false;
}

// Reachable from shaders by address. host_memory is imported in place; nullptr allocates new memory.
static bool buffer_allocate(Graphics *graphics, VkDeviceSize size, void *host_memory, Buffer *buffer) {
    VkDevice device = graphics->device;
    *buffer = (Buffer){.size = size, .mapped = host_memory};
    if (vkCreateBuffer(device, &(VkBufferCreateInfo){
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .pNext = host_memory ? &(VkExternalMemoryBufferCreateInfo){
                .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
                .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
            } : nullptr,
            .size = size,
            .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        }, nullptr, &buffer->buffer) != VK_SUCCESS)
        return false;

    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(device, buffer->buffer, &requirements);
    VkMemoryAllocateFlagsInfo flags = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
        .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT,
    };
    VkImportMemoryHostPointerInfoEXT import = {
        .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT,
        .pNext = &flags,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
        .pHostPointer = host_memory,
    };
    VkMemoryHostPointerPropertiesEXT host_properties = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT,
        .memoryTypeBits = ~0u,
    };
    if (host_memory && graphics->memory_host_pointer_properties(device,
            VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, host_memory, &host_properties) != VK_SUCCESS)
        return false;
    uint32_t type;
    if (!memory_type_choose(graphics, requirements.memoryTypeBits & host_properties.memoryTypeBits, &type)
        || vkAllocateMemory(device, &(VkMemoryAllocateInfo){
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .pNext = host_memory ? (void *)&import : &flags,
            .allocationSize = host_memory ? size : requirements.size,
            .memoryTypeIndex = type,
        }, nullptr, &buffer->memory) != VK_SUCCESS
        || vkBindBufferMemory(device, buffer->buffer, buffer->memory, 0) != VK_SUCCESS)
        return false;
    buffer->address = vkGetBufferDeviceAddress(device, &(VkBufferDeviceAddressInfo){
        .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
        .buffer = buffer->buffer,
    });
    return true;
}

// Zeroed and mapped.
bool buffer_create(Graphics *graphics, VkDeviceSize size, Buffer *buffer) {
    if (!buffer_allocate(graphics, size, nullptr, buffer)
        || vkMapMemory(graphics->device, buffer->memory, 0, VK_WHOLE_SIZE, 0, &buffer->mapped) != VK_SUCCESS)
        return false;
    memset(buffer->mapped, 0, size);
    return true;
}

// The GPU reads host_memory in place. Both it and size must be multiples of import_alignment.
bool buffer_import(Graphics *graphics, void *host_memory, VkDeviceSize size, Buffer *buffer) {
    return buffer_allocate(graphics, size, host_memory, buffer);
}

// Grows the buffer to at least size bytes, losing its contents. The GPU must be done with it.
bool buffer_reserve(Graphics *graphics, Buffer *buffer, VkDeviceSize size) {
    if (buffer->size >= size)
        return true;
    VkDeviceSize grown = buffer->size ? buffer->size : 65536;
    while (grown < size)
        grown *= 2;
    buffer_destroy(graphics, buffer);
    return buffer_create(graphics, grown, buffer);
}

void buffer_destroy(Graphics *graphics, Buffer *buffer) {
    if (!graphics->device)
        return;
    vkDestroyBuffer(graphics->device, buffer->buffer, nullptr);
    vkFreeMemory(graphics->device, buffer->memory, nullptr);
    *buffer = (Buffer){};
}
