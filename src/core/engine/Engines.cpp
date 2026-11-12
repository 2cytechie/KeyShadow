#include "core/engine/Engines.h"

#include "core/scrcpy/Protocol.h"

#include <qt_windows.h>

#include <QPoint>
#include <algorithm>
#include <cmath>

namespace ks {
namespace {

constexpr quint64 kJoystickPointer = 0;
constexpr quint64 kLookPointer = 1;
/// 键位/滚轮触点分配上限：10 保留给窗口态直操（ConsolePage::sendPointerTouch），
/// 避免键位触点与直操触点同 id 冲突。
constexpr quint64 kMaxPointer = 9;

/// lift-recenter 回中间隔：Up 后至少隔这么久才重新 Down，让手机端把两次按下区分开。
constexpr int kRecenterGapMs = 10;

/// 区域中心抖动：每次按下/回中时在 ±(半径比例) 内随机偏移，并钳制在屏幕内。
QPointF jitteredCenter(HumanizerEngine *humanizer, const QPointF &centerPx, double radiusPx,
                       int screenWidth, int screenHeight) {
    if (!humanizer) {
        return centerPx;
    }
    const QPointF shifted(centerPx.x() + humanizer->regionJitter(radiusPx),
                          centerPx.y() + humanizer->regionJitter(radiusPx));
    return QPointF(std::clamp(shifted.x(), 0.0, std::max(0, screenWidth - 1) * 1.0),
                   std::clamp(shifted.y(), 0.0, std::max(0, screenHeight - 1) * 1.0));
}

double easeOutCubic(double t) {
    const double inv = 1.0 - t;
    return 1.0 - inv * inv * inv;
}

QPointF toPixel(const QPointF &norm, int width, int height) {
    return QPointF(norm.x() * width, norm.y() * height);
}

QByteArray touchMessage(quint64 pointer, proto::TouchAction action, const QPointF &pixel, int width,
                        int height, float pressure = 1.0f) {
    return proto::touch(action, pointer, static_cast<int>(std::lround(pixel.x())),
                        static_cast<int>(std::lround(pixel.y())), width, height, pressure);
}

} // namespace

qint64 qpcNowTicks() {
    LARGE_INTEGER value;
    QueryPerformanceCounter(&value);
    return value.QuadPart;
}

double qpcFrequencyHz() {
    static const double freq = [] {
        LARGE_INTEGER value;
        QueryPerformanceFrequency(&value);
        return static_cast<double>(value.QuadPart);
    }();
    return freq;
}

bool vkMatchesSummon(int eventVk, int summonVk) {
    switch (summonVk) {
    case VK_SHIFT:
        return eventVk == VK_SHIFT || eventVk == VK_LSHIFT || eventVk == VK_RSHIFT;
    case VK_CONTROL:
        return eventVk == VK_CONTROL || eventVk == VK_LCONTROL || eventVk == VK_RCONTROL;
    case VK_MENU:
        return eventVk == VK_MENU || eventVk == VK_LMENU || eventVk == VK_RMENU;
    default:
        return eventVk == summonVk;
    }
}

int mouseButtonVk(int button) {
    switch (button) {
    case 0: return VK_LBUTTON;
    case 1: return VK_RBUTTON;
    case 2: return VK_MBUTTON;
    case 3: return VK_XBUTTON1;
    case 4: return VK_XBUTTON2;
    default: return 0;
    }
}

// ---------------------------------------------------------------------------
// HumanizerEngine
// ---------------------------------------------------------------------------
int HumanizerEngine::tapDurationMs() {
    if (!m_enabled) {
        return 50;
    }
    if (m_enhanced) {
        std::uniform_int_distribution<int> dist(35, 160);
        return dist(m_rng);
    }
    std::uniform_int_distribution<int> dist(40, 120);
    return dist(m_rng);
}

int HumanizerEngine::moveSteps(int distancePx) const {
    if (!m_enabled) {
        return 1;
    }
    if (m_enhanced) {
        return std::clamp(distancePx / 12, 3, 12);
    }
    return distancePx > 24 ? 3 : 1;
}

int HumanizerEngine::jitter() {
    if (!m_enabled) {
        return 0;
    }
    const int range = m_enhanced ? 2 : 1;
    std::uniform_int_distribution<int> dist(-range, range);
    return dist(m_rng);
}

int HumanizerEngine::regionJitter(double radiusPx) {
    if (!m_enabled) {
        return 0;
    }
    // 付费版固定 6% 半径；基础版不会启用拟人化。
    const int range = std::max(1, static_cast<int>(std::lround(radiusPx * 0.06)));
    std::uniform_int_distribution<int> dist(-range, range);
    return dist(m_rng);
}

int HumanizerEngine::firstMoveDelayMs() {
    if (!m_enabled) {
        return 0;
    }
    std::uniform_int_distribution<int> dist(0, m_enhanced ? 8 : 3);
    return dist(m_rng);
}

// ---------------------------------------------------------------------------
// JoystickEngine
// ---------------------------------------------------------------------------
void JoystickEngine::configure(const QPointF &centerNorm, double radiusNorm, double deadzone,
                               double curve, int screenWidth, int screenHeight) {
    m_center = centerNorm;
    m_radius = std::clamp(radiusNorm, 0.02, 0.30);
    m_deadzone = std::clamp(deadzone, 0.0, 0.45);
    m_curve = std::clamp(curve, 0.4, 3.0);
    m_screenWidth = std::max(1, screenWidth);
    m_screenHeight = std::max(1, screenHeight);
}

void JoystickEngine::setKeys(bool up, bool down, bool left, bool right) {
    m_up = up;
    m_down = down;
    m_left = left;
    m_right = right;
}

void JoystickEngine::reset() {
    m_active = false;
    m_releasing = false;
    m_sprint = false;
    m_up = m_down = m_left = m_right = false;
}

void JoystickEngine::setSprintRadius(double radiusNorm) {
    m_sprintRadius = std::clamp(radiusNorm, m_radius, 0.30);
}

void JoystickEngine::release(const std::function<void(const QByteArray &)> &send) {
    if (m_active) {
        send(touchMessage(kJoystickPointer, proto::TouchAction::Up, m_currentPixel, m_screenWidth,
                          m_screenHeight));
    }
    reset();
}

bool JoystickEngine::tick(int dtMs, const std::function<void(const QByteArray &)> &send) {
    double fx = (m_right ? 1.0 : 0.0) - (m_left ? 1.0 : 0.0);
    double fy = (m_down ? 1.0 : 0.0) - (m_up ? 1.0 : 0.0);
    const double magnitude = std::sqrt(fx * fx + fy * fy);

    const QPointF centerPx = toPixel(m_center, m_screenWidth, m_screenHeight);
    // 疾跑开启时触点可滑到大圆边缘；同一力度映射更远。
    const double radiusPx = (m_sprint ? m_sprintRadius : m_radius) * std::min(m_screenWidth,
                                                                             m_screenHeight);

    if (magnitude > 0.0) {
        fx /= magnitude;
        fy /= magnitude;

        double t = magnitude;
        if (t <= m_deadzone) {
            t = 0.0;
        } else {
            t = (t - m_deadzone) / (1.0 - m_deadzone);
        }
        t = std::pow(std::clamp(t, 0.0, 1.0), m_curve);

        if (!m_active) {
            m_active = true;
            m_releasing = false;
            // 每次激活对区域中心做拟人化抖动，避免触点长期固定在同一像素。
            m_basePixel = jitteredCenter(m_humanizer, centerPx, radiusPx, m_screenWidth,
                                         m_screenHeight);
            m_currentPixel = m_basePixel;
            send(touchMessage(kJoystickPointer, proto::TouchAction::Down, m_basePixel,
                              m_screenWidth, m_screenHeight));
        }

        const QPointF target = m_basePixel + QPointF(fx * radiusPx * t, fy * radiusPx * t);
        const QPointF delta = target - m_currentPixel;
        if (std::abs(delta.x()) >= 0.5 || std::abs(delta.y()) >= 0.5) {
            m_currentPixel = target;
            send(touchMessage(kJoystickPointer, proto::TouchAction::Move, target, m_screenWidth,
                              m_screenHeight));
        }
        return true;
    }

    if (m_active) {
        if (!m_releasing) {
            m_releasing = true;
            m_releaseFrom = m_currentPixel;
            m_releaseElapsedMs = 0.0;
        }
        m_releaseElapsedMs += dtMs;
        const double k = std::clamp(m_releaseElapsedMs / 80.0, 0.0, 1.0);
        const double eased = easeOutCubic(k);
        const QPointF pos = m_releaseFrom + (m_basePixel - m_releaseFrom) * eased;
        send(touchMessage(kJoystickPointer, proto::TouchAction::Move, pos, m_screenWidth,
                          m_screenHeight));
        m_currentPixel = pos;
        if (k >= 1.0) {
            send(touchMessage(kJoystickPointer, proto::TouchAction::Up, m_basePixel,
                              m_screenWidth, m_screenHeight));
            m_active = false;
            m_releasing = false;
        }
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// LookEngine
// ---------------------------------------------------------------------------
void LookEngine::configure(const QPointF &centerNorm, double radiusNorm, double sensitivity,
                           double countsPer180, int screenWidth, int screenHeight) {
    m_center = centerNorm;
    m_radius = std::clamp(radiusNorm, 0.05, 0.45);
    m_sensitivity = std::clamp(sensitivity, 0.05, 10.0);
    m_countsPer180 = std::max(50.0, countsPer180);
    m_screenWidth = std::max(1, screenWidth);
    m_screenHeight = std::max(1, screenHeight);
}

void LookEngine::addDelta(int dx, int dy, qint64 timestampNs) {
    // 竞技玩家偏好 1:1 跟手：不做平滑，直接累加
    m_accumX += dx * m_sensitivity;
    m_accumY += dy * m_sensitivity;
    m_lastEventNs = timestampNs;
}

void LookEngine::reset() {
    m_accumX = 0.0;
    m_accumY = 0.0;
    m_active = false;
    m_recentering = false;
    m_recenterDelayMs = 0;
    m_pendingX = 0.0;
    m_pendingY = 0.0;
}

void LookEngine::release(const std::function<void(const QByteArray &)> &send) {
    if (m_active) {
        emitTouch(send, static_cast<int>(proto::TouchAction::Up),
                  static_cast<int>(std::lround(m_fingerPixel.x())),
                  static_cast<int>(std::lround(m_fingerPixel.y())));
    }
    reset();
}

void LookEngine::emitTouch(const std::function<void(const QByteArray &)> &send, int action, int x,
                           int y) {
    send(proto::touch(static_cast<proto::TouchAction>(action), kLookPointer, x, y, m_screenWidth,
                      m_screenHeight));
}

bool LookEngine::tick(int dtMs, const std::function<void(const QByteArray &)> &send) {
    const QPointF centerPx = toPixel(m_center, m_screenWidth, m_screenHeight);
    const double radiusX = m_radius * m_screenWidth;
    const double radiusY = m_radius * m_screenHeight;
    const double pixelsPerCount = radiusX / m_countsPer180;

    // 阶段 A：回中等待。Up 已发出，倒计时结束后才重新 Down（Down 当帧不移动），
    // 让手机端把「抬起 → 重新按下」当成两次独立触控，避免回中跳变被算成反向拖动。
    if (m_recentering) {
        m_recenterDelayMs -= dtMs;
        if (m_recenterDelayMs > 0) {
            return false;
        }
        m_recentering = false;
        m_active = true;
        m_basePixel = jitteredCenter(m_humanizer, centerPx,
                                     m_radius * std::min(m_screenWidth, m_screenHeight),
                                     m_screenWidth, m_screenHeight);
        m_fingerPixel = m_basePixel;
        emitTouch(send, static_cast<int>(proto::TouchAction::Down),
                  static_cast<int>(std::lround(m_basePixel.x())),
                  static_cast<int>(std::lround(m_basePixel.y())));
        return true;
    }

    if (!m_active) {
        const bool hasPending = std::abs(m_pendingX) > 0.01 || std::abs(m_pendingY) > 0.01;
        const bool hasAccum = std::abs(m_accumX) >= 0.5 || std::abs(m_accumY) >= 0.5;
        if (!hasPending && !hasAccum) {
            return false;
        }
        // 首次按下：Down 当帧不移动，避免按下帧就携带大位移。
        m_active = true;
        m_basePixel = jitteredCenter(m_humanizer, centerPx,
                                     m_radius * std::min(m_screenWidth, m_screenHeight),
                                     m_screenWidth, m_screenHeight);
        m_fingerPixel = m_basePixel;
        emitTouch(send, static_cast<int>(proto::TouchAction::Down),
                  static_cast<int>(std::lround(m_basePixel.x())),
                  static_cast<int>(std::lround(m_basePixel.y())));
        return true;
    }

    double remainingX = m_accumX * pixelsPerCount + m_pendingX;
    double remainingY = m_accumY * pixelsPerCount + m_pendingY;
    m_accumX = 0.0;
    m_accumY = 0.0;
    m_pendingX = 0.0;
    m_pendingY = 0.0;

    // lift-recenter：手指滑到边界 → Up →（下一 tick 起延迟回中 Down）→ 继续。
    // 未走完的位移缓存到 pending，回中后接着消费，不丢距离。
    int guard = 0;
    while ((std::abs(remainingX) > 0.01 || std::abs(remainingY) > 0.01) && guard++ < 16) {
        const double roomX = remainingX >= 0 ? (m_basePixel.x() + radiusX - m_fingerPixel.x())
                                             : (m_fingerPixel.x() - (m_basePixel.x() - radiusX));
        const double roomY = remainingY >= 0 ? (m_basePixel.y() + radiusY - m_fingerPixel.y())
                                             : (m_fingerPixel.y() - (m_basePixel.y() - radiusY));
        const double stepX = std::clamp(remainingX, -std::max(0.0, roomX), std::max(0.0, roomX));
        const double stepY = std::clamp(remainingY, -std::max(0.0, roomY), std::max(0.0, roomY));

        if (std::abs(stepX) < 0.01 && std::abs(stepY) < 0.01) {
            // 已到边界：抬起，进入回中等待；剩余位移留到回中后继续。
            emitTouch(send, static_cast<int>(proto::TouchAction::Up),
                      static_cast<int>(std::lround(m_fingerPixel.x())),
                      static_cast<int>(std::lround(m_fingerPixel.y())));
            m_recentering = true;
            m_recenterDelayMs = kRecenterGapMs;
            m_pendingX = remainingX;
            m_pendingY = remainingY;
            break;
        }

        m_fingerPixel += QPointF(stepX, stepY);
        emitTouch(send, static_cast<int>(proto::TouchAction::Move),
                  static_cast<int>(std::lround(m_fingerPixel.x())),
                  static_cast<int>(std::lround(m_fingerPixel.y())));
        remainingX -= stepX;
        remainingY -= stepY;
    }

    return true;
}

// ---------------------------------------------------------------------------
// KeymapEngine
// ---------------------------------------------------------------------------
void KeymapEngine::configure(const QVector<KeyBinding> &bindings, int screenWidth, int screenHeight) {
    m_bindings = bindings;
    m_screenWidth = std::max(1, screenWidth);
    m_screenHeight = std::max(1, screenHeight);
}

bool KeymapEngine::isBound(int vk) const {
    return std::any_of(m_bindings.cbegin(), m_bindings.cend(),
                       [vk](const KeyBinding &b) { return b.vk == vk; });
}

QPointF KeymapEngine::toPixel(const QPointF &norm) const {
    return QPointF(norm.x() * m_screenWidth, norm.y() * m_screenHeight);
}

int KeymapEngine::allocatePointer(int vk) {
    for (int candidate = 2; candidate <= static_cast<int>(kMaxPointer); ++candidate) {
        const bool usedByAction =
            std::any_of(m_actions.cbegin(), m_actions.cend(),
                        [candidate](const ActiveAction &a) { return a.pointerId == candidate; });
        const bool usedByWheel =
            std::any_of(m_wheelPresses.cbegin(), m_wheelPresses.cend(),
                        [candidate](const WheelPress &w) { return w.pointerId == candidate; });
        if (!usedByAction && !usedByWheel) {
            return candidate;
        }
    }
    (void)vk;
    return 2;
}

bool KeymapEngine::handleKey(int vk, bool down, const std::function<void(const QByteArray &)> &send) {
    // 直通语义：按下 = 手指按下，抬起 = 手指抬起；按住多久手指就按多久。
    if (!down) {
        m_suspendedVks.remove(vk);
        for (int i = 0; i < m_actions.size(); ++i) {
            if (m_actions[i].vk != vk) {
                continue;
            }
            const ActiveAction &action = m_actions[i];
            send(proto::touch(proto::TouchAction::Up, static_cast<quint64>(action.pointerId),
                              static_cast<int>(std::lround(action.fromPixel.x())),
                              static_cast<int>(std::lround(action.fromPixel.y())), m_screenWidth,
                              m_screenHeight));
            m_actions.removeAt(i);
            return true;
        }
        return false;
    }

    // releaseAll（如呼出鼠标）已抬起但物理键仍按住：忽略其自动重复，等待松开后再生效。
    if (m_suspendedVks.contains(vk)) {
        return true;
    }

    // 按住重复（Raw 键盘自动重复）不重复派发
    for (const ActiveAction &action : m_actions) {
        if (action.vk == vk) {
            return true;
        }
    }

    const KeyBinding *binding = nullptr;
    for (const KeyBinding &b : m_bindings) {
        if (b.vk == vk) {
            binding = &b;
            break;
        }
    }
    if (!binding) {
        return false;
    }

    ActiveAction action;
    action.vk = vk;
    action.pointerId = allocatePointer(vk);
    // 按下位置做拟人化微抖动，避免触点长期固定在同一像素。
    const int jitterX = m_humanizer ? m_humanizer->jitter() : 0;
    const int jitterY = m_humanizer ? m_humanizer->jitter() : 0;
    action.fromPixel = toPixel(binding->center) + QPointF(jitterX, jitterY);

    send(proto::touch(proto::TouchAction::Down, static_cast<quint64>(action.pointerId),
                      static_cast<int>(std::lround(action.fromPixel.x())),
                      static_cast<int>(std::lround(action.fromPixel.y())), m_screenWidth,
                      m_screenHeight));
    m_actions.append(action);
    return true;
}

bool KeymapEngine::handleWheel(int delta, const std::function<void(const QByteArray &)> &send) {
    if (delta == 0) {
        return false;
    }
    const int vk = delta > 0 ? kVkWheelUp : kVkWheelDown;
    const KeyBinding *binding = nullptr;
    for (const KeyBinding &b : m_bindings) {
        if (b.vk == vk) {
            binding = &b;
            break;
        }
    }
    if (!binding) {
        // 未绑定滚轮：不注入居中滚动。手机端会把滚动当成多指手势，与视角单指拖动冲突，
        // 导致滚轮后视角不再响应。
        return false;
    }
    // 快速滚动时避免堆积占满 pointer 池（每个滚轮点按占用一个触点）。
    if (m_wheelPresses.size() >= 8) {
        return true;
    }

    WheelPress press;
    press.pointerId = allocatePointer(vk);
    const int jitterX = m_humanizer ? m_humanizer->jitter() : 0;
    const int jitterY = m_humanizer ? m_humanizer->jitter() : 0;
    press.pixel = toPixel(binding->center) + QPointF(jitterX, jitterY);
    press.remainingMs = 50; // 滚轮没有抬起事件：短按后由 tick 补发抬起
    send(proto::touch(proto::TouchAction::Down, static_cast<quint64>(press.pointerId),
                      static_cast<int>(std::lround(press.pixel.x())),
                      static_cast<int>(std::lround(press.pixel.y())), m_screenWidth,
                      m_screenHeight));
    m_wheelPresses.append(press);
    return true;
}

bool KeymapEngine::tick(int dtMs, const std::function<void(const QByteArray &)> &send) {
    bool emitted = false;
    for (int i = 0; i < m_wheelPresses.size();) {
        m_wheelPresses[i].remainingMs -= dtMs;
        if (m_wheelPresses[i].remainingMs > 0) {
            ++i;
            continue;
        }
        const WheelPress &press = m_wheelPresses.at(i);
        send(proto::touch(proto::TouchAction::Up, static_cast<quint64>(press.pointerId),
                          static_cast<int>(std::lround(press.pixel.x())),
                          static_cast<int>(std::lround(press.pixel.y())), m_screenWidth,
                          m_screenHeight));
        m_wheelPresses.removeAt(i);
        emitted = true;
    }
    return emitted;
}

void KeymapEngine::releaseAll(const std::function<void(const QByteArray &)> &send) {
    for (const ActiveAction &action : m_actions) {
        send(proto::touch(proto::TouchAction::Up, static_cast<quint64>(action.pointerId),
                          static_cast<int>(std::lround(action.fromPixel.x())),
                          static_cast<int>(std::lround(action.fromPixel.y())), m_screenWidth,
                          m_screenHeight));
        // 键仍可能物理按住：记录后忽略其自动重复，避免抬起后又自动按下。
        m_suspendedVks.insert(action.vk);
    }
    for (const WheelPress &press : m_wheelPresses) {
        send(proto::touch(proto::TouchAction::Up, static_cast<quint64>(press.pointerId),
                          static_cast<int>(std::lround(press.pixel.x())),
                          static_cast<int>(std::lround(press.pixel.y())), m_screenWidth,
                          m_screenHeight));
    }
    m_wheelPresses.clear();
    m_actions.clear();
}

// ---------------------------------------------------------------------------
// SensCalibrator
// ---------------------------------------------------------------------------
void SensCalibrator::begin() {
    m_active = true;
    m_counts = 0;
}

void SensCalibrator::addCounts(int dx, int dy) {
    if (!m_active) {
        return;
    }
    m_counts += static_cast<qint64>(std::lround(std::hypot(static_cast<double>(dx),
                                                           static_cast<double>(dy))));
}

void SensCalibrator::cancel() {
    m_active = false;
    m_counts = 0;
}

double SensCalibrator::suggestedCountsPer180(double previous) const {
    if (m_counts < 50) {
        return previous;
    }
    return static_cast<double>(m_counts);
}

// ---------------------------------------------------------------------------
// InputEngine
// ---------------------------------------------------------------------------
InputEngine::InputEngine() {
    m_keymap.setHumanizer(&m_humanizer);
    m_joystick.setHumanizer(&m_humanizer);
    m_look.setHumanizer(&m_humanizer);
    setProfile(m_profile);
}

void InputEngine::applyJoystickConfig() {
    m_joystick.configure(m_profile.joystickCenter, m_profile.joystickRadius,
                         m_profile.joystickDeadzone, m_profile.joystickCurve, m_screenWidth,
                         m_screenHeight);
    m_joystick.setSprintRadius(m_profile.joystickSprintRadius);
    m_look.configure(m_profile.lookCenter, m_profile.lookRadius, m_profile.lookSensitivity,
                     m_profile.lookCountsPer180, m_screenWidth, m_screenHeight);
    m_keymap.configure(m_profile.bindings, m_screenWidth, m_screenHeight);
}

void InputEngine::setProfile(const Profile &profile) {
    m_profile = profile;
    m_humanizer.setEnabled(profile.humanizerEnabled);
    m_humanizer.setEnhanced(profile.proHumanizer);
    applyJoystickConfig();
}

void InputEngine::setScreenSize(int width, int height) {
    m_screenWidth = std::max(1, width);
    m_screenHeight = std::max(1, height);
    applyJoystickConfig();
}

void InputEngine::handleInput(const InputEvent &event) {
    if (m_calibrator.isActive()) {
        if (event.type == InputEventType::MouseMove) {
            m_calibrator.addCounts(event.dx, event.dy);
        }
        return;
    }

    // 窗口态直操：左键与拖动由画布 Qt 事件注入触控，Raw Input 不再参与映射
    if (m_directTouchEnabled) {
        if (event.type == InputEventType::MouseButton && event.button == 0) {
            return;
        }
        if (event.type == InputEventType::MouseMove && m_touchDragActive) {
            return;
        }
    }

    // 指针模式：鼠标用于移动系统光标点击手机 UI，位移不再驱动视角
    if (!m_lookEnabled && event.type == InputEventType::MouseMove) {
        return;
    }

    // 呼出键不参与映射：其按下/抬起不进入键位引擎
    int eventVk = 0;
    if (event.type == InputEventType::Key) {
        eventVk = event.vk;
    } else if (event.type == InputEventType::MouseButton) {
        eventVk = mouseButtonVk(event.button);
    }
    if (eventVk && m_ignoredKey && vkMatchesSummon(eventVk, m_ignoredKey)) {
        return;
    }

    // WASD → 摇杆方向键状态（基础版核心走位，不依赖键位方案）
    if (event.type == InputEventType::Key) {
        switch (event.vk) {
        case 'W':
        case 'S':
        case 'A':
        case 'D': {
            if (event.down && m_suspendedWasd.contains(event.vk)) {
                // releaseAll 已抬起摇杆，物理键仍按住：忽略自动重复，等待松开
                return;
            }
            if (!event.down) {
                m_suspendedWasd.remove(event.vk);
            }
            switch (event.vk) {
            case 'W': m_joyUp = event.down; break;
            case 'S': m_joyDown = event.down; break;
            case 'A': m_joyLeft = event.down; break;
            case 'D': m_joyRight = event.down; break;
            default: break;
            }
            m_joystick.setKeys(m_joyUp, m_joyDown, m_joyLeft, m_joyRight);
            break;
        }
        default:
            break;
        }
    }

    // 采集时刻戳由 RawInputFilter 打（GUI 线程）；滚轮等无戳来源在此补戳。
    // 不再无条件覆盖，否则「GUI 线程排队 → 注入线程」的延迟会被抹掉。
    InputEvent stamped = event;
    if (stamped.timestampNs <= 0) {
        stamped.timestampNs = qpcNowTicks();
    }
    m_lastInputQpc = stamped.timestampNs;
    m_pendingEvents.append(stamped);
}

void InputEngine::tick(int dtMs, const std::function<void(const QByteArray &)> &send) {
    const bool hadEvents = !m_pendingEvents.isEmpty();
    for (const InputEvent &event : m_pendingEvents) {
        switch (event.type) {
        case InputEventType::MouseMove:
            m_look.addDelta(event.dx, event.dy, event.timestampNs);
            break;
        case InputEventType::MouseButton: {
            const int vk = mouseButtonVk(event.button);
            if (vk) {
                m_keymap.handleKey(vk, event.down, send);
            }
            break;
        }
        case InputEventType::Wheel:
            m_keymap.handleWheel(event.dx, send);
            break;
        case InputEventType::Key:
            m_keymap.handleKey(event.vk, event.down, send);
            break;
        }
    }
    m_pendingEvents.clear();

    m_joystick.tick(dtMs, send);
    m_look.tick(dtMs, send);
    m_keymap.tick(dtMs, send);

    if (hadEvents && m_lastInputQpc > 0) {
        // 从「系统采集到按键」算到「触控指令发出」，包含 GUI 线程排队时间。
        const double elapsedMs = (qpcNowTicks() - m_lastInputQpc) * 1000.0 / qpcFrequencyHz();
        m_latencyMs = elapsedMs;
    }
}

void InputEngine::releaseAll(const std::function<void(const QByteArray &)> &send) {
    // 记录仍物理按住的 WASD：抬起摇杆后忽略其自动重复，等待松开后再恢复走位。
    if (m_joyUp) {
        m_suspendedWasd.insert('W');
    }
    if (m_joyDown) {
        m_suspendedWasd.insert('S');
    }
    if (m_joyLeft) {
        m_suspendedWasd.insert('A');
    }
    if (m_joyRight) {
        m_suspendedWasd.insert('D');
    }
    m_joyUp = m_joyDown = m_joyLeft = m_joyRight = false;
    m_joystick.release(send);
    m_look.release(send);
    m_keymap.releaseAll(send);
}

void InputEngine::clearSuspendedKeys() {
    m_suspendedWasd.clear();
    m_keymap.clearSuspended();
}

double InputEngine::takeLatencyMs() {
    const double value = m_latencyMs;
    m_latencyMs = -1.0;
    return value;
}

} // namespace ks
