#include "core/video/VideoDecoder.h"
#include "core/video/GpuFrameConverter.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

// 必须在 extern "C" 之外：该头会引入 C++ 的 <d3d11.h>，放进 C 链接会编译失败。
#include <libavutil/hwcontext_d3d11va.h>

#include <d3d11.h>

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

/// 从 FFmpeg 的硬件设备上下文里取 D3D11 设备（含 FFmpeg 要求的 lock/unlock）。
AVD3D11VADeviceContext *d3d11HwContext(AVBufferRef *deviceRef) {
    if (!deviceRef || !deviceRef->data) {
        return nullptr;
    }
    auto *device = reinterpret_cast<AVHWDeviceContext *>(deviceRef->data);
    return static_cast<AVD3D11VADeviceContext *>(device->hwctx);
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

VideoColorSpace frameColorSpace(const AVFrame *frame) {
    if (frame->colorspace == AVCOL_SPC_BT2020_NCL || frame->colorspace == AVCOL_SPC_BT2020_CL)
        return VideoColorSpace::Bt2020;
    if (frame->colorspace == AVCOL_SPC_BT709 ||
        (frame->colorspace == AVCOL_SPC_UNSPECIFIED && frame->height > 576))
        return VideoColorSpace::Bt709;
    return VideoColorSpace::Bt601;
}

} // namespace

VideoDecoder::VideoDecoder() = default;

VideoDecoder::~VideoDecoder() { close(); }

