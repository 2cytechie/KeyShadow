#pragma once

#include <QByteArray>
#include <QString>

struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct SwrContext;

namespace ks {

/// scrcpy 音频通道解码器（OPUS → float32 交错 PCM）。
class AudioDecoder {
public:
    AudioDecoder();
    ~AudioDecoder();

    AudioDecoder(const AudioDecoder &) = delete;
    AudioDecoder &operator=(const AudioDecoder &) = delete;

    /// 使用设备码流配置包（AOPUSHDR + OpusHead）完成初始化。
    bool configure(const QByteArray &configPayload, int outSampleRate, int outChannels,
                   QString *error = nullptr);
    bool isConfigured() const { return m_ctx != nullptr; }

    /// 解一个 OPUS 包，输出 float32 交错 PCM（48k 或目标采样率）。
    bool decode(const QByteArray &packet, QByteArray *outInterleaved, int *outFrames,
                QString *error = nullptr);

    void close();
    int sampleRate() const { return m_outSampleRate; }
    int channels() const { return m_outChannels; }

private:
    bool openContext(const QByteArray &opusHead, QString *error);

    AVCodecContext *m_ctx = nullptr;
    AVFrame *m_frame = nullptr;
    AVPacket *m_packet = nullptr;
    SwrContext *m_swr = nullptr;
    int m_outSampleRate = 48000;
    int m_outChannels = 2;
    QString m_error;
};

} // namespace ks
