#include "core/scrcpy/ScrcpyClient.h"

#include "core/adb/AdbProcess.h"
#include "core/scrcpy/Protocol.h"
#include "core/video/VideoFrame.h"

#include <QDebug>
#include <QFileInfo>
#include <QRandomGenerator>

#include <winsock2.h>
#include <ws2tcpip.h>

#include <cstring>
#include <mutex>

namespace ks {
namespace {

constexpr const char *kRemoteServerPath = "/data/local/tmp/scrcpy-server.jar";
constexpr int kConnectTimeoutMs = 20000;
constexpr quint32 kMaxPacketSize = 32u * 1024u * 1024u;

void ensureWinsock() {
    static std::once_flag once;
    std::call_once(once, [] {
        WSADATA data{};
        WSAStartup(MAKEWORD(2, 2), &data);
    });
}

bool recvAll(SOCKET socket, char *buffer, int length) {
    int received = 0;
    while (received < length) {
        const int r = ::recv(socket, buffer + received, length - received, 0);
        if (r <= 0) {
            return false;
        }
        received += r;
    }
    return true;
}

quint32 readU32Be(const char *data) {
    const auto *p = reinterpret_cast<const unsigned char *>(data);
    return (static_cast<quint32>(p[0]) << 24) | (static_cast<quint32>(p[1]) << 16) |
           (static_cast<quint32>(p[2]) << 8) | static_cast<quint32>(p[3]);
}

quint64 readU64Be(const char *data) {
    quint64 value = 0;
    for (int i = 0; i < 8; ++i) {
        value = (value << 8) | static_cast<unsigned char>(data[i]);
    }
    return value;
}

void closeSocket(quintptr &socketValue) {
    if (!socketValue) {
        return;
    }
    SOCKET s = static_cast<SOCKET>(socketValue);
    ::shutdown(s, SD_BOTH);
    ::closesocket(s);
    socketValue = 0;
}

QString socketErrorText() {
    return QStringLiteral("socket error %1").arg(WSAGetLastError());
}

} // namespace

// ---------------------------------------------------------------------------
// ScrcpyStreamReader
// ---------------------------------------------------------------------------

ScrcpyStreamReader::ScrcpyStreamReader() = default;

ScrcpyStreamReader::~ScrcpyStreamReader() { stop(); }

bool ScrcpyStreamReader::start(quintptr socket, bool isVideo, CodecHandler onCodec,
                               SessionHandler onSession, PacketHandler onPacket,
                               ClosedHandler onClosed) {
    if (m_running.load()) {
        return false;
    }
    m_socket = socket;
    m_isVideo = isVideo;
    m_onCodec = std::move(onCodec);
    m_onSession = std::move(onSession);
    m_onPacket = std::move(onPacket);
    m_onClosed = std::move(onClosed);
    m_stopRequested.store(false);
    m_running.store(true);
    m_thread = std::thread([this] { run(); });
    return true;
}

void ScrcpyStreamReader::stop() {
    m_stopRequested.store(true);
    if (m_socket) {
        SOCKET s = static_cast<SOCKET>(m_socket);
        ::shutdown(s, SD_BOTH);
    }
    if (m_thread.joinable()) {
        m_thread.join();
    }
    // detachSockets() 后 socket 所有权已移交本对象，必须在这里关闭，否则每次重连都会泄漏句柄。
    if (m_socket) {
        ::closesocket(static_cast<SOCKET>(m_socket));
        m_socket = 0;
    }
    m_running.store(false);
}

void ScrcpyStreamReader::run() {
    ensureWinsock();
    SOCKET socket = static_cast<SOCKET>(m_socket);
    QString reason;

    char codecBytes[4] = {0, 0, 0, 0};
    if (!recvAll(socket, codecBytes, 4)) {
        reason = QStringLiteral("读取码流头失败");
    }

    const quint32 codecTag = readU32Be(codecBytes);
    if (reason.isEmpty()) {
        if (m_onCodec) {
            m_onCodec(codecTag);
        }
        if (codecTag == 0) {
            reason = QStringLiteral("设备端禁用了该码流");
        }
    }

    while (reason.isEmpty() && !m_stopRequested.load()) {
        char header[proto::kPacketHeaderSize];
        if (!recvAll(socket, header, proto::kPacketHeaderSize)) {
            reason = m_stopRequested.load() ? QString() : QStringLiteral("连接已断开");
            break;
        }

        if (m_isVideo && (static_cast<unsigned char>(header[0]) & 0x80)) {
            const int width = static_cast<int>(readU32Be(header + 4));
            const int height = static_cast<int>(readU32Be(header + 8));
            if (m_onSession) {
                m_onSession(width, height);
            }
            continue;
        }

        const quint64 ptsFlags = readU64Be(header);
        const quint32 length = readU32Be(header + 8);
        if (length == 0 || length > kMaxPacketSize) {
            reason = QStringLiteral("非法数据包长度：%1").arg(length);
            break;
        }

        QByteArray payload;
        payload.resize(static_cast<int>(length));
        if (!recvAll(socket, payload.data(), static_cast<int>(length))) {
            reason = m_stopRequested.load() ? QString() : QStringLiteral("读取数据包失败");
            break;
        }

        const bool isConfig = (ptsFlags & proto::kPacketFlagConfig) != 0;
        const bool isKey = (ptsFlags & proto::kPacketFlagKeyFrame) != 0;
        const qint64 pts = isConfig ? -1 : static_cast<qint64>(ptsFlags & proto::kPacketPtsMask);

        if (m_onPacket) {
            m_onPacket(payload, pts, isKey, isConfig);
        }

        // 处理完这一包后，内核接收缓冲区里还剩多少：这是「解码追不上」的直接证据。
        if (m_isVideo) {
            u_long available = 0;
            if (::ioctlsocket(socket, FIONREAD, &available) == 0) {
                m_backlogBytes.store(static_cast<qint64>(available), std::memory_order_relaxed);
            }
        }
    }

    m_running.store(false);
    if (m_onClosed && !reason.isEmpty()) {
        m_onClosed(reason);
    }
}

// ---------------------------------------------------------------------------
// ScrcpyClient
// ---------------------------------------------------------------------------

ScrcpyClient::ScrcpyClient(QObject *parent) : QObject(parent) { ensureWinsock(); }

ScrcpyClient::~ScrcpyClient() { stop(); }

void ScrcpyClient::start(const ScrcpyConfig &config) {
    // 防御：上一次连接失败时 postFailed() 只置位 m_running，线程对象可能仍 joinable。
    // 这里先回收，避免给 joinable std::thread 赋值导致进程终止。
    if (m_thread.joinable()) {
        m_stopRequested.store(true);
        closeSocket(m_listenSocket);
        closeSocket(m_videoSocket);
        closeSocket(m_audioSocket);
        closeSocket(m_controlSocket);
        m_thread.join();
        m_stopRequested.store(false);
        m_running.store(false);
    }

    if (m_running.exchange(true)) {
        return;
    }
    m_stopRequested.store(false);
    m_config = config;
    m_deviceName.clear();
    m_thread = std::thread([this] {
        AdbProcess adb;
        adb.setAdbPath(m_config.adbPath.isEmpty() ? AdbProcess::resolveAdbPath() : m_config.adbPath);
        const QString serial = m_config.serial;

        auto fail = [this](const QString &reason) { postFailed(reason); };

        // 1) 推送 scrcpy-server
        QMetaObject::invokeMethod(this, [this] { emit connecting(QStringLiteral("推送 scrcpy-server")); },
                                  Qt::QueuedConnection);
        if (!QFileInfo::exists(m_config.serverJarPath)) {
            fail(QStringLiteral("缺少 scrcpy-server.jar：%1").arg(m_config.serverJarPath));
            return;
        }
        QString error;
        if (!adb.push(serial, m_config.serverJarPath, QString::fromLatin1(kRemoteServerPath), &error)) {
            fail(QStringLiteral("推送 scrcpy-server 失败：%1").arg(error));
            return;
        }
        qInfo() << "scrcpy-server pushed";

        // 2) 本地监听随机端口 + adb reverse
        SOCKET listenSocket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listenSocket == INVALID_SOCKET) {
            fail(socketErrorText());
            return;
        }
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        if (::bind(listenSocket, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0 ||
            ::listen(listenSocket, 4) != 0) {
            ::closesocket(listenSocket);
            fail(QStringLiteral("本地端口监听失败：%1").arg(socketErrorText()));
            return;
        }
        int addrLen = sizeof(addr);
        ::getsockname(listenSocket, reinterpret_cast<sockaddr *>(&addr), &addrLen);
        m_localPort = ntohs(addr.sin_port);
        m_listenSocket = static_cast<quintptr>(listenSocket);

        const quint32 scid = QRandomGenerator::global()->bounded(1, 0x7FFFFFFF);
        m_socketName = QStringLiteral("scrcpy_%1").arg(scid, 8, 16, QLatin1Char('0'));

        QMetaObject::invokeMethod(this, [this] { emit connecting(QStringLiteral("建立 adb 反向隧道")); },
                                  Qt::QueuedConnection);
        if (!adb.reverse(serial, m_socketName, m_localPort, &error)) {
            fail(QStringLiteral("adb reverse 失败：%1").arg(error));
            cleanupTunnel();
            return;
        }
        qInfo() << "adb reverse ok, port" << m_localPort << "socket" << m_socketName;

        // 3) 启动 scrcpy-server
        QMetaObject::invokeMethod(this, [this] { emit connecting(QStringLiteral("启动 scrcpy-server")); },
                                  Qt::QueuedConnection);
        QString command = QStringLiteral("CLASSPATH=%1 app_process / com.genymobile.scrcpy.Server %2")
                              .arg(QString::fromLatin1(kRemoteServerPath), m_config.serverVersion);
        command += QStringLiteral(" scid=%1").arg(scid, 8, 16, QLatin1Char('0'));
        command += QStringLiteral(" log_level=%1").arg(m_config.logLevel);
        command += QStringLiteral(" video=true audio=%1 control=true")
                       .arg(m_config.audio ? QStringLiteral("true") : QStringLiteral("false"));
        command += QStringLiteral(" video_codec=%1").arg(m_config.videoCodec);
        command += QStringLiteral(" audio_codec=opus");
        command += QStringLiteral(" video_bit_rate=%1").arg(m_config.videoBitRate);
        command += QStringLiteral(" audio_bit_rate=%1").arg(m_config.audioBitRate);
        command += QStringLiteral(" max_fps=%1").arg(m_config.maxFps);
        if (m_config.maxSize > 0) {
            command += QStringLiteral(" max_size=%1").arg(m_config.maxSize);
        }
        command += QStringLiteral(" clipboard_autosync=false");

        if (!adb.startServerDetached(serial, command, &m_serverPid, &error)) {
            fail(QStringLiteral("启动 scrcpy-server 失败：%1").arg(error));
            cleanupTunnel();
            return;
        }
        qInfo() << "scrcpy-server started, pid" << m_serverPid << "cmd" << command;

        // 4) 依次接受 video / audio / control 三路连接
        QMetaObject::invokeMethod(this, [this] { emit connecting(QStringLiteral("等待设备回连")); },
                                  Qt::QueuedConnection);

        const int expected = m_config.audio ? 3 : 2;
        QList<SOCKET> accepted;
        const qint64 deadline = monotonicNowMs() + kConnectTimeoutMs;
        while (accepted.size() < expected && !m_stopRequested.load() &&
               monotonicNowMs() < deadline) {
            const qint64 remaining = deadline - monotonicNowMs();
            if (remaining <= 0) {
                continue;
            }
            timeval tv{};
            tv.tv_sec = static_cast<long>(remaining / 1000);
            tv.tv_usec = static_cast<long>((remaining % 1000) * 1000);
            fd_set readSet;
            FD_ZERO(&readSet);
            FD_SET(listenSocket, &readSet);
            const int ready = ::select(0, &readSet, nullptr, nullptr, &tv);
            if (ready <= 0) {
                continue;
            }
            SOCKET client = ::accept(listenSocket, nullptr, nullptr);
            if (client == INVALID_SOCKET) {
                continue;
            }
            if (accepted.size() == expected - 1) {
                BOOL nodelay = TRUE;
                ::setsockopt(client, IPPROTO_TCP, TCP_NODELAY,
                             reinterpret_cast<const char *>(&nodelay), sizeof(nodelay));
            }
            accepted.append(client);
        }

        if (accepted.size() < expected) {
            for (SOCKET s : accepted) {
                ::closesocket(s);
            }
            if (m_stopRequested.load()) {
                cleanupTunnel();
                m_running.store(false);
                QMetaObject::invokeMethod(this, [this] { emit stopped(); }, Qt::QueuedConnection);
            } else {
                fail(QStringLiteral("等待设备回连超时，请确认手机已授权 USB 调试"));
                cleanupTunnel();
            }
            return;
        }

        ::closesocket(listenSocket);
        m_listenSocket = 0;

        // 5) 读取 64 字节设备名
        char nameBuffer[proto::kDeviceNameFieldLength] = {0};
        if (recvAll(accepted.first(), nameBuffer, proto::kDeviceNameFieldLength)) {
            nameBuffer[proto::kDeviceNameFieldLength - 1] = '\0';
            m_deviceName = QString::fromUtf8(nameBuffer).trimmed();
        }

        if (m_config.audio) {
            m_videoSocket = static_cast<quintptr>(accepted.at(0));
            m_audioSocket = static_cast<quintptr>(accepted.at(1));
            m_controlSocket = static_cast<quintptr>(accepted.at(2));
        } else {
            m_videoSocket = static_cast<quintptr>(accepted.at(0));
            m_audioSocket = 0;
            m_controlSocket = static_cast<quintptr>(accepted.at(1));
        }

        const quintptr videoSocket = m_videoSocket;
        const quintptr audioSocket = m_audioSocket;
        const quintptr controlSocket = m_controlSocket;
        const QString deviceName = m_deviceName;

        QMetaObject::invokeMethod(
            this,
            [this, deviceName, videoSocket, audioSocket, controlSocket] {
                emit connected(deviceName);
                emit channelsReady(videoSocket, audioSocket, controlSocket, m_localPort);
            },
            Qt::QueuedConnection);
    });
}

void ScrcpyClient::detachSockets() {
    m_videoSocket = 0;
    m_audioSocket = 0;
    m_controlSocket = 0;
}

void ScrcpyClient::postFailed(const QString &reason) {
    qWarning() << "scrcpy connection failed:" << reason;
    m_running.store(false);
    QMetaObject::invokeMethod(
        this, [this, reason] { emit failed(reason); }, Qt::QueuedConnection);
}

void ScrcpyClient::cleanupTunnel() {
    // 反向隧道与本会话 scrcpy-server 的清理都改为异步，避免断开/重连时阻塞 UI 线程。
    // 按 scid 精确匹配，避免误杀随后新建的会话进程。
    AdbProcess adb;
    adb.setAdbPath(m_config.adbPath.isEmpty() ? AdbProcess::resolveAdbPath() : m_config.adbPath);
    adb.removeReverseDetached(m_config.serial, m_socketName);
    const QString scid = m_socketName.section(QLatin1Char('_'), 1);
    if (!scid.isEmpty()) {
        adb.shellDetached(m_config.serial, QStringLiteral("pkill -f \"scid=%1\"").arg(scid));
    }
    closeSocket(m_listenSocket);
    closeSocket(m_videoSocket);
    closeSocket(m_audioSocket);
    closeSocket(m_controlSocket);
}

void ScrcpyClient::stop() {
    if (!m_running.exchange(false)) {
        if (m_thread.joinable()) {
            m_thread.join();
        }
        return;
    }
    m_stopRequested.store(true);
    closeSocket(m_listenSocket);
    closeSocket(m_videoSocket);
    closeSocket(m_audioSocket);
    closeSocket(m_controlSocket);
    if (m_thread.joinable()) {
        m_thread.join();
    }
    cleanupTunnel();
    emit stopped();
}

} // namespace ks
