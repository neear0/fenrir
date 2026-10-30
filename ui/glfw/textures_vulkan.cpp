#include "../glfw/ui_glfw.h"

// vulkan side of platform::create_texture(): the pixels go through a staging buffer into a device local image,
// uploaded with a one off command buffer and waited on, images are only made at startup. the ImTextureID is the
// descriptor set ImGui's vulkan backend binds for the image

struct t_texture {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;
};
static std::vector<t_texture> list;
static VkSampler sampler = VK_NULL_HANDLE;
static VkCommandPool command_pool = VK_NULL_HANDLE;

static void release(t_texture& t) {
    if (t.set) ImGui_ImplVulkan_RemoveTexture(t.set);
    if (t.view) vkDestroyImageView(celosia_glfw::vulkan::device, t.view, nullptr);
    if (t.image) vkDestroyImage(celosia_glfw::vulkan::device, t.image, nullptr);
    if (t.memory) vkFreeMemory(celosia_glfw::vulkan::device, t.memory, nullptr);
    t = {};
}

static bool create_shared() { // the sampler and command pool every texture uses, made with the first one
    if (sampler)
        return true;

    VkSamplerCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    info.magFilter = VK_FILTER_LINEAR;
    info.minFilter = VK_FILTER_LINEAR;
    info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    info.addressModeU = info.addressModeV = info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.maxAnisotropy = 1.0f;
    info.maxLod = VK_LOD_CLAMP_NONE;
    if (vkCreateSampler(celosia_glfw::vulkan::device, &info, nullptr, &sampler) != VK_SUCCESS)
        return false;

    VkCommandPoolCreateInfo pool = {};
    pool.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pool.queueFamilyIndex = celosia_glfw::vulkan::queue_family;
    return vkCreateCommandPool(celosia_glfw::vulkan::device, &pool, nullptr, &command_pool) == VK_SUCCESS;
}

static bool allocate(VkMemoryRequirements requirements, VkMemoryPropertyFlags properties, VkDeviceMemory* memory) {
    VkMemoryAllocateInfo allocation = {};
    allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = celosia_glfw::vulkan::memory_type(requirements.memoryTypeBits, properties);
    return allocation.memoryTypeIndex != (uint32_t)-1 && vkAllocateMemory(celosia_glfw::vulkan::device, &allocation, nullptr, memory) == VK_SUCCESS;
}

static bool upload(const unsigned char* rgba, int width, int height, VkImage image) { // copies the pixels into the image and leaves it ready for sampling
    struct t_staging { // freed however this returns
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkCommandBuffer command_buffer = VK_NULL_HANDLE;
        ~t_staging() {
            if (command_buffer) vkFreeCommandBuffers(celosia_glfw::vulkan::device, command_pool, 1, &command_buffer);
            if (buffer) vkDestroyBuffer(celosia_glfw::vulkan::device, buffer, nullptr);
            if (memory) vkFreeMemory(celosia_glfw::vulkan::device, memory, nullptr);
        }
    } staging;
    const VkDeviceSize size = (VkDeviceSize)width * height * 4;

    VkBufferCreateInfo buffer = {};
    buffer.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer.size = size;
    buffer.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    buffer.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(celosia_glfw::vulkan::device, &buffer, nullptr, &staging.buffer) != VK_SUCCESS)
        return false;
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(celosia_glfw::vulkan::device, staging.buffer, &requirements);
    void* mapped = nullptr;
    if (!allocate(requirements, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &staging.memory) ||
        vkBindBufferMemory(celosia_glfw::vulkan::device, staging.buffer, staging.memory, 0) != VK_SUCCESS ||
        vkMapMemory(celosia_glfw::vulkan::device, staging.memory, 0, size, 0, &mapped) != VK_SUCCESS)
        return false;
    memcpy(mapped, rgba, (size_t)size);
    vkUnmapMemory(celosia_glfw::vulkan::device, staging.memory);

    VkCommandBufferAllocateInfo allocation = {};
    allocation.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocation.commandPool = command_pool;
    allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocation.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(celosia_glfw::vulkan::device, &allocation, &staging.command_buffer) != VK_SUCCESS)
        return false;
    const VkCommandBuffer command_buffer = staging.command_buffer;

    VkCommandBufferBeginInfo begin = {};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(command_buffer, &begin);

    VkImageMemoryBarrier barrier = {};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

    VkBufferImageCopy region = {};
    region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.imageExtent = { (uint32_t)width, (uint32_t)height, 1 };
    vkCmdCopyBufferToImage(command_buffer, staging.buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

    VkSubmitInfo submit = {};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command_buffer;
    return vkEndCommandBuffer(command_buffer) == VK_SUCCESS &&
        vkQueueSubmit(celosia_glfw::vulkan::queue, 1, &submit, VK_NULL_HANDLE) == VK_SUCCESS &&
        vkQueueWaitIdle(celosia_glfw::vulkan::queue) == VK_SUCCESS;
}

void celosia_glfw::textures::destroy() {
    for (t_texture& t : list)
        release(t);
    list.clear();
    if (sampler) { vkDestroySampler(celosia_glfw::vulkan::device, sampler, nullptr); sampler = VK_NULL_HANDLE; }
    if (command_pool) { vkDestroyCommandPool(celosia_glfw::vulkan::device, command_pool, nullptr); command_pool = VK_NULL_HANDLE; }
}

ImTextureID celosia::platform::create_texture(const unsigned char* rgba, int width, int height) {
    if (!create_shared())
        return nullptr;

    t_texture t;
    VkImageCreateInfo image = {};
    image.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image.imageType = VK_IMAGE_TYPE_2D;
    image.format = VK_FORMAT_R8G8B8A8_UNORM;
    image.extent = { (uint32_t)width, (uint32_t)height, 1 };
    image.mipLevels = 1;
    image.arrayLayers = 1;
    image.samples = VK_SAMPLE_COUNT_1_BIT;
    image.tiling = VK_IMAGE_TILING_OPTIMAL;
    image.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    image.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkMemoryRequirements requirements;
    if (vkCreateImage(celosia_glfw::vulkan::device, &image, nullptr, &t.image) != VK_SUCCESS)
        return nullptr;
    vkGetImageMemoryRequirements(celosia_glfw::vulkan::device, t.image, &requirements);
    if (!allocate(requirements, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &t.memory) && !allocate(requirements, 0, &t.memory)) {
        release(t);
        return nullptr;
    }

    VkImageViewCreateInfo view = {};
    view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view.image = t.image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = image.format;
    view.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    if (vkBindImageMemory(celosia_glfw::vulkan::device, t.image, t.memory, 0) != VK_SUCCESS ||
        !upload(rgba, width, height, t.image) ||
        vkCreateImageView(celosia_glfw::vulkan::device, &view, nullptr, &t.view) != VK_SUCCESS) {
        release(t);
        return nullptr;
    }

    t.set = ImGui_ImplVulkan_AddTexture(sampler, t.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    list.push_back(t);
    return (ImTextureID)t.set;
}

void celosia::platform::destroy_texture(ImTextureID texture) {
    for (size_t i = 0; i < list.size(); i++)
        if ((ImTextureID)list[i].set == texture) {
            vkDeviceWaitIdle(celosia_glfw::vulkan::device); // frames in flight may still be drawing it
            release(list[i]);
            list.erase(list.begin() + i);
            return;
        }
}
