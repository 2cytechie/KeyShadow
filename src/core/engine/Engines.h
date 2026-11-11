#pragma once

#include <QByteArray>
#include <QPointF>
#include <QString>
#include <QVector>
#include <functional>
#include <random>

namespace ks {

/// 判定事件虚拟键码是否匹配配置的呼出键：通用修饰键（Shift/Ctrl/Alt）匹配左右变体。
bool vkMatchesSummon(int eventVk, int summonVk);

// ---------------------------------------------------------------------------
// 输入事件（由 RawInputFilter 产生，主线程投递到注入线程）
// ---------------------------------------------------------------------------
enum class InputEventType { MouseMove, MouseButton, Wheel, Key };

struct InputEvent {
    InputEventType type = InputEventType::Key;
    qint64 timestampNs = 0;
    int dx = 0;      ///< MouseMove：相对位移；Wheel：滚轮格数
    int dy = 0;
    int button = 0;  ///< MouseButton：0 左键 1 右键 2 中键
    bool down = false;
    int vk = 0;      ///< Key：Windows 虚拟键码
};

// ---------------------------------------------------------------------------
// 键位方案
// ---------------------------------------------------------------------------
enum class BindingAction { Tap, Hold, Slide, Double };

struct KeyBinding {
    int vk = 0;                        ///< 触发键（Windows VK）
    BindingAction action = BindingAction::Tap;
    QPointF center{0.5, 0.5};          ///< 归一化坐标（0..1）
    double radius = 0.05;              ///< 归一化半径（按钮尺寸）
    QPointF slideTo{0.5, 0.5};         ///< Slide 动作终点（归一化）
};

struct Profile {
    QString name = QStringLiteral("默认方案");

    QPointF joystickCenter{0.140, 0.760};
    double joystickRadius = 0.085;
    double joystickDeadzone = 0.12;
    double joystickCurve = 1.0; ///< 1.0 线性，>1 更细腻

    QPointF lookCenter{0.720, 0.460};
    double lookRadius = 0.230;
    double lookSensitivity = 1.0;
    double lookCountsPer180 = 3000.0; ///< 校准结果：180° 转身所需鼠标计数

    bool humanizerEnabled = true;
    bool proHumanizer = false;

    QVector<KeyBinding> bindings;

    // 画面增强（Pro）
    double brightness = 0.0;
    double contrast = 0.0;
    double saturation = 0.0;
    double gamma = 1.0;
    double highlight = 0.0;
    double casSharpness = 0.0;

    // 视频参数
    int videoBitRate = 8000000;
    int maxFps = 60;
    int maxSize = 0;
    bool audioEnabled = true;
    bool screenOff = false;
};

// ---------------------------------------------------------------------------
// 拟人化
// ---------------------------------------------------------------------------
class HumanizerEngine {
public:
    void setEnabled(bool enabled) { m_enabled = enabled; }
    void setEnhanced(bool enhanced) { m_enhanced = enhanced; }
    bool enabled() const { return m_enabled; }

    /// tap 按下时长（40-120ms；增强档 35-160ms）。
    int tapDurationMs();
    /// MOVE 插值段数（短滑 3 段，增强档更多）。
    int moveSteps(int distancePx) const;
    /// 单段抖动（±1px，增强档 ±1.5px）。
    int jitter();
    /// DOWN→MOVE 首帧延迟。
    int firstMoveDelayMs();

private:
    bool m_enabled = true;
    bool m_enhanced = false;
    std::mt19937 m_rng{std::random_device{}()};
};

// ---------------------------------------------------------------------------
// 摇杆
// ---------------------------------------------------------------------------
class JoystickEngine {
public:
    void setKeys(bool up, bool down, bool left, bool right);
    bool isActive() const { return m_active; }
    void reset();
    /// 若触点仍按下则先发送抬触，再清除状态（模式切换/断线时使用）。
    void release(const std::function<void(const QByteArray &)> &send);

    /// 返回 true 表示产生了事件（写入 send）。
    bool tick(int dtMs, const std::function<void(const QByteArray &)> &send);

    void configure(const QPointF &centerNorm, double radiusNorm, double deadzone, double curve,
                   int screenWidth, int screenHeight);

private:
    QPointF m_center{0.14, 0.76};
    double m_radius = 0.085;
    double m_deadzone = 0.12;
    double m_curve = 1.0;
    int m_screenWidth = 1920;
    int m_screenHeight = 1080;

    bool m_up = false;
    bool m_down = false;
    bool m_left = false;
    bool m_right = false;
    bool m_active = false;
    bool m_releasing = false;
    QPointF m_currentPixel; ///< 当前触点像素坐标
    double m_releaseElapsedMs = 0.0;
    QPointF m_releaseFrom;
};

// ---------------------------------------------------------------------------
// 视角（lift-recenter 无限转向）
// ---------------------------------------------------------------------------
class LookEngine {
public:
    void addDelta(int dx, int dy, qint64 timestampNs);
    void reset();
    /// 若视角触点仍按下则先发送抬触，再清除状态。
    void release(const std::function<void(const QByteArray &)> &send);
    bool isActive() const { return m_active; }

