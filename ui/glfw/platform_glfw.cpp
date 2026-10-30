#include "../glfw/ui_glfw.h"

// celosia::platform on top of a glfw window and the vulkan renderer in this folder

static GLFWwindow* glfw_window = nullptr;
static bool blur = false; // the compositor can blur the desktop behind the window
static bool backends = false; // ImGui's vulkan backend is initialized

static void input_callbacks(GLFWwindow* window); // the window's events into celosia::input, see below

static void error_callback(int error, const char* description) {
    std::cerr << "[glfw] error " << error << ": " << description << std::endl;
}

static GLFWmonitor* monitor_at(ImVec2 pos) { // the monitor the window opens on, the primary one when pos is on none
    int count = 0;
    GLFWmonitor** monitors = glfwGetMonitors(&count);
    for (int i = 0; i < count; i++) {
        int x, y;
        glfwGetMonitorPos(monitors[i], &x, &y);
        const GLFWvidmode* mode = glfwGetVideoMode(monitors[i]);
        if (mode && pos.x >= x && pos.y >= y && pos.x < x + mode->width && pos.y < y + mode->height)
            return monitors[i];
    }
    return glfwGetPrimaryMonitor();
}

bool celosia::platform::create(const char* title, ImVec2 pos, ImVec2 size) {
    if (volkInitialize() != VK_SUCCESS) {
        std::cerr << "[vulkan] no vulkan loader found, install your gpu's vulkan driver" << std::endl;
        return false;
    }

    glfwSetErrorCallback(error_callback);
    glfwInitVulkanLoader(vkGetInstanceProcAddr);
    if (glfwPlatformSupported(GLFW_PLATFORM_X11)) // wayland doesn't let windows move themselves or stay on top, xwayland does
        glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_X11);
    if (!glfwInit())
        return false;

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);     // the ui draws its own titlebar
    glfwWindowHint(GLFW_FLOATING, GLFW_TRUE);       // topmost
    glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);
    glfwWindowHint(GLFW_TRANSPARENT_FRAMEBUFFER, GLFW_TRUE);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);       // shown once it's in place

    // set_scale() takes ui::size along with the other sizes, the window follows it. x11 has one scale for every monitor (Xft.dpi)
    float scale = 1.f, scale_y = 1.f;
    if (GLFWmonitor* monitor = monitor_at(pos))
        glfwGetMonitorContentScale(monitor, &scale, &scale_y);
    ui::size = size * style::scale;
    style::set_scale(scale);
    glfw_window = glfwCreateWindow((int)ui::size.x, (int)ui::size.y, title, nullptr, nullptr);
    if (!glfw_window) {
        destroy(); // undoes whatever got made so far
        return false;
    }
    glfwSetWindowPos(glfw_window, (int)pos.x, (int)pos.y);

    if (!celosia_glfw::vulkan::create_device(glfw_window)) {
        destroy();
        return false;
    }

    input_callbacks(glfw_window);

    ImGui_ImplVulkan_InitInfo init_info = {};
    init_info.Instance = celosia_glfw::vulkan::instance;
    init_info.PhysicalDevice = celosia_glfw::vulkan::physical_device;
    init_info.Device = celosia_glfw::vulkan::device;
    init_info.QueueFamily = celosia_glfw::vulkan::queue_family;
    init_info.Queue = celosia_glfw::vulkan::queue;
    init_info.DescriptorPool = celosia_glfw::vulkan::descriptor_pool;
    init_info.MinImageCount = celosia_glfw::vulkan::min_image_count;
    init_info.ImageCount = celosia_glfw::vulkan::window_data.ImageCount;
    init_info.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    init_info.CheckVkResultFn = celosia_glfw::vulkan::check_result;
    ImGui_ImplVulkan_Init(&init_info, celosia_glfw::vulkan::window_data.RenderPass);
    backends = true;
    if (!celosia_glfw::effect_renderer::create()) {
        destroy();
        return false;
    }

    blur = celosia_glfw::x11::blur_supported(glfw_window);
    if (blur)
        set_blur(style::window::background == style::window::blur);
    else // nothing blurs the desktop, use the shader background
        style::window::background = style::window::shader;

    glfwShowWindow(glfw_window);
    return true;
}

