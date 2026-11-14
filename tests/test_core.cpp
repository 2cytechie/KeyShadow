#include <QCoreApplication>
#include <QStandardPaths>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

#include <qt_windows.h>

#include "core/engine/Engines.h"
#include "core/profile/ProfileStore.h"
#include "core/scrcpy/Protocol.h"
#include "license/LicenseVerifier.h"

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
void testWasdJoystick() {
    ks::InputEngine engine;
    engine.setScreenSize(1000, 1000);

    std::vector<QByteArray> messages;
    auto sink = [&messages](const QByteArray &message) { messages.push_back(message); };

    ks::InputEvent down;
    down.type = ks::InputEventType::Key;
    down.vk = 'W';
    down.down = true;
    engine.handleInput(down);
    for (int i = 0; i < 3; ++i) {
        engine.tick(10, sink);
    }

    check(!messages.empty(), "WASD drives joystick through InputEngine");
    if (!messages.empty()) {
        check(read64(messages.front(), 2) == 0, "WASD uses joystick pointer id 0");
        bool movedUp = false;
        for (const QByteArray &message : messages) {
            // 默认摇杆中心 y=760（1000px 高），W 应产生更小的 y。
            if (static_cast<quint8>(message.at(1)) == 2 && read32(message, 14) < 760) {
                movedUp = true;
            }
        }
        check(movedUp, "W moves joystick upward");
    }

    messages.clear();
    ks::InputEvent up = down;
    up.down = false;
    engine.handleInput(up);
    for (int i = 0; i < 12; ++i) {
        engine.tick(10, sink);
    }
    check(!messages.empty() && static_cast<quint8>(messages.back().at(1)) == 1,
          "WASD release ends with UP");

    messages.clear();
    engine.handleInput(down);
    engine.tick(10, sink);
    messages.clear();
    engine.releaseAll(sink);
    check(!messages.empty() && static_cast<quint8>(messages.back().at(1)) == 1 &&
              read64(messages.back(), 2) == 0,
          "releaseAll lifts active joystick touch");
}

// ---------------------------------------------------------------------------
void testLookEngine() {
    ks::HumanizerEngine humanizer;
    humanizer.setEnabled(false);
    ks::LookEngine look;
    look.configure(QPointF(0.5, 0.5), 0.2, 1.0, 1000.0, 1000, 1000);
    look.setHumanizer(&humanizer);

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

    // 触点必须始终在椭圆触发区内。
    bool inside = true;
    for (const QByteArray &message : messages) {
        const int x = static_cast<int>(read32(message, 10));
        const int y = static_cast<int>(read32(message, 14));
        const double nx = (x - 500.0) / 200.0;
        const double ny = (y - 500.0) / 200.0;
        if (nx * nx + ny * ny > 1.0001) {
            inside = false;
        }
    }
    check(inside, "look touch stays within ellipse");
}

// ---------------------------------------------------------------------------
void testLookEllipseBoundary() {
    ks::HumanizerEngine humanizer;
    humanizer.setEnabled(false);
    ks::LookEngine look;
    look.configure(QPointF(0.5, 0.5), 0.2, 1.0, 1000.0, 1000, 1000);
    look.setHumanizer(&humanizer);

    std::vector<QByteArray> messages;
    auto sink = [&messages](const QByteArray &message) { messages.push_back(message); };

    look.addDelta(1500, 1500, 0);
    look.tick(4, sink); // DOWN
    messages.clear();
    look.tick(4, sink); // diagonal boundary + UP

    bool inside = true;
    for (const QByteArray &message : messages) {
        const int x = static_cast<int>(read32(message, 10));
        const int y = static_cast<int>(read32(message, 14));
        const double nx = (x - 500.0) / 200.0;
        const double ny = (y - 500.0) / 200.0;
        if (nx * nx + ny * ny > 1.0001) {
            inside = false;
        }
    }
    check(inside, "diagonal look movement stays inside ellipse");

    messages.clear();
    look.reset();
    look.configure(QPointF(0.0, 0.0), 0.2, 1.0, 1000.0, 1000, 1000);
    look.addDelta(1, 0, 0);
    look.tick(4, sink);
    check(!messages.empty() && read32(messages.front(), 10) == 200 &&
              read32(messages.front(), 14) == 200,
          "look center is clamped to keep ellipse on screen");
}

