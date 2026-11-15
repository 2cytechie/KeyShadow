#pragma once

#include <QByteArray>
#include <QtGlobal>
#include <memory>

struct ID3D11Texture2D;
struct ID3D11Device;

namespace ks {

enum class VideoColorSpace { Bt601, Bt709, Bt2020 };

struct GpuVideoTexture {
    GpuVideoTexture();
    ~GpuVideoTexture();
    GpuVideoTexture(const GpuVideoTexture &) = delete;
    GpuVideoTexture &operator=(const GpuVideoTexture &) = delete;
    ID3D11Texture2D *texture = nullptr;
    ID3D11Device *device = nullptr;
};

/// 解码后的一帧画面（YUV420P 平面，可能带行填充）。
struct VideoFrame {
    int width = 0;
    int height = 0;
    QByteArray y;
    QByteArray u;
    QByteArray v;
    int yStride = 0;
    int uStride = 0;
    int vStride = 0;
    qint64 pts = 0;
    qint64 decodedAtMs = 0; ///< 单调时钟毫秒（延迟统计用）
    VideoColorSpace colorSpace = VideoColorSpace::Bt709;
    bool fullRange = false;
    std::shared_ptr<GpuVideoTexture> gpu; ///< BGRA texture; no CPU planes when present
};

using VideoFramePtr = std::shared_ptr<VideoFrame>;
using VideoFrameConstPtr = std::shared_ptr<const VideoFrame>;

/// 一帧解码后的音频（交错 float32）。
struct AudioFrame {
    QByteArray samples;
    int sampleRate = 48000;
    int channels = 2;
    int frames = 0;
};

using AudioFramePtr = std::shared_ptr<AudioFrame>;

qint64 monotonicNowMs();

} // namespace ks