bool celosia::platform::poll() {
    glfwPollEvents();
    if (celosia_glfw::vulkan::swapchain_rebuild)
        celosia_glfw::vulkan::resize_swapchain(glfw_window);
    return !glfwWindowShouldClose(glfw_window);
}

void celosia::platform::new_frame() {
    ImGui_ImplVulkan_NewFrame();
}

ImVec2 celosia::platform::display_size() {
    if (glfwGetWindowAttrib(glfw_window, GLFW_ICONIFIED))
        return ImVec2(0, 0);
    int w, h;
    glfwGetWindowSize(glfw_window, &w, &h);
    return ImVec2((float)w, (float)h);
}

ImVec2 celosia::platform::framebuffer_scale() { // the last one known while the window is minimized
    static ImVec2 scale(1.f, 1.f);
    int w, h, framebuffer_w, framebuffer_h;
    glfwGetWindowSize(glfw_window, &w, &h);
    glfwGetFramebufferSize(glfw_window, &framebuffer_w, &framebuffer_h);
    if (w > 0 && h > 0 && framebuffer_w > 0 && framebuffer_h > 0)
        scale = ImVec2((float)framebuffer_w / (float)w, (float)framebuffer_h / (float)h);
    return scale;
}

void celosia::platform::present() {
    ImDrawData* draw_data = core::draw_data();
    if (draw_data->DisplaySize.x <= 0.0f || draw_data->DisplaySize.y <= 0.0f) // minimized
        return;
    celosia_glfw::vulkan::render(draw_data);
    celosia_glfw::vulkan::present();
}

void celosia::platform::destroy() { // also cleans up after a create() that failed partway
    if (celosia_glfw::vulkan::device)
        vkDeviceWaitIdle(celosia_glfw::vulkan::device);
    resources::images::clear();
    celosia_glfw::textures::destroy();
    celosia_glfw::effect_renderer::destroy();
    if (backends) {
        ImGui_ImplVulkan_Shutdown();
        backends = false;
    }
    celosia_glfw::vulkan::destroy();
    if (glfw_window) {
        glfwDestroyWindow(glfw_window);
        glfw_window = nullptr;
    }
    glfwTerminate();
}

static int glfw_key(int key) { // celosia::keys (windows virtual key codes) to glfw, -1 when there's no match
    if ((key >= '0' && key <= '9') || (key >= 'A' && key <= 'Z'))
        return key; // glfw uses ascii for these as well
    if (key >= celosia::keys::f1 && key < celosia::keys::f1 + 12)
        return GLFW_KEY_F1 + (key - celosia::keys::f1);

    switch (key) {
    case celosia::keys::backspace: return GLFW_KEY_BACKSPACE;
    case celosia::keys::tab: return GLFW_KEY_TAB;
    case celosia::keys::enter: return GLFW_KEY_ENTER;
    case celosia::keys::escape: return GLFW_KEY_ESCAPE;
    case celosia::keys::space: return GLFW_KEY_SPACE;
    case celosia::keys::left: return GLFW_KEY_LEFT;
    case celosia::keys::up: return GLFW_KEY_UP;
    case celosia::keys::right: return GLFW_KEY_RIGHT;
    case celosia::keys::down: return GLFW_KEY_DOWN;
    case celosia::keys::insert: return GLFW_KEY_INSERT;
    case celosia::keys::del: return GLFW_KEY_DELETE;
    case celosia::keys::home: return GLFW_KEY_HOME;
    case celosia::keys::end: return GLFW_KEY_END;
    }
    return -1;
}

static int celosia_key(int glfw) { // glfw to celosia::keys, -1 when there's no match
    switch (glfw) {
    case GLFW_KEY_LEFT_SHIFT: case GLFW_KEY_RIGHT_SHIFT: return celosia::keys::shift;
    case GLFW_KEY_LEFT_CONTROL: case GLFW_KEY_RIGHT_CONTROL: return celosia::keys::control;
    case GLFW_KEY_LEFT_ALT: case GLFW_KEY_RIGHT_ALT: return celosia::keys::alt;
    case GLFW_KEY_KP_ENTER: return celosia::keys::enter;
    }
    for (int key = 0; key < 256; key++)
        if (glfw_key(key) == glfw)
            return key;
    return -1;
}

