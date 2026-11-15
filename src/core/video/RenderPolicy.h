#pragma once

#include <QtGlobal>

namespace ks {

enum class VideoScaleFilter { Linear, FastCubic, QualityCubic };

struct VideoRenderDecision {
    VideoScaleFilter filter = VideoScaleFilter::FastCubic;
    float sharpness = 0.0f; // percent; zero remains a true bypass
};

VideoRenderDecision videoRenderDecision(bool qualityPriority, float sharpness,
                                       int degradation, bool bypass);

// Uses measured processing time, never decoded FPS or monitor refresh. Two slow
// windows reduce work; six cheap windows restore one step to avoid oscillation.
class FrameBudgetController {
public:
    void reset();
    bool observe(double processingMs, double budgetMs, qint64 nowMs);
    int degradation() const { return m_degradation; }

private:
    int m_degradation = 0;
    int m_slowWindows = 0;
    int m_fastWindows = 0;
    qint64 m_windowStart = -1;
    double m_totalMs = 0.0;
    int m_samples = 0;
};

} // namespace ks
