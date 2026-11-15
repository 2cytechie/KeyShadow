#pragma once

#include "core/video/VideoFrame.h"
#include <QGenericMatrix>
#include <QVector3D>

namespace ks {
inline QMatrix3x3 yuvToRgbMatrix(VideoColorSpace space, bool fullRange) {
    const float kr = space == VideoColorSpace::Bt601 ? 0.299f
                   : space == VideoColorSpace::Bt2020 ? 0.2627f : 0.2126f;
    const float kb = space == VideoColorSpace::Bt601 ? 0.114f
                   : space == VideoColorSpace::Bt2020 ? 0.0593f : 0.0722f;
    const float kg = 1.0f - kr - kb;
    const float y = fullRange ? 1.0f : 255.0f / 219.0f;
    const float c = fullRange ? 1.0f : 255.0f / 224.0f;
    QMatrix3x3 m;
    m(0, 0) = m(1, 0) = m(2, 0) = y;
    m(0, 1) = 0; m(0, 2) = 2 * (1 - kr) * c;
    m(1, 1) = -2 * kb * (1 - kb) / kg * c;
    m(1, 2) = -2 * kr * (1 - kr) / kg * c;
    m(2, 1) = 2 * (1 - kb) * c; m(2, 2) = 0;
    return m;
}
inline QVector3D yuvOffset(bool fullRange) {
    return {fullRange ? 0.0f : -16.0f / 255.0f, -128.0f / 255.0f, -128.0f / 255.0f};
}
} // namespace ks
