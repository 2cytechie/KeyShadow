#pragma once

#include "core/video/VideoFrame.h"
#include <memory>

struct AVFrame;
struct AVBufferRef;

namespace ks {

// Bounded GPU texture pool. A texture is reused only after the GUI releases it.
class GpuFrameConverter {
public:
    GpuFrameConverter();
    ~GpuFrameConverter();
    bool convert(const AVFrame *source, AVBufferRef *device, VideoFramePtr &output);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace ks
