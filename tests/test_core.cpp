#include <QCoreApplication>
#include <QStandardPaths>

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include <qt_windows.h>

#include "core/engine/Engines.h"
#include "core/profile/ProfileStore.h"
#include "core/scrcpy/Protocol.h"

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool condition, const std::string &name) {
    ++g_checks;
    if (!condition) {
        ++g_failures;
        std::printf("[FAIL] %s\n", name.c_str());
    } else {
        std::printf("[ ok ] %s\n", name.c_str());
    }
}

quint32 read32(const QByteArray &data, int offset) {
    const auto *p = reinterpret_cast<const unsigned char *>(data.constData() + offset);
    return (static_cast<quint32>(p[0]) << 24) | (static_cast<quint32>(p[1]) << 16) |
           (static_cast<quint32>(p[2]) << 8) | p[3];
}

quint16 read16(const QByteArray &data, int offset) {
    const auto *p = reinterpret_cast<const unsigned char *>(data.constData() + offset);
    return static_cast<quint16>((static_cast<quint16>(p[0]) << 8) | p[1]);
}

quint64 read64(const QByteArray &data, int offset) {
    quint64 value = 0;
    for (int i = 0; i < 8; ++i) {
        value = (value << 8) | static_cast<unsigned char>(data.at(offset + i));
    }
    return value;
}

// ---------------------------------------------------------------------------
void testProtocol() {
    const QByteArray touch = ks::proto::touch(ks::proto::TouchAction::Down, 1, 540, 1200, 1080,
                                              2400, 1.0f, ks::proto::ButtonPrimary,
                                              ks::proto::ButtonPrimary);
    check(touch.size() == 32, "touch message is 32 bytes");
    check(static_cast<quint8>(touch.at(0)) == 2, "touch type == 2");
    check(static_cast<quint8>(touch.at(1)) == 0, "touch action down == 0");
    check(read64(touch, 2) == 1, "touch pointer id big endian");
    check(read32(touch, 10) == 540 && read32(touch, 14) == 1200, "touch coordinates");
    check(read16(touch, 18) == 1080 && read16(touch, 20) == 2400, "touch screen size");
    check(read16(touch, 22) == 0xFFFF, "pressure 1.0 == 0xFFFF");
    check(read32(touch, 24) == 1 && read32(touch, 28) == 1, "buttons primary");

    const QByteArray key = ks::proto::keycode(ks::proto::KeyAction::Down, 66, 0, 0);
    check(key.size() == 14, "keycode message is 14 bytes");
    check(static_cast<quint8>(key.at(0)) == 0, "keycode type == 0");
    check(read32(key, 2) == 66, "keycode value");

    const QByteArray scroll = ks::proto::scroll(100, 200, 1080, 2400, 0.0f, 1.0f);
    check(scroll.size() == 21, "scroll message is 21 bytes");
    check(static_cast<quint8>(scroll.at(0)) == 3, "scroll type == 3");
    check(read32(scroll, 1) == 100 && read32(scroll, 5) == 200, "scroll position");
    const qint16 vscroll = static_cast<qint16>(read16(scroll, 15));
    check(vscroll > 1800 && vscroll < 2200, "scroll vertical fixed point");

    const QByteArray power = ks::proto::displayPower(false);
    check(power.size() == 2 && static_cast<quint8>(power.at(1)) == 0, "display power off payload");
}

// ---------------------------------------------------------------------------
void testJoystick() {
    ks::JoystickEngine joystick;
    joystick.configure(QPointF(0.2, 0.8), 0.1, 0.1, 1.0, 1000, 1000);
    joystick.setKeys(true, false, true, false); // W + A

    std::vector<QByteArray> messages;
    auto sink = [&messages](const QByteArray &message) { messages.push_back(message); };

    joystick.tick(16, sink);
    check(!messages.empty(), "joystick emits on first tick");

    const QByteArray &down = messages.front();
    check(static_cast<quint8>(down.at(1)) == 0, "joystick first message is DOWN");
    check(read64(down, 2) == 0, "joystick pointer id 0");
    check(messages.size() >= 2, "joystick emits MOVE while held");

    if (messages.size() >= 2) {
        const QByteArray &move = messages.back();
        const int x = static_cast<int>(read32(move, 10));
        const int y = static_cast<int>(read32(move, 14));
        const double distance = std::hypot(x - 200.0, y - 800.0);
        check(distance <= 100.0 + 1.0, "joystick vector within radius (no diagonal over-speed)");
        check(x < 200 && y < 800, "W+A moves up-left");
    }

    messages.clear();
    joystick.setKeys(false, false, false, false);
    for (int i = 0; i < 10; ++i) {
        joystick.tick(16, sink);
    }
    check(!messages.empty() && static_cast<quint8>(messages.back().at(1)) == 1,
          "joystick ends with UP after release");
}

