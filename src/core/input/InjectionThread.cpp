#include "core/input/InjectionThread.h"

#include "core/scrcpy/Protocol.h"
#include "core/video/VideoFrame.h"

#include <winsock2.h>

#include <qt_windows.h>
#include <mmsystem.h>

#include <algorithm>
#include <vector>

namespace ks {
namespace {

constexpr int kTickHz = 240;
constexpr double kTickSeconds = 1.0 / kTickHz;

/// 大端 32 位读取（触点封包坐标字段）。
int readBe32(const QByteArray &data, int offset) {
    return (static_cast<int>(static_cast<quint8>(data.at(offset))) << 24) |
           (static_cast<int>(static_cast<quint8>(data.at(offset + 1))) << 16) |
           (static_cast<int>(static_cast<quint8>(data.at(offset + 2))) << 8) |
           static_cast<int>(static_cast<quint8>(data.at(offset + 3)));
}

qint64 qpcNow() {
    LARGE_INTEGER value;
    QueryPerformanceCounter(&value);
    return value.QuadPart;
}

double qpcFrequency() {
    static double freq = [] {
        LARGE_INTEGER value;
        QueryPerformanceFrequency(&value);
        return static_cast<double>(value.QuadPart);
    }();
    return freq;
}

} // namespace

InjectionThread::InjectionThread() = default;

InjectionThread::~InjectionThread() { stop(); }

bool InjectionThread::start(quintptr controlSocket, int screenWidth, int screenHeight,
                            QString *error) {
    if (m_running.exchange(true)) {
        return true;
    }
    m_socket = controlSocket;
    m_screenWidth = std::max(1, screenWidth);
    m_screenHeight = std::max(1, screenHeight);
    m_traceStartQpc = qpcNow();
    m_engine.setScreenSize(m_screenWidth, m_screenHeight);
    m_stopRequested.store(false);
    m_thread = std::thread([this] { loop(); });
    if (error) {
        error->clear();
    }
    return true;
}

void InjectionThread::stop() {
    m_stopRequested.store(true);
    m_running.store(false);
    if (m_socket) {
        ::shutdown(static_cast<SOCKET>(m_socket), SD_BOTH);
    }
    if (m_thread.joinable()) {
        m_thread.join();
    }
    // 控制通道 socket 已由 ScrcpyClient::detachSockets 移交，停止后在此关闭避免句柄泄漏。
    if (m_socket) {
        ::closesocket(static_cast<SOCKET>(m_socket));
        m_socket = 0;
    }
}

void InjectionThread::postInput(const InputEvent &event) {
    if (!m_running.load()) {
        return;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    m_events.append(event);
}

void InjectionThread::setProfile(const Profile &profile) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_pendingProfile = profile;
    m_profileDirty = true;
}

void InjectionThread::setScreenSize(int width, int height) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_pendingScreenWidth = std::max(1, width);
    m_pendingScreenHeight = std::max(1, height);
}

void InjectionThread::setIgnoredKey(int vk) { m_ignoredKey.store(vk); }

void InjectionThread::setDirectTouchEnabled(bool enabled) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_directTouch = enabled;
}

void InjectionThread::setTouchDragActive(bool active) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_touchDrag = active;
}

void InjectionThread::setSprint(bool active) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_sprint = active;
}

void InjectionThread::sendMessage(const QByteArray &message) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_outbox.append({message, 0});
}

void InjectionThread::sendInputMessage(const QByteArray &message, qint64 qpcTimestamp) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_outbox.append({message, qpcTimestamp});
}

void InjectionThread::releaseAll() {
    m_releaseRequested.fetch_add(1);
}

void InjectionThread::beginCalibration() {
    // 清理上一次测量遗留的结果，避免新一次测量拿到旧计数。
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_calibrationDone = false;
        m_calibratedCounts = 0;
    }
    m_calibrationRequest.store(1);
}

void InjectionThread::endCalibration() { m_calibrationRequest.store(2); }

qint64 InjectionThread::takeCalibratedCounts() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_calibrationDone) {
        return -1;
    }
    m_calibrationDone = false;
    return m_calibratedCounts;
}