// ---------------------------------------------------------------------------
// lift-recenter 回中时序：Up 与回中 Down 必须跨 tick 分开，Down 当帧不携带 Move
// ---------------------------------------------------------------------------
void testLookRecenterTiming() {
    ks::HumanizerEngine humanizer;
    humanizer.setEnabled(false); // 确定坐标
    ks::LookEngine look;
    look.configure(QPointF(0.5, 0.5), 0.2, 1.0, 1000.0, 1000, 1000);
    look.setHumanizer(&humanizer);

    std::vector<QByteArray> messages;
    auto sink = [&messages](const QByteArray &message) { messages.push_back(message); };
    auto actionOf = [](const QByteArray &m) { return static_cast<quint8>(m.at(1)); };

    // 首次输入：第一 tick 只发 Down，不携带任何 Move
    look.addDelta(3000, 0, 0);
    look.tick(4, sink);
    check(messages.size() == 1 && actionOf(messages.front()) == 0,
          "first tick emits only the DOWN");
    messages.clear();

    // 第二 tick：移动到边界并以 Up 结尾
    look.tick(4, sink);
    check(!messages.empty() && actionOf(messages.back()) == 1,
          "boundary tick ends with UP");
    messages.clear();

    // 回中间隔内（<10ms）不得重新按下
    look.tick(4, sink);
    check(messages.empty(), "no recenter DOWN within 4ms");
    look.tick(4, sink);
    check(messages.empty(), "no recenter DOWN within 8ms");
    look.tick(4, sink);
    check(messages.size() == 1 && actionOf(messages.front()) == 0,
          "recenter DOWN after >=10ms gap");
    messages.clear();

    // 回中按下后下一 tick 才恢复移动
    look.tick(4, sink);
    bool hasMove = false;
    bool hasDown = false;
    for (const QByteArray &m : messages) {
        if (actionOf(m) == 2) {
            hasMove = true;
        }
        if (actionOf(m) == 0) {
            hasDown = true;
        }
    }
    check(hasMove && !hasDown, "movement resumes on the next tick without re-DOWN");
}

// ---------------------------------------------------------------------------
void testMouseButtonVk() {
    check(ks::mouseButtonVk(3) == 0x05, "mouse button 3 maps to VK_XBUTTON1");
    check(ks::mouseButtonVk(4) == 0x06, "mouse button 4 maps to VK_XBUTTON2");
}

