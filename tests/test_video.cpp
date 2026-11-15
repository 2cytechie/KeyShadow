#include "core/video/VideoDecoder.h"
#include "ui/VideoWidget.h"
#include "ui/VideoPresentationSettings.h"
#include "app/Session.h"
#include "license/LicenseManager.h"
#include <QSettings>
#include <QTemporaryDir>

#include <QApplication>
#include <QElapsedTimer>
#include <QImage>
#include <QOpenGLContext>
#include <QWindow>
#include <cstdio>
#include <cstring>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
}

namespace {
int failures = 0;
int checks = 0;
void check(bool ok, const char *message) {
    ++checks;
    if (!ok) ++failures;
    std::printf("[%s] %s\n", ok ? " ok " : "FAIL", message);
}
struct Packet { QByteArray bytes; qint64 pts; bool key; };

std::vector<Packet> makeStream(bool hevc = false) {
    std::vector<Packet> packets;
    const AVCodec *encoder = avcodec_find_encoder_by_name(hevc ? "libx265" : "libx264");
    check(encoder != nullptr, hevc ? "HEVC fixture encoder is available" : "H264 fixture encoder is available");
    if (!encoder) return packets;
    AVCodecContext *ctx = avcodec_alloc_context3(encoder);
    // Tiny HEVC pictures can fall below hardware decoder dimension limits.
    ctx->width = hevc ? 512 : 128; ctx->height = hevc ? 256 : 64;
    ctx->pix_fmt = AV_PIX_FMT_YUV420P;
    ctx->time_base = {1,120}; ctx->framerate = {120,1};
    ctx->max_b_frames = 0;
    ctx->colorspace = AVCOL_SPC_BT709;
    ctx->color_range = AVCOL_RANGE_MPEG;
    av_opt_set(ctx->priv_data, "preset", "ultrafast", 0);
    av_opt_set(ctx->priv_data, "tune", "zerolatency", 0);
    if (hevc) av_opt_set(ctx->priv_data, "x265-params", "log-level=error:pools=1:frame-threads=1:bframes=0", 0);
    if (avcodec_open2(ctx, encoder, nullptr) < 0) {
        check(false, "fixture encoder opens"); avcodec_free_context(&ctx); return packets;
    }
    AVFrame *frame = av_frame_alloc();
    frame->width = ctx->width; frame->height = ctx->height; frame->format = ctx->pix_fmt;
    frame->colorspace = ctx->colorspace; frame->color_range = ctx->color_range;
    av_frame_get_buffer(frame, 32);
    AVPacket *packet = av_packet_alloc();
    auto drain = [&] {
        while (avcodec_receive_packet(ctx, packet) >= 0) {
            packets.push_back({QByteArray(reinterpret_cast<char *>(packet->data), packet->size),
                               packet->pts, (packet->flags & AV_PKT_FLAG_KEY) != 0});
            av_packet_unref(packet);
        }
    };
    for (int i=0; i<120; ++i) {
        av_frame_make_writable(frame);
        for (int y=0; y<ctx->height; ++y)
            std::memset(frame->data[0]+y*frame->linesize[0], y<ctx->height/2 ? 32+i : 48, ctx->width);
        for (int y=0; y<ctx->height; ++y)
            std::memset(frame->data[0]+y*frame->linesize[0], y<ctx->height/2 ? 63 : 32, ctx->width/4);
        for (int y=0; y<ctx->height/2; ++y) {
            std::memset(frame->data[1]+y*frame->linesize[1], 128, ctx->width/2);
            std::memset(frame->data[2]+y*frame->linesize[2], 128, ctx->width/2);
            std::memset(frame->data[1]+y*frame->linesize[1], y<ctx->height/4 ? 102 : 240, ctx->width/8);
            std::memset(frame->data[2]+y*frame->linesize[2], y<ctx->height/4 ? 240 : 118, ctx->width/8);
        }
        frame->pts = i;
        avcodec_send_frame(ctx, frame); drain();
    }
    avcodec_send_frame(ctx, nullptr); drain();
    av_packet_free(&packet); av_frame_free(&frame); avcodec_free_context(&ctx);
    check(packets.size() == 120, "fixture contains 120 distinct encoded frames");
    return packets;
}

void testDecodeCount(const std::vector<Packet> &packets, quint32 codec = 0x68323634u) {
    qputenv("KEYSHADOW_FORCE_SW_DECODE", "1");
    ks::VideoDecoder decoder;
    QString error;
    check(decoder.open(codec, &error), "software video decoder opens");
    int callbacks = 0;
    bool ok = true;
    for (const auto &packet : packets)
        ok &= decoder.decode(packet.bytes.data(), packet.bytes.size(), packet.pts, packet.key,
                              [&](const ks::VideoFramePtr &) { ++callbacks; }, &error, false);
    check(ok, "all fixture packets decode successfully");
    check(callbacks == 0 && decoder.takeSkippedFrames() == 120,
          "conversion-disabled frames are skipped without output callbacks");
    check(decoder.takeDecodedFrames() == 120,
          "decoded FPS includes successful frames skipped before rendering");
    check(decoder.takeDecodedFrames() == 0, "interval counter resets without double counting");
    decoder.flush([](const ks::VideoFramePtr &) {});
    check(decoder.takeDecodedFrames() == 0, "flush never invents duplicate decoded frames");
    qunsetenv("KEYSHADOW_FORCE_SW_DECODE");
}

ks::VideoFramePtr colorFrame(bool fullRange) {
    auto frame = std::make_shared<ks::VideoFrame>();
    frame->width=64; frame->height=32;
    frame->yStride=64; frame->uStride=frame->vStride=32;
    frame->y.resize(64*32); frame->u.resize(32*16); frame->v.resize(32*16);
    frame->colorSpace=ks::VideoColorSpace::Bt601;
    frame->fullRange=fullRange;
    for (int y=0; y<32; ++y)
        std::memset(frame->y.data()+y*64, y<16 ? (fullRange?76:81) : (fullRange?29:41), 64);
    for (int y=0; y<16; ++y) {
        std::memset(frame->u.data()+y*32, y<8 ? (fullRange?85:90) : (fullRange?255:240), 32);
        std::memset(frame->v.data()+y*32, y<8 ? (fullRange?255:240) : (fullRange?107:110), 32);
    }
    return frame;
}

bool isRed(const QColor &color) { return color.red()>235 && color.green()<15 && color.blue()<15; }
bool isBlue(const QColor &color) { return color.blue()>235 && color.green()<15 && color.red()<15; }

void testRendering(ks::ui::VideoWidget &widget) {
    for (bool full : {false,true}) for (bool quality : {false,true}) {
        widget.setQualityPriority(quality);
        widget.setFrame(colorFrame(full));
        widget.setEnhanceParams({});
        const QImage original=widget.grabFramebuffer();
        check(!original.isNull(), "video shader renders a framebuffer");
        if (original.isNull()) continue;
        const int x=original.width()/2;
        check(isRed(original.pixelColor(x,original.height()/4)) &&
              isBlue(original.pixelColor(x,original.height()*3/4)),
              "range conversion and two-pass scaling retain color and orientation");
        ks::ui::VideoWidget::EnhanceParams params; params.sharpness=100;
        widget.setEnhanceParams(params);
        const QImage sharpened=widget.grabFramebuffer();
        check(isRed(sharpened.pixelColor(x,sharpened.height()/4)),
              "CAS preserves constant color regions without halos");
        params.brightness=100;
        widget.setEnhanceParams(params);
        widget.setBypassEnhance(true);
        const QImage bypass=widget.grabFramebuffer();
        check(isRed(bypass.pixelColor(x,bypass.height()/4)), "compare-original bypasses enhancements");
        widget.setBypassEnhance(false);
    }
    widget.setQualityPriority(true);
    auto edge=colorFrame(true);
    edge->u.fill(char(128)); edge->v.fill(char(128));
    for (int y=0;y<edge->height;++y) {
        std::memset(edge->y.data()+y*edge->yStride,96,32);
        std::memset(edge->y.data()+y*edge->yStride+32,160,32);
    }
    widget.setFrame(edge); widget.setEnhanceParams({});
    const QImage soft=widget.grabFramebuffer();
    ks::ui::VideoWidget::EnhanceParams strong; strong.sharpness=100;
    widget.setEnhanceParams(strong);
    const QImage crisp=widget.grabFramebuffer();
    const int x=soft.width()/2, y=soft.height()/2;
    const int before=soft.pixelColor(x+2,y).red()-soft.pixelColor(x-3,y).red();
    const int after=crisp.pixelColor(x+2,y).red()-crisp.pixelColor(x-3,y).red();
    check(after>before, "CAS increases edge contrast at output resolution");
    widget.clearFrame();
    const QImage black=widget.grabFramebuffer();
    check(!black.isNull() && black.pixelColor(black.width()/2,black.height()/2).red()<20,
          "disconnect clears the previous frame");
}

void testGpu(const std::vector<Packet> &packets, ks::ui::VideoWidget &widget,
             quint32 codec = 0x68323634u) {
    ks::VideoDecoder decoder;
    decoder.setGpuFramesEnabled(true);
    QString error;
    if (!decoder.open(codec, &error) || !decoder.hardwareAccelerated()) {
        std::puts("[SKIP] D3D11 hardware decoder unavailable on this machine"); return;
    }
    ks::VideoFramePtr last;
    bool ok=true;
    int packetIndex = 0, firstOutputPacket = -1;
    for (const auto &packet:packets) {
        ok &= decoder.decode(packet.bytes.data(),packet.bytes.size(),packet.pts,packet.key,
                              [&](const ks::VideoFramePtr &frame){
                                  if (firstOutputPacket < 0) firstOutputPacket = packetIndex;
                                  last=frame;
                              },&error);
        ++packetIndex;
    }
    decoder.flush([&](const ks::VideoFramePtr &frame){last=frame;});
    check(ok && last!=nullptr, "hardware decoder produces displayable frames");
    check(firstOutputPacket >= 0 && firstOutputPacket <= 1,
          "zero-B-frame stream produces a first frame within two packets without flushing");
    if (!last || !last->gpu) {
        std::puts("[SKIP] D3D11 video processor unavailable; compatible readback path used"); return;
    }
    bool fallback=false;
    QObject::connect(&widget,&ks::ui::VideoWidget::gpuFramesUnavailable,&widget,[&]{fallback=true;});
    widget.setEnhanceParams({});
    widget.setFrame(last);
    for (bool quality : {false,true}) {
        widget.setQualityPriority(quality);
        const QImage image=widget.grabFramebuffer();
        if (fallback) { std::puts("[SKIP] WGL sharing unavailable; fallback signal verified"); break; }
        check(!image.isNull(), "GPU texture renders through WGL interop");
        const QColor top=image.pixelColor(image.width()/2,image.height()/4);
        const QColor bottom=image.pixelColor(image.width()/2,image.height()*3/4);
        check(top.red()>120 && top.red()<190 && std::abs(top.red()-top.green())<5 &&
              top.red()>bottom.red()+80,
              "GPU conversion and scaling preserve gray range and vertical orientation");
        check(isRed(image.pixelColor(image.width()/8,image.height()/4)) &&
              isBlue(image.pixelColor(image.width()/8,image.height()*3/4)),
              "GPU BT709 matrix and BGRA sharing preserve red and blue channels");
    }
    decoder.takeDecodedFrames();
    ks::VideoFramePtr recovered;
    decoder.disableGpuFramesAndRecover([&](const ks::VideoFramePtr &frame){recovered=frame;});
    check(recovered && !recovered->gpu && !recovered->y.isEmpty(),
          "sharing failure recovers the static last frame without waiting for a new packet");
    check(decoder.takeDecodedFrames()==0, "fallback recovery does not inflate decoded FPS");
    decoder.close();
    widget.grabFramebuffer(); // GUI surface remains valid after decoder/device shutdown.
    decoder.setGpuFramesEnabled(false);
    check(decoder.open(codec,&error), "decoder can reopen for compatible rendering");
    last.reset();
    for (const auto &packet:packets)
        decoder.decode(packet.bytes.data(),packet.bytes.size(),packet.pts,packet.key,
                       [&](const ks::VideoFramePtr &frame){ last=frame; },&error);
    decoder.flush([&](const ks::VideoFramePtr &frame){ last=frame; });
    check(last && !last->gpu && !last->y.isEmpty(), "disabled sharing falls back to CPU planes");
    if (last) {
        widget.setFrame(last); widget.setQualityPriority(true);
        const QImage compatible=widget.grabFramebuffer();
        const auto top=compatible.pixelColor(compatible.width()/2,compatible.height()/4);
        check(top.red()>120 && top.red()<190, "GPU-to-compatible transition continues rendering");
    }
    widget.clearFrame(); widget.grabFramebuffer();
    QObject::disconnect(&widget,&ks::ui::VideoWidget::gpuFramesUnavailable,nullptr,nullptr);
}

void testPresentationPreferences() {
    QTemporaryDir directory;
    const QString path = directory.filePath("presentation.ini");
    {
        QSettings settings(path, QSettings::IniFormat);
        check(ks::ui::verticalSyncEnabled(settings), "vertical sync is enabled by default");
        ks::ui::setVerticalSyncEnabled(settings, false);
        settings.sync();
        check(settings.status() == QSettings::NoError, "vertical-sync preference saves successfully");
    }
    {
        QSettings settings(path, QSettings::IniFormat);
        check(!ks::ui::verticalSyncEnabled(settings), "disabled vertical sync survives settings recreation");
    }
    for (bool enabled : {false, true}) {
        {
            QSettings settings(path, QSettings::IniFormat);
            ks::ui::setVerticalSyncEnabled(settings, enabled);
            settings.sync();
        }
        const QSettings restored(path, QSettings::IniFormat);
        const QSurfaceFormat format = ks::ui::videoPresentationFormat(restored);
        check(format.swapInterval() == (enabled ? 1 : 0) &&
              format.swapBehavior() == QSurfaceFormat::DoubleBuffer,
              "startup format reads the saved sync preference and keeps double buffering");
    }
    const QSettings restored(path, QSettings::IniFormat);
    check(ks::ui::verticalSyncEnabled(restored), "re-enabled vertical sync survives settings recreation");
}

void testRenderTimings(ks::ui::VideoWidget &widget) {
    widget.setQualityPriority(false);
    widget.setTargetFps(120);
    ks::ui::VideoWidget::EnhanceParams params;
    params.sharpness = 100;
    widget.setEnhanceParams(params);
    widget.setFrame(colorFrame(true));
    for (int i=0; i<4; ++i) widget.grabFramebuffer();
    const auto fast = widget.renderStats();
    std::printf("Render timings: GPU=%.3f ms, CPU submit=%.3f ms\n", fast.gpuMs, fast.submitMs);
    check(fast.gpuMs >= 0.0 && fast.submitMs >= 0.0,
          "asynchronous GPU timing and separate CPU submission timing produce real measurements");
    check(fast.status.contains(QStringLiteral("CAS 25%")) && fast.degradation == 0,
          "frame priority reports the effective gentle CAS strength");
    widget.setQualityPriority(true);
    for (int i=0; i<3; ++i) widget.grabFramebuffer();
    check(widget.renderStats().status.contains(QStringLiteral("CAS 100%")),
          "quality priority retains the selected full CAS strength");
    widget.setBypassEnhance(true);
    widget.grabFramebuffer();
    check(widget.renderStats().status.contains(QStringLiteral("CAS 0%")),
          "original comparison reports the actual bypass instead of stale enhancement settings");
    widget.setBypassEnhance(false);
    widget.setTargetFps(60);
    check(widget.renderStats().gpuMs < 0.0,
          "a changed processing budget invalidates old GPU measurements");
    widget.clearFrame(); widget.grabFramebuffer();
}

void testVideoPreferences() {
    QTemporaryDir directory;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, directory.path());
    {
        ks::Session session(nullptr, directory.path()+"/profiles");
        check(!session.qualityPriority(), "frame priority is the default mode");
        check(session.qualityCodec() == QStringLiteral("h264"), "both modes default to H264 until HEVC is selected");
        const auto before=session.profile();
        session.setQualityPriority(true);
        session.setQualityCodec(QStringLiteral("h265"));
        check(session.profile().videoShortEdge==before.videoShortEdge &&
              session.profile().maxFps==before.maxFps && session.profile().videoBitRate==before.videoBitRate,
              "mode and codec changes preserve source resolution, bitrate and FPS target");
        session.setQualityPriority(false);
        check(session.qualityCodec() == QStringLiteral("h265"), "frame priority preserves the separate quality-codec preference");
        session.setQualityPriority(true);
        session.setScreenOff(true);
        session.setHumanizerEnabled(true);
        check(!session.screenOff() && !session.humanizerEnabled(),
              "basic paid-feature enforcement also applies outside the UI");
#ifndef NDEBUG
        auto &license=ks::LicenseManager::instance();
        license.setDevUnlocked(true);
        ks::Profile pro=session.profile();
        pro.videoShortEdge=0; pro.maxFps=120;
        session.applyProfile(pro,false);
        session.setScreenOff(true); session.setHumanizerEnabled(true);
        check(session.profile().videoShortEdge==0 && session.profile().maxFps==120 &&
              session.screenOff() && session.humanizerEnabled(),
              "Pro allows native 120 fps and opt-in paid switches");
        license.setDevUnlocked(false);
        session.refreshLicense();
        check(session.profile().videoShortEdge==1080 && session.profile().maxFps==60 &&
              !session.screenOff() && !session.humanizerEnabled(),
              "removing Pro reclaims video limits and paid switches");
#endif
    }
    {
        ks::Session restored(nullptr, directory.path()+"/profiles");
        check(restored.qualityPriority(), "quality-priority preference survives Session recreation");
        check(restored.qualityCodec() == QStringLiteral("h265"), "quality-codec preference survives Session recreation");
        restored.setQualityCodec(QStringLiteral("untrusted;h265"));
        check(restored.qualityCodec() == QStringLiteral("h264"), "invalid codec preferences are normalized before server arguments");
    }
}

