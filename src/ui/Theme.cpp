#include "ui/Theme.h"

#include <QApplication>
#include <QFile>
#include <QHash>
#include <QFontDatabase>
#include <QPalette>

namespace ks::ui {
namespace {

QHash<QString, QColor> buildPalette() {
    QHash<QString, QColor> palette;
    palette.insert(QStringLiteral("bg-app"), QColor(0x0B, 0x0D, 0x10));
    palette.insert(QStringLiteral("bg-surface"), QColor(0x12, 0x15, 0x1A));
    palette.insert(QStringLiteral("bg-elevated"), QColor(0x1A, 0x1F, 0x26));
    palette.insert(QStringLiteral("bg-hover"), QColor(0x22, 0x28, 0x33));
    palette.insert(QStringLiteral("bg-active"), QColor(0x2A, 0x32, 0x3E));
    palette.insert(QStringLiteral("bg-input"), QColor(0x0E, 0x11, 0x16));
    palette.insert(QStringLiteral("border-subtle"), QColor(0x23, 0x2A, 0x33));
    palette.insert(QStringLiteral("border-strong"), QColor(0x33, 0x3D, 0x4A));
    palette.insert(QStringLiteral("text-primary"), QColor(0xE8, 0xED, 0xF2));
    palette.insert(QStringLiteral("text-secondary"), QColor(0x9B, 0xA7, 0xB4));
    palette.insert(QStringLiteral("text-tertiary"), QColor(0x6B, 0x77, 0x85));
    palette.insert(QStringLiteral("text-disabled"), QColor(0x4A, 0x55, 0x63));
    palette.insert(QStringLiteral("brand-primary"), QColor(0x00, 0xD9, 0xB8));
    palette.insert(QStringLiteral("brand-primary-hover"), QColor(0x1F, 0xE8, 0xC8));
    palette.insert(QStringLiteral("brand-pro"), QColor(0x7C, 0x6B, 0xFF));
    palette.insert(QStringLiteral("on-brand"), QColor(0x04, 0x21, 0x1D));
    palette.insert(QStringLiteral("state-success"), QColor(0x3D, 0xD6, 0x8C));
    palette.insert(QStringLiteral("state-warning"), QColor(0xFF, 0xB0, 0x20));
    palette.insert(QStringLiteral("state-danger"), QColor(0xFF, 0x5C, 0x5C));
    palette.insert(QStringLiteral("state-info"), QColor(0x4E, 0xA8, 0xFF));
    return palette;
}

} // namespace

void Theme::apply(QApplication &app) {
    app.setStyle(QStringLiteral("Fusion"));

    QFile file(QStringLiteral(":/theme.qss"));
    if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        app.setStyleSheet(QString::fromUtf8(file.readAll()));
    }

    QPalette palette;
    palette.setColor(QPalette::Window, bgApp());
    palette.setColor(QPalette::WindowText, textPrimary());
    palette.setColor(QPalette::Base, QColor(0x0E, 0x11, 0x16));
    palette.setColor(QPalette::AlternateBase, bgSurface());
    palette.setColor(QPalette::Text, textPrimary());
    palette.setColor(QPalette::Button, bgSurface());
    palette.setColor(QPalette::ButtonText, textPrimary());
    palette.setColor(QPalette::Highlight, brandPrimary());
    palette.setColor(QPalette::HighlightedText, QColor(0x04, 0x21, 0x1D));
    palette.setColor(QPalette::ToolTipBase, bgElevated());
    palette.setColor(QPalette::ToolTipText, textPrimary());
    app.setPalette(palette);
}

QColor Theme::color(const QString &token) {
    static const QHash<QString, QColor> palette = buildPalette();
    return palette.value(token, QColor(0xFF, 0x00, 0xFF));
}

QFont Theme::monoFont(int pixelSize, int weight) {
    QFont font(QStringLiteral("Cascadia Mono"));
    font.setStyleHint(QFont::Monospace);
    if (!QFontDatabase::families().contains(QStringLiteral("Cascadia Mono"))) {
        font = QFont(QStringLiteral("Consolas"));
        font.setStyleHint(QFont::Monospace);
    }
    font.setPixelSize(pixelSize);
    font.setWeight(static_cast<QFont::Weight>(weight));
    return font;
}

QFont Theme::uiFont(int pixelSize, int weight) {
    QFont font(QStringLiteral("Microsoft YaHei UI"));
    font.setStyleHint(QFont::SansSerif);
    font.setPixelSize(pixelSize);
    font.setWeight(static_cast<QFont::Weight>(weight));
    return font;
}

QColor Theme::bgApp() { return color(QStringLiteral("bg-app")); }
QColor Theme::bgSurface() { return color(QStringLiteral("bg-surface")); }
QColor Theme::bgElevated() { return color(QStringLiteral("bg-elevated")); }
QColor Theme::textPrimary() { return color(QStringLiteral("text-primary")); }
QColor Theme::textSecondary() { return color(QStringLiteral("text-secondary")); }
QColor Theme::brandPrimary() { return color(QStringLiteral("brand-primary")); }
QColor Theme::brandPro() { return color(QStringLiteral("brand-pro")); }
QColor Theme::stateWarning() { return color(QStringLiteral("state-warning")); }
QColor Theme::stateDanger() { return color(QStringLiteral("state-danger")); }
QColor Theme::stateSuccess() { return color(QStringLiteral("state-success")); }

} // namespace ks::ui
