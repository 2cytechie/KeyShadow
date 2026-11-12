// KeyShadow 无界面联调探针：验证 adb / scrcpy 握手 / 视频解码 / 音频解码链路。
// 用法：keyshadow_probe.exe <serial> [scrcpy-server.jar 路径]

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QImage>
#include <QTimer>

#include <cstdarg>
#include <cstdio>
#include <cmath>
#include <algorithm>

#include "core/adb/AdbProcess.h"
#include "core/audio/AudioDecoder.h"
#include "core/audio/AudioPlayer.h"
#include "core/scrcpy/Protocol.h"
#include "core/scrcpy/ScrcpyClient.h"
#include "core/video/VideoDecoder.h"
#include "core/video/VideoFrame.h"

#include <winsock2.h>

namespace {

void logLine(const char *format, ...) {
    va_list args;
    va_start(args, format);
    std::vfprintf(stdout, format, args);
    std::fputc('\n', stdout);
    va_end(args);
    std::fflush(stdout);
}

/// 用 CPU 做 YUV420P(BT.709 limited) → RGB 转换并保存，用于核对解码输出是否正确。
bool saveFrame(const ks::VideoFrame &frame, const QString &path) {
    QImage image(frame.width, frame.height, QImage::Format_RGB888);
    for (int y = 0; y < frame.height; ++y) {
        const auto *rowY = reinterpret_cast<const unsigned char *>(frame.y.constData() +
                                                                   y * frame.yStride);
        const auto *rowU = reinterpret_cast<const unsigned char *>(
            frame.u.constData() + (y / 2) * frame.uStride);
        const auto *rowV = reinterpret_cast<const unsigned char *>(
            frame.v.constData() + (y / 2) * frame.vStride);
        auto *out = image.scanLine(y);
        for (int x = 0; x < frame.width; ++x) {
            const double yy = std::max(0.0, (rowY[x] - 16.0) * 1.164383);
            const double u = rowU[x / 2] - 128.0;
            const double v = rowV[x / 2] - 128.0;
            const double r = yy + 1.5748 * v;
            const double g = yy - 0.1873 * u - 0.4681 * v;
            const double b = yy + 1.8556 * u;
            out[x * 3 + 0] = static_cast<unsigned char>(std::clamp(r, 0.0, 255.0));
            out[x * 3 + 1] = static_cast<unsigned char>(std::clamp(g, 0.0, 255.0));
            out[x * 3 + 2] = static_cast<unsigned char>(std::clamp(b, 0.0, 255.0));
        }
    }
    return image.save(path);
}

} // namespace