// ---------------------------------------------------------------------------
// 许可证验签：测试专用 RSA-2048 密钥（不用于生产签发）
// ---------------------------------------------------------------------------
constexpr const char *kTestModulusB64 = "z5a21U3wEZb4xJTZFfdchKPNIwxQIRX1dyUxBb4_uQFV3rh43mBVZGxXrxN5cBrPUF8DAB6_cPXOsdZn3rDr97QSQ_D7Jqipx7C7AQky212-b6HL76r5FcjZe3xYuinhpJRnUC6Z4PWt01Pos5QBypIS5NpgIb-Qauobw6C56J9sFoHNVYFcYubKfrNy5LJD5dO1K5ZpKRWIbCFrSmeHLQdYZYDT2zWEI9xihe3lwJb_p4TIFaFI-gb983lSLWH4EZRtAlZirMDNWqD8dgFf3YF1uK3uahaIQjT0hBOBr91wjeLihwlxoYkYdkE5ESbc3yxSYq5ntG8i71mo53VIlQ";
constexpr const char *kTestExponentB64 = "AQAB";
constexpr const char *kTestFingerprint = "9F3A2C1B4D5E6F708192A3B4C5D6E7F8";
constexpr const char *kTestPayloadOk = "eyJlZGl0aW9uIjoicHJvIiwiZXhwIjowLCJmZWF0dXJlcyI6WyJlbmhhbmNlIiwiZnBzMTIwIl0sImZwIjoiOUYzQTJDMUI0RDVFNkY3MDgxOTJBM0I0QzVENkU3RjgiLCJpYXQiOjE3MDAwMDAwMDAsImxpZCI6IkxJQy1URVNULTAwMDEiLCJub25jZSI6ImFiY2QxMjM0IiwidiI6MX0";
constexpr const char *kTestSigOk = "m8GBBZuhFLMkyttB23bt7zW2VctkS8Eckce4BgJKshx78Boc-7JqfTQ815h9ZqIke7BHa-gBC_jpNiQPQxXbSpicbvtywy6a-tVVEVDstuZSAxU5JJSumcq3OG3LUDUCxsXKYDURBv4Xav4UKc4-FAq0hRpSMMvIWc2b0azua_LbTywUwQFPmqvDR_rF4S6mC932JsziF_GQ5x8elC0l0Chzg9F0puV5zKq3EniKIrzl-LDY95xgjqL7U2KVMuA2Ji6TB_furAqYTo3dSNM4E5K1-SMHPUTNq4on-Gewm4E50w3zuqIa0_grqU9JtUbrN6onQ5P4GLDTh0ApZujHvw";
constexpr const char *kTestPayloadExpired = "eyJlZGl0aW9uIjoicHJvIiwiZXhwIjoxNzAwMDAwMDAwLCJmZWF0dXJlcyI6WyJlbmhhbmNlIiwiZnBzMTIwIl0sImZwIjoiOUYzQTJDMUI0RDVFNkY3MDgxOTJBM0I0QzVENkU3RjgiLCJpYXQiOjE3MDAwMDAwMDAsImxpZCI6IkxJQy1URVNULUVYUElSRUQiLCJub25jZSI6ImFiY2QxMjM0IiwidiI6MX0";
constexpr const char *kTestSigExpired = "DPwSZ5KK1VM9-6Ua2s9zOzW1wRSkVN36_Ev7hL7thF4AlSEekcTR-VKE_8Kb7nj2AbkCGUA6zPKsN-RJ9Sl9ryztsHeQpyUzxgoIdXPuqDl_h1xZHKK-bG7hrz3e-G5zicGC5RnNCSgSsXl4ty8rRPi4bCujhHhXK9eCo63vTTCEqlF1bHdTNSylS6Xu5Q4P9bk_H20JTLuxJWAYU_QcmXSljUYhcGklw_ORk_sJoFCzT9I5vEoMmJtIZCm4QcJpgmz7K8bdC9Z6zY6y8wn_vvsnQlfWTUZizGSqLGZ1NQTsDTuUvfrBVJeYq8esLlP06Z6PPAHQiYux7CcIpVSZ3A";
constexpr const char *kTestPayloadVersion2 = "eyJlZGl0aW9uIjoicHJvIiwiZXhwIjowLCJmZWF0dXJlcyI6WyJlbmhhbmNlIiwiZnBzMTIwIl0sImZwIjoiOUYzQTJDMUI0RDVFNkY3MDgxOTJBM0I0QzVENkU3RjgiLCJpYXQiOjE3MDAwMDAwMDAsImxpZCI6IkxJQy1URVNULVYyIiwibm9uY2UiOiJhYmNkMTIzNCIsInYiOjJ9";
constexpr const char *kTestSigVersion2 = "K_zWbXfUMsqkB6l0KNGOsNYZG01bAHTIOa04vkonISSLe83L50YAN_5KldeXU-nzkaiuQYvtZeKHPDOR4n67eCb3QTwf45B_GkspkEfEOl9TkHZRuameHefd7R0cMJw9bdrNmFanINjPvbqDDT0ttpvhXcDTrvxebjezQyOAe2cWsPI7dbGC5zpc8SKtc3uR0doU77dVCM2bS-Rl6v3V0va8-jWIiGfFCID87T3gsKZX6t9IxpeZHz65sI5Z_EsmpuQImmo5umqbHfSGpAyiplINIuSsYLJHlq9Voxw01n6T8d-gBGNN9f1dRdNdkrfIcpkZ8pB4vSn9UDal1-Xaew";

QByteArray testLicenseJson(const char *payload, const char *sig) {
    QByteArray json = R"({"payload":")";
    json += payload;
    json += R"(","sig":")";
    json += sig;
    json += R"("})";
    return json;
}

