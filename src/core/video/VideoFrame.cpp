#include "core/video/VideoFrame.h"
#include <d3d11.h>

#include <chrono>

namespace ks {

GpuVideoTexture::GpuVideoTexture() = default;
GpuVideoTexture::~GpuVideoTexture() {
    if (texture) texture->Release();
    if (device) device->Release();
}

qint64 monotonicNowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

} // namespace ks
