#pragma once

#include <QObject>
#include <QTimer>
#include <memory>

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
        double fps = 0.0;
        double decodeMs = 0.0;
        double inputLatencyMs = -1.0;
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

    /// 呼出鼠标键（Windows VK，默认 Alt）。呼出键不参与键位映射。
    void setSummonKey(int vk);
    int summonKey() const { return m_summonKey; }

    /// 向注入线程投递一个输入事件（Qt 滚轮路径使用）。
    void postInputEvent(const InputEvent &event);

    void setScreenOff(bool off);
    void beginCalibration();
    void endCalibration();
    qint64 pollCalibratedCounts();

    void sendControlMessage(const QByteArray &message);
    void releaseAllTouches();

    const Stats &stats() const { return m_stats; }

signals:
    void devicesChanged(QList<ks::AdbDevice> devices);
    void stateChanged(ks::Session::State state, QString message);
    void frameReady(ks::VideoFrameConstPtr frame);
    void statsChanged(ks::Session::Stats stats);
    void logMessage(QString message);
    void calibrationFinished(qint64 counts);
    void profileChanged(ks::Profile profile);
    void summonKeyPressed(bool down);

private:
    void setState(State state, const QString &message);
    void startAudio();
    void stopStreams();
    QString ensureServerJar();
    void onVideoCodec(quint32 codecTag);
    void onVideoSession(int width, int height);
    void onVideoPacket(const QByteArray &payload, qint64 pts, bool keyFrame, bool configPacket);
    void onAudioConfig(const QByteArray &payload);
    void onAudioPacket(const QByteArray &payload);
    void updateStats();
    void handleStreamClosed(const QString &source, const QString &reason);

    AdbProcess m_adb;
    ScrcpyClient m_client;
    QList<AdbDevice> m_devices;

    std::unique_ptr<ScrcpyStreamReader> m_videoReader;
    std::unique_ptr<ScrcpyStreamReader> m_audioReader;
    VideoDecoder m_videoDecoder;
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

    Stats m_stats;
    QTimer m_statsTimer;
    qint64 m_frameCounter = 0;
    qint64 m_byteCounter = 0;
    qint64 m_lastStatsTickMs = 0;
    double m_decodeAccumMs = 0.0;
    qint64 m_decodeSamples = 0;
    QTimer m_reconnectTimer;
    bool m_screenOff = false;
    int m_summonKey = 0x12; ///< VK_MENU
    bool m_summonDown = false;
};

} // namespace ks
