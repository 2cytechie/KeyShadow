#pragma once

#include <QObject>
#include <QTimer>
#include <atomic>
#include <memory>
#include <mutex>

#include "core/adb/AdbProcess.h"
#include "core/audio/AudioDecoder.h"
#include "core/audio/AudioPlayer.h"
#include "core/engine/Engines.h"
#include "core/input/InjectionThread.h"
#include "core/input/RawInput.h"
#include "core/profile/ProfileStore.h"
#include "core/scrcpy/ScrcpyClient.h"
#include "core/video/VideoDecoder.h"

namespace ks {

/// 会话编排：连接设备、拉流解码、渲染帧分发、输入注入与统计。
class Session : public QObject {
    Q_OBJECT

public:
    enum class State { Disconnected, Connecting, Streaming, Error };
    Q_ENUM(State)

    struct Stats {
        double fps = 0.0;            ///< 实际转换并显示出去的帧率
        double skippedFps = 0.0;     ///< 为压低延迟而跳过转换的帧率
        double decodeMs = 0.0;
        double inputLatencyMs = -1.0; ///< 系统采集 → 触控指令发出（含 GUI 线程排队）
        int pendingFrame = 0;         ///< GUI 线程是否有待显示帧（0/1）
        double backlogKb = -1.0;      ///< 本地接收缓冲区未读字节（KB），持续变大＝解码追不上
        double bitrateMbps = 0.0;
        QString videoCodec;
        int videoWidth = 0;
        int videoHeight = 0;
        bool hardwareDecode = false;
        bool audioEnabled = false;
    };

    explicit Session(QObject *parent = nullptr);
    ~Session() override;

    void refreshDevices();
    QList<AdbDevice> devices() const { return m_devices; }

    State state() const { return m_state; }
    QString stateMessage() const { return m_stateMessage; }
    QString connectedDeviceName() const { return m_deviceName; }
    QString connectedSerial() const { return m_serial; }

    void connectDevice(const QString &serial);
    void disconnectDevice();
    void tryReconnect();

    ProfileStore &profileStore() { return m_store; }
    Profile &profile() { return m_profile; }
    void applyProfile(const Profile &profile, bool persist = true);
    void reloadProfiles();

    /// 安装 Raw Input 过滤器（传入主窗口句柄）。
    void attachInputWindow(quintptr hwnd);
    void setInputCaptureEnabled(bool enabled);
    bool inputCaptureEnabled() const;

    /// 窗口态直操：左键由画布 Qt 事件注入触控，Raw Input 不再映射左键与拖动期视角。
    void setDirectTouchEnabled(bool enabled);
    /// 画布触控拖动按下/抬起时同步，拖动期间暂停鼠标视角注入。
    void setTouchDragActive(bool active);
    /// 指针模式：鼠标位移不驱动视角，键盘/其它鼠标键仍映射。
    void setLookEnabled(bool enabled);

    /// 呼出鼠标键（Windows VK，默认 Alt）。呼出键不参与键位映射。
    void setSummonKey(int vk);
    int summonKey() const { return m_summonKey; }

    /// 疾跑键（Windows VK，默认 Shift）。疾跑键不参与键位映射，由主线程拦截转发注入侧。
    void setSprintKey(int vk);
    int sprintKey() const { return m_sprintKey; }
    /// 疾跑触发方式："hold" 按住生效 / "switch" 按下切换。
    void setSprintMode(const QString &mode);

    /// 向注入线程投递一个输入事件（Qt 滚轮路径使用）。
    void postInputEvent(const InputEvent &event);

    void setScreenOff(bool off);
    void setAudioEnabled(bool enabled);
    void setHumanizerEnabled(bool enabled);
    bool screenOff() const { return m_screenOff; }
    bool audioEnabled() const { return m_audioEnabled; }
    bool humanizerEnabled() const { return m_humanizerEnabled; }
    void beginCalibration();
    void endCalibration();
    qint64 pollCalibratedCounts();

    void sendControlMessage(const QByteArray &message);
    void releaseAllTouches();