int main(int argc, char *argv[]) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    logLine("---- probe start ----");

    QCoreApplication app(argc, argv);
    logLine("---- QCoreApplication ready ----");

    const QStringList args = app.arguments();
    if (args.size() < 2) {
        logLine("usage: keyshadow_probe <serial> [scrcpy-server.jar]");
        return 2;
    }
    const QString serial = args.at(1);
    const QString jar = args.size() > 2
                            ? args.at(2)
                            : QStringLiteral("C:/Users/kongb/AppData/Roaming/KeyShadow/KeyShadow/"
                                             "server/scrcpy-server.jar");

    ks::ScrcpyClient client;
    ks::VideoDecoder decoder;
    ks::AudioDecoder audioDecoder;
    ks::AudioPlayer audioPlayer;
    auto videoReader = std::make_unique<ks::ScrcpyStreamReader>();
    auto audioReader = std::make_unique<ks::ScrcpyStreamReader>();

    int frames = 0;
    int audioPackets = 0;
    qint64 decodeAccumNs = 0;
    int decodeSamples = 0;
    int width = 0;
    int height = 0;
    quint32 codecTag = 0;

    QObject::connect(&client, &ks::ScrcpyClient::connecting,
                     [](const QString &stage) { logLine("[stage] %s", qPrintable(stage)); });
    QObject::connect(&client, &ks::ScrcpyClient::failed, [&](const QString &reason) {
        logLine("[FAIL] %s", qPrintable(reason));
        app.exit(1);
    });
    QObject::connect(&client, &ks::ScrcpyClient::connected,
                     [&](const QString &name) { logLine("[ok] connected: %s", qPrintable(name)); });
    QObject::connect(&client, &ks::ScrcpyClient::channelsReady, &app,
                     [&](quintptr videoSocket, quintptr audioSocket, quintptr controlSocket,
                         quint16 port) {
                         logLine("[ok] channels ready, control port %u", port);
                         client.detachSockets();

                         if (qEnvironmentVariableIntValue("KEYSHADOW_PROBE_INPUT") == 1) {
                             QTimer::singleShot(2500, [controlSocket] {
                                 SOCKET socket = static_cast<SOCKET>(controlSocket);
                                 const QByteArray down =
                                     ks::proto::keycode(ks::proto::KeyAction::Down, 3); // HOME
                                 const QByteArray up =
                                     ks::proto::keycode(ks::proto::KeyAction::Up, 3);
                                 ::send(socket, down.constData(), down.size(), 0);
                                 ::send(socket, up.constData(), up.size(), 0);
                                 logLine("[input] sent HOME key via control channel");
                             });
                         }

                         videoReader->start(
                             videoSocket, true,
                             [&](quint32 tag) {
                                 codecTag = tag;
                                 logLine("[video] codec tag 0x%08x", tag);
                             },
                             [&](int w, int h) {
                                 width = w;
                                 height = h;
                                 logLine("[video] session %dx%d", w, h);
                             },
                             [&](const QByteArray &payload, qint64 pts, bool key, bool config) {
                                 if (config) {
                                     decoder.setExtradata(payload);
                                     return;
                                 }
                                 if (!decoder.isOpen()) {
                                     QString error;
                                     if (!decoder.open(codecTag, &error)) {
                                         logLine("[FAIL] decoder: %s", qPrintable(error));
                                         app.exit(1);
                                         return;
                                     }
                                     logLine("[video] decoder %s (%s)",
                                             qPrintable(decoder.codecName()),
                                             decoder.hardwareAccelerated() ? "hardware"
                                                                           : "software");
                                 }
                                 QElapsedTimer decodeTimer;
                                 decodeTimer.start();
                                 // KEYSHADOW_PROBE_SKIP_CONVERT=1：只解码不回读/不转换，
                                 // 用于量出 GPU→CPU 回读本身占多少时间。
                                 const bool convert =
                                     qEnvironmentVariableIntValue("KEYSHADOW_PROBE_SKIP_CONVERT") !=
                                     1;
                                 decoder.decode(payload.constData(), payload.size(), pts, key,
                                                [&](const ks::VideoFramePtr &frame) {
                                                    frames += 1;
                                                    if (frames == 1) {
                                                        logLine("[ok] first frame %dx%d",
                                                                frame->width, frame->height);
                                                    }
                                                    static bool dumped1 = false;
                                                    static bool dumped2 = false;
                                                    if (qEnvironmentVariableIntValue(
                                                            "KEYSHADOW_PROBE_DUMP") == 1) {
                                                        if (!dumped1) {
                                                            dumped1 = true;
                                                            saveFrame(*frame,
                                                                      "D:/code/KeyShadow/build/"
                                                                      "frame1.png");
                                                        } else if (!dumped2 && frames > 120) {
                                                            dumped2 = true;
                                                            saveFrame(*frame,
                                                                      "D:/code/KeyShadow/build/"
                                                                      "frame2.png");
                                                        }
                                                    }
                                                },
                                                nullptr, convert);
                                 decodeAccumNs += decodeTimer.nsecsElapsed();
                                 decodeSamples += 1;
                             },
                             [&](const QString &reason) {
                                 logLine("[video] closed: %s", qPrintable(reason));
                             });

                         if (audioSocket) {
                             QString error;
                             if (!audioPlayer.start(&error)) {
                                 logLine("[warn] audio player: %s", qPrintable(error));
                             }
                             audioReader->start(
                                 audioSocket, false,
                                 [&](quint32 tag) {
                                     logLine("[audio] codec tag 0x%08x", tag);
                                 },
                                 [](int, int) {},
                                 [&](const QByteArray &payload, qint64, bool, bool configPacket) {
                                     if (configPacket) {
                                         QString error;
                                         if (!audioDecoder.configure(payload,
                                                                     audioPlayer.sampleRate(),
                                                                     audioPlayer.channels(),
                                                                     &error)) {
                                             logLine("[warn] opus config: %s", qPrintable(error));
                                         } else {
                                             logLine("[audio] opus ready %d Hz %d ch",
                                                     audioPlayer.sampleRate(),
                                                     audioPlayer.channels());
                                         }
                                         return;
                                     }
                                     QByteArray pcm;
                                     int decoded = 0;
                                     QString error;
                                     if (audioDecoder.decode(payload, &pcm, &decoded, &error)) {
                                         audioPackets += 1;
                                         if (decoded > 0) {
                                             audioPlayer.push(
                                                 reinterpret_cast<const float *>(pcm.constData()),
                                                 decoded);
                                         }
                                     }
                                 },
                                 [&](const QString &reason) {
                                     logLine("[audio] closed: %s", qPrintable(reason));
                                 });
                         }
                     });

    ks::ScrcpyConfig config;
    config.serial = serial;
    config.adbPath = ks::AdbProcess::resolveAdbPath();
    config.serverJarPath = jar;
    // 允许用环境变量调参，便于对照 60/90/120 帧与不同分辨率。
    const int probeBitrate = qEnvironmentVariableIntValue("KEYSHADOW_PROBE_BITRATE");
    config.videoBitRate = probeBitrate > 0 ? probeBitrate : 8000000;
    const int probeFps = qEnvironmentVariableIntValue("KEYSHADOW_PROBE_MAX_FPS");
    config.maxFps = probeFps > 0 ? probeFps : 60;
    config.maxSize = qEnvironmentVariableIntValue("KEYSHADOW_PROBE_MAX_SIZE");

    logLine("adb=%s jar=%s serial=%s", qPrintable(config.adbPath),
            qPrintable(config.serverJarPath), qPrintable(serial));
    client.start(config);

    QTimer stats;
    QObject::connect(&stats, &QTimer::timeout, [&] {
        const double decodeMs =
            decodeSamples > 0 ? decodeAccumNs / 1e6 / decodeSamples : 0.0;
        logLine("[stat] frames=%d audio=%d session=%dx%d decode=%.2fms", frames, audioPackets,
                width, height, decodeMs);
    });
    stats.start(1000);

    QTimer::singleShot(15000, &app, [&] {
        logLine("[done] frames=%d audioPackets=%d", frames, audioPackets);
        const bool ok = frames >= 10;
        videoReader->stop();
        audioReader->stop();
        audioPlayer.stop();
        audioDecoder.close();
        decoder.close();
        client.stop();
        app.exit(ok ? 0 : 1);
    });

    return app.exec();
}
