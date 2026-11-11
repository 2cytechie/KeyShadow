#include "core/input/RawInput.h"

#include <qt_windows.h>

namespace ks {

RawInputFilter::RawInputFilter(EventHandler handler) : m_handler(std::move(handler)) {}

RawInputFilter::~RawInputFilter() = default;

void RawInputFilter::attachTo(quintptr hwnd) {
    m_hwnd = hwnd;
    if (!m_hwnd) {
        return;
    }
    RAWINPUTDEVICE devices[2]{};
    devices[0].usUsagePage = 0x01; // Generic Desktop
    devices[0].usUsage = 0x02;     // Mouse
    devices[0].dwFlags = 0;        // 仅在窗口聚焦时接收
    devices[0].hwndTarget = reinterpret_cast<HWND>(m_hwnd);

    devices[1].usUsagePage = 0x01;
    devices[1].usUsage = 0x06; // Keyboard
    devices[1].dwFlags = 0;
    devices[1].hwndTarget = reinterpret_cast<HWND>(m_hwnd);

    RegisterRawInputDevices(devices, 2, sizeof(RAWINPUTDEVICE));
}

bool RawInputFilter::nativeEventFilter(const QByteArray &eventType, void *message, qintptr *result) {
    (void)result;
    if (!m_enabled || eventType != "windows_generic_MSG" || !message) {
        return false;
    }

    auto *msg = static_cast<MSG *>(message);
    if (msg->message != WM_INPUT) {
        return false;
    }

    RAWINPUT raw{};
    UINT size = sizeof(raw);
    const UINT read = GetRawInputData(reinterpret_cast<HRAWINPUT>(msg->lParam), RID_INPUT, &raw,
                                      &size, sizeof(RAWINPUTHEADER));
    if (read == static_cast<UINT>(-1) || read == 0) {
        return false;
    }

    if (raw.header.dwType == RIM_TYPEMOUSE) {
        const RAWMOUSE &mouse = raw.data.mouse;
        if (!(mouse.usFlags & MOUSE_MOVE_ABSOLUTE)) {
            if (mouse.lLastX != 0 || mouse.lLastY != 0) {
                InputEvent event;
                event.type = InputEventType::MouseMove;
                event.dx = mouse.lLastX;
                event.dy = mouse.lLastY;
                if (m_handler) {
                    m_handler(event);
                }
            }
        }

        const USHORT flags = mouse.usButtonFlags;
        auto emitButton = [this](int button, bool down) {
            InputEvent event;
            event.type = InputEventType::MouseButton;
            event.button = button;
            event.down = down;
            if (m_handler) {
                m_handler(event);
            }
        };

        if (flags & RI_MOUSE_LEFT_BUTTON_DOWN) {
            emitButton(0, true);
        }
        if (flags & RI_MOUSE_LEFT_BUTTON_UP) {
            emitButton(0, false);
        }
        if (flags & RI_MOUSE_RIGHT_BUTTON_DOWN) {
            emitButton(1, true);
        }
        if (flags & RI_MOUSE_RIGHT_BUTTON_UP) {
            emitButton(1, false);
        }
        if (flags & RI_MOUSE_MIDDLE_BUTTON_DOWN) {
            emitButton(2, true);
        }
        if (flags & RI_MOUSE_MIDDLE_BUTTON_UP) {
            emitButton(2, false);
        }
        // 滚轮不在此处理：滚轮由投屏画布的 Qt 事件按上下文门控注入（R6）
    } else if (raw.header.dwType == RIM_TYPEKEYBOARD) {
        const RAWKEYBOARD &keyboard = raw.data.keyboard;
        if (keyboard.VKey == 0 || keyboard.VKey == 0xFF) {
            return false;
        }
        const bool down = (keyboard.Flags & RI_KEY_BREAK) == 0;
        const bool extended = (keyboard.Flags & RI_KEY_E0) != 0;

        int vk = keyboard.VKey;
        // 左右 Ctrl/Alt 区分
        if (vk == VK_CONTROL) {
            vk = extended ? VK_RCONTROL : VK_LCONTROL;
        } else if (vk == VK_MENU) {
            vk = extended ? VK_RMENU : VK_LMENU;
        } else if (vk == VK_SHIFT) {
            const UINT scancode = MapVirtualKey(VK_SHIFT, MAPVK_VK_TO_VSC);
            vk = (keyboard.MakeCode == scancode) ? VK_LSHIFT : VK_RSHIFT;
        }

        InputEvent event;
        event.type = InputEventType::Key;
        event.vk = vk;
        event.down = down;
        if (m_handler) {
            m_handler(event);
        }
    }

    return false;
}

QPoint RawInputFilter::cursorClientPos() const {
    if (!m_hwnd) {
        return {};
    }
    POINT pos{};
    if (!GetCursorPos(&pos)) {
        return {};
    }
    ScreenToClient(reinterpret_cast<HWND>(m_hwnd), &pos);
    return QPoint(pos.x, pos.y);
}

} // namespace ks
