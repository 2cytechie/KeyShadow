#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>
#include <atomic>
#include <functional>
#include <thread>

namespace ks {

struct ScrcpyConfig {
    QString serial;
    QString adbPath;
    QString serverJarPath; ///< 本地 scrcpy-server.jar 路径
    QString serverVersion = QStringLiteral("5.0.1");
    int maxSize = 0; ///< 0 = 使用设备原生分辨率
    int videoBitRate = 8000000;
    int maxFps = 60;
    QString videoCodec = QStringLiteral("h264");
    bool audio = true;
    int audioBitRate = 128000;
    QString logLevel = QStringLiteral("info");
};

/// 读取 scrcpy 视频/音频裸流通道（12 字节包头协议）。
class ScrcpyStreamReader {
public:
    using CodecHandler = std::function<void(quint32 codecTag)>;
    using SessionHandler = std::function<void(int width, int height)>;
    using PacketHandler = std::function<void(const QByteArray &payload, qint64 pts, bool keyFrame,
                                              bool configPacket)>;
    using ClosedHandler = std::function<void(const QString &reason)>;

    ScrcpyStreamReader();
    ~ScrcpyStreamReader();

    bool start(quintptr socket, bool isVideo, CodecHandler onCodec, SessionHandler onSession,
               PacketHandler onPacket, ClosedHandler onClosed);
    void stop();
    bool isRunning() const { return m_running.load(); }

    /// 本地接收缓冲区里尚未读走的字节数（FIONREAD）。
    /// 稳定为 0 表示消费跟得上；持续很大说明解码追不上，延迟就堆在这条链路上。
    qint64 backlogBytes() const { return m_backlogBytes.load(std::memory_order_relaxed); }

private:
    void run();

    quintptr m_socket = 0;
    bool m_isVideo = false;
    CodecHandler m_onCodec;
    SessionHandler m_onSession;
    PacketHandler m_onPacket;
    ClosedHandler m_onClosed;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_stopRequested{false};
    std::atomic<qint64> m_backlogBytes{0};
    std::thread m_thread;
};

/// scrcpy 会话建立器：推送 server、建立 adb reverse 隧道、等待设备回连。
class ScrcpyClient : public QObject {
    Q_OBJECT

public:
    explicit ScrcpyClient(QObject *parent = nullptr);
    ~ScrcpyClient() override;

    void start(const ScrcpyConfig &config);
    void stop();
    bool isRunning() const { return m_running.load(); }
    QString deviceName() const { return m_deviceName; }
    QString socketName() const { return m_socketName; }

    /// 会话建立后把 socket 所有权移交给流读取线程/注入线程，避免重复关闭。
    void detachSockets();

signals:
    void connecting(QString stage);
    void connected(QString deviceName);
    void channelsReady(quintptr videoSocket, quintptr audioSocket, quintptr controlSocket,
                       quint16 controlPort);
    void failed(QString reason);
    void stopped();

private:
    void cleanupTunnel();
    void postFailed(const QString &reason, quint64 generation);

    ScrcpyConfig m_config;
    QString m_deviceName;
    QString m_socketName;
    std::thread m_thread;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_stopRequested{false};
    std::atomic<quint64> m_generation{0};
    quintptr m_videoSocket = 0;
    quintptr m_audioSocket = 0;
    quintptr m_controlSocket = 0;
    quintptr m_listenSocket = 0;
    quint16 m_localPort = 0;
    qint64 m_serverPid = 0;
};

} // namespace ks
