#include "../win32/ui_win32.h"

#ifdef _DEBUG
static ID3D11InfoQueue* info_queue = nullptr; // debug layer messages, only there when the layer is installed
#endif

void celosia_win32::d3d::report_debug_messages() {
#ifdef _DEBUG
    if (!info_queue)
        return;
    static const char* severities[] = { "corruption", "error", "warning", "info", "message" };
    for (UINT64 i = 0; i < info_queue->GetNumStoredMessages(); i++) {
        SIZE_T size = 0;
        info_queue->GetMessage(i, nullptr, &size);
        std::string buffer(size, '\0');
        D3D11_MESSAGE* message = (D3D11_MESSAGE*)buffer.data();
        if (SUCCEEDED(info_queue->GetMessage(i, message, &size)))
            std::cerr << "[d3d11] " << severities[message->Severity] << ": " << std::string_view(message->pDescription, message->DescriptionByteLength - 1) << std::endl;
    }
    info_queue->ClearStoredMessages();
#endif
}

bool celosia_win32::d3d::create_device(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC sd;
    ZeroMemory(&sd, sizeof(sd));
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT create_device_flags = 0;
#ifdef _DEBUG
    create_device_flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    D3D_FEATURE_LEVEL feature_level;
    const D3D_FEATURE_LEVEL feature_level_array[2] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0, };
    auto create = [&](D3D_DRIVER_TYPE driver_type) {
        return D3D11CreateDeviceAndSwapChain(nullptr, driver_type, nullptr, create_device_flags, feature_level_array, 2, D3D11_SDK_VERSION, &sd, &d3d::swapchain, &d3d::device, &feature_level, &d3d::device_context);
    };

    HRESULT res = create(D3D_DRIVER_TYPE_HARDWARE);
    if (res == DXGI_ERROR_SDK_COMPONENT_MISSING) { // the debug layer isn't installed (windows optional feature "Graphics Tools")
        create_device_flags &= ~D3D11_CREATE_DEVICE_DEBUG;
        res = create(D3D_DRIVER_TYPE_HARDWARE);
    }
    if (res == DXGI_ERROR_UNSUPPORTED) // Try high-performance WARP software driver if hardware is not available.
        res = create(D3D_DRIVER_TYPE_WARP);
    if (res != S_OK)
        return false;
#ifdef _DEBUG
    d3d::device->QueryInterface(IID_PPV_ARGS(&info_queue)); // stays null without the debug layer
#endif

    ID3D11Texture2D* back_buffer = nullptr;
    if (FAILED(d3d::swapchain->GetBuffer(0, IID_PPV_ARGS(&back_buffer))))
        return false;
    res = d3d::device->CreateRenderTargetView(back_buffer, nullptr, &d3d::render_target_view);
    back_buffer->Release();
    return SUCCEEDED(res);
}

void celosia_win32::d3d::destroy_device() {
    if (d3d::render_target_view) { d3d::render_target_view->Release(); d3d::render_target_view = nullptr; }
    if (d3d::swapchain) { d3d::swapchain->Release(); d3d::swapchain = nullptr; }
    if (d3d::device_context) { d3d::device_context->Release(); d3d::device_context = nullptr; }
#ifdef _DEBUG
    ID3D11Debug* debug = nullptr; // lists whatever is still alive besides the device itself, i.e. leaks
    if (d3d::device && SUCCEEDED(d3d::device->QueryInterface(IID_PPV_ARGS(&debug)))) {
        debug->ReportLiveDeviceObjects(D3D11_RLDO_SUMMARY | D3D11_RLDO_IGNORE_INTERNAL);
        debug->Release();
    }
    report_debug_messages();
    if (info_queue) { info_queue->Release(); info_queue = nullptr; }
#endif
    if (d3d::device) { d3d::device->Release(); d3d::device = nullptr; }
}
