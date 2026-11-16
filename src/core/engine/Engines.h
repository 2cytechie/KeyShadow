#pragma once

#include <QByteArray>
#include <QPointF>
#include <QSet>
#include <QString>
#include <QVector>
#include <functional>
#include <random>

namespace ks {

/// 判定事件虚拟键码是否匹配配置的呼出键：通用修饰键（Shift/Ctrl/Alt）匹配左右变体。
bool vkMatchesSummon(int eventVk, int summonVk);

/// Raw Input 鼠标按钮序号 → Windows VK（0 左键 1 右键 2 中键 3 侧键1 4 侧键2）。
int mouseButtonVk(int button);

/// 自定义虚拟键：鼠标滚轮上/下（Windows VK 表中无对应项，仅用于键位绑定）。
constexpr int kVkWheelUp = 0x0A;
constexpr int kVkWheelDown = 0x0B;

/// 高精度计时（QPC 计数），用于给输入事件打「采集时刻」戳并测量端到端延迟。
/// RawInputFilter 与注入线程共用同一套实现，保证时间戳可直接相减。
qint64 qpcNowTicks();
double qpcFrequencyHz();

/// Clamp a normalized region center so an ellipse with the given radius stays on screen.
QPointF clampNormalizedRegionCenter(const QPointF &center, double radius);

// ---------------------------------------------------------------------------
// 输入事件（由 RawInputFilter 产生，主线程投递到注入线程）
// ---------------------------------------------------------------------------
enum class InputEventType { MouseMove, MouseButton, Wheel, Key };

struct InputEvent {
    InputEventType type = InputEventType::Key;
    qint64 timestampNs = 0; ///< 采集时刻的 QPC 计数（非纳秒，避免长时间运行后溢出）
    int dx = 0;      ///< MouseMove：相对位移；Wheel：滚轮格数
    int dy = 0;
    int button = 0;  ///< MouseButton：0 左键 1 右键 2 中键
    bool down = false;
    int vk = 0;      ///< Key：Windows 虚拟键码
};

// ---------------------------------------------------------------------------
// 键位方案
// ---------------------------------------------------------------------------
/// 键位直通语义：按下 = 手指按下，抬起 = 手指抬起；多键同按 = 多指（每个绑定独立 pointerId）。
struct KeyBinding {
    int vk = 0;                        ///< 触发键（Windows VK）
    QPointF center{0.5, 0.5};          ///< 归一化坐标（0..1）
    double radius = 0.05;              ///< 归一化半径（按钮尺寸）
};

struct Profile {
    QString name = QStringLiteral("默认方案");

    QPointF joystickCenter{0.140, 0.760};
    double joystickRadius = 0.085;
    double joystickSprintRadius = 0.13; ///< 疾跑大圆半径（≥ joystickRadius）
    double joystickDeadzone = 0.12;
    double joystickCurve = 1.0; ///< 1.0 线性，>1 更细腻

    QPointF lookCenter{0.720, 0.460};
    double lookRadius = 0.230;
    double lookSensitivity = 1.0;
    double lookCountsPer180 = 3000.0; ///< 校准结果：180° 转身所需鼠标计数

    /// 运行期注入值：Session 按全局设置与授权状态决定实际是否启用。
    bool humanizerEnabled = false;
    bool proHumanizer = false;

    QVector<KeyBinding> bindings;

    // 画面增强（Pro）
    double brightness = 0.0;  ///< -100..100
    double contrast = 0.0;    ///< -100..100
    double saturation = 0.0;  ///< -100..100
    double shadow = 0.0;      ///< 0..100，暗部提亮
    double sharpness = 0.0;   ///< 0..100

    // 视频参数
    int videoBitRate = 8000000;
    int maxFps = 60;
    int videoShortEdge = 0; ///< 0 = 原生分辨率；正数 = 短边像素数
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