// 用 tools/license-issuer 的生产私钥签发，验证「工具私钥 ↔ 客户端内置公钥」一致。
constexpr const char *kBuiltinLicenseJson =
    R"({"payload":"eyJlZGl0aW9uIjoicHJvIiwiZXhwIjowLCJmZWF0dXJlcyI6WyJlbmhhbmNlIiwic2NyZWVuT2ZmIiwicHJlc2V0cyIsImZwczEyMCIsIndpcmVsZXNzIiwicmVjb3JkIl0sImZwIjoiMTExMTIyMjIzMzMzNDQ0NDU1NTU2NjY2Nzc3Nzg4ODgiLCJpYXQiOjE3OTQ2NzAxNzcsImxpZCI6IkxJQy1CVUlMVElOLVRFU1QiLCJub25jZSI6ImI5YTRkMDI4IiwidiI6MX0","sig":"ilBlNeJfJJr6zG9h3mpAEg0lFEKjVWj55HTy55Q3XiLBGBFapNp-a8Al1lcSeSASWyHPkj-DWTwqtJ4cYSkoZyTJ1-YFoaunCpI4F2fRo70kE5MSMx_AjRPOjeCeu6oXnCruA5u7frLMFe78vLqdULxoxPNg0311xpd1ei8DUH7AN_O9Y1ZYZG0uOso9A_IAeavdfyi0s-oFe6iMKHadw6DXQgpp7YWFAoR0TolBgsOxIr2Lls47NOYg57a7huKKvpktd6xP13bo_N5gISK8L0Tw0PZx0d9REkto7iye3C0T-vxMn4ymaN4TXkTuuZjth-HknNoJxYf4YNr_pMjicg"})";

void testLicenseVerifier() {
    const QByteArray modulus = ks::LicenseVerifier::base64UrlDecode(kTestModulusB64);
    const QByteArray exponent = ks::LicenseVerifier::base64UrlDecode(kTestExponentB64);
    const QString fingerprint = QString::fromLatin1(kTestFingerprint);
    const qint64 now = 1800000000; // 晚于 iat、早于无

    auto verify = [&](const QByteArray &json, const QString &fp, qint64 lastSeen = 0) {
        return ks::LicenseVerifier::verifyWithKey(json, fp, now, lastSeen, modulus, exponent);
    };

    const ks::LicenseCheck ok = verify(testLicenseJson(kTestPayloadOk, kTestSigOk), fingerprint);
    check(ok.ok(), "license: valid file accepted");
    check(ok.payload.licenseId == QStringLiteral("LIC-TEST-0001"), "license: license id parsed");
    check(ok.payload.features.size() == 2, "license: features parsed");
    check(ok.payload.expiresAt == 0, "license: permanent license");

    // 篡改 payload（改一个字符）
    QByteArray tamperedPayload = QByteArray(kTestPayloadOk);
    tamperedPayload[10] = tamperedPayload.at(10) == 'A' ? 'B' : 'A';
    check(verify(testLicenseJson(tamperedPayload.constData(), kTestSigOk), fingerprint).error ==
              ks::LicenseError::BadSignature,
          "license: tampered payload rejected");

    // 篡改签名
    QByteArray tamperedSig = QByteArray(kTestSigOk);
    tamperedSig[5] = tamperedSig.at(5) == 'A' ? 'B' : 'A';
    check(verify(testLicenseJson(kTestPayloadOk, tamperedSig.constData()), fingerprint).error ==
              ks::LicenseError::BadSignature,
          "license: tampered signature rejected");

    // 指纹不匹配
    check(verify(testLicenseJson(kTestPayloadOk, kTestSigOk),
                 QStringLiteral("00000000000000000000000000000000"))
                  .error == ks::LicenseError::FingerprintMismatch,
          "license: other machine rejected");

    // 指纹归一化：带分隔符也能匹配
    check(verify(testLicenseJson(kTestPayloadOk, kTestSigOk),
                 QStringLiteral("9f3a-2c1b-4d5e-6f70-8192-a3b4-c5d6-e7f8"))
              .ok(),
          "license: fingerprint separators tolerated");

    // 过期
    check(verify(testLicenseJson(kTestPayloadExpired, kTestSigExpired), fingerprint).error ==
              ks::LicenseError::Expired,
          "license: expired rejected");

    // 未知版本
    check(verify(testLicenseJson(kTestPayloadVersion2, kTestSigVersion2), fingerprint).error ==
              ks::LicenseError::UnsupportedVersion,
          "license: unsupported version rejected");

    // 结构错误
    check(verify(QByteArray("not json"), fingerprint).error == ks::LicenseError::Malformed,
          "license: non-json rejected");
    check(verify(QByteArray(R"({"payload":"abc"})"), fingerprint).error ==
              ks::LicenseError::Malformed,
          "license: missing signature rejected");
    check(verify(QByteArray(), fingerprint).error == ks::LicenseError::FileMissing,
          "license: empty content reports missing");

    // 时钟回拨：系统时间比上次可信时间倒退超过 24h
    check(verify(testLicenseJson(kTestPayloadOk, kTestSigOk), fingerprint,
                 now + 2 * 24 * 60 * 60)
                  .error == ks::LicenseError::ClockRollback,
          "license: clock rollback rejected");
    check(verify(testLicenseJson(kTestPayloadOk, kTestSigOk), fingerprint, now - 60).ok(),
          "license: small clock skew tolerated");

    // base64url 往返
    const QByteArray raw = QByteArray::fromHex("00fbff1020304050");
    QByteArray encoded = raw.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
    check(ks::LicenseVerifier::base64UrlDecode(encoded) == raw, "license: base64url roundtrip");

    // 内置公钥与发码工具私钥必须匹配（否则正式签发的许可证全部无法验证）
    const ks::LicenseCheck builtin =
        ks::LicenseVerifier::verify(QByteArray(kBuiltinLicenseJson),
                                    QStringLiteral("11112222333344445555666677778888"), 1900000000, 0);
    check(builtin.ok(), "license: builtin public key matches issuer");
    check(builtin.payload.licenseId == QStringLiteral("LIC-BUILTIN-TEST"),
          "license: builtin license id parsed");
}