// Optional real-device integration: isolated preferences, no injected keys/touch.
// Exercises Session (rather than only the decoder) and the live renderer.
int testDeviceSession(QApplication &app, const QString &serial, const QString &mode) {
    QTemporaryDir directory;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, directory.path());
    QSettings().setValue(QStringLiteral("video/audioEnabled"), false);
    const bool expectFallback = mode == QStringLiteral("h265-fallback");
    if (expectFallback) qputenv("KEYSHADOW_FORCE_SW_DECODE", "1");
    ks::Session session(nullptr, directory.filePath("profiles"));
    auto profile = session.profile();
    profile.videoShortEdge = 1080;
    profile.maxFps = 60; // basic-license integration test; never bypass paid gates
    profile.videoBitRate = 32'000'000;
    session.applyProfile(profile, false);
    session.setQualityPriority(mode != QStringLiteral("h264"));
    session.setQualityCodec(mode == QStringLiteral("h264") ? QStringLiteral("h264")
                                                           : QStringLiteral("h265"));
    ks::ui::VideoWidget widget;
    widget.setAttribute(Qt::WA_DontShowOnScreen);
    widget.resize(1920, 1080);
    widget.setQualityPriority(session.qualityPriority());
    ks::ui::VideoWidget::EnhanceParams params;
    params.sharpness = 20;
    widget.setEnhanceParams(params);
    widget.show();
    int frames = 0;
    bool fallback = false, hevcSeen = false, h264Seen = false;
    QObject::connect(&session, &ks::Session::frameReady, &widget,
                     [&](const ks::VideoFrameConstPtr &frame) { ++frames; widget.setFrame(frame); });
    QObject::connect(&widget, &ks::ui::VideoWidget::gpuFramesUnavailable,
                     &session, &ks::Session::disableGpuFrames);
    QObject::connect(&session, &ks::Session::videoOptionsChanged, &app, [&] {
        fallback |= session.codecFallbackActive();
        if (session.codecFallbackActive()) qunsetenv("KEYSHADOW_FORCE_SW_DECODE");
        std::printf("[device notice] %s\n", qPrintable(session.videoNotice()));
        std::fflush(stdout);
    });
    QObject::connect(&session, &ks::Session::statsChanged, &app, [&](const ks::Session::Stats &stats) {
        hevcSeen |= stats.videoCodec == QStringLiteral("h265") && stats.fps > 0;
        h264Seen |= stats.videoCodec == QStringLiteral("h264") && stats.fps > 0;
        std::printf("[device stats] %s %dx%d %.1f decoded fps GPU=%.3f submit=%.3f ms\n",
                    qPrintable(stats.videoCodec), stats.videoWidth, stats.videoHeight, stats.fps,
                    widget.renderStats().gpuMs, widget.renderStats().submitMs);
        std::fflush(stdout);
    });
    QTimer deadline;
    deadline.setSingleShot(true);
    QObject::connect(&deadline, &QTimer::timeout, &app, [&] {
        check(frames > 10, "real-device Session delivers frames to the renderer");
        check(session.profile().videoShortEdge == 1080 && session.profile().maxFps == 60 &&
              session.profile().videoBitRate == 32'000'000,
              "real-device mode/codec selection retains every selected source parameter");
        if (expectFallback)
            check(fallback && h264Seen && !session.videoNotice().isEmpty(),
                  "HEVC software-only failure falls back to H264 with a visible reason");
        else if (mode == QStringLiteral("h265"))
            check(hevcSeen && h264Seen && !fallback,
                  "real-device HEVC first-frame validation and a live switch to H264 both work");
        else
            check(h264Seen && !fallback, "real-device frame priority uses H264");
        check(widget.renderStats().gpuMs >= 0.0, "real-device renderer produces GPU processing timings");
        session.disconnectDevice();
        widget.clearFrame();
        qunsetenv("KEYSHADOW_FORCE_SW_DECODE");
        std::printf("%d checks, %d failures\n", checks, failures);
        app.exit(failures ? 1 : 0);
    });
    session.connectDevice(serial);
    if (mode == QStringLiteral("h265")) QTimer::singleShot(6000, &app, [&] {
        session.setQualityPriority(false);
        widget.setQualityPriority(false);
    });
    deadline.start(expectFallback ? 20000 : 12000);
    return app.exec();
}
} // namespace