    bool tick(int dtMs, const std::function<void(const QByteArray &)> &send);

    void configure(const QPointF &centerNorm, double radiusNorm, double sensitivity,
                   double countsPer180, int screenWidth, int screenHeight);

private:
    void emitTouch(const std::function<void(const QByteArray &)> &send, int action, int x, int y);

    QPointF m_center{0.72, 0.46};
    double m_radius = 0.23;
    double m_sensitivity = 1.0;
    double m_countsPer180 = 3000.0;
    int m_screenWidth = 1920;
    int m_screenHeight = 1080;

    double m_accumX = 0.0;
    double m_accumY = 0.0;
    QPointF m_fingerPixel;
    bool m_active = false;
    qint64 m_lastEventNs = 0;
};

// ---------------------------------------------------------------------------
// 键位映射
// ---------------------------------------------------------------------------
struct ActiveAction {
    int pointerId = 0;
    int vk = 0;
    BindingAction type = BindingAction::Tap;
    QPointF fromPixel;
    QPointF toPixel;
    qint64 elapsedMs = 0;
    qint64 durationMs = 0;
    int moveStep = 0;
    int totalSteps = 0;
    int nextStepAtMs = 0;
    bool holding = false;
    bool waitingRelease = false;
    bool downSent = false;
};

class KeymapEngine {
public:
    void configure(const QVector<KeyBinding> &bindings, int screenWidth, int screenHeight);
    void setHumanizer(HumanizerEngine *humanizer) { m_humanizer = humanizer; }

    bool handleKey(int vk, bool down, const std::function<void(const QByteArray &)> &send);
    bool handleWheel(int delta, const std::function<void(const QByteArray &)> &send);

    bool tick(int dtMs, const std::function<void(const QByteArray &)> &send);
    void releaseAll(const std::function<void(const QByteArray &)> &send);
    bool isBound(int vk) const;

private:
    int allocatePointer(int vk);
    void releasePointer(int vk, const std::function<void(const QByteArray &)> &send);
    QPointF toPixel(const QPointF &norm) const;

    QVector<KeyBinding> m_bindings;
    QVector<ActiveAction> m_actions;
    int m_screenWidth = 1920;
    int m_screenHeight = 1080;
    int m_nextPointer = 2;
    HumanizerEngine *m_humanizer = nullptr;
    std::mt19937 m_rng{std::random_device{}()};
};

// ---------------------------------------------------------------------------
// 灵敏度校准
// ---------------------------------------------------------------------------
class SensCalibrator {
public:
    void begin();
    void addCounts(int dx, int dy);
    void cancel();
    bool isActive() const { return m_active; }
    qint64 measuredCounts() const { return m_counts; }
    /// 由实测计数计算建议的 countsPer180（把当前手感的换算系数写回方案）。
    double suggestedCountsPer180(double previous) const;

private:
    bool m_active = false;
    qint64 m_counts = 0;
};

// ---------------------------------------------------------------------------
// 组合引擎
// ---------------------------------------------------------------------------
class InputEngine {
public:
    InputEngine();

    void setProfile(const Profile &profile);
    const Profile &profile() const { return m_profile; }

    void setScreenSize(int width, int height);
    int screenWidth() const { return m_screenWidth; }
    int screenHeight() const { return m_screenHeight; }

    void handleInput(const InputEvent &event);
    void tick(int dtMs, const std::function<void(const QByteArray &)> &send);
    void releaseAll(const std::function<void(const QByteArray &)> &send);

    /// 呼出键不参与键位映射（长按呼出时避免同时触发游戏动作）。
    void setIgnoredKey(int vk) { m_ignoredKey = vk; }
    int ignoredKey() const { return m_ignoredKey; }

    SensCalibrator &calibrator() { return m_calibrator; }
    JoystickEngine &joystick() { return m_joystick; }
    LookEngine &look() { return m_look; }
    KeymapEngine &keymap() { return m_keymap; }
    HumanizerEngine &humanizer() { return m_humanizer; }

    /// 消费一次「最近输入到发送」的延迟（毫秒），无样本返回 -1。
    double takeLatencyMs();

private:
    void applyJoystickConfig();

    Profile m_profile;
    int m_screenWidth = 1920;
    int m_screenHeight = 1080;
    HumanizerEngine m_humanizer;
    JoystickEngine m_joystick;
    LookEngine m_look;
    KeymapEngine m_keymap;
    SensCalibrator m_calibrator;
    QVector<InputEvent> m_pendingEvents;
    qint64 m_lastInputQpc = 0;
    double m_latencyMs = -1.0;
    int m_ignoredKey = 0;

    // WASD 摇杆按键状态（WASD 固定用于走位，不依赖键位方案）
    bool m_joyUp = false;
    bool m_joyDown = false;
    bool m_joyLeft = false;
    bool m_joyRight = false;
};

} // namespace ks
