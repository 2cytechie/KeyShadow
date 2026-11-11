#include "core/audio/AudioDecoder.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

#include <cstring>

namespace ks {
namespace {

/// 取出 OpusHead 段。scrcpy-server 会先剥离 AOPUSHDR 外框；旧版本则保留完整配置包。
QByteArray extractOpusHead(const QByteArray &payload) {
    if (payload.startsWith("OpusHead")) {
        return payload;
    }
    if (payload.size() < 16 || !payload.startsWith("AOPUSHDR")) {
        return {};
    }
    quint64 size = 0;
    std::memcpy(&size, payload.constData() + 8, sizeof(size));
    if (size == 0 || size > static_cast<quint64>(payload.size() - 16)) {
        return {};
    }
    return payload.mid(16, static_cast<int>(size));
}

} // namespace

AudioDecoder::AudioDecoder() = default;

AudioDecoder::~AudioDecoder() { close(); }

bool AudioDecoder::configure(const QByteArray &configPayload, int outSampleRate, int outChannels,
                             QString *error) {
    close();
    const QByteArray opusHead = extractOpusHead(configPayload);
    m_outSampleRate = outSampleRate;
    m_outChannels = outChannels;
    if (opusHead.isEmpty()) {
        m_error = QStringLiteral("音频配置包无效");
        if (error) {
            *error = m_error;
        }
        return false;
    }
    if (!openContext(opusHead, error)) {
        return false;
    }
    if (error) {
        error->clear();
    }
    return true;
}

bool AudioDecoder::openContext(const QByteArray &opusHead, QString *error) {
    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_OPUS);
    if (!codec) {
        m_error = QStringLiteral("FFmpeg 缺少 OPUS 解码器");
        if (error) {
            *error = m_error;
        }
        return false;
    }

    m_ctx = avcodec_alloc_context3(codec);
    if (!m_ctx) {
        m_error = QStringLiteral("无法创建音频解码上下文");
        if (error) {
            *error = m_error;
        }
        return false;
    }

    m_ctx->extradata = static_cast<uint8_t *>(av_mallocz(opusHead.size() + AV_INPUT_BUFFER_PADDING_SIZE));
    if (m_ctx->extradata) {
        std::memcpy(m_ctx->extradata, opusHead.constData(), static_cast<size_t>(opusHead.size()));
        m_ctx->extradata_size = opusHead.size();
    }
    m_ctx->sample_rate = 48000;
    av_channel_layout_default(&m_ctx->ch_layout, 2);
    m_ctx->request_sample_fmt = AV_SAMPLE_FMT_FLT;

    if (avcodec_open2(m_ctx, codec, nullptr) != 0) {
        m_error = QStringLiteral("OPUS 解码器初始化失败");
        if (error) {
            *error = m_error;
        }
        close();
        return false;
    }

    m_frame = av_frame_alloc();
    m_packet = av_packet_alloc();
    if (!m_frame || !m_packet) {
        m_error = QStringLiteral("音频帧内存不足");
        if (error) {
            *error = m_error;
        }
        close();
        return false;
    }

    AVChannelLayout outLayout{};
    av_channel_layout_default(&outLayout, m_outChannels);
    const int ret = swr_alloc_set_opts2(&m_swr, &outLayout, AV_SAMPLE_FMT_FLT, m_outSampleRate,
                                        &m_ctx->ch_layout, m_ctx->sample_fmt, m_ctx->sample_rate,
                                        0, nullptr);
    av_channel_layout_uninit(&outLayout);
    if (ret < 0 || !m_swr || swr_init(m_swr) < 0) {
        m_error = QStringLiteral("音频重采样初始化失败");
        if (error) {
            *error = m_error;
        }
        close();
        return false;
    }

    return true;
}

bool AudioDecoder::decode(const QByteArray &packet, QByteArray *outInterleaved, int *outFrames,
                          QString *error) {
    if (!m_ctx || !m_swr) {
        if (error) {
            *error = QStringLiteral("音频解码器未初始化");
        }
        return false;
    }
    if (packet.isEmpty()) {
        return true;
    }

    av_packet_unref(m_packet);
    if (av_new_packet(m_packet, packet.size()) != 0) {
        if (error) {
            *error = QStringLiteral("音频包内存分配失败");
        }
        return false;
    }
    std::memcpy(m_packet->data, packet.constData(), static_cast<size_t>(packet.size()));

    int ret = avcodec_send_packet(m_ctx, m_packet);
    if (ret < 0 && ret != AVERROR(EAGAIN)) {
        if (error) {
            *error = QStringLiteral("音频解码失败：%1").arg(ret);
        }
        return false;
    }

    int totalFrames = 0;
    outInterleaved->clear();

    for (;;) {
        ret = avcodec_receive_frame(m_ctx, m_frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        }
        if (ret < 0) {
            return false;
        }

        const int maxOutSamples = static_cast<int>(
            av_rescale_rnd(swr_get_delay(m_swr, m_ctx->sample_rate) + m_frame->nb_samples,
                           m_outSampleRate, m_ctx->sample_rate, AV_ROUND_UP));
        const int oldSize = outInterleaved->size();
        outInterleaved->resize(oldSize + maxOutSamples * m_outChannels *
                                            static_cast<int>(sizeof(float)));
        uint8_t *outPlanes[1] = {
            reinterpret_cast<uint8_t *>(outInterleaved->data() + oldSize)};
        const int converted = swr_convert(m_swr, outPlanes, maxOutSamples,
                                          const_cast<const uint8_t **>(m_frame->extended_data),
                                          m_frame->nb_samples);
        if (converted > 0) {
            totalFrames += converted;
            outInterleaved->resize(oldSize + converted * m_outChannels *
                                                static_cast<int>(sizeof(float)));
        } else {
            outInterleaved->resize(oldSize);
        }
        av_frame_unref(m_frame);
    }

    if (outFrames) {
        *outFrames = totalFrames;
    }
    if (error) {
        error->clear();
    }
    return true;
}

void AudioDecoder::close() {
    if (m_packet) {
        av_packet_free(&m_packet);
    }
    if (m_frame) {
        av_frame_free(&m_frame);
    }
    if (m_swr) {
        swr_free(&m_swr);
    }
    if (m_ctx) {
        avcodec_free_context(&m_ctx);
    }
}

} // namespace ks
