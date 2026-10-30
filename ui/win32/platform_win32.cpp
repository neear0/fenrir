#include "../win32/ui_win32.h"

// celosia::platform on top of the Win32 window and D3D11 device in this folder

static HWND hwnd = nullptr;
static bool backends = false; // ImGui's dx11 backend is initialized

bool celosia::platform::create(const char* title, ImVec2 pos, ImVec2 size) {
    std::wstring title_wide;
    title_wide.resize(MultiByteToWideChar(CP_UTF8, 0, title, -1, nullptr, 0));
    MultiByteToWideChar(CP_UTF8, 0, title, -1, title_wide.data(), (int)title_wide.size());

    hwnd = celosia_win32::window::create(title_wide.c_str(), pos, size, WS_EX_TOPMOST | WS_EX_LAYERED);
    if (!hwnd)
        return false;
    // the scale of the monitor it opened on, 1 unless the process is dpi aware (main.cpp makes it so). set_scale() takes
    // ui::size along with the other sizes, the window and the swapchain follow it
    ui::size = size * style::scale;
    style::set_scale((float)GetDpiForWindow(hwnd) / USER_DEFAULT_SCREEN_DPI);
    SetWindowPos(hwnd, nullptr, 0, 0, (int)ui::size.x, (int)ui::size.y, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    if (!celosia_win32::d3d::create_device(hwnd)) {
        destroy(); // undoes whatever got made so far
        return false;
    }

    ImGui_ImplDX11_Init(celosia_win32::d3d::device, celosia_win32::d3d::device_context);
    backends = true;
    if (!celosia_win32::effect_renderer::create()) {
        destroy();
        return false;
    }

    celosia_win32::window::enable_transparency(hwnd);
    celosia_win32::window::enable_blur(hwnd, style::window::background == style::window::blur ? 4 : 0);
    celosia_win32::window::show(hwnd);
    return true;
}

bool celosia::platform::poll() {
    MSG msg;
    bool running = true;
    while (PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
        if (msg.message == WM_QUIT)
            running = false;
    }
    return running;
}

void celosia::platform::new_frame() {
    celosia_win32::effect_renderer::update_backdrop();
    ImGui_ImplDX11_NewFrame();
}

void celosia::platform::present() {
    const float clear_color_with_alpha[4] = { 0,0,0,0 };
    celosia_win32::d3d::device_context->OMSetRenderTargets(1, &celosia_win32::d3d::render_target_view, nullptr);
    celosia_win32::d3d::device_context->ClearRenderTargetView(celosia_win32::d3d::render_target_view, clear_color_with_alpha);

    ImGui_ImplDX11_RenderDrawData(core::draw_data());
    celosia_win32::d3d::swapchain->Present(1, 0); // with vsync
    celosia_win32::d3d::report_debug_messages();
}

void celosia::platform::destroy() {
    resources::images::clear();
    if (backends) {
        ImGui_ImplDX11_Shutdown();
        backends = false;
    }
    celosia_win32::effect_renderer::destroy();
    celosia_win32::d3d::destroy_device();
    celosia_win32::window::destroy();
    hwnd = nullptr;
}

bool celosia::platform::key_held(int key) {
    if ((key == keys::mouse_left || key == keys::mouse_right) && GetSystemMetrics(SM_SWAPBUTTON)) // GetAsyncKeyState() reads the physical buttons, keys are the logical ones
        key = key == keys::mouse_left ? keys::mouse_right : keys::mouse_left;
    return GetAsyncKeyState(key) & 0x8000;
}

ImVec2 celosia::platform::cursor_pos() {
    POINT p;
    if (GetCursorPos(&p))
        return ImVec2((float)p.x, (float)p.y);
    return ImVec2(0, 0);
}

ImVec2 celosia::platform::display_size() {
    RECT r = {};
    if (IsIconic(hwnd) || !GetClientRect(hwnd, &r))
        return ImVec2(0, 0);
    return ImVec2((float)(r.right - r.left), (float)(r.bottom - r.top));
}

ImVec2 celosia::platform::framebuffer_scale() { // the window is dpi aware, a unit is a pixel
    return ImVec2(1.f, 1.f);
}

ImRect celosia::platform::window_rect() {
    RECT r;
    GetWindowRect(hwnd, &r);
    return ImRect((float)r.left, (float)r.top, (float)r.right, (float)r.bottom);
}

ImVec2 celosia::platform::screen_size() {
    return ImVec2((float)GetSystemMetrics(SM_CXSCREEN), (float)GetSystemMetrics(SM_CYSCREEN));
}

void celosia::platform::move(ImVec2 pos) {
    celosia_win32::window::move(hwnd, pos);
}

void celosia::platform::bring_to_top() { // back above other topmost windows that opened since, without taking the focus
    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

bool celosia::platform::blur_supported() {
    return true;
}

void celosia::platform::set_blur(bool enabled) { // 4 is acrylic blur behind the window, 0 turns it off
    celosia_win32::window::enable_blur(hwnd, enabled ? 4 : 0);
}

ImTextureID celosia::platform::create_texture(const unsigned char* rgba, int width, int height) { // the ImTextureID is the shader resource view, as ImGui's dx11 backend expects
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA data = { rgba, (UINT)width * 4, 0 };

    ID3D11Texture2D* texture = nullptr;
    if (FAILED(celosia_win32::d3d::device->CreateTexture2D(&desc, &data, &texture)))
        return nullptr;
    ID3D11ShaderResourceView* view = nullptr;
    celosia_win32::d3d::device->CreateShaderResourceView(texture, nullptr, &view);
    texture->Release(); // the view keeps it alive
    return (ImTextureID)view;
}

void celosia::platform::destroy_texture(ImTextureID texture) {
    if (texture)
        ((ID3D11ShaderResourceView*)texture)->Release();
}
