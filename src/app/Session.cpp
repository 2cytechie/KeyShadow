#include "app/Session.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>

#include "core/scrcpy/Protocol.h"
#include "core/video/VideoFrame.h"
#include "core/video/VideoOptions.h"
#include "license/LicenseManager.h"

#include <QDebug>

namespace ks {

Session::Session(QObject *parent, const QString &profileDirectory)
    : QObject(parent), m_store(profileDirectory) {
    m_profile = ProfileStore::makeDefaultProfile();
    reloadProfiles();
    loadGlobalSettings();
    m_qualityPriority = QSettings().value(QStringLiteral("video/qualityPriority"), false).toBool();
    m_qualityCodec = videoCodecForMode(true,
        QSettings().value(QStringLiteral("video/qualityCodec"), QStringLiteral("h264")).toString());
    applyProfile(m_profile, false);

    QSettings settings;
    m_summonKey = settings.value(QStringLiteral("input/summonKey"), 0x12).toInt();
    m_injection.setIgnoredKey(m_summonKey);
    m_sprintKey = settings.value(QStringLiteral("input/sprintKey"), 0x10).toInt();
    m_sprintMode = settings.value(QStringLiteral("input/sprintMode"), QStringLiteral("hold"))
                       .toString();

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
        if (m_streamHevc.load() && !m_stopping) {
            requestCodecFallback(QStringLiteral("H.265 建立连接失败：%1").arg(reason));
            return;
        }
        handleStreamClosed(QStringLiteral("连接"), reason);
    });
    connect(&m_client, &ScrcpyClient::channelsReady, this,
            [this](quintptr videoSocket, quintptr audioSocket, quintptr controlSocket,
                   quint16 controlPort) {
                Q_UNUSED(controlPort);
                m_client.detachSockets();
                m_stopping = false;
                m_audioConfigured = false;
                if (m_streamHevc.load() && !m_firstVideoFrame.load()) m_firstFrameTimer.start();

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
                m_injection.setProfile(injectionProfile());

                if (m_screenOff) {
                    m_injection.sendMessage(proto::displayPower(false));
                }

                m_lastStatsTickMs = monotonicNowMs();
                m_statsTimer.start();
                m_frameFlushTimer.start();
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

    m_firstFrameTimer.setSingleShot(true);
    m_firstFrameTimer.setInterval(10000);
    connect(&m_firstFrameTimer, &QTimer::timeout, this, [this] {
        if (!m_firstVideoFrame.load())
            requestCodecFallback(QStringLiteral("H.265 在 10 秒内未输出可显示的硬解帧"));
    });

    // 环形回读会把每帧推迟一拍发射。画面静止后不再有新包，最后一帧会一直躺在槽里，
    // 所以这里兜底：待处理帧超过阈值还没发，就主动取出来显示。
    // 阈值 40ms 高于 30fps 的帧间隔（33ms），保证正常串流时不会误触发。
    m_frameFlushTimer.setInterval(20);
    connect(&m_frameFlushTimer, &QTimer::timeout, this, [this] {
        if (m_frameFlushTimer.isActive() && m_videoDecoder.pendingAgeMs() >= 40) {
            m_videoDecoder.flushPendingFrame(
                [this](const VideoFramePtr &frame) { postDecodedFrame(frame); });
        }
    });
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
    startConnection(serial, true);
}

void Session::startConnection(const QString &serial, bool resetCodecFallback) {
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
    if (resetCodecFallback) m_forceH264 = false;
    {
        std::lock_guard<std::mutex> lock(m_statsMutex);
        m_stats = Stats{};
    }

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
    normalizeVideoOptions(m_profile, LicenseManager::instance().isPro());
    m_streamMaxFps.store(m_profile.maxFps);
    m_streamShortEdge = m_profile.videoShortEdge;
    config.videoBitRate = m_profile.videoBitRate;
    config.maxFps = m_profile.maxFps;
    config.maxSize = streamMaxSize(m_profile, m_screenWidth, m_screenHeight);
    config.videoCodec = videoCodecForMode(m_qualityPriority, m_qualityCodec, m_forceH264);
    m_streamHevc.store(config.videoCodec == QStringLiteral("h265"));
    if (!m_forceH264)
        setVideoNotice(m_streamHevc.load()
            ? QStringLiteral("H.265 试播中：正在验证设备编码与本机硬解，失败将回退 H.264。")
            : QStringLiteral("H.264 低延迟路径"));
    m_videoDecoder.setGpuFramesEnabled(
        qEnvironmentVariableIntValue("KEYSHADOW_DISABLE_GPU_SHARING") != 1);
    config.audio = m_audioEnabled;

    if (config.serverJarPath.isEmpty()) {
        setState(State::Error, QStringLiteral("scrcpy-server 准备失败"));
        return;
    }

    setState(State::Connecting, QStringLiteral("准备连接"));
    qInfo() << "connectDevice" << serial << "jar" << config.serverJarPath << "screen"
            << m_screenWidth << m_screenHeight << "fps" << config.maxFps << "bitrate"
            << config.videoBitRate << "size" << config.maxSize << "codec" << config.videoCodec
            << "audio" << config.audio;
    m_client.start(config);
}

void Session::stopStreams() {
    m_streamGeneration.fetch_add(1);
    m_firstFrameTimer.stop();
    m_statsTimer.stop();
    // 必须在停止读线程/关闭解码器之前停掉，避免与解码线程争用解码器。
    m_frameFlushTimer.stop();
    if (m_videoReader) {
        m_videoReader->stop();
        m_videoReader.reset();
    }
    if (m_audioReader) {
        m_audioReader->stop();
        m_audioReader.reset();
    }
    // 读线程已 join，此后不会再有新帧；清空帧槽并放开标志，
    // 已排队的 deliverLatestFrame 取到空帧后自然变成空操作。
    {
        std::lock_guard<std::mutex> lock(m_frameMutex);
        m_latestFrame.reset();
        m_framePostPending.store(false, std::memory_order_relaxed);
    }
    m_injection.stop();
    m_audioPlayer.stop();
    m_audioDecoder.close();
    m_videoDecoder.close();
    m_videoDecoder.takeDecodedFrames();
    m_videoDecoder.takeSkippedFrames();
    m_byteCounter.store(0);
    m_decodeAccumMs.store(0.0);
    m_decodeSamples.store(0);
    m_firstVideoFrame.store(false);
    m_codecFallbackQueued.store(false);
    {
        std::lock_guard<std::mutex> lock(m_statsMutex);
        m_stats = Stats{};
    }
}

void Session::disconnectDevice() {
    m_stopping = true;
    m_reconnectTimer.stop();
    m_statsTimer.stop();
    if (m_rawInput) {
        m_rawInput->setEnabled(false);
    }
    m_summonDown = false;
    resetSprint();
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
    const auto generation = m_streamGeneration.load();
    QMetaObject::invokeMethod(
        this,
        [this, source, reason, generation] {
            if (m_stopping || generation != m_streamGeneration.load()) {
                return;
            }
            if (m_streamHevc.load()) {
                requestCodecFallback(QStringLiteral("H.265 %1通道断开：%2").arg(source, reason));
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
    startConnection(m_serial, false);
}

void Session::loadGlobalSettings() {
    QSettings settings;

    if (!settings.contains(QStringLiteral("video/audioEnabled"))) {
        settings.setValue(QStringLiteral("video/audioEnabled"), m_profile.audioEnabled);
    }
    if (!settings.contains(QStringLiteral("video/screenOff"))) {
        settings.setValue(QStringLiteral("video/screenOff"), m_profile.screenOff);
    }
    if (!settings.contains(QStringLiteral("input/humanizerEnabled"))) {
        settings.setValue(QStringLiteral("input/humanizerEnabled"), false);
    }

    m_audioEnabled = settings.value(QStringLiteral("video/audioEnabled"), true).toBool();
    m_screenOff = settings.value(QStringLiteral("video/screenOff"), false).toBool();
    m_humanizerEnabled =
        settings.value(QStringLiteral("input/humanizerEnabled"), false).toBool();
    m_globalSettingsLoaded = true;
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
    const bool pro = LicenseManager::instance().isPro();
    normalizeVideoOptions(m_profile, pro);
    if (m_globalSettingsLoaded) {
        if (!pro && m_screenOff) setScreenOff(false);
        if (!pro && m_humanizerEnabled) {
            m_humanizerEnabled = false;
            QSettings().setValue(QStringLiteral("input/humanizerEnabled"), false);
        }
        // 音频、息屏和拟人化是全局设置，不随键位方案切换。
        m_profile.audioEnabled = m_audioEnabled;
        m_profile.screenOff = m_screenOff;
        m_profile.humanizerEnabled = m_humanizerEnabled;
    }
    // 拟人化为付费能力：授权只决定是否允许，实际开关由全局设置决定。
    m_injection.setProfile(injectionProfile());
    QSettings().setValue(QStringLiteral("profile/lastName"), m_profile.name);
    if (persist) {
        QString error;
        if (!m_store.save(m_profile, &error)) {
            emit logMessage(error);
        }
    }
    emit profileChanged(m_profile);
}

Profile Session::injectionProfile() const {
    Profile injected = m_profile;
    injected.humanizerEnabled = LicenseManager::instance().isPro() && m_humanizerEnabled;
    injected.proHumanizer = injected.humanizerEnabled;
    return injected;
}

void Session::setQualityPriority(bool enabled) {
    if (m_qualityPriority == enabled) return;
    m_qualityPriority = enabled;
    QSettings().setValue(QStringLiteral("video/qualityPriority"), enabled);
    applyCodecPreference();
    emit profileChanged(m_profile);
}

void Session::setQualityCodec(const QString &codec) {
    const QString normalized = videoCodecForMode(true, codec);
    if (m_qualityCodec == normalized) return;
    m_qualityCodec = normalized;
    QSettings().setValue(QStringLiteral("video/qualityCodec"), normalized);
    applyCodecPreference();
}

void Session::retryQualityCodec() {
    if (m_qualityPriority && m_qualityCodec == QStringLiteral("h265")) applyCodecPreference();
}

void Session::setVideoNotice(const QString &notice) {
    if (m_videoNotice == notice) return;
    m_videoNotice = notice;
    emit logMessage(notice);
    emit videoOptionsChanged();
}

void Session::applyCodecPreference() {
    m_forceH264 = false;
    const QString codec = videoCodecForMode(m_qualityPriority, m_qualityCodec);
    setVideoNotice(codec == QStringLiteral("h265")
                       ? QStringLiteral("已选择 H.265；连接后验证硬解首帧，失败回退 H.264。")
                       : QStringLiteral("H.264 低延迟路径"));
    emit videoOptionsChanged();
    if ((m_state == State::Streaming || m_state == State::Connecting) &&
        (m_streamHevc.load() != (codec == QStringLiteral("h265")))) {
        const QString serial = m_serial;
        startConnection(serial, true);
    }
}

void Session::requestCodecFallback(const QString &reason) {
    if (!m_streamHevc.load() || m_codecFallbackQueued.exchange(true)) return;
    const auto generation = m_streamGeneration.load();
    QMetaObject::invokeMethod(this, [this, reason, generation] {
        if (m_stopping || generation != m_streamGeneration.load()) return;
        m_forceH264 = true;
        setVideoNotice(QStringLiteral("%1。已回退 H.264；分辨率、码率和帧率目标保持不变。").arg(reason));
        const QString serial = m_serial;
        startConnection(serial, false);
    }, Qt::QueuedConnection);
}

void Session::disableGpuFrames() {
    m_videoDecoder.disableGpuFramesAndRecover(
        [this](const VideoFramePtr &frame) { postDecodedFrame(frame); });
    emit logMessage(QStringLiteral("GPU 画面共享不可用，已切换兼容渲染"));
}

void Session::refreshLicense() {
    const bool reconnect = !LicenseManager::instance().isPro() &&
        (m_state == State::Streaming || m_state == State::Connecting) &&
        (m_streamMaxFps.load() > 60 || m_streamShortEdge == 0 || m_streamShortEdge > 1080);
    applyProfile(m_profile, false);
    if (reconnect && !m_serial.isEmpty()) {
        emit logMessage(QStringLiteral("授权已变更，正在按基础版视频参数重连"));
        connectDevice(m_serial);
    }
}

void Session::attachInputWindow(quintptr hwnd) {
    if (!m_rawInput) {
        m_rawInput = std::make_unique<RawInputFilter>([this](const InputEvent &event) {
            int vk = 0;
            if (event.type == InputEventType::Key) {
                vk = event.vk;
            } else if (event.type == InputEventType::MouseButton) {
                vk = mouseButtonVk(event.button);
            }
            // 疾跑键不参与键位映射：主线程拦截，转发注入侧摇杆疾跑状态。
            if (vk && vkMatchesSummon(vk, m_sprintKey)) {
                handleSprintKey(event.down);
                return;
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

void Session::setSprintKey(int vk) {
    if (m_sprintKey == vk) {
        return;
    }
    m_sprintKey = vk;
    m_sprintRawDown = false;
    QSettings().setValue(QStringLiteral("input/sprintKey"), vk);
}

void Session::setSprintMode(const QString &mode) {
    const QString next = mode == QStringLiteral("switch") ? QStringLiteral("switch")
                                                         : QStringLiteral("hold");
    if (m_sprintMode == next) {
        return;
    }
    m_sprintMode = next;
    QSettings().setValue(QStringLiteral("input/sprintMode"), next);
    // 切换方式时复位疾跑状态，避免「按住→切换」后疾跑卡在开启。
    resetSprint();
}

void Session::handleSprintKey(bool down) {
    if (m_sprintMode == QStringLiteral("switch")) {
        // 按下边沿翻转：Raw 键盘自动重复会产生连续 down，需记录原始按下状态去抖。
        if (down && !m_sprintRawDown) {
            m_sprintActive = !m_sprintActive;
            m_injection.setSprint(m_sprintActive);
            emit sprintToggled(m_sprintActive);
        }
    } else if (m_sprintActive != down) {
        m_sprintActive = down;
        m_injection.setSprint(down);
        emit sprintToggled(down);
    }
    m_sprintRawDown = down;
}

void Session::resetSprint() {
    if (m_sprintActive || m_sprintRawDown) {
        m_sprintActive = false;
        m_sprintRawDown = false;
        m_injection.setSprint(false);
    }
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
    const bool wasEnabled = inputCaptureEnabled();
    if (m_rawInput) {
        m_rawInput->setEnabled(enabled);
    }
    if (!enabled) {
        m_summonDown = false;
        resetSprint();
        m_injection.releaseAll();
    } else if (!wasEnabled) {
        // 重新开始捕获：物理按键状态未知，清空「等待松开」集合，按住中的键可恢复。
        m_injection.clearSuspendedKeys();
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
        // 进入直操前释放可能已按下的映射动作（如按住的键位触点），避免手机端触点卡住。
        resetSprint();
        m_injection.releaseAll();
    }
}

void Session::setTouchDragActive(bool active) { m_injection.setTouchDragActive(active); }

void Session::setLookEnabled(bool enabled) { m_injection.setLookEnabled(enabled); }

void Session::setScreenOff(bool off) {
    off = off && LicenseManager::instance().isPro();
    m_screenOff = off;
    QSettings().setValue(QStringLiteral("video/screenOff"), off);
    m_profile.screenOff = off;
    m_injection.sendMessage(proto::displayPower(!off));
    if (off) {
        emit logMessage(QStringLiteral("已开启息屏投屏"));
    }
    emit screenOffChanged(off);
}

void Session::setAudioEnabled(bool enabled) {
    m_audioEnabled = enabled;
    QSettings().setValue(QStringLiteral("video/audioEnabled"), enabled);
    m_profile.audioEnabled = enabled;
    emit profileChanged(m_profile);
}

void Session::setHumanizerEnabled(bool enabled) {
    enabled = enabled && LicenseManager::instance().isPro();
    m_humanizerEnabled = enabled;
    QSettings().setValue(QStringLiteral("input/humanizerEnabled"), enabled);
    m_profile.humanizerEnabled = enabled;
    applyProfile(m_profile, false);
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
    if (m_codecFallbackQueued.load()) return;
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
            requestCodecFallback(QStringLiteral("H.265 解码器初始化失败：%1").arg(error));
            return;
        }
        const QString codecName = m_videoDecoder.codecName();
        const bool hardwareDecode = m_videoDecoder.hardwareAccelerated();
        if (m_streamHevc.load() && !hardwareDecode) {
            requestCodecFallback(QStringLiteral("H.265 未能启用硬件解码"));
            return;
        }
        {
            std::lock_guard<std::mutex> lock(m_statsMutex);
            m_stats.videoCodec = codecName;
            m_stats.hardwareDecode = hardwareDecode;
        }
        emit logMessage(QStringLiteral("视频解码：%1（%2）")
                            .arg(codecName, hardwareDecode ? QStringLiteral("硬件")
                                                           : QStringLiteral("软件")));
        const int streamFps = m_streamMaxFps.load();
        if (!hardwareDecode && streamFps > 60) {
            // 软解每帧开销远高于硬解，高帧率下收包线程追不上会持续积压。
            emit logMessage(QStringLiteral(
                "当前为软件解码，%1 帧可能跟不上；若出现延迟请降到 60 帧或降低分辨率")
                                .arg(streamFps));
        }
    }

    // 上一帧还排在 GUI 线程没显示完，就没必要再回读 + 转换这一帧：
    // 保留一次「强制转换」的兜底（16ms），避免持续高负载时画面被冻住。
    constexpr qint64 kHoldCeilingMs = 16;
    const bool convertAllowed =
        !m_framePostPending.load(std::memory_order_acquire) ||
        (monotonicNowMs() - m_lastFramePostMs.load(std::memory_order_relaxed) >= kHoldCeilingMs);

    const qint64 start = monotonicNowMs();
    QString error;
    const bool ok = m_videoDecoder.decode(
        payload.constData(), payload.size(), pts, keyFrame,
        [this](const VideoFramePtr &frame) {
            {
                std::lock_guard<std::mutex> lock(m_statsMutex);
                m_stats.videoWidth = frame->width;
                m_stats.videoHeight = frame->height;
            }
            postDecodedFrame(frame);
        },
        &error, convertAllowed);
    m_decodeAccumMs.store(m_decodeAccumMs.load(std::memory_order_relaxed) +
                          static_cast<double>(monotonicNowMs() - start));
    m_decodeSamples.fetch_add(1);
    if (!ok && !error.isEmpty()) {
        emit logMessage(QStringLiteral("解码错误：%1").arg(error));
        requestCodecFallback(QStringLiteral("H.265 解码失败：%1").arg(error));
    }
}

void Session::postDecodedFrame(const VideoFramePtr &frame) {
    if (!frame) {
        return;
    }
    if (!m_firstVideoFrame.exchange(true)) {
        const auto generation = m_streamGeneration.load();
        QMetaObject::invokeMethod(this, [this, generation] {
            if (m_stopping || generation != m_streamGeneration.load()) return;
            m_firstFrameTimer.stop();
            if (m_streamHevc.load())
                setVideoNotice(QStringLiteral("H.265 硬解首帧验证通过；持续性能以实际解码 FPS 和处理耗时为准。"));
        }, Qt::QueuedConnection);
    }
    bool post = false;
    {
        std::lock_guard<std::mutex> lock(m_frameMutex);
        // 旧帧被新帧顶替：它还没被 GUI 取走，属于主动丢弃（不是错误）。
        m_latestFrame = frame;
        if (!m_framePostPending.load(std::memory_order_relaxed)) {
            m_framePostPending.store(true, std::memory_order_relaxed);
            m_lastFramePostMs.store(monotonicNowMs(), std::memory_order_relaxed);
            post = true;
        }
    }
    if (post) {
        QMetaObject::invokeMethod(this, [this] { deliverLatestFrame(); }, Qt::QueuedConnection);
    }
}

void Session::deliverLatestFrame() {
    VideoFrameConstPtr frame;
    {
        std::lock_guard<std::mutex> lock(m_frameMutex);
        frame = std::move(m_latestFrame);
        m_latestFrame.reset();
        // 与取帧同处一个临界区，且在 emit 之前清零：否则新帧到达时会看到「已有事件在排」，
        // 从而不再投递，造成最后一帧永远显示不出来。
        m_framePostPending.store(false, std::memory_order_relaxed);
    }
    if (frame) {
        emit frameReady(frame);
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

    const qint64 frames = m_videoDecoder.takeDecodedFrames();
    const qint64 bytes = m_byteCounter.exchange(0);
    const qint64 decodeSamples = m_decodeSamples.exchange(0);
    const double decodeAccumMs = m_decodeAccumMs.exchange(0.0);
    const qint64 skipped = m_videoDecoder.takeSkippedFrames();

    Stats snapshot;
    {
        std::lock_guard<std::mutex> lock(m_statsMutex);
        m_stats.fps = frames / elapsedSeconds;
        m_stats.skippedFps = skipped / elapsedSeconds;
        m_stats.pendingFrame = m_framePostPending.load(std::memory_order_relaxed) ? 1 : 0;
        m_stats.backlogKb = m_videoReader ? m_videoReader->backlogBytes() / 1024.0 : -1.0;
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
