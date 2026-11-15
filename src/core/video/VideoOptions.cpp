#include "core/video/VideoOptions.h"

#include <algorithm>
#include <cmath>

namespace ks {

QString videoCodecForMode(bool qualityPriority, const QString &qualityCodec, bool fallback) {
    return qualityPriority && !fallback && qualityCodec == QStringLiteral("h265")
               ? QStringLiteral("h265") : QStringLiteral("h264");
}

int streamMaxSize(const Profile &profile, int screenWidth, int screenHeight) {
    if (profile.videoShortEdge <= 0 || screenWidth <= 0 || screenHeight <= 0) {
        return 0;
    }
    const int shortEdge = std::min(screenWidth, screenHeight);
    const int longEdge = std::max(screenWidth, screenHeight);
    const double scale = std::min(1.0, double(profile.videoShortEdge) / shortEdge);
    return std::max(8, static_cast<int>(std::floor(longEdge * scale / 8.0)) * 8);
}

void normalizeVideoOptions(Profile &profile, bool pro) {
    profile.videoShortEdge = std::clamp(profile.videoShortEdge, 0, 2160);
    profile.maxFps = std::clamp(profile.maxFps, 30, pro ? 120 : 60);
    profile.videoBitRate = std::clamp(profile.videoBitRate, 1'000'000, 80'000'000);
    if (!pro && (profile.videoShortEdge > 1080 || profile.videoShortEdge == 0)) {
        profile.videoShortEdge = 1080;
    }
}

} // namespace ks
