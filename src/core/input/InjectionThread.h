#pragma once

#include <QByteArray>
#include <QString>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

#include "core/engine/Engines.h"

namespace ks {

/// 触点调试跟踪：注入线程记录的每条发往设备的触点事件（临时排查「视角突然转向」）。
struct TouchTraceEntry {
    qint64 atMs = 0; ///< 相对注入线程启动的毫秒数
    int action = 0;  ///< proto::TouchAction
    int pointerId = 0;
    int x = 0;
    int y = 0;
};

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
    /// 窗口态直操：屏蔽 Raw Input 左键映射与拖动期视角注入。
    void setDirectTouchEnabled(bool enabled);
    void setTouchDragActive(bool active);
    /// 疾跑开关（Session 主线程按疾跑键状态转发）。
    void setSprint(bool active);
    /// 指针模式：鼠标位移不驱动视角（键盘/其它鼠标键仍映射）。
    void setLookEnabled(bool enabled) { m_lookEnabled.store(enabled); }
    /// 清空「已抬起但物理仍按住」的按键集合。
    void clearSuspendedKeys() { m_clearSuspended.store(true); }
    void sendMessage(const QByteArray &message);
    /// 直操输入消息：携带 GUI 线程采集时刻（QPC），用于窗口态输入延迟统计。
    void sendInputMessage(const QByteArray &message, qint64 qpcTimestamp);
    void releaseAll();

    void beginCalibration();
    void endCalibration();
    qint64 takeCalibratedCounts();
    double takeLatencyMs();
    Stats stats() const;

    /// 触点调试可视化：开启后记录出站触点事件并导出视角引擎快照。
    void setTouchTraceEnabled(bool enabled);
    QVector<TouchTraceEntry> takeTouchTrace();
    LookDebugState lookDebug() const;

private:
    void loop();
    bool sendAll(const char *data, int size);
    void recordTouch(const QByteArray &message);

    InputEngine m_engine;
    std::thread m_thread;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_stopRequested{false};
    std::atomic<qint64> m_releaseRequested{0};
    std::atomic<int> m_calibrationRequest{0};
    std::atomic<int> m_ignoredKey{0};
    std::atomic<bool> m_lookEnabled{true};
    std::atomic<bool> m_clearSuspended{false};

    mutable std::mutex m_mutex;
    QVector<InputEvent> m_events;
    /// 出站消息：qpc 为 0 表示非输入消息（不计延迟）。
    struct OutgoingMessage {
        QByteArray data;
        qint64 qpc = 0;
    };
    QVector<OutgoingMessage> m_outbox;
    Profile m_pendingProfile;
    bool m_profileDirty = false;
    int m_pendingScreenWidth = 0;
    int m_pendingScreenHeight = 0;
    bool m_calibrationDone = false;
    qint64 m_calibratedCounts = 0;
    bool m_directTouch = false;
    bool m_touchDrag = false;
    bool m_sprint = false;

    quintptr m_socket = 0;
    int m_screenWidth = 1080;
    int m_screenHeight = 2400;
    std::atomic<quint64> m_messagesSent{0};
    std::atomic<double> m_latencyMs{-1.0};

    // 触点调试可视化（临时排查视角突然转向）
    std::atomic<bool> m_traceEnabled{false};
    mutable std::mutex m_traceMutex;
    QVector<TouchTraceEntry> m_trace;
    LookDebugState m_lookDebug;
    qint64 m_traceStartQpc = 0;
};

} // namespace ks
