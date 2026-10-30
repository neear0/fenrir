#include "../glfw/ui_glfw.h"

// vulkan setup for the glfw window, based on ImGui's example_glfw_vulkan

void celosia_glfw::vulkan::check_result(VkResult err) {
    if (err == VK_SUCCESS)
        return;
    std::cerr << "[vulkan] error: VkResult = " << err << std::endl;
    if (err < 0)
        abort();
}

uint32_t celosia_glfw::vulkan::memory_type(uint32_t type_bits, VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties memory;
    vkGetPhysicalDeviceMemoryProperties(physical_device, &memory);
    for (uint32_t i = 0; i < memory.memoryTypeCount; i++)
        if ((type_bits & (1u << i)) && (memory.memoryTypes[i].propertyFlags & properties) == properties)
            return i;
    return (uint32_t)-1;
}


static bool has_extension(const ImVector<VkExtensionProperties>& properties, const char* extension) {
    for (const VkExtensionProperties& p : properties)
        if (strcmp(p.extensionName, extension) == 0)
            return true;
    return false;
}

static bool create_instance() {
    uint32_t glfw_extension_count = 0;
    const char** glfw_extensions = glfwGetRequiredInstanceExtensions(&glfw_extension_count);
    if (!glfw_extensions)
        return false;

    ImVector<const char*> extensions;
    for (uint32_t i = 0; i < glfw_extension_count; i++)
        extensions.push_back(glfw_extensions[i]);

    uint32_t properties_count;
    ImVector<VkExtensionProperties> properties;
    vkEnumerateInstanceExtensionProperties(nullptr, &properties_count, nullptr);
    properties.resize(properties_count);
    celosia_glfw::vulkan::check_result(vkEnumerateInstanceExtensionProperties(nullptr, &properties_count, properties.Data));

    VkInstanceCreateInfo create_info = {};
    create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    if (has_extension(properties, VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME))
        extensions.push_back(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
    if (has_extension(properties, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
        extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
        create_info.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    }
    create_info.enabledExtensionCount = (uint32_t)extensions.Size;
    create_info.ppEnabledExtensionNames = extensions.Data;

    if (vkCreateInstance(&create_info, nullptr, &celosia_glfw::vulkan::instance) != VK_SUCCESS)
        return false;
    volkLoadInstance(celosia_glfw::vulkan::instance);
    ImGui_ImplVulkan_LoadFunctions([](const char* name, void*) { return vkGetInstanceProcAddr(celosia_glfw::vulkan::instance, name); }); // the backend has its own function pointers
    return true;
}

static uint32_t draw_queue_family(VkPhysicalDevice gpu, VkSurfaceKHR surface) { // a queue that can both draw and present to the window, -1 if the gpu has none
    uint32_t count;
    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &count, nullptr);
    ImVector<VkQueueFamilyProperties> queues;
    queues.resize(count);
    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &count, queues.Data);
    for (uint32_t i = 0; i < count; i++) {
        VkBool32 can_present = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(gpu, i, surface, &can_present);
        if ((queues[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && can_present)
            return i;
    }
    return (uint32_t)-1;
}

static bool select_physical_device(VkSurfaceKHR surface) { // first discrete gpu that can draw to the window, otherwise the first other one that can
    uint32_t gpu_count = 0;
    vkEnumeratePhysicalDevices(celosia_glfw::vulkan::instance, &gpu_count, nullptr);
    ImVector<VkPhysicalDevice> gpus;
    gpus.resize(gpu_count);
    if (gpu_count == 0 || vkEnumeratePhysicalDevices(celosia_glfw::vulkan::instance, &gpu_count, gpus.Data) < 0)
        return false;

    bool found_discrete = false;
    for (VkPhysicalDevice gpu : gpus) {
        const uint32_t family = draw_queue_family(gpu, surface);
        if (family == (uint32_t)-1) // e.g. a compute card or a gpu without the display attached
            continue;

        VkPhysicalDeviceProperties properties;
        vkGetPhysicalDeviceProperties(gpu, &properties);
        const bool discrete = properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
        if (celosia_glfw::vulkan::physical_device == VK_NULL_HANDLE || (discrete && !found_discrete)) {
            celosia_glfw::vulkan::physical_device = gpu;
            celosia_glfw::vulkan::queue_family = family;
            found_discrete = discrete;
        }
    }
    return celosia_glfw::vulkan::physical_device != VK_NULL_HANDLE;
}

static bool create_logical_device() {
    ImVector<const char*> device_extensions;
    device_extensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);

    uint32_t properties_count;
    ImVector<VkExtensionProperties> properties;
    vkEnumerateDeviceExtensionProperties(celosia_glfw::vulkan::physical_device, nullptr, &properties_count, nullptr);
    properties.resize(properties_count);
    vkEnumerateDeviceExtensionProperties(celosia_glfw::vulkan::physical_device, nullptr, &properties_count, properties.Data);
    if (has_extension(properties, "VK_KHR_portability_subset"))
        device_extensions.push_back("VK_KHR_portability_subset");

    const float queue_priority[] = { 1.0f };
    VkDeviceQueueCreateInfo queue_info = {};
    queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue_info.queueFamilyIndex = celosia_glfw::vulkan::queue_family;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = queue_priority;

    VkDeviceCreateInfo create_info = {};
    create_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    create_info.queueCreateInfoCount = 1;
    create_info.pQueueCreateInfos = &queue_info;
    create_info.enabledExtensionCount = (uint32_t)device_extensions.Size;
    create_info.ppEnabledExtensionNames = device_extensions.Data;
    if (vkCreateDevice(celosia_glfw::vulkan::physical_device, &create_info, nullptr, &celosia_glfw::vulkan::device) != VK_SUCCESS)
        return false;
    volkLoadDevice(celosia_glfw::vulkan::device);
    vkGetDeviceQueue(celosia_glfw::vulkan::device, celosia_glfw::vulkan::queue_family, 0, &celosia_glfw::vulkan::queue);

    VkDescriptorPoolSize pool_sizes[] = { { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 64 } }; // the font texture, the effects' backdrop and the images
    VkDescriptorPoolCreateInfo pool_info = {};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pool_info.maxSets = 64;
    pool_info.poolSizeCount = (uint32_t)IM_ARRAYSIZE(pool_sizes);
    pool_info.pPoolSizes = pool_sizes;
    return vkCreateDescriptorPool(celosia_glfw::vulkan::device, &pool_info, nullptr, &celosia_glfw::vulkan::descriptor_pool) == VK_SUCCESS;
}

bool celosia_glfw::vulkan::create_device(GLFWwindow* window) {
    if (!create_instance()) {
        std::cerr << "[vulkan] couldn't create an instance" << std::endl;
        return false;
    }

    VkSurfaceKHR surface;
    if (glfwCreateWindowSurface(instance, window, nullptr, &surface) != VK_SUCCESS) {
        std::cerr << "[vulkan] couldn't create a surface for the window" << std::endl;
        return false;
    }
    window_data.Surface = surface; // destroy() cleans it up from here on

    if (!select_physical_device(surface)) {
        std::cerr << "[vulkan] no gpu can draw to the window" << std::endl;
        return false;
    }

    if (!create_logical_device()) {
        std::cerr << "[vulkan] couldn't create the device" << std::endl;
        return false;
    }

    const VkFormat formats[] = { VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8_UNORM, VK_FORMAT_R8G8B8_UNORM };
    window_data.SurfaceFormat = ImGui_ImplVulkanH_SelectSurfaceFormat(physical_device, surface, formats, (size_t)IM_ARRAYSIZE(formats), VK_COLORSPACE_SRGB_NONLINEAR_KHR);
    VkPresentModeKHR present_modes[] = { VK_PRESENT_MODE_FIFO_KHR }; // vsync, same as the d3d11 swapchain
    window_data.PresentMode = ImGui_ImplVulkanH_SelectPresentMode(physical_device, surface, present_modes, IM_ARRAYSIZE(present_modes));
    window_data.ClearValue = {}; // fully transparent, like the d3d11 clear

    int width, height;
    glfwGetFramebufferSize(window, &width, &height);
    ImGui_ImplVulkanH_CreateOrResizeWindow(instance, physical_device, device, &window_data, queue_family, nullptr, width, height, min_image_count);
    return true;
}

void celosia_glfw::vulkan::resize_swapchain(GLFWwindow* window) {
    int width, height;
    glfwGetFramebufferSize(window, &width, &height);
    if (width <= 0 || height <= 0) // minimized
        return;
    ImGui_ImplVulkan_SetMinImageCount(min_image_count);
    ImGui_ImplVulkanH_CreateOrResizeWindow(instance, physical_device, device, &window_data, queue_family, nullptr, width, height, min_image_count);
    window_data.FrameIndex = 0;
    window_data.SemaphoreIndex = 0;
    swapchain_rebuild = false;
    effect_renderer::destroy_backdrop(); // the device is idle after the rebuild
    effect_renderer::create_backdrop();
}

static bool frame_pending = false; // render() submitted a frame that present() still has to show

// frames in flight cycle through slots (wd->SemaphoreIndex), each with its own command buffer, fence and acquire semaphore,
// and the slot's fence is waited on before any of them is reused. the framebuffer and the render complete semaphore belong
// to the swapchain image (wd->FrameIndex), since presenting that image is what waits on the semaphore
void celosia_glfw::vulkan::render(ImDrawData* draw_data) {
    ImGui_ImplVulkanH_Window* wd = &window_data;
    ImGui_ImplVulkanH_Frame* fd = &wd->Frames[wd->SemaphoreIndex];
    VkSemaphore image_acquired = wd->FrameSemaphores[wd->SemaphoreIndex].ImageAcquiredSemaphore;
    check_result(vkWaitForFences(device, 1, &fd->Fence, VK_TRUE, UINT64_MAX));

    VkResult err = vkAcquireNextImageKHR(device, wd->Swapchain, UINT64_MAX, image_acquired, VK_NULL_HANDLE, &wd->FrameIndex);
    if (err == VK_ERROR_OUT_OF_DATE_KHR) { // no image, nothing was signaled
        swapchain_rebuild = true;
        return;
    }
    if (err == VK_SUBOPTIMAL_KHR) // an image was still acquired, draw and present it and rebuild afterwards
        swapchain_rebuild = true;
    else
        check_result(err);
    check_result(vkResetFences(device, 1, &fd->Fence));
    VkSemaphore render_complete = wd->FrameSemaphores[wd->FrameIndex].RenderCompleteSemaphore;

    check_result(vkResetCommandPool(device, fd->CommandPool, 0));
    VkCommandBufferBeginInfo begin_info = {};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags |= VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check_result(vkBeginCommandBuffer(fd->CommandBuffer, &begin_info));
    effect_renderer::command_buffer = fd->CommandBuffer;

    VkRenderPassBeginInfo pass_info = {};
    pass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    pass_info.renderPass = wd->RenderPass;
    pass_info.framebuffer = wd->Frames[wd->FrameIndex].Framebuffer;
    pass_info.renderArea.extent.width = wd->Width;
    pass_info.renderArea.extent.height = wd->Height;
    pass_info.clearValueCount = 1;
    pass_info.pClearValues = &wd->ClearValue;
    vkCmdBeginRenderPass(fd->CommandBuffer, &pass_info, VK_SUBPASS_CONTENTS_INLINE);

    ImGui_ImplVulkan_RenderDrawData(draw_data, fd->CommandBuffer);

    vkCmdEndRenderPass(fd->CommandBuffer);
    check_result(vkEndCommandBuffer(fd->CommandBuffer));

    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submit_info = {};
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit_info.waitSemaphoreCount = 1;
    submit_info.pWaitSemaphores = &image_acquired;
    submit_info.pWaitDstStageMask = &wait_stage;
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &fd->CommandBuffer;
    submit_info.signalSemaphoreCount = 1;
    submit_info.pSignalSemaphores = &render_complete;
    check_result(vkQueueSubmit(queue, 1, &submit_info, fd->Fence));
    frame_pending = true;
}

void celosia_glfw::vulkan::present() {
    if (!frame_pending)
        return;
    frame_pending = false;
    ImGui_ImplVulkanH_Window* wd = &window_data;
    VkSemaphore render_complete = wd->FrameSemaphores[wd->FrameIndex].RenderCompleteSemaphore;

    VkPresentInfoKHR info = {};
    info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    info.waitSemaphoreCount = 1;
    info.pWaitSemaphores = &render_complete;
    info.swapchainCount = 1;
    info.pSwapchains = &wd->Swapchain;
    info.pImageIndices = &wd->FrameIndex;
    VkResult err = vkQueuePresentKHR(queue, &info);
    wd->SemaphoreIndex = (wd->SemaphoreIndex + 1) % wd->ImageCount; // the slot was used either way
    if (err == VK_ERROR_OUT_OF_DATE_KHR || err == VK_SUBOPTIMAL_KHR) {
        swapchain_rebuild = true;
        return;
    }
    check_result(err);
}

void celosia_glfw::vulkan::destroy() { // whatever create_device() got through
    if (device) {
        ImGui_ImplVulkanH_DestroyWindow(instance, device, &window_data, nullptr); // the surface along with the swapchain
        vkDestroyDescriptorPool(device, descriptor_pool, nullptr);
        vkDestroyDevice(device, nullptr);
    }
    else if (window_data.Surface)
        vkDestroySurfaceKHR(instance, window_data.Surface, nullptr);
    if (instance)
        vkDestroyInstance(instance, nullptr);
    window_data = ImGui_ImplVulkanH_Window();
    descriptor_pool = VK_NULL_HANDLE;
    device = VK_NULL_HANDLE;
    physical_device = VK_NULL_HANDLE;
    instance = VK_NULL_HANDLE;
}
