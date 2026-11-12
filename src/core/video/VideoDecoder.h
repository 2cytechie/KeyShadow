#pragma once

#include <QByteArray>
#include <QString>
#include <atomic>
#include <functional>

#include "core/video/VideoFrame.h"

struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct AVBufferRef;
struct SwsContext;

namespace ks {

/// 视频解码器：优先 D3D11VA 硬解（RTX 显卡走 NVDEC），失败自动降级软解。
/// 输出统一为 YUV420P 的 VideoFrame，供 OpenGL 渲染器上传纹理。
class VideoDecoder {
public:
    using FrameCallback = std::function<void(const VideoFramePtr &)>;

    VideoDecoder();
    ~VideoDecoder();

    VideoDecoder(const VideoDecoder &) = delete;
    VideoDecoder &operator=(const VideoDecoder &) = delete;

    /// codecTag 为 scrcpy 码流头上的四字节 ASCII（'h264' / 'h265' / 'av1'）。
    bool open(quint32 codecTag, QString *error = nullptr);
    void close();
    bool isOpen() const { return m_ctx != nullptr; }

    /// scrcpy 配置包（SPS/PPS 等编解码器初始化数据）。应在 open() 之前设置。
    void setExtradata(const QByteArray &data) { m_extradata = data; }
    QByteArray extradata() const { return m_extradata; }

    /// 送入一个压缩包；解出的帧通过回调返回（可能 0 帧或多帧）。
    /// convertAllowed=false 时仍然完整解码（参考帧链不受影响），但跳过 GPU→CPU 回读、
    /// 色彩转换与回调 —— 用于「上一帧还没显示完，这一帧没必要再转换」的丢帧策略。
    bool decode(const char *data, int size, qint64 pts, bool keyFrame,
                const FrameCallback &callback, QString *error = nullptr,
                bool convertAllowed = true);

    void flush(const FrameCallback &callback);

    /// 取走并清零被跳过的解码帧数（convertAllowed=false 造成的）。
    qint64 takeSkippedFrames() { return m_skippedFrames.exchange(0); }

    bool hardwareAccelerated() const { return m_hwAccelerated; }
    QString codecName() const { return m_codecName; }
    QString errorString() const { return m_error; }

private:
    bool openContext(const char *name, QString *error);
    void convertAndEmit(AVFrame *frame, const FrameCallback &callback);
    void drain(const FrameCallback &callback, bool convertAllowed);

    AVCodecContext *m_ctx = nullptr;
    AVBufferRef *m_hwDeviceCtx = nullptr;
    AVFrame *m_frame = nullptr;
    AVFrame *m_swFrame = nullptr;
    AVPacket *m_packet = nullptr;
    SwsContext *m_sws = nullptr;
    int m_swsWidth = 0;
    int m_swsHeight = 0;
    int m_swsFormat = -1;
    bool m_hwAccelerated = false;
    bool m_loggedFormat = false;
    int m_swWidth = 0;  ///< m_swFrame 当前缓冲区尺寸（变化时才重新分配）
    int m_swHeight = 0;
    std::atomic<qint64> m_skippedFrames{0};
    QString m_codecName;
    QString m_error;
    QByteArray m_extradata;
    qint64 m_firstPacketAtMs = 0;
};

} // namespace ks