static void input_callbacks(GLFWwindow* window) {
    glfwSetCursorPosCallback(window, [](GLFWwindow*, double x, double y) { celosia::input::add_mouse_pos(ImVec2((float)x, (float)y)); });
    glfwSetCursorEnterCallback(window, [](GLFWwindow* w, int entered) {
        if (entered) { // where it came in, the position callback only follows once it moves
            double x, y;
            glfwGetCursorPos(w, &x, &y);
            celosia::input::add_mouse_pos(ImVec2((float)x, (float)y));
        }
        else
            celosia::input::add_mouse_pos(ImVec2(-FLT_MAX, -FLT_MAX));
    });
    glfwSetMouseButtonCallback(window, [](GLFWwindow*, int button, int action, int) {
        const int ours = button == GLFW_MOUSE_BUTTON_LEFT ? celosia::input::left : button == GLFW_MOUSE_BUTTON_RIGHT ? celosia::input::right : button == GLFW_MOUSE_BUTTON_MIDDLE ? celosia::input::middle : -1;
        if (ours != -1)
            celosia::input::add_mouse_button(ours, action == GLFW_PRESS);
    });
    glfwSetScrollCallback(window, [](GLFWwindow*, double, double y) { celosia::input::add_wheel((float)y); });
    glfwSetKeyCallback(window, [](GLFWwindow*, int key, int, int action, int) {
        const int ours = celosia_key(key);
        if (ours != -1)
            celosia::input::add_key(ours, action != GLFW_RELEASE);
    });
    glfwSetWindowFocusCallback(window, [](GLFWwindow*, int focused) { celosia::input::add_focus(focused != 0); });
}

bool celosia::platform::key_held(int key) { // glfw only sees input aimed at its own window
    switch (key) {
    case keys::mouse_left: return glfwGetMouseButton(glfw_window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
    case keys::mouse_right: return glfwGetMouseButton(glfw_window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS;
    case keys::mouse_middle: return glfwGetMouseButton(glfw_window, GLFW_MOUSE_BUTTON_MIDDLE) == GLFW_PRESS;
    case keys::shift: return glfwGetKey(glfw_window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS || glfwGetKey(glfw_window, GLFW_KEY_RIGHT_SHIFT) == GLFW_PRESS;
    case keys::control: return glfwGetKey(glfw_window, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS || glfwGetKey(glfw_window, GLFW_KEY_RIGHT_CONTROL) == GLFW_PRESS;
    case keys::alt: return glfwGetKey(glfw_window, GLFW_KEY_LEFT_ALT) == GLFW_PRESS || glfwGetKey(glfw_window, GLFW_KEY_RIGHT_ALT) == GLFW_PRESS;
    }
    const int glfw = glfw_key(key);
    return glfw != -1 && glfwGetKey(glfw_window, glfw) == GLFW_PRESS;
}

ImVec2 celosia::platform::cursor_pos() { // glfw reports the cursor relative to the window
    double x, y;
    int window_x, window_y;
    glfwGetCursorPos(glfw_window, &x, &y);
    glfwGetWindowPos(glfw_window, &window_x, &window_y);
    return ImVec2((float)(window_x + x), (float)(window_y + y));
}

ImRect celosia::platform::window_rect() {
    int x, y, w, h;
    glfwGetWindowPos(glfw_window, &x, &y);
    glfwGetWindowSize(glfw_window, &w, &h);
    return ImRect((float)x, (float)y, (float)(x + w), (float)(y + h));
}

ImVec2 celosia::platform::screen_size() {
    const GLFWvidmode* mode = glfwGetVideoMode(glfwGetPrimaryMonitor());
    return mode ? ImVec2((float)mode->width, (float)mode->height) : ImVec2(0, 0);
}

void celosia::platform::move(ImVec2 pos) {
    glfwSetWindowPos(glfw_window, (int)pos.x, (int)pos.y);
}

void celosia::platform::bring_to_top() {
    // GLFW_FLOATING already keeps the window above others, raising it every frame would steal focus
}

bool celosia::platform::blur_supported() { // only known after create(), the compositor is asked once
    return blur;
}

void celosia::platform::set_blur(bool enabled) {
    celosia_glfw::x11::set_blur(glfw_window, enabled && blur, (float)style::general::rounding_window);
}
