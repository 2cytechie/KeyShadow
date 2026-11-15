#include "core/video/RenderPolicy.h"

#include <algorithm>
#include <cmath>

namespace ks {

VideoRenderDecision videoRenderDecision(bool quality, float sharpness, int level, bool bypass) {
    if (bypass) return {VideoScaleFilter::Linear, 0.0f};
    sharpness = std::isfinite(sharpness) ? std::clamp(sharpness, 0.0f, 100.0f) : 0.0f;
    if (quality) return {VideoScaleFilter::QualityCubic, sharpness};
    level = std::clamp(level, 0, 3);
    return {level < 3 ? VideoScaleFilter::FastCubic : VideoScaleFilter::Linear,
            level >= 2 ? 0.0f : std::min(sharpness, level == 1 ? 10.0f : 25.0f)};
}

void FrameBudgetController::reset() { *this = FrameBudgetController{}; }

bool FrameBudgetController::observe(double ms, double budget, qint64 now) {
    if (!std::isfinite(ms) || !std::isfinite(budget) || ms < 0.0 || budget <= 0.0) return false;
    // An idle/static stream is not evidence of either spare capacity or overload.
    if (m_windowStart < 0 || now < m_windowStart || now - m_windowStart > 2000) {
        m_windowStart = now;
        m_totalMs = 0.0;
        m_samples = 0;
        m_slowWindows = m_fastWindows = 0;
    }
    m_totalMs += ms;
    ++m_samples;
    if (now - m_windowStart < 500 || m_samples < 8) return false;
    const double average = m_totalMs / m_samples;
    m_windowStart = now;
    m_totalMs = 0.0;
    m_samples = 0;
    m_slowWindows = average > budget * 0.85 ? m_slowWindows + 1 : 0;
    m_fastWindows = average < budget * 0.45 ? m_fastWindows + 1 : 0;
    const int before = m_degradation;
    if (m_slowWindows >= 2 && m_degradation < 3) {
        ++m_degradation;
        m_slowWindows = m_fastWindows = 0;
    } else if (m_fastWindows >= 6 && m_degradation > 0) {
        --m_degradation;
        m_slowWindows = m_fastWindows = 0;
    }
    return before != m_degradation;
}

} // namespace ks
