#pragma once

#include <QByteArray>
#include <QtGlobal>
#include <memory>

namespace ks {

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