double InjectionThread::takeLatencyMs() { return m_latencyMs.exchange(-1.0); }

void InjectionThread::setTouchTraceEnabled(bool enabled) {
    m_traceEnabled.store(enabled);
    if (!enabled) {
        std::lock_guard<std::mutex> lock(m_traceMutex);
        m_trace.clear();
        m_lookDebug = LookDebugState{};
    }
}

QVector<TouchTraceEntry> InjectionThread::takeTouchTrace() {
    std::lock_guard<std::mutex> lock(m_traceMutex);
    QVector<TouchTraceEntry> trace;
    trace.swap(m_trace);
    return trace;
}

LookDebugState InjectionThread::lookDebug() const {
    std::lock_guard<std::mutex> lock(m_traceMutex);
    return m_lookDebug;
}

void InjectionThread::recordTouch(const QByteArray &message) {
    if (!m_traceEnabled.load(std::memory_order_relaxed)) {
        return;
    }
    // InjectTouch 封包：类型(1) 动作(1) pointer(8) x(4) y(4) 宽(2) 高(2) 压力(2) 按钮(8)
    if (message.size() < 18 ||
        static_cast<quint8>(message.at(0)) != static_cast<quint8>(proto::MsgType::InjectTouch)) {
        return;
    }
    TouchTraceEntry entry;
    entry.atMs = static_cast<qint64>((qpcNow() - m_traceStartQpc) * 1000.0 / qpcFrequency());
    entry.action = static_cast<int>(static_cast<quint8>(message.at(1)));
    quint64 pointer = 0;
    for (int i = 2; i < 10; ++i) {
        pointer = (pointer << 8) | static_cast<quint8>(message.at(i));
    }
    entry.pointerId = static_cast<int>(pointer);
    entry.x = readBe32(message, 10);
    entry.y = readBe32(message, 14);

    std::lock_guard<std::mutex> lock(m_traceMutex);
    // UI 每 33ms 拉取一次，240Hz 注入下 2048 条足够回溯数秒；超限丢最旧的一半。
    if (m_trace.size() >= 2048) {
        m_trace.remove(0, m_trace.size() - 1024);
    }
    m_trace.append(entry);
}

InjectionThread::Stats InjectionThread::stats() const {
    Stats stats;
    stats.latencyMs = m_latencyMs.load();
    stats.messagesSent = m_messagesSent.load();
    stats.running = m_running.load();
    std::lock_guard<std::mutex> lock(m_mutex);
    stats.pendingEvents = static_cast<int>(m_events.size());
    return stats;
}

bool InjectionThread::sendAll(const char *data, int size) {
    SOCKET socket = static_cast<SOCKET>(m_socket);
    int sent = 0;
    while (sent < size) {
        const int r = ::send(socket, data + sent, size - sent, 0);
        if (r <= 0) {
            return false;
        }
        sent += r;
    }
    m_messagesSent.fetch_add(1);
    return true;
}

