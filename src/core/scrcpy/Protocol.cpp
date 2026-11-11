#include "core/scrcpy/Protocol.h"

#include <algorithm>
#include <cmath>

namespace ks::proto {
namespace {

void put8(QByteArray &buf, quint8 v) { buf.append(static_cast<char>(v)); }

void put16(QByteArray &buf, quint16 v) {
    buf.append(static_cast<char>((v >> 8) & 0xFF));
    buf.append(static_cast<char>(v & 0xFF));
}

void put32(QByteArray &buf, quint32 v) {
    buf.append(static_cast<char>((v >> 24) & 0xFF));
    buf.append(static_cast<char>((v >> 16) & 0xFF));
    buf.append(static_cast<char>((v >> 8) & 0xFF));
    buf.append(static_cast<char>(v & 0xFF));
}

void put64(QByteArray &buf, quint64 v) {
    for (int i = 7; i >= 0; --i) {
        buf.append(static_cast<char>((v >> (i * 8)) & 0xFF));
    }
}

/// 与 scrcpy util/binary.h 的 sc_float_to_u16fp 保持一致。
quint16 floatToU16Fixed(float f) {
    f = std::clamp(f, 0.0f, 1.0f);
    quint32 u = static_cast<quint32>(f * 65536.0f);
    if (u >= 0xFFFFu) {
        u = 0xFFFFu;
    }
    return static_cast<quint16>(u);
}

/// 与 scrcpy util/binary.h 的 sc_float_to_i16fp 保持一致。
qint16 floatToI16Fixed(float f) {
    f = std::clamp(f, -1.0f, 1.0f);
    qint32 i = static_cast<qint32>(f * 32768.0f);
    if (i >= 0x7FFF) {
        i = 0x7FFF;
    } else if (i < -0x8000) {
        i = -0x8000;
    }
    return static_cast<qint16>(i);
}

} // namespace

QByteArray touch(TouchAction action, quint64 pointerId, int x, int y,
                 int screenWidth, int screenHeight, float pressure,
                 qint32 actionButton, qint32 buttons) {
    QByteArray buf;
    buf.reserve(32);
    put8(buf, static_cast<quint8>(MsgType::InjectTouch));
    put8(buf, static_cast<quint8>(action));
    put64(buf, pointerId);
    put32(buf, static_cast<quint32>(x));
    put32(buf, static_cast<quint32>(y));
    put16(buf, static_cast<quint16>(screenWidth));
    put16(buf, static_cast<quint16>(screenHeight));
    put16(buf, floatToU16Fixed(pressure));
    put32(buf, static_cast<quint32>(actionButton));
    put32(buf, static_cast<quint32>(buttons));
    return buf;
}

QByteArray keycode(KeyAction action, qint32 keycode, quint32 repeat,
                   quint32 metaState) {
    QByteArray buf;
    buf.reserve(14);
    put8(buf, static_cast<quint8>(MsgType::InjectKeycode));
    put8(buf, static_cast<quint8>(action));
    put32(buf, static_cast<quint32>(keycode));
    put32(buf, repeat);
    put32(buf, metaState);
    return buf;
}

QByteArray scroll(int x, int y, int screenWidth, int screenHeight, float hscroll,
                  float vscroll, qint32 buttons) {
    QByteArray buf;
    buf.reserve(21);
    put8(buf, static_cast<quint8>(MsgType::InjectScroll));
    put32(buf, static_cast<quint32>(x));
    put32(buf, static_cast<quint32>(y));
    put16(buf, static_cast<quint16>(screenWidth));
    put16(buf, static_cast<quint16>(screenHeight));
    const float hn = std::clamp(hscroll / 16.0f, -1.0f, 1.0f);
    const float vn = std::clamp(vscroll / 16.0f, -1.0f, 1.0f);
    put16(buf, static_cast<quint16>(floatToI16Fixed(hn)));
    put16(buf, static_cast<quint16>(floatToI16Fixed(vn)));
    put32(buf, static_cast<quint32>(buttons));
    return buf;
}

QByteArray displayPower(bool on) {
    QByteArray buf;
    put8(buf, static_cast<quint8>(MsgType::SetDisplayPower));
    put8(buf, on ? 1 : 0);
    return buf;
}

QByteArray backOrScreenOn(KeyAction action) {
    QByteArray buf;
    put8(buf, static_cast<quint8>(MsgType::BackOrScreenOn));
    put8(buf, static_cast<quint8>(action));
    return buf;
}

QByteArray rotateDevice() {
    QByteArray buf;
    put8(buf, static_cast<quint8>(MsgType::RotateDevice));
    return buf;
}

QString msgTypeName(MsgType type) {
    switch (type) {
    case MsgType::InjectKeycode: return QStringLiteral("inject_keycode");
    case MsgType::InjectText: return QStringLiteral("inject_text");
    case MsgType::InjectTouch: return QStringLiteral("inject_touch");
    case MsgType::InjectScroll: return QStringLiteral("inject_scroll");
    case MsgType::BackOrScreenOn: return QStringLiteral("back_or_screen_on");
    case MsgType::SetDisplayPower: return QStringLiteral("set_display_power");
    case MsgType::RotateDevice: return QStringLiteral("rotate_device");
    default: return QStringLiteral("other");
    }
}

} // namespace ks::proto