    /// tap 按下时长（35-160ms）。
    int tapDurationMs();
    /// MOVE 插值段数（按距离分段）。
    int moveSteps(int distancePx) const;
    /// 单段抖动（±2px）。
    int jitter();
    /// DOWN→MOVE 首帧延迟。
    int firstMoveDelayMs();
    /// 区域中心抖动（像素）：±6% 半径，每轴独立均匀；关闭时返回 0。
    int regionJitter(double radiusPx);

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
    /// 注入拟人化抖动：每次激活时对区域中心做随机偏移。
    void setHumanizer(HumanizerEngine *humanizer) { m_humanizer = humanizer; }

    /// 疾跑：开启后轮盘触点可滑到大圆（sprintRadius）边缘。
    void setSprint(bool active) { m_sprint = active; }
    void setSprintRadius(double radiusNorm);

    /// 返回 true 表示产生了事件（写入 send）。
    bool tick(int dtMs, const std::function<void(const QByteArray &)> &send);

    void configure(const QPointF &centerNorm, double radiusNorm, double deadzone, double curve,
                   int screenWidth, int screenHeight);

private:
    QPointF m_center{0.14, 0.76};
    double m_radius = 0.085;
    double m_sprintRadius = 0.13;
    double m_deadzone = 0.12;
    double m_curve = 1.0;
    int m_screenWidth = 1920;
    int m_screenHeight = 1080;
    HumanizerEngine *m_humanizer = nullptr;
    QPointF m_basePixel; ///< 本次激活的抖动后中心

    bool m_up = false;
    bool m_down = false;
    bool m_left = false;
    bool m_right = false;
    bool m_active = false;
    bool m_sprint = false;
    bool m_releasing = false;
    QPointF m_currentPixel; ///< 当前触点像素坐标
    double m_releaseElapsedMs = 0.0;
    QPointF m_releaseFrom;
};

// ---------------------------------------------------------------------------
// 视角（lift-recenter 无限转向）
// ---------------------------------------------------------------------------
/// 视角引擎调试快照：仅用于画布可视化排查「视角突然转向」，不参与注入逻辑。
struct LookDebugState {
    bool active = false;
    bool recentering = false;
    int recenterDelayMs = 0;
    double baseX = 0.0;   ///< 本轮抖动后的触发区中心（设备像素）
    double baseY = 0.0;
    double fingerX = 0.0; ///< 当前模拟手指位置（设备像素）
    double fingerY = 0.0;
    double radiusX = 0.0; ///< 触发区椭圆半径（设备像素）
    double radiusY = 0.0;
    double pendingX = 0.0; ///< 回中后待消费的位移（设备像素）
    double pendingY = 0.0;
    double accumX = 0.0;  ///< 尚未消费的鼠标累计位移（计数）
    double accumY = 0.0;
    double countsPer180 = 0.0;
};

class LookEngine {
public:
    void addDelta(int dx, int dy, qint64 timestampNs);
    void reset();
    /// 若视角触点仍按下则先发送抬触，再清除状态。
    void release(const std::function<void(const QByteArray &)> &send);
    bool isActive() const { return m_active; }
    LookDebugState debugState() const;

    bool tick(int dtMs, const std::function<void(const QByteArray &)> &send);

    void configure(const QPointF &centerNorm, double radiusNorm, double sensitivity,
                   double countsPer180, int screenWidth, int screenHeight);
    /// 注入拟人化抖动：每次按下与回中时对区域中心做随机偏移。
    void setHumanizer(HumanizerEngine *humanizer) { m_humanizer = humanizer; }

private:
    void emitTouch(const std::function<void(const QByteArray &)> &send, int action, int x, int y);

    QPointF m_center{0.72, 0.46};
    double m_radius = 0.23;
    double m_sensitivity = 1.0;
    double m_countsPer180 = 3000.0;
    int m_screenWidth = 1920;
    int m_screenHeight = 1080;
    HumanizerEngine *m_humanizer = nullptr;
    QPointF m_basePixel; ///< 本轮按下/回中后的抖动中心

    double m_accumX = 0.0;
    double m_accumY = 0.0;
    QPointF m_fingerPixel;
    bool m_active = false;
    qint64 m_lastEventNs = 0;

