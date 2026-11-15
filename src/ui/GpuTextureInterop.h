#pragma once

#include "core/video/VideoFrame.h"
#include <memory>

namespace ks::ui {

class GpuTextureInterop {
public:
    GpuTextureInterop();
    ~GpuTextureInterop();
    // Requires the widget GL context. The texture remains locked until release().
    unsigned acquire(const std::shared_ptr<GpuVideoTexture> &frame);
    void release();
    void reset();
private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace ks::ui
