#pragma once

#include <QColor>
#include <QIcon>
#include <QPixmap>
#include <QString>

namespace ks::ui {

/// 轻量线性图标工厂：用 QPainterPath 绘制，避免引入图标资源与额外依赖。
class IconFactory {
public:
    static QIcon icon(const QString &name, const QColor &color, int size = 20);
    static QPixmap pixmap(const QString &name, const QColor &color, int size = 20);

    /// 应用图标（键影：半影键帽）。
    static QPixmap appIcon(int size = 20);
};

} // namespace ks::ui
