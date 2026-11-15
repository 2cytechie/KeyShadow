#pragma once

#include "core/engine/Engines.h"
#include <QComboBox>
#include <QSignalBlocker>

namespace ks::ui {

inline void populateResolutions(QComboBox *combo) {
    combo->addItem(QStringLiteral("原生 · PRO"), 0);
    combo->addItem(QStringLiteral("1080p"), 1080);
    combo->addItem(QStringLiteral("720p"), 720);
    combo->addItem(QStringLiteral("480p"), 480);
    combo->setToolTip(QStringLiteral("按短边限制分辨率并保持比例；实际编码尺寸以设备输出为准"));
}

inline void syncResolution(QComboBox *combo, const Profile &profile) {
    const QSignalBlocker blocker(combo);
    while (combo->count() > 4) combo->removeItem(4);
    const int value = profile.videoShortEdge;
    int index = combo->findData(value);
    if (index < 0) {
        combo->addItem(QStringLiteral("短边 %1").arg(value), value);
        index = combo->count() - 1;
    }
    combo->setCurrentIndex(index);
}

inline void readResolution(const QComboBox *combo, Profile &profile) {
    const int value = combo->currentData().toInt();
    profile.videoShortEdge = std::max(0, value);
}

inline void populateBitrates(QComboBox *combo) {
    for (int mbps : {4, 8, 12, 16, 24, 32, 48, 64})
        combo->addItem(QStringLiteral("%1 Mbps").arg(mbps), mbps * 1'000'000);
}

inline void syncBitrate(QComboBox *combo, int bitrate) {
    const QSignalBlocker blocker(combo);
    while (combo->count() > 8) combo->removeItem(8);
    int index = combo->findData(bitrate);
    if (index < 0) {
        combo->addItem(QStringLiteral("%1 Mbps").arg(bitrate / 1'000'000.0, 0, 'f', 1), bitrate);
        index = combo->count() - 1;
    }
    combo->setCurrentIndex(index);
}

} // namespace ks::ui
