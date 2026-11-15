#pragma once

#include "core/engine/Engines.h"

namespace ks {

// Translate short-edge presets to scrcpy's longest-edge limit.
int streamMaxSize(const Profile &profile, int screenWidth, int screenHeight);
void normalizeVideoOptions(Profile &profile, bool pro);
QString videoCodecForMode(bool qualityPriority, const QString &qualityCodec, bool fallback = false);

} // namespace ks