void InjectionThread::loop() {
    timeBeginPeriod(1);

    QVector<InputEvent> events;
    QVector<OutgoingMessage> outbox;
    Profile profile;
    bool hasProfile = false;
    int screenWidth = m_screenWidth;
    int screenHeight = m_screenHeight;
    bool directTouch = false;
    bool touchDrag = false;
    bool sprint = false;
    bool lookEnabled = true;

    qint64 previous = qpcNow();
    double accumulator = 0.0;

    auto send = [this](const QByteArray &message) {
        if (!message.isEmpty()) {
            recordTouch(message);
            sendAll(message.constData(), message.size());
        }
    };

    while (!m_stopRequested.load()) {
        const qint64 now = qpcNow();
        const double elapsed = (now - previous) / qpcFrequency();
        previous = now;
        accumulator += elapsed;

        // 拉取主线程投递的数据
        bool screenSizeChanged = false;
        int nextScreenWidth = screenWidth;
        int nextScreenHeight = screenHeight;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_events.isEmpty()) {
                events = std::move(m_events);
                m_events.clear();
            }
            if (!m_outbox.isEmpty()) {
                outbox = std::move(m_outbox);
                m_outbox.clear();
            }
            if (m_profileDirty) {
                profile = m_pendingProfile;
                hasProfile = true;
                m_profileDirty = false;
            }
            if (m_pendingScreenWidth > 0 && m_pendingScreenHeight > 0) {
                nextScreenWidth = m_pendingScreenWidth;
                nextScreenHeight = m_pendingScreenHeight;
                m_pendingScreenWidth = 0;
                m_pendingScreenHeight = 0;
                screenSizeChanged = nextScreenWidth != screenWidth || nextScreenHeight != screenHeight;
            }
            directTouch = m_directTouch;
            touchDrag = m_touchDrag;
            sprint = m_sprint;
        }

        m_engine.setDirectTouchEnabled(directTouch);
        m_engine.setTouchDragActive(touchDrag);
        m_engine.setSprint(sprint);
        const bool nextLookEnabled = m_lookEnabled.load();
        if (lookEnabled && !nextLookEnabled) {
            m_engine.look().release(send);
        }
        if (screenSizeChanged) {
            m_engine.releaseAll(send);
            screenWidth = nextScreenWidth;
            screenHeight = nextScreenHeight;
            m_engine.setScreenSize(screenWidth, screenHeight);
        }
        lookEnabled = nextLookEnabled;
        m_engine.setLookEnabled(nextLookEnabled);
        if (m_clearSuspended.exchange(false)) {
            m_engine.clearSuspendedKeys();
        }

        if (hasProfile) {
            m_engine.setProfile(profile);
            hasProfile = false;
        }

        const int ignoredKey = m_ignoredKey.exchange(0);
        if (ignoredKey > 0) {
            m_engine.setIgnoredKey(ignoredKey);
        }

        double batchLatencyMs = -1.0;
        for (const OutgoingMessage &outgoing : outbox) {
            send(outgoing.data);
            if (outgoing.qpc > 0) {
                // 窗口态直操：从 GUI 采集到写入控制 socket 的耗时，与映射态口径一致。
                // 一批消息取最旧一条（最大）延迟，避免突发鼠标拖动时读数偏乐观。
                batchLatencyMs = std::max(
                    batchLatencyMs, (qpcNowTicks() - outgoing.qpc) * 1000.0 / qpcFrequencyHz());
            }
        }
        outbox.clear();
        if (batchLatencyMs >= 0.0) {
            m_latencyMs.store(batchLatencyMs);
        }

        for (const InputEvent &event : events) {
            m_engine.handleInput(event);
        }
        events.clear();

        if (m_releaseRequested.exchange(0) > 0) {
            m_engine.releaseAll(send);
        }

        const int calibrationRequest = m_calibrationRequest.exchange(0);
        if (calibrationRequest == 1) {
            m_engine.calibrator().begin();
        } else if (calibrationRequest == 2) {
            const qint64 counts = m_engine.calibrator().measuredCounts();
            m_engine.calibrator().cancel();
            std::lock_guard<std::mutex> lock(m_mutex);
            m_calibratedCounts = counts;
            m_calibrationDone = true;
        }

        const int dtMs = std::max(1, static_cast<int>(std::lround(accumulator * 1000.0)));
        if (accumulator >= kTickSeconds) {
            m_engine.tick(dtMs, send);
            accumulator = 0.0;

            if (m_traceEnabled.load(std::memory_order_relaxed)) {
                const LookDebugState lookState = m_engine.look().debugState();
                std::lock_guard<std::mutex> lock(m_traceMutex);
                m_lookDebug = lookState;
            }

            const double latency = m_engine.takeLatencyMs();
            if (latency >= 0.0) {
                m_latencyMs.store(latency);
            }
        }

        // 高精度等待到下一拍
        const double remaining = kTickSeconds - ((qpcNow() - previous) / qpcFrequency());
        if (remaining > 0.0015) {
            const DWORD sleepMs = static_cast<DWORD>(std::max(0.0, (remaining - 0.001) * 1000.0));
            Sleep(sleepMs);
        } else {
            Sleep(0);
        }
    }

    timeEndPeriod(1);
    m_running.store(false);
}

} // namespace ks
