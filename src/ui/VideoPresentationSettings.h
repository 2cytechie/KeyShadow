#pragma once

#include <QSettings>
#include <QSurfaceFormat>

namespace ks::ui {

inline bool verticalSyncEnabled(const QSettings &settings) {
    return settings.value(QStringLiteral("video/verticalSync"), true).toBool();
}

inline void setVerticalSyncEnabled(QSettings &settings, bool enabled) {
    settings.setValue(QStringLiteral("video/verticalSync"), enabled);
}

// Apply before QApplication/window creation: changing the default format cannot
// reconfigure the compositor of an existing top-level QWidget window.
inline QSurfaceFormat videoPresentationFormat(const QSettings &settings) {
    QSurfaceFormat format = QSurfaceFormat::defaultFormat();
    format.setSwapInterval(verticalSyncEnabled(settings) ? 1 : 0);
    format.setSwapBehavior(QSurfaceFormat::DoubleBuffer);
    return format;
}

} // namespace ks::ui
