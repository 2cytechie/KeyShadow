#include "app/Session.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>

#include "core/scrcpy/Protocol.h"
#include "core/video/VideoFrame.h"
#include "license/LicenseManager.h"

#include <QDebug>

namespace ks {

Session::Session(QObject *parent) : QObject(parent) {
    m_profile = ProfileStore::makeDefaultProfile();
    reloadProfiles();

    QSettings settings;
    m_summonKey = settings.value(QStringLiteral("input/summonKey"), 0x12).toInt();
    m_injection.setIgnoredKey(m_summonKey);

    connect(&m_client, &ScrcpyClient::connecting, this, [this](const QString &stage) {
        setState(State::Connecting, stage);
    });
    connect(&m_client, &ScrcpyClient::connected, this, [this](const QString &deviceName) {
        m_deviceName = deviceName;
        m_reconnectAttempts = 0;
        setState(State::Streaming, QStringLiteral("已连接 %1").arg(deviceName));
        if (m_rawInput) {
            m_rawInput->setEnabled(true);
        }
    });
    connect(&m_client, &ScrcpyClient::failed, this, [this](const QString &reason) {
        handleStreamClosed(QStringLiteral("连接"), reason);
    });
    connect(&m_client, &ScrcpyClient::channelsReady, this,
            [this](quintptr videoSocket, quintptr audioSocket, quintptr controlSocket,
                   quint16 controlPort) {
                Q_UNUSED(controlPort);
                m_client.detachSockets();
                m_stopping = false;
                m_audioConfigured = false;

                // 视频通道
                m_videoReader = std::make_unique<ScrcpyStreamReader>();
                m_videoReader->start(
                    videoSocket, true,
                    [this](quint32 tag) { onVideoCodec(tag); },
                    [this](int width, int height) { onVideoSession(width, height); },
                    [this](const QByteArray &payload, qint64 pts, bool key, bool config) {
                        onVideoPacket(payload, pts, key, config);
                    },
                    [this](const QString &reason) {
                        handleStreamClosed(QStringLiteral("视频"), reason);
                    });

                // 音频通道
                if (audioSocket) {
                    startAudio();
                    m_audioReader = std::make_unique<ScrcpyStreamReader>();
                    m_audioReader->start(
                        audioSocket, false,
                        [this](quint32 tag) {
                            if (tag == 0) {
                                emit logMessage(QStringLiteral("设备未启用音频通道"));
                            }
                        },
                        [](int, int) {},
                        [this](const QByteArray &payload, qint64, bool, bool configPacket) {
                            if (configPacket) {
                                onAudioConfig(payload);
                            } else {
                                onAudioPacket(payload);
                            }
                        },
                        [this](const QString &reason) {
                            Q_UNUSED(reason);
                            emit logMessage(QStringLiteral("音频通道已断开"));
                        });
                }

                // 控制通道
                QString error;
                if (!m_injection.start(controlSocket, m_screenWidth, m_screenHeight, &error)) {
                    emit logMessage(QStringLiteral("输入注入启动失败：%1").arg(error));
                }
                m_injection.setProfile(m_profile);

                if (m_screenOff) {
                    m_injection.sendMessage(proto::displayPower(false));
                }

                m_lastStatsTickMs = monotonicNowMs();
                m_statsTimer.start();
            });
    connect(&m_client, &ScrcpyClient::stopped, this, [this] {
        if (m_state != State::Error) {
            setState(State::Disconnected, QStringLiteral("已断开"));
        }
    });

    m_statsTimer.setInterval(1000);
    connect(&m_statsTimer, &QTimer::timeout, this, &Session::updateStats);

    m_reconnectTimer.setSingleShot(true);
    m_reconnectTimer.setInterval(1200);
    connect(&m_reconnectTimer, &QTimer::timeout, this, &Session::tryReconnect);
}

Session::~Session() {
    m_statsTimer.stop();
    m_reconnectTimer.stop();
    stopStreams();
    m_client.stop();
}

void Session::setState(State state, const QString &message) {
    m_state = state;
    m_stateMessage = message;
    emit stateChanged(state, message);
}

void Session::refreshDevices() {
    QString error;
    m_devices = m_adb.listDevices(&error);
    if (!error.isEmpty()) {
        emit logMessage(QStringLiteral("设备枚举失败：%1").arg(error));
    }
    emit devicesChanged(m_devices);
}

QString Session::ensureServerJar() {
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
                        QStringLiteral("/server");
    if (!QDir().mkpath(dir)) {
        emit logMessage(QStringLiteral("无法创建目录：%1").arg(dir));
        return {};
    }
    const QString target = dir + QStringLiteral("/scrcpy-server.jar");

    QFile resource(QStringLiteral(":/scrcpy-server.jar"));
    if (!resource.open(QIODevice::ReadOnly)) {
        emit logMessage(QStringLiteral("内置 scrcpy-server 资源缺失"));
        return {};
    }
    const QByteArray data = resource.readAll();
    resource.close();
    if (data.isEmpty()) {
        emit logMessage(QStringLiteral("内置 scrcpy-server 资源为空"));
        return {};
    }

    QFileInfo info(target);
    if (info.exists() && info.size() == data.size()) {
        return target;
    }

    qInfo() << "extract scrcpy-server to" << target << "bytes" << data.size();

    QFile output(target);
    if (!output.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        emit logMessage(QStringLiteral("无法写入 scrcpy-server：%1（%2）")
                            .arg(target, output.errorString()));
        return {};
    }
    output.write(data);
    output.close();
    return target;
}

void Session::connectDevice(const QString &serial) {
    if (m_state == State::Connecting || m_state == State::Streaming) {
        disconnectDevice();
    } else {
        // 上一次连接失败后，ScrcpyClient 的工作线程对象仍可能 joinable；
        // 必须先回收，否则 start() 给 joinable std::thread 赋值会触发 std::terminate。
        stopStreams();
        m_client.stop();
    }

    if (serial.isEmpty()) {
        setState(State::Error, QStringLiteral("未检测到设备"));
        return;
    }

    m_serial = serial;
    m_stopping = false;
    m_reconnectAttempts = 0;

    int width = 0;
    int height = 0;
    if (m_adb.queryScreenSize(serial, &width, &height)) {
        m_screenWidth = width;
        m_screenHeight = height;
    }

    ScrcpyConfig config;
    config.serial = serial;
    config.adbPath = m_adb.adbPath();
    config.serverJarPath = ensureServerJar();
    config.videoBitRate = m_profile.videoBitRate;
    config.maxFps = m_profile.maxFps;
    config.maxSize = m_profile.maxSize;
    config.audio = m_profile.audioEnabled;

    if (config.serverJarPath.isEmpty()) {
        setState(State::Error, QStringLiteral("scrcpy-server 准备失败"));
        return;
    }

    setState(State::Connecting, QStringLiteral("准备连接"));
    qInfo() << "connectDevice" << serial << "jar" << config.serverJarPath << "screen"
            << m_screenWidth << m_screenHeight << "fps" << config.maxFps << "bitrate"
            << config.videoBitRate << "size" << config.maxSize << "audio" << config.audio;
    m_client.start(config);
}

void Session::stopStreams() {
    m_statsTimer.stop();
    if (m_videoReader) {
        m_videoReader->stop();
        m_videoReader.reset();
    }
    if (m_audioReader) {
        m_audioReader->stop();
        m_audioReader.reset();
    }
    m_injection.stop();
    m_audioPlayer.stop();
    m_audioDecoder.close();
    m_videoDecoder.close();
    m_frameCounter.store(0);
    m_byteCounter.store(0);
    m_decodeAccumMs.store(0.0);
    m_decodeSamples.store(0);
}

void Session::disconnectDevice() {
    m_stopping = true;
    m_reconnectTimer.stop();
    m_statsTimer.stop();
    if (m_rawInput) {
        m_rawInput->setEnabled(false);
    }
    m_client.stop();
    stopStreams();
    m_deviceName.clear();
    setState(State::Disconnected, QStringLiteral("已断开"));
}

void Session::handleStreamClosed(const QString &source, const QString &reason) {
    if (m_stopping) {
        return;
    }
    // 回调来自流读取线程：切回主线程再操作定时器与状态
    QMetaObject::invokeMethod(
        this,
        [this, source, reason] {
            if (m_stopping) {
                return;
            }
            emit logMessage(QStringLiteral("%1通道断开：%2").arg(source, reason));
            if (m_state == State::Streaming && m_reconnectAttempts < 3 && !m_serial.isEmpty()) {
                setState(State::Connecting, QStringLiteral("正在重连…"));
                m_reconnectAttempts += 1;
                m_reconnectTimer.start();
                return;
            }
            setState(State::Error, reason);
        },
        Qt::QueuedConnection);
}

void Session::tryReconnect() {
    if (m_serial.isEmpty()) {
        return;
    }
    stopStreams();
    m_client.stop();
    connectDevice(m_serial);
}

void Session::reloadProfiles() {
    const QVector<Profile> profiles = m_store.loadAll();
    m_profileNames.clear();
    for (const Profile &profile : profiles) {
        m_profileNames.append(profile.name);
    }
    const QString lastName = QSettings().value(QStringLiteral("profile/lastName")).toString();
    if (m_profileNames.contains(lastName)) {
        bool ok = false;
        const Profile loaded = m_store.load(lastName, &ok);
        if (ok) {
            m_profile = loaded;
        }
    } else if (!m_profileNames.contains(m_profile.name)) {
        m_profile = profiles.first();
    }
    applyProfile(m_profile, false);
}

void Session::applyProfile(const Profile &profile, bool persist) {
    m_profile = profile;
    // 拟人化为 Pro 自动能力：不读取方案偏好，注入侧按授权统一决定。
    Profile injected = m_profile;
    const bool pro = LicenseManager::instance().isPro();
    injected.humanizerEnabled = pro;
    injected.proHumanizer = pro;
    m_injection.setProfile(injected);
    QSettings().setValue(QStringLiteral("profile/lastName"), m_profile.name);
    if (persist) {
        QString error;
        if (!m_store.save(m_profile, &error)) {
            emit logMessage(error);
        }
    }
    emit profileChanged(m_profile);
}

void Session::attachInputWindow(quintptr hwnd) {
    if (!m_rawInput) {
        m_rawInput = std::make_unique<RawInputFilter>([this](const InputEvent &event) {
            int vk = 0;
            if (event.type == InputEventType::Key) {
                vk = event.vk;
            } else if (event.type == InputEventType::MouseButton) {
                vk = event.button == 0 ? 0x01 : event.button == 1 ? 0x02 : 0x04;
            }
            if (vk && vkMatchesSummon(vk, m_summonKey)) {
                if (event.down != m_summonDown) {
                    m_summonDown = event.down;
                    emit summonKeyPressed(event.down);
                }
                return;
            }
            m_injection.postInput(event);
        });
        qApp->installNativeEventFilter(m_rawInput.get());
        m_rawInput->setEnabled(false);
    }
    m_rawInput->attachTo(hwnd);
}

void Session::setSummonKey(int vk) {
    if (m_summonKey == vk) {
        return;
    }
    m_summonKey = vk;
    m_summonDown = false;
    m_injection.setIgnoredKey(vk);
    QSettings().setValue(QStringLiteral("input/summonKey"), vk);
}

void Session::postInputEvent(const InputEvent &event) { m_injection.postInput(event); }

void Session::setInputCaptureEnabled(bool enabled) {
    if (m_rawInput) {
        m_rawInput->setEnabled(enabled);
    }
    if (!enabled) {
        m_summonDown = false;
        m_injection.releaseAll();
    }
}

bool Session::inputCaptureEnabled() const {
    return m_rawInput && m_rawInput->isEnabled();
}

void Session::setDirectTouchEnabled(bool enabled) {
    if (m_directTouchEnabled == enabled) {
        return;
    }
    m_directTouchEnabled = enabled;
    m_injection.setDirectTouchEnabled(enabled);
    if (enabled) {
        // 进入直操前释放可能已按下的映射动作（如左键 Hold），避免手机端触点卡住。
        m_injection.releaseAll();
    }
}

void Session::setTouchDragActive(bool active) { m_injection.setTouchDragActive(active); }

void Session::setScreenOff(bool off) {
    m_screenOff = off;
    m_profile.screenOff = off;
    m_injection.sendMessage(proto::displayPower(!off));
    if (off) {
        emit logMessage(QStringLiteral("已开启息屏投屏"));
    }
}

void Session::beginCalibration() { m_injection.beginCalibration(); }

void Session::endCalibration() { m_injection.endCalibration(); }

qint64 Session::pollCalibratedCounts() { return m_injection.takeCalibratedCounts(); }

void Session::sendControlMessage(const QByteArray &message) { m_injection.sendMessage(message); }

void Session::releaseAllTouches() { m_injection.releaseAll(); }

void Session::startAudio() {
    QString error;
    if (!m_audioPlayer.start(&error)) {
        emit logMessage(QStringLiteral("音频输出不可用：%1").arg(error));
        return;
    }
    {
        std::lock_guard<std::mutex> lock(m_statsMutex);
        m_stats.audioEnabled = true;
    }
    emit logMessage(QStringLiteral("音频通道已开启（%1Hz / %2 声道）")
                        .arg(m_audioPlayer.sampleRate())
                        .arg(m_audioPlayer.channels()));
}

void Session::onVideoCodec(quint32 codecTag) {
    m_videoCodecTag = codecTag;
    m_videoExtradata.clear();
    qInfo() << "video codec tag" << Qt::hex << codecTag;
}

void Session::onVideoSession(int width, int height) {
    if (width <= 0 || height <= 0) {
        return;
    }
    bool resolutionChanged = false;
    {
        std::lock_guard<std::mutex> lock(m_statsMutex);
        resolutionChanged =
            m_stats.videoWidth > 0 && (m_stats.videoWidth != width || m_stats.videoHeight != height);
        m_stats.videoWidth = width;
        m_stats.videoHeight = height;
    }
    m_screenWidth = width;
    m_screenHeight = height;
    m_injection.setScreenSize(width, height);
    if (resolutionChanged) {
        // 横竖屏切换/分辨率变化：丢弃旧解码器，等待新的配置包后按新参数重建。
        m_videoDecoder.close();
    }
    emit logMessage(QStringLiteral("视频流分辨率：%1×%2").arg(width).arg(height));
}

void Session::onVideoPacket(const QByteArray &payload, qint64 pts, bool keyFrame, bool configPacket) {
    m_byteCounter.fetch_add(static_cast<qint64>(payload.size()));

    // 配置包（SPS/PPS）作为解码器 extradata，避免部分机型仅在此处携带参数集。
    // 旋转/分辨率变化时配置包内容会更新，必须重建解码器，否则会沿用旧参数导致花屏。
    if (configPacket) {
        if (m_videoExtradata != payload) {
            m_videoExtradata = payload;
            if (m_videoDecoder.isOpen()) {
                m_videoDecoder.close();
            }
        }
        return;
    }

    if (!m_videoDecoder.isOpen()) {
        QString error;
        m_videoDecoder.setExtradata(m_videoExtradata);
        if (!m_videoDecoder.open(m_videoCodecTag, &error)) {
            emit logMessage(QStringLiteral("视频解码初始化失败：%1").arg(error));
            return;
        }
        const QString codecName = m_videoDecoder.codecName();
        const bool hardwareDecode = m_videoDecoder.hardwareAccelerated();
        {
            std::lock_guard<std::mutex> lock(m_statsMutex);
            m_stats.videoCodec = codecName;
            m_stats.hardwareDecode = hardwareDecode;
        }
        emit logMessage(QStringLiteral("视频解码：%1（%2）")
                            .arg(codecName, hardwareDecode ? QStringLiteral("硬件")
                                                           : QStringLiteral("软件")));
    }

    const qint64 start = monotonicNowMs();
    QString error;
    const bool ok = m_videoDecoder.decode(
        payload.constData(), payload.size(), pts, keyFrame,
        [this](const VideoFramePtr &frame) {
            m_frameCounter.fetch_add(1);
            {
                std::lock_guard<std::mutex> lock(m_statsMutex);
                m_stats.videoWidth = frame->width;
                m_stats.videoHeight = frame->height;
            }
            emit frameReady(frame);
        },
        &error);
    m_decodeAccumMs.store(m_decodeAccumMs.load(std::memory_order_relaxed) +
                          static_cast<double>(monotonicNowMs() - start));
    m_decodeSamples.fetch_add(1);
    if (!ok && !error.isEmpty()) {
        emit logMessage(QStringLiteral("解码错误：%1").arg(error));
    }
}

void Session::onAudioConfig(const QByteArray &payload) {
    if (!m_audioPlayer.isRunning()) {
        return;
    }
    QString error;
    if (!m_audioDecoder.configure(payload, m_audioPlayer.sampleRate(),
                                  m_audioPlayer.channels(), &error)) {
        emit logMessage(QStringLiteral("音频解码初始化失败：%1").arg(error));
        return;
    }
    m_audioConfigured = true;
}

void Session::onAudioPacket(const QByteArray &payload) {
    if (!m_audioConfigured || !m_audioPlayer.isRunning()) {
        return;
    }
    QByteArray pcm;
    int frames = 0;
    QString error;
    if (!m_audioDecoder.decode(payload, &pcm, &frames, &error)) {
        return;
    }
    if (frames > 0) {
        m_audioPlayer.push(reinterpret_cast<const float *>(pcm.constData()), frames);
    }
}

void Session::updateStats() {
    const qint64 now = monotonicNowMs();
    const double elapsedSeconds = std::max(0.001, (now - m_lastStatsTickMs) / 1000.0);
    m_lastStatsTickMs = now;

    const qint64 frames = m_frameCounter.exchange(0);
    const qint64 bytes = m_byteCounter.exchange(0);
    const qint64 decodeSamples = m_decodeSamples.exchange(0);
    const double decodeAccumMs = m_decodeAccumMs.exchange(0.0);

    Stats snapshot;
    {
        std::lock_guard<std::mutex> lock(m_statsMutex);
        m_stats.fps = frames / elapsedSeconds;
        m_stats.bitrateMbps = (bytes * 8.0) / elapsedSeconds / 1'000'000.0;
        m_stats.inputLatencyMs = m_injection.takeLatencyMs();
        m_stats.decodeMs = decodeSamples > 0 ? decodeAccumMs / decodeSamples : 0.0;
        snapshot = m_stats;
    }
    emit statsChanged(snapshot);
}

Session::Stats Session::stats() const {
    std::lock_guard<std::mutex> lock(m_statsMutex);
    return m_stats;
}

} // namespace ks
