#include "core/video/GpuFrameConverter.h"

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
}
#include <libavutil/hwcontext_d3d11va.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <vector>

namespace ks {
using Microsoft::WRL::ComPtr;

struct GpuFrameConverter::Impl {
    ComPtr<ID3D11VideoDevice> device;
    ComPtr<ID3D11VideoContext> context;
    ComPtr<ID3D11VideoProcessorEnumerator> enumerator;
    ComPtr<ID3D11VideoProcessor> processor;
    std::vector<std::shared_ptr<GpuVideoTexture>> pool;
    int width = 0;
    int height = 0;
};

GpuFrameConverter::GpuFrameConverter() : m_impl(std::make_unique<Impl>()) {}
GpuFrameConverter::~GpuFrameConverter() = default;

bool GpuFrameConverter::convert(const AVFrame *source, AVBufferRef *deviceRef,
                                VideoFramePtr &output) {
    output.reset();
    if (!deviceRef || source->width <= 0 || source->height <= 0 ||
        source->colorspace == AVCOL_SPC_BT2020_NCL ||
        source->colorspace == AVCOL_SPC_BT2020_CL) return false;
    auto *hw = static_cast<AVD3D11VADeviceContext *>(
        reinterpret_cast<AVHWDeviceContext *>(deviceRef->data)->hwctx);
    auto *texture = reinterpret_cast<ID3D11Texture2D *>(source->data[0]);
    if (!hw || !texture) return false;
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    if (desc.Format != DXGI_FORMAT_NV12) return false;

    // Serialize all immediate/video context commands with FFmpeg's context lock.
    struct ContextLock {
        AVD3D11VADeviceContext *hw;
        explicit ContextLock(AVD3D11VADeviceContext *value) : hw(value) {
            if (hw->lock) hw->lock(hw->lock_ctx);
        }
        ~ContextLock() { if (hw->unlock) hw->unlock(hw->lock_ctx); }
    } lock(hw);
    auto &p = *m_impl;
    if (p.width != source->width || p.height != source->height) {
        p = Impl{};
        if (FAILED(hw->device->QueryInterface(IID_PPV_ARGS(&p.device))) ||
            FAILED(hw->device_context->QueryInterface(IID_PPV_ARGS(&p.context)))) return false;
        D3D11_VIDEO_PROCESSOR_CONTENT_DESC content{};
        content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        content.InputWidth = content.OutputWidth = source->width;
        content.InputHeight = content.OutputHeight = source->height;
        content.InputFrameRate = content.OutputFrameRate = {120, 1};
        content.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
        if (FAILED(p.device->CreateVideoProcessorEnumerator(&content, &p.enumerator))) return false;
        UINT inputSupport = 0, outputSupport = 0;
        p.enumerator->CheckVideoProcessorFormat(DXGI_FORMAT_NV12, &inputSupport);
        p.enumerator->CheckVideoProcessorFormat(DXGI_FORMAT_B8G8R8A8_UNORM, &outputSupport);
        if (!(inputSupport & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT) ||
            !(outputSupport & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT)) return false;
        if (FAILED(p.device->CreateVideoProcessor(p.enumerator.Get(), 0, &p.processor))) return false;
        p.width = source->width;
        p.height = source->height;
    }

    std::shared_ptr<GpuVideoTexture> target;
    for (auto &entry : p.pool) {
        if (entry.use_count() == 1) { target = entry; break; }
    }
    if (!target) {
        // A busy GUI must never grow an unbounded queue of decoder surfaces.
        if (p.pool.size() >= 4) return true;
        target = std::make_shared<GpuVideoTexture>();
        D3D11_TEXTURE2D_DESC rgba{};
        rgba.Width = p.width;
        rgba.Height = p.height;
        rgba.MipLevels = rgba.ArraySize = 1;
        rgba.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        rgba.SampleDesc.Count = 1;
        rgba.Usage = D3D11_USAGE_DEFAULT;
        rgba.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(hw->device->CreateTexture2D(&rgba, nullptr, &target->texture))) return false;
        target->device = hw->device;
        target->device->AddRef();
        p.pool.push_back(target);
    }

    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC inputDesc{};
    inputDesc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    inputDesc.Texture2D.ArraySlice = static_cast<UINT>(reinterpret_cast<uintptr_t>(source->data[1]));
    ComPtr<ID3D11VideoProcessorInputView> input;
    if (FAILED(p.device->CreateVideoProcessorInputView(texture, p.enumerator.Get(),
                                                      &inputDesc, &input))) return false;
    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC outputDesc{};
    outputDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    ComPtr<ID3D11VideoProcessorOutputView> view;
    if (FAILED(p.device->CreateVideoProcessorOutputView(target->texture, p.enumerator.Get(),
                                                       &outputDesc, &view))) return false;

    D3D11_VIDEO_PROCESSOR_COLOR_SPACE inputColor{};
    inputColor.YCbCr_Matrix = source->colorspace == AVCOL_SPC_BT709 ||
        (source->colorspace == AVCOL_SPC_UNSPECIFIED && source->height > 576);
    inputColor.Nominal_Range = source->color_range == AVCOL_RANGE_JPEG ? 2 : 1;
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE outputColor{};
    outputColor.RGB_Range = 0; // full-range RGB texture
    p.context->VideoProcessorSetStreamColorSpace(p.processor.Get(), 0, &inputColor);
    p.context->VideoProcessorSetOutputColorSpace(p.processor.Get(), &outputColor);
    p.context->VideoProcessorSetStreamFrameFormat(p.processor.Get(), 0,
                                                 D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
    p.context->VideoProcessorSetStreamAutoProcessingMode(p.processor.Get(), 0, FALSE);
    const RECT rect{0, 0, p.width, p.height};
    p.context->VideoProcessorSetStreamSourceRect(p.processor.Get(), 0, TRUE, &rect);
    p.context->VideoProcessorSetStreamDestRect(p.processor.Get(), 0, TRUE, &rect);
    p.context->VideoProcessorSetOutputTargetRect(p.processor.Get(), TRUE, &rect);
    D3D11_VIDEO_PROCESSOR_STREAM stream{};
    stream.Enable = TRUE;
    stream.pInputSurface = input.Get();
    if (FAILED(p.context->VideoProcessorBlt(p.processor.Get(), view.Get(), 0, 1, &stream))) return false;
    hw->device_context->Flush();

    output = std::make_shared<VideoFrame>();
    output->width = p.width;
    output->height = p.height;
    output->pts = source->pts;
    output->decodedAtMs = monotonicNowMs();
    output->gpu = std::move(target);
    return true;
}

} // namespace ks