bool VideoDecoder::open(quint32 codecTag, QString *error) {
    close();

    m_deferReadback = true;
    m_deferFailed = false;
    m_gpuFailed = false;

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
    av_hwdevice_ctx_create(&m_hwDeviceCtx, AV_HWDEVICE_TYPE_D3D11VA, nullptr, nullptr, 0);
    if (m_hwDeviceCtx) {
        AVCodecContext *hwCtx = avcodec_alloc_context3(codec);
        if (hwCtx) {
            hwCtx->flags |= AV_CODEC_FLAG_LOW_DELAY;
            hwCtx->flags2 |= AV_CODEC_FLAG2_FAST;
            // Frame threading adds one frame of latency per thread. Hardware
            // decode does not benefit from queuing an automatic frame pool.
            hwCtx->thread_count = 1;
            hwCtx->thread_type = FF_THREAD_SLICE;
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
    ctx->thread_type = FF_THREAD_SLICE; // parallel slices without a queue of whole frames
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
    // 主线程的 flushPendingFrame 可能正在用 staging 纹理，释放前先串行化。
    std::lock_guard<std::mutex> lock(m_decodeMutex);
    if (m_packet) {
        av_packet_free(&m_packet);
    }
    if (m_frame) {
        av_frame_free(&m_frame);
    }
    if (m_swFrame) {
        av_frame_free(&m_swFrame);
    }
    av_frame_free(&m_lastGpuSource);
    m_swWidth = 0;
    m_swHeight = 0;
    if (m_ctx) {
        avcodec_free_context(&m_ctx);
    }
    // 必须在释放硬件设备上下文之前释放 staging 纹理（它们属于那个 D3D11 设备）。
    releaseStaging();
    m_gpuConverter.reset();
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
                          const FrameCallback &callback, QString *error,
                          bool convertAllowed) {
    if (!m_ctx) {
        if (error) {
            *error = QStringLiteral("解码器未初始化");
        }
        return false;
    }
    // 与主线程的 flushPendingFrame 串行化（流静止时主线程会把待处理帧取走）。
    std::lock_guard<std::mutex> lock(m_decodeMutex);
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
    if (ret == AVERROR(EAGAIN)) {
        // 解码器输入队列已满：必须先取走已解出的帧腾出空间，否则这个包会被静默丢弃，
        // 后续 P 帧引用了不存在的参考帧，画面会花到下一个 IDR。帧率越高越容易触发。
        drain(callback, convertAllowed);
        ret = avcodec_send_packet(m_ctx, m_packet);
    }
    if (ret < 0 && ret != AVERROR(EAGAIN)) {
        if (error) {
            *error = QStringLiteral("送入解码包失败：%1").arg(ret);
        }
        return false;
    }

    drain(callback, convertAllowed);
    if (error) {
        error->clear();
    }
    return true;
}

void VideoDecoder::drain(const FrameCallback &callback, bool convertAllowed) {
    for (;;) {
        const int ret = avcodec_receive_frame(m_ctx, m_frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            return;
        }
        if (ret < 0) {
            return;
        }

        m_decodedFrames.fetch_add(1, std::memory_order_relaxed);
        const bool hardwareFrame = m_frame->format == AV_PIX_FMT_D3D11 ||
                                   m_frame->format == AV_PIX_FMT_D3D11VA_VLD;

        if (!convertAllowed) {
            // 解出来的帧不被显示：直接释放，省掉回读与转换。参考帧链完整，不影响后续帧。
            if (hardwareFrame && deferReadbackEnabled()) {
                clearPending(); // 待处理帧也不显示，一并丢弃，避免延迟无界增长
            }
            av_frame_unref(m_frame);
            m_skippedFrames.fetch_add(1);
            continue;
        }

        if (hardwareFrame && m_gpuFramesEnabled.load() && !m_gpuFailed) {
            if (!m_gpuConverter) m_gpuConverter = std::make_unique<GpuFrameConverter>();
            VideoFramePtr output;
            if (m_gpuConverter->convert(m_frame, m_hwDeviceCtx, output)) {
                clearPending();
                if (output) {
                    av_frame_free(&m_lastGpuSource);
                    m_lastGpuSource = av_frame_clone(m_frame);
                    callback(output);
                }
                else m_skippedFrames.fetch_add(1);
                av_frame_unref(m_frame);
                continue;
            }
            m_gpuFailed = true;
            qWarning() << "GPU color conversion unavailable, using CPU readback";
        }

        if (hardwareFrame && deferReadbackEnabled()) {
            // 环形回读：先把上一帧（GPU 早已拷完）Map + 转换发出去，再把本帧提交进另一个槽。
            if (emitPendingFrame(callback) && stageHwFrame(m_frame)) {
                av_frame_unref(m_frame);
                continue;
            }
            // 任一环节不支持/失败：本解码器实例内永久退回旧路径，本帧按旧路径处理。
            clearPending();
            m_deferFailed = true;
            qWarning() << "deferred readback unavailable, falling back to av_hwframe_transfer_data";
        }

        emitFrameLegacy(m_frame, callback);
        av_frame_unref(m_frame);
    }
}

void VideoDecoder::disableGpuFramesAndRecover(const FrameCallback &callback) {
    m_gpuFramesEnabled.store(false);
    std::lock_guard<std::mutex> lock(m_decodeMutex);
    if (m_lastGpuSource) {
        emitFrameLegacy(m_lastGpuSource, callback);
        av_frame_free(&m_lastGpuSource);
    }
}

void VideoDecoder::emitFrameLegacy(AVFrame *frame, const FrameCallback &callback) {
    AVFrame *source = frame;
    if (frame->format == AV_PIX_FMT_D3D11 || frame->format == AV_PIX_FMT_D3D11VA_VLD) {
        // 复用回读目标缓冲：每帧重建 7MB 缓冲（malloc + 缺页）本身就要几毫秒。
        if (m_swWidth != frame->width || m_swHeight != frame->height) {
            av_frame_unref(m_swFrame);
            m_swWidth = frame->width;
            m_swHeight = frame->height;
        }
        if (av_hwframe_transfer_data(m_swFrame, frame, 0) != 0) {
            return;
        }
        source = m_swFrame;
        av_frame_copy_props(source, frame);
    }
    convertAndEmit(source, callback);
}

qint64 VideoDecoder::pendingAgeMs() const {
    const qint64 stagedAt = m_pendingAtMs.load(std::memory_order_relaxed);
    return stagedAt < 0 ? -1 : monotonicNowMs() - stagedAt;
}

void VideoDecoder::flushPendingFrame(const FrameCallback &callback) {
    if (!hasPendingFrame()) {
        return;
    }
    std::lock_guard<std::mutex> lock(m_decodeMutex);
    emitPendingFrame(callback);
}

void VideoDecoder::clearPending() {
    m_pendingSlot = -1;
    m_pendingAtMs.store(-1, std::memory_order_relaxed);
}

void VideoDecoder::releaseStaging() {
    for (ID3D11Texture2D *&texture : m_staging) {
        if (texture) {
            texture->Release();
            texture = nullptr;
        }
    }
    m_stagingWidth = 0;
    m_stagingHeight = 0;
    m_stagingFormat = 0;
    m_nextSlot = 0;
    clearPending();
}

bool VideoDecoder::ensureStaging(unsigned width, unsigned height, unsigned format) {
    if (m_staging[0] && m_stagingWidth == width && m_stagingHeight == height &&
        m_stagingFormat == format) {
        return true;
    }
    releaseStaging();

    AVD3D11VADeviceContext *hw = d3d11HwContext(m_hwDeviceCtx);
    if (!hw || !hw->device) {
        return false;
    }
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = static_cast<DXGI_FORMAT>(format);
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    for (ID3D11Texture2D *&texture : m_staging) {
        if (FAILED(hw->device->CreateTexture2D(&desc, nullptr, &texture))) {
            releaseStaging();
            return false;
        }
    }
    m_stagingWidth = width;
    m_stagingHeight = height;
    m_stagingFormat = format;
    m_nextSlot = 0;
    return true;
}

bool VideoDecoder::stageHwFrame(const AVFrame *frame) {
    auto *texture = reinterpret_cast<ID3D11Texture2D *>(frame->data[0]);
    if (!texture) {
        return false;
    }
    const int width = frame->width;
    const int height = frame->height;
    if (width <= 0 || height <= 0 || (width & 1) || (height & 1)) {
        return false;
    }

    D3D11_TEXTURE2D_DESC sourceDesc{};
    texture->GetDesc(&sourceDesc);
    if (sourceDesc.Format != DXGI_FORMAT_NV12 || sourceDesc.SampleDesc.Count != 1) {
        return false;
    }
    if (static_cast<UINT>(width) > sourceDesc.Width ||
        static_cast<UINT>(height) > sourceDesc.Height) {
        return false;
    }

    AVD3D11VADeviceContext *hw = d3d11HwContext(m_hwDeviceCtx);
    if (!hw || !hw->device_context) {
        return false;
    }
    if (!ensureStaging(sourceDesc.Width, sourceDesc.Height, sourceDesc.Format)) {
        return false;
    }

    const int slot = m_nextSlot;
    const UINT subresource = D3D11CalcSubresource(
        0, static_cast<UINT>(reinterpret_cast<intptr_t>(frame->data[1])), sourceDesc.MipLevels);
    // CopySubresourceRegion 只是提交，不等待 GPU；等待被推迟到下一帧的 Map。
    if (hw->lock) {
        hw->lock(hw->lock_ctx);
    }
    hw->device_context->CopySubresourceRegion(m_staging[slot], 0, 0, 0, 0, texture, subresource,
                                              nullptr);
    if (hw->unlock) {
        hw->unlock(hw->lock_ctx);
    }

    m_pendingSlot = slot;
    m_nextSlot ^= 1;
    m_pendingWidth = width;
    m_pendingHeight = height;
    m_pendingPts = frame->pts;
    m_pendingColorSpace = frameColorSpace(frame);
    m_pendingFullRange = frame->color_range == AVCOL_RANGE_JPEG;
    m_pendingStagedAtMs = monotonicNowMs();
    m_pendingAtMs.store(m_pendingStagedAtMs, std::memory_order_relaxed);
    return true;
}

bool VideoDecoder::emitPendingFrame(const FrameCallback &callback) {
    const int slot = m_pendingSlot;
    if (slot < 0) {
        return true;
    }
    ID3D11Texture2D *texture = m_staging[slot];
    const int width = m_pendingWidth;
    const int height = m_pendingHeight;
    const qint64 pts = m_pendingPts;
    const qint64 stagedAt = m_pendingStagedAtMs;
    const unsigned textureHeight = m_stagingHeight;
    clearPending();
    if (!texture) {
        return true;
    }

    AVD3D11VADeviceContext *hw = d3d11HwContext(m_hwDeviceCtx);
    if (!hw || !hw->device_context) {
        return false;
    }

    if (hw->lock) {
        hw->lock(hw->lock_ctx);
    }
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(hw->device_context->Map(texture, 0, D3D11_MAP_READ, 0, &mapped))) {
        if (hw->unlock) {
            hw->unlock(hw->lock_ctx);
        }
        return false;
    }
    if (!ensureSws(width, height, AV_PIX_FMT_NV12)) {
        hw->device_context->Unmap(texture, 0);
        if (hw->unlock) {
            hw->unlock(hw->lock_ctx);
        }
        return false;
    }

    VideoFramePtr out = makeFrame(width, height, pts, stagedAt);
    out->colorSpace = m_pendingColorSpace;
    out->fullRange = m_pendingFullRange;
    uint8_t *dstData[4] = {reinterpret_cast<uint8_t *>(out->y.data()),
                           reinterpret_cast<uint8_t *>(out->u.data()),
                           reinterpret_cast<uint8_t *>(out->v.data()), nullptr};
    int dstLinesize[4] = {out->yStride, out->uStride, out->vStride, 0};

    // NV12 映射布局：Y 在前，UV 紧随其后；UV 偏移必须用纹理高度（可能大于显示高度）。
    const int rowPitch = static_cast<int>(mapped.RowPitch);
    const auto *base = static_cast<const uint8_t *>(mapped.pData);
    const uint8_t *srcData[3] = {
        base, base + static_cast<size_t>(rowPitch) * textureHeight, nullptr};
    const int srcLinesize[3] = {rowPitch, rowPitch, 0};
    sws_scale(m_sws, srcData, srcLinesize, 0, height, dstData, dstLinesize);

    hw->device_context->Unmap(texture, 0);
    if (hw->unlock) {
        hw->unlock(hw->lock_ctx);
    }
    callback(out);
    return true;
}

VideoFramePtr VideoDecoder::makeFrame(int width, int height, qint64 pts, qint64 decodedAtMs) {
    auto out = std::make_shared<VideoFrame>();
    out->width = width;
    out->height = height;
    out->pts = pts;
    out->decodedAtMs = decodedAtMs;
    // 直接在目标缓冲上做转换，避免额外一次拷贝
    out->yStride = width;
    out->uStride = (width + 1) / 2;
    out->vStride = (width + 1) / 2;
    out->y.resize(out->yStride * height);
    out->u.resize(out->uStride * ((height + 1) / 2));
    out->v.resize(out->vStride * ((height + 1) / 2));
    return out;
}

bool VideoDecoder::ensureSws(int width, int height, int format) {
    if (m_sws && m_swsWidth == width && m_swsHeight == height && m_swsFormat == format) {
        return true;
    }
    if (m_sws) {
        sws_freeContext(m_sws);
        m_sws = nullptr;
    }
    // 尺寸相同，只是像素格式转换（NV12 → YUV420P）：SWS_POINT 足够且最省。
    // 实测手写的标量拆交织反而比 sws 的 SIMD 实现慢约 1.5ms，故保留 sws。
    m_sws = sws_getContext(width, height, static_cast<AVPixelFormat>(format), width, height,
                           AV_PIX_FMT_YUV420P, SWS_POINT, nullptr, nullptr, nullptr);
    m_swsWidth = width;
    m_swsHeight = height;
    m_swsFormat = format;
    return m_sws != nullptr;
}

void VideoDecoder::logFormatOnce(int format) {
    if (m_loggedFormat) {
        return;
    }
    m_loggedFormat = true;
    qInfo() << "video pixel format"
            << av_get_pix_fmt_name(static_cast<AVPixelFormat>(format));
}

void VideoDecoder::convertAndEmit(AVFrame *frame, const FrameCallback &callback) {
    const int width = frame->width;
    const int height = frame->height;
    if (width <= 0 || height <= 0) {
        return;
    }

    logFormatOnce(frame->format);
    if (!ensureSws(width, height, frame->format)) {
        return;
    }

    VideoFramePtr out = makeFrame(width, height, frame->pts, monotonicNowMs());
    out->colorSpace = frameColorSpace(frame);
    out->fullRange = frame->color_range == AVCOL_RANGE_JPEG;
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
    std::lock_guard<std::mutex> lock(m_decodeMutex);
    avcodec_send_packet(m_ctx, nullptr);
    drain(callback, true);
    emitPendingFrame(callback);
}

} // namespace ks
