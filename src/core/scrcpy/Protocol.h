#pragma once

#include <QByteArray>
#include <QString>
#include <QtGlobal>

/// scrcpy-server 控制协议（对应 scrcpy v5.0.1 的 control_msg.h）。
namespace ks::proto {

enum class MsgType : quint8 {
    InjectKeycode = 0,
    InjectText = 1,
    InjectTouch = 2,
    InjectScroll = 3,
    BackOrScreenOn = 4,
    ExpandNotificationPanel = 5,
    ExpandSettingsPanel = 6,
    CollapsePanels = 7,
    GetClipboard = 8,
    SetClipboard = 9,
    SetDisplayPower = 10,
    RotateDevice = 11,
};

enum class TouchAction : quint8 {
    Down = 0,
    Up = 1,
    Move = 2,
    Cancel = 3,
    Outside = 4,
    PointerDown = 5,
    PointerUp = 6,
};

enum class KeyAction : quint8 { Down = 0, Up = 1 };

/// Android MotionEvent 按键位（actionButton / buttons 字段）。
enum Button : qint32 {
    ButtonNone = 0,
    ButtonPrimary = 1,
    ButtonSecondary = 2,
    ButtonTertiary = 4,
};

/// 特殊 pointer id 约定（负数按 uint64 解释）。
constexpr quint64 kPointerMouse = static_cast<quint64>(-1);
constexpr quint64 kPointerFinger = static_cast<quint64>(-2);

/// 视频/音频流包头标识（demuxer 解析用）。
constexpr quint64 kPacketFlagSession = Q_UINT64_C(1) << 63;
constexpr quint64 kPacketFlagConfig = Q_UINT64_C(1) << 62;
constexpr quint64 kPacketFlagKeyFrame = Q_UINT64_C(1) << 61;
constexpr quint64 kPacketPtsMask = kPacketFlagKeyFrame - 1;
constexpr int kPacketHeaderSize = 12;
constexpr int kDeviceNameFieldLength = 64;

/// 触摸事件封包（32 字节）。
QByteArray touch(TouchAction action, quint64 pointerId, int x, int y,
                 int screenWidth, int screenHeight, float pressure = 1.0f,
                 qint32 actionButton = ButtonNone, qint32 buttons = ButtonNone);

/// 按键事件封包（14 字节）。
QByteArray keycode(KeyAction action, qint32 keycode, quint32 repeat = 0,
                   quint32 metaState = 0);

/// 滚轮事件封包（21 字节）。
QByteArray scroll(int x, int y, int screenWidth, int screenHeight,
                  float hscroll, float vscroll, qint32 buttons = ButtonNone);

/// 屏幕电源（息屏投屏，2 字节）。
QByteArray displayPower(bool on);

/// 返回键 / 点亮屏幕（2 字节）。
QByteArray backOrScreenOn(KeyAction action);

/// 发起旋转（1 字节）。
QByteArray rotateDevice();

QString msgTypeName(MsgType type);

} // namespace ks::proto