// ---------------------------------------------------------------------------
void testKeymap() {
    ks::HumanizerEngine humanizer;
    humanizer.setEnabled(false); // 直通测试使用确定坐标
    ks::KeymapEngine keymap;
    ks::KeyBinding binding;
    binding.vk = 0x20;
    binding.center = QPointF(0.5, 0.5);
    binding.radius = 0.05;
    ks::KeyBinding second;
    second.vk = 0x41;
    second.center = QPointF(0.6, 0.5);
    second.radius = 0.05;
    keymap.configure({binding, second}, 1000, 1000);
    keymap.setHumanizer(&humanizer);

    std::vector<QByteArray> messages;
    auto sink = [&messages](const QByteArray &message) { messages.push_back(message); };

    // 直通：按下立即 DOWN
    check(keymap.handleKey(0x20, true, sink), "keymap handles bound key");
    check(!messages.empty() && static_cast<quint8>(messages.front().at(1)) == 0,
          "keymap sends DOWN on press");
    check(!messages.empty() && read32(messages.front(), 10) == 500 &&
              read32(messages.front(), 14) == 500,
          "keymap DOWN at binding center");
    const int firstPointer = static_cast<int>(read64(messages.front(), 2));

    // 直通：按住期间不自动抬起（无 tick 合成时序）
    messages.clear();
    check(keymap.handleKey(0x20, true, sink), "repeat press consumed");
    check(messages.empty(), "repeat press does not re-send DOWN");

    // 多键同按 = 多指：第二个绑定分配不同 pointerId
    messages.clear();
    check(keymap.handleKey(0x41, true, sink), "second key handled");
    check(messages.size() == 1 && static_cast<quint8>(messages.front().at(1)) == 0,
          "second key sends DOWN");
    const int secondPointer = static_cast<int>(read64(messages.front(), 2));
    check(firstPointer != secondPointer, "multi-key uses distinct pointers");

    // 抬起立即 UP
    messages.clear();
    check(keymap.handleKey(0x20, false, sink), "keymap handles release");
    check(!messages.empty() && static_cast<quint8>(messages.back().at(1)) == 1,
          "keymap sends UP on release");
    messages.clear();
    check(!keymap.handleKey(0x20, false, sink), "release without active action reports false");
    check(messages.empty(), "release without active action sends nothing");
}