    // lift-recenter 状态机：到边界先 Up，延迟数毫秒再回中 Down（Down 当帧不移动），
    // 给手机端明确的「抬起 → 重新按下」序列，避免回中跳变被解释为一次反向拖动。
    bool m_recentering = false; ///< 已发 Up，等待回中 Down
    int m_recenterDelayMs = 0;  ///< 距回中 Down 的倒计时
    double m_pendingX = 0.0;    ///< 回中后待消费的像素位移（边界处未走完的部分）
    double m_pendingY = 0.0;
};

// ---------------------------------------------------------------------------
// 键位映射（直通：按下=手指按下、抬起=手指抬起）
// ---------------------------------------------------------------------------
struct ActiveAction {
    int pointerId = 0;
    int vk = 0;
    QPointF fromPixel;
};

/// 滚轮按下的临时触点：滚轮是离散事件，按下后延时抬起，模拟一次点按。
struct WheelPress {
    int pointerId = 0;
    QPointF pixel;
    int remainingMs = 0;
};

class KeymapEngine {
public:
    void configure(const QVector<KeyBinding> &bindings, int screenWidth, int screenHeight);
    void setHumanizer(HumanizerEngine *humanizer) { m_humanizer = humanizer; }

    bool handleKey(int vk, bool down, const std::function<void(const QByteArray &)> &send);
    /// 滚轮只触发已绑定的上/下滚轮键位；未绑定时不注入任何滚动（避免与视角拖动冲突）。
    bool handleWheel(int delta, const std::function<void(const QByteArray &)> &send);

    bool tick(int dtMs, const std::function<void(const QByteArray &)> &send);
    void releaseAll(const std::function<void(const QByteArray &)> &send);
    /// 清空「已抬起但物理仍按住」集合（重新开始捕获时调用）。
    void clearSuspended() { m_suspendedVks.clear(); }
    bool isBound(int vk) const;

private:
    int allocatePointer(int vk);
    QPointF toPixel(const QPointF &norm) const;

    QVector<KeyBinding> m_bindings;
    QVector<ActiveAction> m_actions;
    QVector<WheelPress> m_wheelPresses;
    QSet<int> m_suspendedVks; ///< releaseAll 抬起后仍物理按住的键：忽略其自动重复
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

    /// 呼出键不参与键位映射（避免同时触发游戏动作）。
    void setIgnoredKey(int vk) { m_ignoredKey = vk; }
    int ignoredKey() const { return m_ignoredKey; }

    /// 窗口态直操：左键由画布 Qt 事件注入触控，Raw Input 不再映射鼠标左键。
    void setDirectTouchEnabled(bool enabled) { m_directTouchEnabled = enabled; }
    /// 画布触控拖动进行中：丢弃鼠标位移，避免滑动手机 UI 时视角同时转动。
    void setTouchDragActive(bool active) { m_touchDragActive = active; }
    /// 指针模式：系统光标用于点击手机 UI，鼠标位移不再驱动视角（键盘/其它鼠标键仍映射）。
    void setLookEnabled(bool enabled);
    /// 清空「已抬起但物理仍按住」的按键集合。
    void clearSuspendedKeys();

    SensCalibrator &calibrator() { return m_calibrator; }
    JoystickEngine &joystick() { return m_joystick; }
    LookEngine &look() { return m_look; }
    KeymapEngine &keymap() { return m_keymap; }
    HumanizerEngine &humanizer() { return m_humanizer; }

    /// 疾跑开关：由 Session 主线程按疾跑键状态转发（呼出键/疾跑键不进入键位映射）。
    void setSprint(bool active) { m_joystick.setSprint(active); }

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
    bool m_directTouchEnabled = false;
    bool m_touchDragActive = false;
    bool m_lookEnabled = true;

    // WASD 摇杆按键状态（WASD 固定用于走位，不依赖键位方案）
    bool m_joyUp = false;
    bool m_joyDown = false;
    bool m_joyLeft = false;
    bool m_joyRight = false;
    QSet<int> m_suspendedWasd; ///< releaseAll 抬起后仍物理按住的 WASD
};

} // namespace ks
