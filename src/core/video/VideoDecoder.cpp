#include "core/video/VideoDecoder.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

#include <cstring>

#include <QDebug>

namespace ks {
namespace {

AVCodecID codecIdFromTag(quint32 tag) {
    switch (tag) {
    case 0x68323634u: // "h264"
        return AV_CODEC_ID_H264;
    case 0x68323635u: // "h265"
        return AV_CODEC_ID_HEVC;
    case 0x00617631u: // "av1"
        return AV_CODEC_ID_AV1;
    case 0x00767038u: // "vp8"
        return AV_CODEC_ID_VP8;
    case 0x00767039u: // "vp9"
        return AV_CODEC_ID_VP9;
    default:
        return AV_CODEC_ID_NONE;
    }
}

QString codecNameFromTag(quint32 tag) {
    const char chars[5] = {static_cast<char>((tag >> 24) & 0xFF),
                           static_cast<char>((tag >> 16) & 0xFF),
                           static_cast<char>((tag >> 8) & 0xFF),
                           static_cast<char>(tag & 0xFF), '\0'};
    return QString::fromLatin1(chars).trimmed();
}

enum AVPixelFormat pickHwFormat(AVCodecContext *ctx, const enum AVPixelFormat *pixFmts) {
    (void)ctx;
    for (const enum AVPixelFormat *p = pixFmts; *p != AV_PIX_FMT_NONE; ++p) {
        if (*p == AV_PIX_FMT_D3D11) {
            return *p;
        }
    }
    return AV_PIX_FMT_NONE;
}

void applyExtradata(AVCodecContext *ctx, const QByteArray &data) {
    if (!ctx || data.isEmpty()) {
        return;
    }
    ctx->extradata = static_cast<uint8_t *>(
        av_mallocz(static_cast<size_t>(data.size()) + AV_INPUT_BUFFER_PADDING_SIZE));
    if (ctx->extradata) {
        std::memcpy(ctx->extradata, data.constData(), static_cast<size_t>(data.size()));
        ctx->extradata_size = data.size();
    }
}

} // namespace

VideoDecoder::VideoDecoder() = default;

VideoDecoder::~VideoDecoder() { close(); }

bool VideoDecoder::open(quint32 codecTag, QString *error) {
    close();

    const AVCodecID codecId = codecIdFromTag(codecTag);
    m_codecName = codecNameFromTag(codecTag);
    if (codecId == AV_CODEC_ID_NONE) {
        m_error = QStringLiteral("不支持的视频编码：%1").arg(m_codecName);
        if (error) {
            *error = m_error;
        }
        return false;
    }

    const AVCodec *codec = avcodec_find_decoder(codecId);
    if (!codec) {
        m_error = QStringLiteral("FFmpeg 缺少解码器：%1").arg(m_codecName);
        if (error) {
            *error = m_error;
        }
        return false;
    }

    // 1) 先尝试 D3D11VA 硬解
    const bool forceSoftware = qEnvironmentVariableIntValue("KEYSHADOW_FORCE_SW_DECODE") == 1;
    if (!forceSoftware) {
        av_hwdevice_ctx_create(&m_hwDeviceCtx, AV_HWDEVICE_TYPE_D3D11VA, nullptr, nullptr, 0);
    }
    if (!forceSoftware && m_hwDeviceCtx) {
        AVCodecContext *hwCtx = avcodec_alloc_context3(codec);
        if (hwCtx) {
            hwCtx->flags |= AV_CODEC_FLAG_LOW_DELAY;
            hwCtx->flags2 |= AV_CODEC_FLAG2_FAST;
            hwCtx->thread_count = 0;
            hwCtx->hw_device_ctx = av_buffer_ref(m_hwDeviceCtx);
            hwCtx->get_format = pickHwFormat;
            applyExtradata(hwCtx, m_extradata);
            if (avcodec_open2(hwCtx, codec, nullptr) == 0) {
                m_ctx = hwCtx;
                m_hwAccelerated = true;
            } else {
                avcodec_free_context(&hwCtx);
            }
        }
    }

    // 2) 软解兜底
    if (!m_ctx) {
        if (!openContext(codec->name, error)) {
            return false;
        }
        m_hwAccelerated = false;
    }

    m_frame = av_frame_alloc();
    m_swFrame = av_frame_alloc();
    m_packet = av_packet_alloc();
    if (!m_frame || !m_swFrame || !m_packet) {
        m_error = QStringLiteral("内存不足：无法分配解码帧");
        if (error) {
            *error = m_error;
        }
        close();
        return false;
    }

    m_firstPacketAtMs = 0;
    if (error) {
        error->clear();
    }
    return true;
}

bool VideoDecoder::openContext(const char *name, QString *error) {
    const AVCodec *codec = avcodec_find_decoder_by_name(name);
    if (!codec) {
        m_error = QStringLiteral("找不到软件解码器 %1").arg(QString::fromLatin1(name));
        if (error) {
            *error = m_error;
        }
        return false;
    }
    AVCodecContext *ctx = avcodec_alloc_context3(codec);
    if (!ctx) {
        m_error = QStringLiteral("无法创建解码上下文");
        if (error) {
            *error = m_error;
        }
        return false;
    }
    ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    ctx->flags2 |= AV_CODEC_FLAG2_FAST;
    ctx->thread_count = 0;
    applyExtradata(ctx, m_extradata);
    if (avcodec_open2(ctx, codec, nullptr) != 0) {
        avcodec_free_context(&ctx);
        m_error = QStringLiteral("软解初始化失败：%1").arg(QString::fromLatin1(name));
        if (error) {
            *error = m_error;
        }
        return false;
    }
    m_ctx = ctx;
    return true;
}

void VideoDecoder::close() {
    if (m_packet) {
        av_packet_free(&m_packet);
    }
    if (m_frame) {
        av_frame_free(&m_frame);
    }
    if (m_swFrame) {
        av_frame_free(&m_swFrame);
    }
    if (m_ctx) {
        avcodec_free_context(&m_ctx);
    }
    if (m_hwDeviceCtx) {
        av_buffer_unref(&m_hwDeviceCtx);
    }
    if (m_sws) {
        sws_freeContext(m_sws);
        m_sws = nullptr;
    }
    m_swsWidth = 0;
    m_swsHeight = 0;
    m_swsFormat = -1;
    m_hwAccelerated = false;
}

bool VideoDecoder::decode(const char *data, int size, qint64 pts, bool keyFrame,
                          const FrameCallback &callback, QString *error) {
    if (!m_ctx) {
        if (error) {
            *error = QStringLiteral("解码器未初始化");
        }
        return false;
    }
    if (size <= 0) {
        return true;
    }
    if (!m_firstPacketAtMs) {
        m_firstPacketAtMs = monotonicNowMs();
    }

    av_packet_unref(m_packet);
    if (av_new_packet(m_packet, size) != 0) {
        if (error) {
            *error = QStringLiteral("解码包内存分配失败");
        }
        return false;
    }
    std::memcpy(m_packet->data, data, static_cast<size_t>(size));
    if (pts < 0) {
        m_packet->pts = AV_NOPTS_VALUE;
        m_packet->dts = AV_NOPTS_VALUE;
        m_packet->flags |= AV_PKT_FLAG_KEY;
    } else {
        m_packet->pts = pts;
        m_packet->dts = pts;
    }
    if (keyFrame) {
        m_packet->flags |= AV_PKT_FLAG_KEY;
    }

    int ret = avcodec_send_packet(m_ctx, m_packet);
    if (ret < 0 && ret != AVERROR(EAGAIN)) {
        if (error) {
            *error = QStringLiteral("送入解码包失败：%1").arg(ret);
        }
        return false;
    }

    drain(callback);
    if (error) {
        error->clear();
    }
    return true;
}

void VideoDecoder::drain(const FrameCallback &callback) {
    for (;;) {
        const int ret = avcodec_receive_frame(m_ctx, m_frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            return;
        }
        if (ret < 0) {
            return;
        }

        AVFrame *source = m_frame;
        if (m_frame->format == AV_PIX_FMT_D3D11 || m_frame->format == AV_PIX_FMT_D3D11VA_VLD) {
            av_frame_unref(m_swFrame);
            if (av_hwframe_transfer_data(m_swFrame, m_frame, 0) == 0) {
                source = m_swFrame;
            } else {
                av_frame_unref(m_frame);
                continue;
            }
        }

        convertAndEmit(source, callback);
        av_frame_unref(m_frame);
        if (source == m_swFrame) {
            av_frame_unref(m_swFrame);
        }
    }
}

void VideoDecoder::convertAndEmit(AVFrame *frame, const FrameCallback &callback) {
    const int width = frame->width;
    const int height = frame->height;
    if (width <= 0 || height <= 0) {
        return;
    }

    const AVPixelFormat srcFormat = static_cast<AVPixelFormat>(frame->format);
    if (!m_sws || m_swsWidth != width || m_swsHeight != height || m_swsFormat != srcFormat) {
        if (m_sws) {
            sws_freeContext(m_sws);
        }
        m_sws = sws_getContext(width, height, srcFormat, width, height, AV_PIX_FMT_YUV420P,
                               SWS_BILINEAR, nullptr, nullptr, nullptr);
        m_swsWidth = width;
        m_swsHeight = height;
        m_swsFormat = srcFormat;
    }
    if (!m_sws) {
        return;
    }

    auto out = std::make_shared<VideoFrame>();
    out->width = width;
    out->height = height;
    out->pts = frame->pts;
    out->decodedAtMs = monotonicNowMs();

    // 直接在目标缓冲上做转换，避免额外一次拷贝
    out->yStride = width;
    out->uStride = (width + 1) / 2;
    out->vStride = (width + 1) / 2;
    out->y.resize(out->yStride * height);
    out->u.resize(out->uStride * ((height + 1) / 2));
    out->v.resize(out->vStride * ((height + 1) / 2));

    uint8_t *dstData[4] = {reinterpret_cast<uint8_t *>(out->y.data()),
                           reinterpret_cast<uint8_t *>(out->u.data()),
                           reinterpret_cast<uint8_t *>(out->v.data()), nullptr};
    int dstLinesize[4] = {out->yStride, out->uStride, out->vStride, 0};

    sws_scale(m_sws, frame->data, frame->linesize, 0, height, dstData, dstLinesize);

    callback(out);
}

void VideoDecoder::flush(const FrameCallback &callback) {
    if (!m_ctx) {
        return;
    }
    avcodec_send_packet(m_ctx, nullptr);
    drain(callback);
}

} // namespace ks
