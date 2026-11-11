#include "ui/KeyNames.h"

#include <QKeyEvent>

#include <qt_windows.h>

namespace ks::ui {

QString vkName(int vk) {
    if (vk >= 'A' && vk <= 'Z') {
        return QString(QChar(vk));
    }
    if (vk >= '0' && vk <= '9') {
        return QString(QChar(vk));
    }
    if (vk >= VK_F1 && vk <= VK_F12) {
        return QStringLiteral("F%1").arg(vk - VK_F1 + 1);
    }
    switch (vk) {
    case VK_LBUTTON: return QStringLiteral("左键");
    case VK_RBUTTON: return QStringLiteral("右键");
    case VK_MBUTTON: return QStringLiteral("中键");
    case VK_SPACE: return QStringLiteral("Space");
    case VK_SHIFT: return QStringLiteral("Shift");
    case VK_LSHIFT: return QStringLiteral("左 Shift");
    case VK_RSHIFT: return QStringLiteral("右 Shift");
    case VK_CONTROL: return QStringLiteral("Ctrl");
    case VK_LCONTROL: return QStringLiteral("左 Ctrl");
    case VK_RCONTROL: return QStringLiteral("右 Ctrl");
    case VK_MENU: return QStringLiteral("Alt");
    case VK_LMENU: return QStringLiteral("左 Alt");
    case VK_RMENU: return QStringLiteral("右 Alt");
    case VK_TAB: return QStringLiteral("Tab");
    case VK_RETURN: return QStringLiteral("Enter");
    case VK_ESCAPE: return QStringLiteral("Esc");
    case VK_BACK: return QStringLiteral("Backspace");
    case VK_CAPITAL: return QStringLiteral("CapsLock");
    case VK_DELETE: return QStringLiteral("Delete");
    case VK_INSERT: return QStringLiteral("Insert");
    case VK_HOME: return QStringLiteral("Home");
    case VK_END: return QStringLiteral("End");
    case VK_PRIOR: return QStringLiteral("PageUp");
    case VK_NEXT: return QStringLiteral("PageDown");
    case VK_LEFT: return QStringLiteral("←");
    case VK_UP: return QStringLiteral("↑");
    case VK_RIGHT: return QStringLiteral("→");
    case VK_DOWN: return QStringLiteral("↓");
    case VK_OEM_1: return QStringLiteral(";");
    case VK_OEM_2: return QStringLiteral("/");
    case VK_OEM_3: return QStringLiteral("`");
    case VK_OEM_4: return QStringLiteral("[");
    case VK_OEM_5: return QStringLiteral("\\");
    case VK_OEM_6: return QStringLiteral("]");
    case VK_OEM_7: return QStringLiteral("'");
    case VK_OEM_PLUS: return QStringLiteral("=");
    case VK_OEM_COMMA: return QStringLiteral(",");
    case VK_OEM_MINUS: return QStringLiteral("-");
    case VK_OEM_PERIOD: return QStringLiteral(".");
    case VK_LWIN: return QStringLiteral("左 Win");
    case VK_RWIN: return QStringLiteral("右 Win");
    default: break;
    }
    return QStringLiteral("VK%1").arg(vk, 2, 16, QLatin1Char('0')).toUpper();
}

int vkFromKeyEvent(const QKeyEvent &event) {
    switch (event.key()) {
    case Qt::Key_Alt:
    case Qt::Key_AltGr:
        return VK_MENU;
    case Qt::Key_Control:
        return VK_CONTROL;
    case Qt::Key_Shift:
        return VK_SHIFT;
    case Qt::Key_Meta:
        return VK_LWIN;
    case Qt::Key_Tab:
        return VK_TAB;
    case Qt::Key_CapsLock:
        return VK_CAPITAL;
    case Qt::Key_Space:
        return VK_SPACE;
    default:
        break;
    }
    if (event.key() >= Qt::Key_F1 && event.key() <= Qt::Key_F12) {
        return VK_F1 + (event.key() - Qt::Key_F1);
    }
    if (event.key() >= Qt::Key_A && event.key() <= Qt::Key_Z) {
        return 'A' + (event.key() - Qt::Key_A);
    }
    if (event.key() >= Qt::Key_0 && event.key() <= Qt::Key_9) {
        return '0' + (event.key() - Qt::Key_0);
    }
    const int native = event.nativeVirtualKey();
    if (native > 0) {
        return native;
    }
    return event.key() & 0xFF;
}

bool isForbiddenSummonKey(int vk) {
    switch (vk) {
    case VK_ESCAPE:
    case VK_F9:
    case VK_F10:
    case VK_F11:
    case VK_LBUTTON:
    case VK_LWIN:
    case VK_RWIN:
        return true;
    default:
        return false;
    }
}

} // namespace ks::ui