// ---------------------------------------------------------------------------
void testJoystickSprint() {
    ks::HumanizerEngine humanizer;
    humanizer.setEnabled(false);
    ks::JoystickEngine joystick;
    joystick.setHumanizer(&humanizer);
    joystick.configure(QPointF(0.5, 0.5), 0.10, 0.0, 1.0, 1000, 1000);
    joystick.setSprintRadius(0.20);

    std::vector<QByteArray> messages;
    auto sink = [&messages](const QByteArray &message) { messages.push_back(message); };
    auto maxOffsetX = [&messages]() {
        int maxX = 0;
        for (const QByteArray &message : messages) {
            const quint8 action = static_cast<quint8>(message.at(1));
            if (action == 0 || action == 2) { // DOWN / MOVE
                maxX = std::max(maxX, static_cast<int>(read32(message, 10)) - 500);
            }
        }
        return maxX;
    };

    // 普通态：满力推到小圆边缘（0.10 × 1000 = 100px）
    joystick.setKeys(false, false, false, true); // D → +x
    joystick.tick(16, sink);
    check(maxOffsetX() >= 99 && maxOffsetX() <= 101, "normal push limited to inner circle");

    // 疾跑态：同一力度可滑到大圆边缘（0.20 × 1000 = 200px）
    joystick.setSprint(true);
    messages.clear();
    joystick.tick(16, sink);
    check(maxOffsetX() >= 199 && maxOffsetX() <= 201, "sprint push reaches outer circle");

    // 关闭疾跑：触点回落小圆
    joystick.setSprint(false);
    messages.clear();
    joystick.tick(16, sink);
    check(maxOffsetX() <= 101, "sprint off pulls touch back to inner circle");

    joystick.release(sink);
}

// ---------------------------------------------------------------------------
void testWheelBinding() {
    ks::HumanizerEngine humanizer;
    humanizer.setEnabled(false);
    ks::KeymapEngine keymap;
    ks::KeyBinding wheelUp;
    wheelUp.vk = ks::kVkWheelUp;
    wheelUp.center = QPointF(0.5, 0.5);
    wheelUp.radius = 0.05;
    keymap.configure({wheelUp}, 1000, 1000);
    keymap.setHumanizer(&humanizer);

    std::vector<QByteArray> messages;
    auto sink = [&messages](const QByteArray &message) { messages.push_back(message); };

    // 未绑定方向：滚轮不注入任何消息（不再发送居中滚动）
    check(!keymap.handleWheel(-1, sink), "unbound wheel direction reports false");
    check(messages.empty(), "unbound wheel injects nothing");

    // 已绑定方向：按下 → 延时抬起（模拟一次点按）
    check(keymap.handleWheel(1, sink), "bound wheel triggers");
    check(messages.size() == 1 && static_cast<quint8>(messages.front().at(1)) == 0,
          "bound wheel sends DOWN");
    check(!messages.empty() && read32(messages.front(), 10) == 500 &&
              read32(messages.front(), 14) == 500,
          "bound wheel DOWN at binding center");

    messages.clear();
    for (int i = 0; i < 6; ++i) {
        keymap.tick(10, sink);
    }
    check(!messages.empty() && static_cast<quint8>(messages.back().at(1)) == 1,
          "bound wheel sends UP after hold");
}