int main(int argc,char **argv) {
    QSurfaceFormat format;
    format.setVersion(3,3); format.setProfile(QSurfaceFormat::CoreProfile);
    QSurfaceFormat::setDefaultFormat(format);
    // Exercise each startup mode in a fresh process, before Qt caches the format.
    // Both modes run the entire rendering/decode suite with isolated settings.
    QTemporaryDir startupDirectory;
    QSettings startupSettings(startupDirectory.filePath("startup.ini"), QSettings::IniFormat);
    if (argc > 1 && std::strcmp(argv[1], "--no-vsync") == 0)
        ks::ui::setVerticalSyncEnabled(startupSettings, false);
    QSurfaceFormat::setDefaultFormat(ks::ui::videoPresentationFormat(startupSettings));
    QApplication app(argc,argv);
    app.setOrganizationName("KeyShadowTests");
    app.setApplicationName("VideoValidation");
    if (argc >= 4 && std::strcmp(argv[1], "--device-smoke") == 0)
        return testDeviceSession(app, QString::fromLocal8Bit(argv[2]), QString::fromLocal8Bit(argv[3]));
    const auto stream=makeStream();
    testDecodeCount(stream);
    ks::ui::VideoWidget widget;
    widget.setAttribute(Qt::WA_DontShowOnScreen);
    widget.resize(640,320); widget.show();
    app.processEvents();
    const int expectedInterval = ks::ui::verticalSyncEnabled(startupSettings) ? 1 : 0;
    std::printf("Startup swap interval: requested=%d, context=%d, window=%d\n", expectedInterval,
                widget.context() ? widget.context()->format().swapInterval() : -1,
                widget.windowHandle() ? widget.windowHandle()->format().swapInterval() : -1);
    check(widget.context() && widget.context()->isValid() && widget.windowHandle() &&
          widget.windowHandle()->format().swapInterval() == expectedInterval,
          "top-level video window uses the saved startup sync interval");
    testRendering(widget);
    testGpu(stream,widget);
    const auto hevc = makeStream(true);
    testDecodeCount(hevc, 0x68323635u);
    testGpu(hevc,widget, 0x68323635u);
    testRenderTimings(widget);
    testPresentationPreferences();
    testVideoPreferences();
    std::printf("%d checks, %d failures\n",checks,failures);
    return failures ? 1 : 0;
}
