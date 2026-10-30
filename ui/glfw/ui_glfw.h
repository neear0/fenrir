#pragma once

#include "../ui.h"

#include <volk.h> // loads vulkan at runtime, so nothing has to be linked against it
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include "../../external/imgui/vulkan/imgui_impl_vulkan.h"

namespace celosia_glfw {
	namespace vulkan {
		inline VkInstance instance = VK_NULL_HANDLE;
		inline VkPhysicalDevice physical_device = VK_NULL_HANDLE;
		inline VkDevice device = VK_NULL_HANDLE;
		inline uint32_t queue_family = (uint32_t)-1;
		inline VkQueue queue = VK_NULL_HANDLE;
		inline VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;

		inline ImGui_ImplVulkanH_Window window_data;
		inline uint32_t min_image_count = 2;
		inline bool swapchain_rebuild = false;

		void check_result(VkResult err);
		uint32_t memory_type(uint32_t type_bits, VkMemoryPropertyFlags properties); // index of a memory type with those properties, -1 if there's none
		bool create_device(GLFWwindow* window); // instance, device and the window's swapchain
		void resize_swapchain(GLFWwindow* window);
		void render(ImDrawData* draw_data);
		void present();
		void destroy();
	}

	namespace textures { // images' textures, see textures_vulkan.cpp
		void destroy(); // what create_texture() keeps around, the device has to be idle
	}

	namespace x11 { // the compositor blurring the desktop behind the window (kwin), see blur_x11.cpp
		bool blur_supported(GLFWwindow* window);
		void set_blur(GLFWwindow* window, bool enabled, float rounding); // rounding: the window's corners, the blur stays inside them
	}

	namespace effect_renderer { // vulkan side of celosia::effects, see effects_vulkan.cpp
		inline VkCommandBuffer command_buffer = VK_NULL_HANDLE; // the frame being recorded, set by vulkan::render()

		bool create();           // after ImGui_ImplVulkan_Init()
		void create_backdrop();  // sized like the swapchain, call again after it's rebuilt. without it the blur draws plain panels
		void destroy_backdrop();
		void destroy();
	}
}
