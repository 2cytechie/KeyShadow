#pragma once

#include <QAbstractNativeEventFilter>
#include <QPoint>
#include <QtGlobal>
#include <functional>

#include "core/engine/Engines.h"

namespace ks {

/// 基于 Win32 Raw Input 的输入捕获（绕过系统指针加速，读取硬件原始位移）。
/// 注册在应用主窗口上：仅窗口获得焦点时接收，避免全局钩子类安全软件误报。
class RawInputFilter : public QAbstractNativeEventFilter {
public:
    using EventHandler = std::function<void(const InputEvent &)>;

    explicit RawInputFilter(EventHandler handler);
    ~RawInputFilter() override;

    void attachTo(quintptr hwnd);
    void setEnabled(bool enabled) { m_enabled = enabled; }
    bool isEnabled() const { return m_enabled; }

    bool nativeEventFilter(const QByteArray &eventType, void *message, qintptr *result) override;

    /// 物理光标在本窗口客户区的坐标（呼出鼠标模式下的绝对定位用）。
    QPoint cursorClientPos() const;

private:
    quintptr m_hwnd = 0;
    EventHandler m_handler;
    bool m_enabled = true;
};

} // namespace ks
