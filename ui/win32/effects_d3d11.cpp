#include "../win32/ui_win32.h"

#include "shaders/gradient_dxbc.h"
#include "shaders/glow_dxbc.h"
#include "shaders/blur_dxbc.h"
#include "shaders/background_dxbc.h"
#include "shaders/noise_dxbc.h"

// d3d11 side of celosia::effects: every effect is a pixel shader that the draw list callback swaps in,
// ImGui's vertex shader, input layout and blending stay as they are (except for the blur, which doesn't blend)

static ID3D11PixelShader* shaders[(int)celosia::effects::e_type::count] = {};
static ID3D11Buffer* constant_buffer = nullptr;
static ID3D11SamplerState* clamp_sampler = nullptr; // the blur reads near the frame's edges, ImGui's sampler wraps around
static ID3D11BlendState* no_blend = nullptr;        // the blur works out the final pixel itself, see blur.hlsl
static ID3D11Texture2D* backdrop = nullptr;           // copy of the back buffer the blur reads from
static ID3D11ShaderResourceView* backdrop_view = nullptr;

bool celosia_win32::effect_renderer::create() {
    struct { const BYTE* code; SIZE_T size; } code[] = { // same order as effects::e_type
        { gradient_dxbc, sizeof(gradient_dxbc) },
        { glow_dxbc, sizeof(glow_dxbc) },
        { blur_dxbc, sizeof(blur_dxbc) },
        { background_dxbc, sizeof(background_dxbc) },
        { noise_dxbc, sizeof(noise_dxbc) },
    };
    static_assert(std::size(code) == (size_t)celosia::effects::e_type::count);
    for (int i = 0; i < (int)celosia::effects::e_type::count; i++)
        if (FAILED(d3d::device->CreatePixelShader(code[i].code, code[i].size, nullptr, &shaders[i])))
            return false;

    D3D11_BUFFER_DESC buffer = {};
    buffer.ByteWidth = sizeof(celosia::effects::t_constants);
    buffer.Usage = D3D11_USAGE_DYNAMIC;
    buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    buffer.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(d3d::device->CreateBuffer(&buffer, nullptr, &constant_buffer)))
        return false;

    D3D11_SAMPLER_DESC sampler = {};
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.ComparisonFunc = D3D11_COMPARISON_ALWAYS;
    sampler.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(d3d::device->CreateSamplerState(&sampler, &clamp_sampler)))
        return false;

    D3D11_BLEND_DESC blend = {};
    blend.RenderTarget[0].BlendEnable = FALSE;
    blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(d3d::device->CreateBlendState(&blend, &no_blend)))
        return false;

    update_backdrop(); // without it the blur draws a plain panel, see backdrop_texture()
    return true;
}

void celosia_win32::effect_renderer::update_backdrop() {
    ID3D11Texture2D* back_buffer = nullptr;
    if (FAILED(d3d::swapchain->GetBuffer(0, IID_PPV_ARGS(&back_buffer))))
        return;
    D3D11_TEXTURE2D_DESC desc;
    back_buffer->GetDesc(&desc);
    back_buffer->Release();

    if (backdrop) {
        D3D11_TEXTURE2D_DESC current;
        backdrop->GetDesc(&current);
        if (current.Width == desc.Width && current.Height == desc.Height && current.Format == desc.Format)
            return;
        backdrop_view->Release(); backdrop_view = nullptr;
        backdrop->Release(); backdrop = nullptr;
    }

    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.CPUAccessFlags = 0;
    desc.MiscFlags = 0;
    if (FAILED(d3d::device->CreateTexture2D(&desc, nullptr, &backdrop)))
        return;
    if (FAILED(d3d::device->CreateShaderResourceView(backdrop, nullptr, &backdrop_view))) {
        backdrop->Release();
        backdrop = nullptr;
    }
}

void celosia_win32::effect_renderer::destroy() {
    for (ID3D11PixelShader*& shader : shaders)
        if (shader) { shader->Release(); shader = nullptr; }
    if (constant_buffer) { constant_buffer->Release(); constant_buffer = nullptr; }
    if (clamp_sampler) { clamp_sampler->Release(); clamp_sampler = nullptr; }
    if (no_blend) { no_blend->Release(); no_blend = nullptr; }
    if (backdrop_view) { backdrop_view->Release(); backdrop_view = nullptr; }
    if (backdrop) { backdrop->Release(); backdrop = nullptr; }
}

static void copy_frame() { // what's been drawn so far, for the blur to read
    ID3D11Resource* back_buffer = nullptr;
    celosia_win32::d3d::render_target_view->GetResource(&back_buffer);
    celosia_win32::d3d::device_context->CopyResource(backdrop, back_buffer);
    back_buffer->Release();
}

static void bind(const celosia::effects::t_command& effect) {
    ID3D11DeviceContext* context = celosia_win32::d3d::device_context;
    if (effect.type == celosia::effects::e_type::blur) {
        copy_frame();
        context->PSSetSamplers(0, 1, &clamp_sampler); // ImGui puts its own sampler and blending back when the effect ends
        context->OMSetBlendState(no_blend, nullptr, 0xffffffff);
    }

    D3D11_MAPPED_SUBRESOURCE mapped;
    if (SUCCEEDED(context->Map(constant_buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        memcpy(mapped.pData, &effect.constants, sizeof(effect.constants));
        context->Unmap(constant_buffer, 0);
    }
    context->PSSetConstantBuffers(0, 1, &constant_buffer);
    context->PSSetShader(shaders[(int)effect.type], nullptr, 0);
}

void celosia::platform::draw_effect(const ImDrawList*, const ImDrawCmd* command) {
    bind(effects::commands[(size_t)(intptr_t)command->UserCallbackData]);
}

ImTextureID celosia::platform::backdrop_texture() {
    return (ImTextureID)backdrop_view;
}