// ---------------------------------------------------------------------------
void testLookEngine() {
    ks::LookEngine look;
    look.configure(QPointF(0.5, 0.5), 0.2, 1.0, 1000.0, 1000, 1000);

    std::vector<QByteArray> messages;
    auto sink = [&messages](const QByteArray &message) { messages.push_back(message); };

    // 输入 180° 所需计数的一半 → 不应触发 lift-recenter
    look.addDelta(500, 0, 0);
    look.tick(4, sink);
    bool hasUp = false;
    for (const QByteArray &message : messages) {
        if (static_cast<quint8>(message.at(1)) == 1) {
            hasUp = true;
        }
    }
    check(!hasUp, "look engine keeps finger down below threshold");

    messages.clear();
    look.addDelta(1500, 0, 0);
    look.tick(4, sink);
    int upCount = 0;
    for (const QByteArray &message : messages) {
        if (static_cast<quint8>(message.at(1)) == 1) {
            ++upCount;
        }
    }
    check(upCount >= 1, "look engine lifts and recenters beyond boundary");

    // 触点必须始终在触发区内
    bool inside = true;
    for (const QByteArray &message : messages) {
        const int x = static_cast<int>(read32(message, 10));
        const int y = static_cast<int>(read32(message, 14));
        if (std::abs(x - 500) > 200 + 2 || std::abs(y - 500) > 200 + 2) {
            inside = false;
        }
    }
    check(inside, "look touch stays within radius");
}

// ---------------------------------------------------------------------------
void testKeymap() {
    ks::HumanizerEngine humanizer;
    ks::KeymapEngine keymap;
    ks::KeyBinding binding;
    binding.vk = 0x20;
    binding.action = ks::BindingAction::Tap;
    binding.center = QPointF(0.5, 0.5);
    binding.radius = 0.05;
    keymap.configure({binding}, 1000, 1000);
    keymap.setHumanizer(&humanizer);

    std::vector<QByteArray> messages;
    auto sink = [&messages](const QByteArray &message) { messages.push_back(message); };

    check(keymap.handleKey(0x20, true, sink), "keymap handles bound key");
    check(!messages.empty() && static_cast<quint8>(messages.front().at(1)) == 0,
          "keymap sends DOWN on press");

    messages.clear();
    for (int i = 0; i < 30; ++i) {
        keymap.tick(10, sink);
    }
    check(!messages.empty() && static_cast<quint8>(messages.back().at(1)) == 1,
          "keymap sends UP after tap duration");

    for (int i = 0; i < 32; ++i) {
        const int duration = humanizer.tapDurationMs();
        if (duration < 40 || duration > 120) {
            check(false, "humanizer tap duration in range");
            return;
        }
    }
    check(true, "humanizer tap duration in range");
}

// ---------------------------------------------------------------------------
void testSummonKeyFilter() {
    check(ks::vkMatchesSummon(VK_LMENU, VK_MENU), "summon key matches left Alt");
    check(ks::vkMatchesSummon(VK_RCONTROL, VK_CONTROL), "summon key matches right Ctrl");
    check(!ks::vkMatchesSummon(VK_LBUTTON, VK_CONTROL), "summon key ignores other keys");

    ks::Profile profile;
    ks::KeyBinding binding;
    binding.vk = VK_LMENU;
    binding.action = ks::BindingAction::Tap;
    binding.center = QPointF(0.5, 0.5);
    binding.radius = 0.05;
    profile.bindings.append(binding);

    ks::InputEngine engine;
    engine.setProfile(profile);
    engine.setScreenSize(1000, 1000);
    engine.setIgnoredKey(VK_MENU);

    ks::InputEvent down;
    down.type = ks::InputEventType::Key;
    down.vk = VK_LMENU;
    down.down = true;
    engine.handleInput(down);

    std::vector<QByteArray> messages;
    auto sink = [&messages](const QByteArray &message) { messages.push_back(message); };
    for (int i = 0; i < 5; ++i) {
        engine.tick(10, sink);
    }
    check(messages.empty(), "summon key is filtered from keymap");

    engine.setIgnoredKey(0);
    engine.handleInput(down);
    for (int i = 0; i < 3; ++i) {
        engine.tick(10, sink);
    }
    check(!messages.empty(), "keys reach keymap when not ignored");
}

// ---------------------------------------------------------------------------
void testCalibrator() {
    ks::SensCalibrator calibrator;
    calibrator.begin();
    for (int i = 0; i < 10; ++i) {
        calibrator.addCounts(100, 0);
    }
    check(calibrator.suggestedCountsPer180(3000.0) == 1000.0, "calibrator suggests measured counts");

    calibrator.cancel();
    check(!calibrator.isActive() && calibrator.measuredCounts() == 0, "calibrator cancel resets");
}

// ---------------------------------------------------------------------------
void testProfileStore() {
    ks::ProfileStore store;
    ks::Profile profile = ks::ProfileStore::makeDefaultProfile();
    profile.name = QStringLiteral("测试方案");
    profile.brightness = 12;
    profile.lookCountsPer180 = 1234.0;
    profile.bindings.first().center = QPointF(0.25, 0.35);

    QString error;
    check(store.save(profile, &error), "profile save");

    bool ok = false;
    const ks::Profile loaded = store.load(QStringLiteral("测试方案"), &ok);
    check(ok, "profile load");
    check(loaded.brightness == 12, "profile brightness roundtrip");
    check(std::abs(loaded.lookCountsPer180 - 1234.0) < 0.001, "profile sensitivity roundtrip");
    check(loaded.bindings.size() == profile.bindings.size(), "profile bindings roundtrip");
    check(std::abs(loaded.bindings.first().center.x() - 0.25) < 0.001,
          "profile binding position roundtrip");

    store.remove(QStringLiteral("测试方案"));
}

} // namespace

int main(int argc, char *argv[]) {
    QCoreApplication app(argc, argv);
    QStandardPaths::setTestModeEnabled(true);

    testProtocol();
    testJoystick();
    testLookEngine();
    testKeymap();
    testSummonKeyFilter();
    testCalibrator();
    testProfileStore();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