// ---------------------------------------------------------------------------
void testKeySuspension() {
    ks::KeymapEngine keymap;
    ks::KeyBinding binding;
    binding.vk = 0x20;
    binding.center = QPointF(0.5, 0.5);
    binding.radius = 0.05;
    keymap.configure({binding}, 1000, 1000);

    std::vector<QByteArray> messages;
    auto sink = [&messages](const QByteArray &message) { messages.push_back(message); };

    keymap.handleKey(0x20, true, sink);
    check(!messages.empty(), "press before releaseAll sends DOWN");

    messages.clear();
    keymap.releaseAll(sink); // 呼出鼠标：抬起全部触点
    check(!messages.empty() && static_cast<quint8>(messages.back().at(1)) == 1,
          "releaseAll lifts active key");

    messages.clear();
    check(keymap.handleKey(0x20, true, sink), "held key auto-repeat consumed");
    check(messages.empty(), "auto-repeat does not re-press after releaseAll");

    check(!keymap.handleKey(0x20, false, sink), "release of suspended key has no action");
    messages.clear();
    check(keymap.handleKey(0x20, true, sink), "re-press works after physical release");
    check(!messages.empty() && static_cast<quint8>(messages.back().at(1)) == 0,
          "re-press sends DOWN again");
    keymap.releaseAll(sink);
}

// ---------------------------------------------------------------------------
void testLookGating() {
    ks::InputEngine engine;
    engine.setScreenSize(1000, 1000);

    std::vector<QByteArray> messages;
    auto sink = [&messages](const QByteArray &message) { messages.push_back(message); };

    ks::InputEvent move;
    move.type = ks::InputEventType::MouseMove;
    move.dx = 200;

    // 指针模式：鼠标位移不驱动视角
    engine.setLookEnabled(false);
    engine.handleInput(move);
    engine.tick(10, sink);
    check(messages.empty(), "pointer mode suppresses look movement");

    engine.setLookEnabled(true);
    engine.handleInput(move);
    engine.tick(10, sink);
    check(!messages.empty(), "look resumes when enabled");

    messages.clear();
    engine.handleInput(move);
    engine.setLookEnabled(false);
    engine.tick(10, sink);
    check(messages.empty(), "disabling look drops queued movement");
    engine.setLookEnabled(true);
    engine.tick(10, sink);
    check(messages.empty(), "re-enabling look does not replay stale movement");
}

