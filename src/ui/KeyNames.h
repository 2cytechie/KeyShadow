#pragma once

#include <QString>

class QKeyEvent;

namespace ks::ui {

/// Windows 虚拟键码 → 可读名称（修饰键左右区分，鼠标键单列）。
QString vkName(int vk);

/// 由 Qt 按键事件解析出 Windows 虚拟键码（修饰键合并为通用 VK）。
int vkFromKeyEvent(const QKeyEvent &event);

/// 呼出键禁止列表：Esc、F9/F10/F11、鼠标左键、Win。
bool isForbiddenSummonKey(int vk);

} // namespace ks::ui
