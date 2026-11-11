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
constexpr quint64 kMaxPointer = 15;

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

double easeOutCubic(double t) {
    const double inv = 1.0 - t;
    return 1.0 - inv * inv * inv;
}

QPointF toPixel(const QPointF &norm, int width, int height) {
    return QPointF(norm.x() * width, norm.y() * height);
}

int mouseButtonVk(int button) {
    switch (button) {
    case 0: return VK_LBUTTON;
    case 1: return VK_RBUTTON;
    case 2: return VK_MBUTTON;
    default: return 0;
    }
}

QByteArray touchMessage(quint64 pointer, proto::TouchAction action, const QPointF &pixel, int width,
                        int height, float pressure = 1.0f) {
    return proto::touch(action, pointer, static_cast<int>(std::lround(pixel.x())),
                        static_cast<int>(std::lround(pixel.y())), width, height, pressure);
}

} // namespace

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
    m_up = m_down = m_left = m_right = false;
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
    const double radiusPx = m_radius * std::min(m_screenWidth, m_screenHeight);

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

        const QPointF target = centerPx + QPointF(fx * radiusPx * t, fy * radiusPx * t);

        if (!m_active) {
            m_active = true;
            m_releasing = false;
            m_currentPixel = centerPx;
            send(touchMessage(kJoystickPointer, proto::TouchAction::Down, centerPx, m_screenWidth,
                              m_screenHeight));
        }

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
        const QPointF pos = m_releaseFrom + (centerPx - m_releaseFrom) * eased;
        send(touchMessage(kJoystickPointer, proto::TouchAction::Move, pos, m_screenWidth,
                          m_screenHeight));
        m_currentPixel = pos;
        if (k >= 1.0) {
            send(touchMessage(kJoystickPointer, proto::TouchAction::Up, centerPx, m_screenWidth,
                              m_screenHeight));
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
    (void)dtMs;
    if (std::abs(m_accumX) < 0.5 && std::abs(m_accumY) < 0.5) {
        return false;
    }

    const QPointF centerPx = toPixel(m_center, m_screenWidth, m_screenHeight);
    const double radiusX = m_radius * m_screenWidth;
    const double radiusY = m_radius * m_screenHeight;
    const double pixelsPerCount = radiusX / m_countsPer180;

    if (!m_active) {
        m_active = true;
        m_fingerPixel = centerPx;
        emitTouch(send, static_cast<int>(proto::TouchAction::Down),
                  static_cast<int>(std::lround(centerPx.x())),
                  static_cast<int>(std::lround(centerPx.y())));
    }

    double remainingX = m_accumX * pixelsPerCount;
    double remainingY = m_accumY * pixelsPerCount;
    m_accumX = 0.0;
    m_accumY = 0.0;

    // lift-recenter 循环：手指滑到边界 → 抬起 → 回中心 → 继续，实现无限转向
    int guard = 0;
    while ((std::abs(remainingX) > 0.01 || std::abs(remainingY) > 0.01) && guard++ < 16) {
        const double roomX = remainingX >= 0 ? (centerPx.x() + radiusX - m_fingerPixel.x())
                                             : (m_fingerPixel.x() - (centerPx.x() - radiusX));
        const double roomY = remainingY >= 0 ? (centerPx.y() + radiusY - m_fingerPixel.y())
                                             : (m_fingerPixel.y() - (centerPx.y() - radiusY));
        const double stepX = std::clamp(remainingX, -std::max(0.0, roomX), std::max(0.0, roomX));
        const double stepY = std::clamp(remainingY, -std::max(0.0, roomY), std::max(0.0, roomY));

        if (std::abs(stepX) < 0.01 && std::abs(stepY) < 0.01) {
            // 已到边界：抬起 → 回中心重新按下
            emitTouch(send, static_cast<int>(proto::TouchAction::Up),
                      static_cast<int>(std::lround(m_fingerPixel.x())),
                      static_cast<int>(std::lround(m_fingerPixel.y())));
            m_fingerPixel = centerPx;
            emitTouch(send, static_cast<int>(proto::TouchAction::Down),
                      static_cast<int>(std::lround(centerPx.x())),
                      static_cast<int>(std::lround(centerPx.y())));
            continue;
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
        const bool used = std::any_of(m_actions.cbegin(), m_actions.cend(),
                                      [candidate](const ActiveAction &a) {
                                          return a.pointerId == candidate;
                                      });
        if (!used) {
            return candidate;
        }
    }
    (void)vk;
    return 2;
}

void KeymapEngine::releasePointer(int vk, const std::function<void(const QByteArray &)> &send) {
    for (int i = 0; i < m_actions.size(); ++i) {
        if (m_actions[i].vk != vk) {
            continue;
        }
        ActiveAction &action = m_actions[i];
        send(proto::touch(proto::TouchAction::Up, static_cast<quint64>(action.pointerId),
                          static_cast<int>(std::lround(action.fromPixel.x())),
                          static_cast<int>(std::lround(action.fromPixel.y())), m_screenWidth,
                          m_screenHeight));
        m_actions.removeAt(i);
        return;
    }
}

bool KeymapEngine::handleKey(int vk, bool down, const std::function<void(const QByteArray &)> &send) {
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

    if (!down) {
        // 抬起：Hold 立即抬起；其它动作等待计时器完成
        for (int i = 0; i < m_actions.size(); ++i) {
            if (m_actions[i].vk != vk) {
                continue;
            }
            if (m_actions[i].type == BindingAction::Hold && !m_actions[i].waitingRelease) {
                ActiveAction &action = m_actions[i];
                send(proto::touch(proto::TouchAction::Up, static_cast<quint64>(action.pointerId),
                                  static_cast<int>(std::lround(action.fromPixel.x())),
                                  static_cast<int>(std::lround(action.fromPixel.y())),
                                  m_screenWidth, m_screenHeight));
                m_actions.removeAt(i);
                return true;
            }
            m_actions[i].waitingRelease = true;
        }
        return true;
    }

    // 按住重复不重复派发
    for (const ActiveAction &action : m_actions) {
        if (action.vk == vk) {
            return true;
        }
    }

    ActiveAction action;
    action.vk = vk;
    action.type = binding->action;
    action.pointerId = allocatePointer(vk);
    action.fromPixel = toPixel(binding->center);
    action.toPixel = toPixel(binding->slideTo);
    action.downSent = true;

    const int jitterX = m_humanizer ? m_humanizer->jitter() : 0;
    const int jitterY = m_humanizer ? m_humanizer->jitter() : 0;
    const QPointF start = action.fromPixel + QPointF(jitterX, jitterY);

    switch (action.type) {
    case BindingAction::Hold:
        action.holding = true;
        send(proto::touch(proto::TouchAction::Down, static_cast<quint64>(action.pointerId),
                          static_cast<int>(std::lround(start.x())),
                          static_cast<int>(std::lround(start.y())), m_screenWidth, m_screenHeight));
        break;
    case BindingAction::Tap: {
        const int duration = m_humanizer ? m_humanizer->tapDurationMs() : 50;
        action.durationMs = duration;
        action.elapsedMs = 0;
        action.fromPixel = start;
        send(proto::touch(proto::TouchAction::Down, static_cast<quint64>(action.pointerId),
                          static_cast<int>(std::lround(start.x())),
                          static_cast<int>(std::lround(start.y())), m_screenWidth, m_screenHeight));
        break;
    }
    case BindingAction::Double: {
        const int duration = m_humanizer ? m_humanizer->tapDurationMs() : 50;
        action.durationMs = duration * 2 + 60;
        action.moveStep = 0;
        action.fromPixel = start;
        send(proto::touch(proto::TouchAction::Down, static_cast<quint64>(action.pointerId),
                          static_cast<int>(std::lround(start.x())),
                          static_cast<int>(std::lround(start.y())), m_screenWidth, m_screenHeight));
        break;
    }
    case BindingAction::Slide: {
        const QPointF delta = action.toPixel - action.fromPixel;
        const int distance = static_cast<int>(std::hypot(delta.x(), delta.y()));
        action.totalSteps = m_humanizer ? m_humanizer->moveSteps(distance) : 3;
        action.durationMs = std::max(60, 40 + distance / 4);
        action.elapsedMs = 0;
        action.moveStep = 0;
        action.fromPixel = start;
        send(proto::touch(proto::TouchAction::Down, static_cast<quint64>(action.pointerId),
                          static_cast<int>(std::lround(start.x())),
                          static_cast<int>(std::lround(start.y())), m_screenWidth, m_screenHeight));
        break;
    }
    }

    m_actions.append(action);
    return true;
}

bool KeymapEngine::handleWheel(int delta, const std::function<void(const QByteArray &)> &send) {
    const QPointF center = QPointF(m_screenWidth * 0.5, m_screenHeight * 0.5);
    send(proto::scroll(static_cast<int>(center.x()), static_cast<int>(center.y()), m_screenWidth,
                       m_screenHeight, 0.0f, static_cast<float>(delta)));
    return true;
}

bool KeymapEngine::tick(int dtMs, const std::function<void(const QByteArray &)> &send) {
    bool emitted = false;
    for (int i = 0; i < m_actions.size();) {
        ActiveAction &action = m_actions[i];
        if (action.holding) {
            ++i;
            continue;
        }

        action.elapsedMs += dtMs;

        if (action.type == BindingAction::Tap) {
            if (action.elapsedMs >= action.durationMs) {
                send(proto::touch(proto::TouchAction::Up, static_cast<quint64>(action.pointerId),
                                  static_cast<int>(std::lround(action.fromPixel.x())),
                                  static_cast<int>(std::lround(action.fromPixel.y())),
                                  m_screenWidth, m_screenHeight));
                m_actions.removeAt(i);
                emitted = true;
                continue;
            }
        } else if (action.type == BindingAction::Double) {
            const int half = action.durationMs / 2;
            if (action.moveStep == 0 && action.elapsedMs >= half) {
                send(proto::touch(proto::TouchAction::Up, static_cast<quint64>(action.pointerId),
                                  static_cast<int>(std::lround(action.fromPixel.x())),
                                  static_cast<int>(std::lround(action.fromPixel.y())),
                                  m_screenWidth, m_screenHeight));
                action.moveStep = 1;
                action.nextStepAtMs = action.elapsedMs + 60;
            } else if (action.moveStep == 1 && action.elapsedMs >= action.nextStepAtMs) {
                send(proto::touch(proto::TouchAction::Down, static_cast<quint64>(action.pointerId),
                                  static_cast<int>(std::lround(action.fromPixel.x())),
                                  static_cast<int>(std::lround(action.fromPixel.y())),
                                  m_screenWidth, m_screenHeight));
                action.moveStep = 2;
            } else if (action.moveStep >= 2 && action.elapsedMs >= action.durationMs) {
                send(proto::touch(proto::TouchAction::Up, static_cast<quint64>(action.pointerId),
                                  static_cast<int>(std::lround(action.fromPixel.x())),
                                  static_cast<int>(std::lround(action.fromPixel.y())),
                                  m_screenWidth, m_screenHeight));
                m_actions.removeAt(i);
                emitted = true;
                continue;
            }
        } else if (action.type == BindingAction::Slide) {
            const double progress =
                std::clamp(action.elapsedMs / static_cast<double>(action.durationMs), 0.0, 1.0);
            const int expectedStep = static_cast<int>(progress * action.totalSteps);
            if (expectedStep > action.moveStep) {
                action.moveStep = expectedStep;
                const double eased = easeOutCubic(progress);
                QPointF pos = action.fromPixel + (action.toPixel - action.fromPixel) * eased;
                if (action.moveStep < action.totalSteps && m_humanizer) {
                    pos += QPointF(m_humanizer->jitter(), m_humanizer->jitter());
                }
                send(proto::touch(proto::TouchAction::Move,
                                  static_cast<quint64>(action.pointerId),
                                  static_cast<int>(std::lround(pos.x())),
                                  static_cast<int>(std::lround(pos.y())), m_screenWidth,
                                  m_screenHeight));
                emitted = true;
            }
            if (action.elapsedMs >= action.durationMs) {
                send(proto::touch(proto::TouchAction::Up, static_cast<quint64>(action.pointerId),
                                  static_cast<int>(std::lround(action.toPixel.x())),
                                  static_cast<int>(std::lround(action.toPixel.y())), m_screenWidth,
                                  m_screenHeight));
                m_actions.removeAt(i);
                emitted = true;
                continue;
            }
        }
        ++i;
    }
    return emitted;
}

void KeymapEngine::releaseAll(const std::function<void(const QByteArray &)> &send) {
    for (const ActiveAction &action : m_actions) {
        send(proto::touch(proto::TouchAction::Up, static_cast<quint64>(action.pointerId),
                          static_cast<int>(std::lround(action.fromPixel.x())),
                          static_cast<int>(std::lround(action.fromPixel.y())), m_screenWidth,
                          m_screenHeight));
    }
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
    setProfile(m_profile);
}

void InputEngine::applyJoystickConfig() {
    m_joystick.configure(m_profile.joystickCenter, m_profile.joystickRadius,
                         m_profile.joystickDeadzone, m_profile.joystickCurve, m_screenWidth,
                         m_screenHeight);
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
        bool wasd = true;
        switch (event.vk) {
        case 'W': m_joyUp = event.down; break;
        case 'S': m_joyDown = event.down; break;
        case 'A': m_joyLeft = event.down; break;
        case 'D': m_joyRight = event.down; break;
        default: wasd = false; break;
        }
        if (wasd) {
            m_joystick.setKeys(m_joyUp, m_joyDown, m_joyLeft, m_joyRight);
        }
    }

    InputEvent stamped = event;
    stamped.timestampNs = qpcNow();
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
        const double elapsedMs = (qpcNow() - m_lastInputQpc) / qpcFrequency() * 1000.0;
        m_latencyMs = elapsedMs;
    }
}

void InputEngine::releaseAll(const std::function<void(const QByteArray &)> &send) {
    m_joyUp = m_joyDown = m_joyLeft = m_joyRight = false;
    m_joystick.release(send);
    m_look.release(send);
    m_keymap.releaseAll(send);
}

double InputEngine::takeLatencyMs() {
    const double value = m_latencyMs;
    m_latencyMs = -1.0;
    return value;
}

} // namespace ks
