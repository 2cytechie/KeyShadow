#pragma once

#include <QByteArray>
#include <QString>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

#include "core/engine/Engines.h"

namespace ks {

/// 240Hz 高精度触控注入线程：消费输入事件 → 引擎合成 → 控制通道发送。
/// 使用 QPC 计时 + timeBeginPeriod(1)，与 scrcpy 控制协议解耦。
class InjectionThread {
public:
    struct Stats {
        double latencyMs = -1.0;
        int pendingEvents = 0;
        quint64 messagesSent = 0;
        bool running = false;
    };

    InjectionThread();
    ~InjectionThread();

    InjectionThread(const InjectionThread &) = delete;
    InjectionThread &operator=(const InjectionThread &) = delete;

    bool start(quintptr controlSocket, int screenWidth, int screenHeight,
               QString *error = nullptr);
    void stop();
    bool isRunning() const { return m_running.load(); }

    void postInput(const InputEvent &event);
    void setProfile(const Profile &profile);
    void setScreenSize(int width, int height);
    void setIgnoredKey(int vk);
    void sendMessage(const QByteArray &message);
    void releaseAll();

    void beginCalibration();
    void endCalibration();
    qint64 takeCalibratedCounts();
    double takeLatencyMs();
    Stats stats() const;

private:
    void loop();
    bool sendAll(const char *data, int size);

    InputEngine m_engine;
    std::thread m_thread;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_stopRequested{false};
    std::atomic<qint64> m_releaseRequested{0};
    std::atomic<int> m_calibrationRequest{0};
    std::atomic<int> m_ignoredKey{0};

    mutable std::mutex m_mutex;
    QVector<InputEvent> m_events;
    QVector<QByteArray> m_outbox;
    Profile m_pendingProfile;
    bool m_profileDirty = false;
    int m_pendingScreenWidth = 0;
    int m_pendingScreenHeight = 0;
    bool m_calibrationDone = false;
    qint64 m_calibratedCounts = 0;

    quintptr m_socket = 0;
    int m_screenWidth = 1080;
    int m_screenHeight = 2400;
    std::atomic<quint64> m_messagesSent{0};
    std::atomic<double> m_latencyMs{-1.0};
};

} // namespace ks