    /// 返回统计快照（流线程会并发更新分辨率/编码信息）。
    Stats stats() const;

signals:
    void devicesChanged(QList<ks::AdbDevice> devices);
    void stateChanged(ks::Session::State state, QString message);
    void frameReady(ks::VideoFrameConstPtr frame);
    void statsChanged(ks::Session::Stats stats);
    void logMessage(QString message);
    void calibrationFinished(qint64 counts);
    void profileChanged(ks::Profile profile);
    void summonKeyPressed(bool down);
    void sprintToggled(bool active);

private:
    void setState(State state, const QString &message);
    void startAudio();
    void stopStreams();
    QString ensureServerJar();
    void onVideoCodec(quint32 codecTag);
    void onVideoSession(int width, int height);
    void onVideoPacket(const QByteArray &payload, qint64 pts, bool keyFrame, bool configPacket);
    /// 解码线程投递：只保留最新一帧，GUI 线程最多排一个显示事件。
    void postDecodedFrame(const VideoFramePtr &frame);
    void deliverLatestFrame();
    void onAudioConfig(const QByteArray &payload);
    void onAudioPacket(const QByteArray &payload);
    void updateStats();
    void handleStreamClosed(const QString &source, const QString &reason);
    void handleSprintKey(bool down);
    void resetSprint();
    void loadGlobalSettings();

    AdbProcess m_adb;
    ScrcpyClient m_client;
    QList<AdbDevice> m_devices;

    std::unique_ptr<ScrcpyStreamReader> m_videoReader;
    std::unique_ptr<ScrcpyStreamReader> m_audioReader;
    VideoDecoder m_videoDecoder;

    // 帧投递合并：解码线程只更新「最新帧槽」，GUI 线程最多排一个显示事件，
    // 避免高帧率下 GUI 事件队列无限积压（那会连带拖慢 WM_INPUT 键鼠采集）。
    mutable std::mutex m_frameMutex;
    VideoFrameConstPtr m_latestFrame;
    std::atomic<bool> m_framePostPending{false};
    std::atomic<qint64> m_lastFramePostMs{0};
    /// 本次连接的帧率上限快照（供读线程判断是否提示软解跟不上，避免跨线程读 m_profile）。
    std::atomic<int> m_streamMaxFps{60};
    AudioDecoder m_audioDecoder;
    AudioPlayer m_audioPlayer;
    InjectionThread m_injection;
    std::unique_ptr<RawInputFilter> m_rawInput;

    ProfileStore m_store;
    Profile m_profile;
    QStringList m_profileNames;

    State m_state = State::Disconnected;
    QString m_stateMessage;
    QString m_serial;
    QString m_deviceName;
    int m_screenWidth = 1080;
    int m_screenHeight = 2400;
    bool m_stopping = false;
    int m_reconnectAttempts = 0;
    bool m_audioConfigured = false;
    quint32 m_videoCodecTag = 0;
    QByteArray m_videoExtradata;

    mutable std::mutex m_statsMutex;
    Stats m_stats;
    QTimer m_statsTimer;
    std::atomic<qint64> m_frameCounter{0};
    std::atomic<qint64> m_byteCounter{0};
    qint64 m_lastStatsTickMs = 0;
    std::atomic<double> m_decodeAccumMs{0.0};
    std::atomic<qint64> m_decodeSamples{0};
    QTimer m_reconnectTimer;
    QTimer m_frameFlushTimer; ///< 流静止时把环形回读里滞留的最后一帧取出来
    bool m_screenOff = false;
    bool m_audioEnabled = true;
    bool m_humanizerEnabled = false;
    bool m_globalSettingsLoaded = false;
    int m_summonKey = 0x12; ///< VK_MENU
    bool m_summonDown = false;
    int m_sprintKey = 0x10; ///< VK_SHIFT
    QString m_sprintMode = QStringLiteral("hold");
    bool m_sprintActive = false;
    bool m_sprintRawDown = false;
    bool m_directTouchEnabled = false;
};

} // namespace ks