// ---------------------------------------------------------------------------
void testSummonKeyFilter() {
    check(ks::vkMatchesSummon(VK_LMENU, VK_MENU), "summon key matches left Alt");
    check(ks::vkMatchesSummon(VK_RCONTROL, VK_CONTROL), "summon key matches right Ctrl");
    check(!ks::vkMatchesSummon(VK_LBUTTON, VK_CONTROL), "summon key ignores other keys");

    ks::Profile profile;
    ks::KeyBinding binding;
    binding.vk = VK_LMENU;
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
void testDirectTouchFilter() {
    ks::Profile profile;
    ks::KeyBinding binding;
    binding.vk = 0x01; // VK_LBUTTON
    binding.center = QPointF(0.5, 0.5);
    binding.radius = 0.05;
    profile.bindings.append(binding);

    ks::InputEngine engine;
    engine.setScreenSize(1000, 1000);
    engine.setProfile(profile);

    std::vector<QByteArray> messages;
    auto sink = [&messages](const QByteArray &message) { messages.push_back(message); };

    ks::InputEvent down;
    down.type = ks::InputEventType::MouseButton;
    down.button = 0;
    down.down = true;

    engine.setDirectTouchEnabled(true);
    engine.handleInput(down);
    engine.tick(10, sink);
    check(messages.empty(), "direct touch drops left button mapping");

    engine.setDirectTouchEnabled(false);
    engine.handleInput(down);
    engine.tick(10, sink);
    check(!messages.empty(), "left button maps again after direct touch off");

    engine.releaseAll(sink);
    messages.clear();
    ks::InputEvent move;
    move.type = ks::InputEventType::MouseMove;
    move.dx = 200;
    engine.setDirectTouchEnabled(true);
    engine.setTouchDragActive(true);
    engine.handleInput(move);
    engine.tick(10, sink);
    check(messages.empty(), "direct touch drag suppresses look move");

    engine.setTouchDragActive(false);
    engine.handleInput(move);
    engine.tick(10, sink);
    check(!messages.empty(), "look resumes after drag ends");
}

// ---------------------------------------------------------------------------
void testRegionJitter() {
    ks::HumanizerEngine humanizer;
    std::vector<QByteArray> messages;
    auto sink = [&messages](const QByteArray &message) { messages.push_back(message); };
    auto collectDown = [&messages](std::set<std::pair<int, int>> *points) {
        for (const QByteArray &message : messages) {
            if (static_cast<quint8>(message.at(1)) == 0) {
                points->insert({static_cast<int>(read32(message, 10)),
                                static_cast<int>(read32(message, 14))});
            }
        }
    };

    ks::JoystickEngine joystick;
    joystick.configure(QPointF(0.2, 0.8), 0.1, 0.0, 1.0, 1080, 2400);
    joystick.setHumanizer(&humanizer);

    std::set<std::pair<int, int>> points;
    for (int i = 0; i < 12; ++i) {
        joystick.reset();
        joystick.setKeys(false, false, false, true);
        messages.clear();
        joystick.tick(16, sink);
        collectDown(&points);
    }
    check(points.size() > 1, "joystick center jitter varies across activations");

    humanizer.setEnabled(false);
    points.clear();
    for (int i = 0; i < 5; ++i) {
        joystick.reset();
        joystick.setKeys(false, false, false, true);
        messages.clear();
        joystick.tick(16, sink);
        collectDown(&points);
    }
    check(points.size() == 1, "joystick center fixed when humanizer disabled");
    check(!points.empty() && points.begin()->first == 216 && points.begin()->second == 1920,
          "joystick center matches configured point");

    humanizer.setEnabled(true);
    ks::LookEngine look;
    look.configure(QPointF(0.7, 0.5), 0.2, 1.0, 3000.0, 1080, 2400);
    look.setHumanizer(&humanizer);
    points.clear();
    for (int i = 0; i < 12; ++i) {
        look.reset();
        look.addDelta(100, 0, 0);
        messages.clear();
        look.tick(16, sink);
        collectDown(&points);
    }
    check(points.size() > 1, "look center jitter varies across activations");

    humanizer.setEnabled(false);
    points.clear();
    for (int i = 0; i < 5; ++i) {
        look.reset();
        look.addDelta(100, 0, 0);
        messages.clear();
        look.tick(16, sink);
        collectDown(&points);
    }
    check(points.size() == 1, "look center fixed when humanizer disabled");
    check(!points.empty() && points.begin()->first == 756 && points.begin()->second == 1200,
          "look center matches configured point");
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
    check(std::abs(loaded.joystickSprintRadius - profile.joystickSprintRadius) < 0.0001,
          "sprint radius roundtrip");

    // 小圆 > 大圆：读取时钳制 sprintRadius ≥ joystickRadius
    ks::Profile clamped = ks::ProfileStore::makeDefaultProfile();
    clamped.name = QStringLiteral("测试疾跑钳制");
    clamped.joystickRadius = 0.20;
    clamped.joystickSprintRadius = 0.05;
    check(store.save(clamped, &error), "clamped profile save");
    const ks::Profile clampedLoaded = store.load(QStringLiteral("测试疾跑钳制"), &ok);
    check(ok && clampedLoaded.joystickSprintRadius >= clampedLoaded.joystickRadius - 1e-6,
          "sprint radius clamped to inner radius on load");

    store.remove(QStringLiteral("测试方案"));
    store.remove(QStringLiteral("测试疾跑钳制"));
}

// ---------------------------------------------------------------------------
void testDefaultProfile() {
    const ks::Profile profile = ks::ProfileStore::makeDefaultProfile();
    check(!profile.humanizerEnabled, "humanizer is disabled by default");
}

} // namespace

int main(int argc, char *argv[]) {
    QCoreApplication app(argc, argv);
    QStandardPaths::setTestModeEnabled(true);

    testProtocol();
    testJoystick();
    testJoystickSprint();
    testWasdJoystick();
    testLookEngine();
    testLookEllipseBoundary();
    testLookRecenterTiming();
    testMouseButtonVk();
    testKeymap();
    testWheelBinding();
    testKeySuspension();
    testLookGating();
    testSummonKeyFilter();
    testDirectTouchFilter();
    testRegionJitter();
    testCalibrator();
    testLicenseVerifier();
    testProfileStore();
    testDefaultProfile();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
