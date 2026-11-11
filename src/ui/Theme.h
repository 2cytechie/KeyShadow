#pragma once

#include <QColor>
#include <QFont>
#include <QString>

class QApplication;

namespace ks::ui {

/// 主题令牌访问（与 theme/tokens.json 保持一致）。
class Theme {
public:
    static void apply(QApplication &app);
    static QColor color(const QString &token);
    static QFont monoFont(int pixelSize = 12, int weight = QFont::Normal);
    static QFont uiFont(int pixelSize = 13, int weight = QFont::Normal);

    // 常用色
    static QColor bgApp();
    static QColor bgSurface();
    static QColor bgElevated();
    static QColor textPrimary();
    static QColor textSecondary();
    static QColor brandPrimary();
    static QColor brandPro();
    static QColor stateWarning();
    static QColor stateDanger();
    static QColor stateSuccess();
};

} // namespace ks::ui
