#pragma once

#include <QOpenGLFunctions_3_3_Core>
#include <QOpenGLWidget>
#include <QRect>
#include <atomic>
#include <mutex>
#include <array>

#include "core/video/VideoFrame.h"
#include "core/video/RenderPolicy.h"

class QOpenGLShaderProgram;
class QOpenGLFramebufferObject;

namespace ks::ui {

class GpuTextureInterop;

/// GPU texture/YUV canvas with optional high-quality scaling and CAS.
class VideoWidget : public QOpenGLWidget, protected QOpenGLFunctions_3_3_Core {
    Q_OBJECT

public:
    struct EnhanceParams {
        float brightness = 0.0f;  ///< -100..100
        float contrast = 0.0f;    ///< -100..100
        float saturation = 0.0f;  ///< -100..100
        float shadow = 0.0f;      ///< 0..100（暗部提亮）
        float sharpness = 0.0f;   ///< 0..100
    };

    struct RenderStats {
        double gpuMs = -1.0; ///< GPU scaling + enhancement drawing, not incremental overhead
        double submitMs = -1.0; ///< CPU submission, including upload/interop; excludes swap/vsync
        int degradation = 0;
        bool overBudget = false;
        QString status;
    };

    explicit VideoWidget(QWidget *parent = nullptr);
    ~VideoWidget() override;

    void setFrame(const VideoFrameConstPtr &frame);
    void clearFrame();
    bool hasFrame() const;

    /// 最近一帧「解码完成 → 上传到 GPU」的耗时（毫秒），无样本返回 -1。
    double presentLatencyMs() const { return m_presentLatencyMs.load(std::memory_order_relaxed); }

    void setEnhanceParams(const EnhanceParams &params);
    EnhanceParams enhanceParams() const { return m_params; }

    /// 长按「对比原图」时临时旁路全部增强参数。
    void setBypassEnhance(bool bypass);
    void setQualityPriority(bool enabled);
    void setTargetFps(int fps);
    RenderStats renderStats() const; // GUI thread

    /// 无视频帧时的占位宽高比（如设备分辨率 1080×2400）；有帧时以帧宽高比为准。
    void setFallbackAspect(double aspect);

    /// 画面（含信箱黑边）在控件内的实际显示矩形。
    QRect videoRect() const;

    QSize minimumSizeHint() const override;
    QSize sizeHint() const override { return {960, 540}; }

signals:
    void framePresented();
    void gpuFramesUnavailable();
    void doubleClicked();
    /// 画面矩形变化（窗口缩放、视频尺寸变化、无帧占位）。
    void videoRectChanged(QRect rect);

protected:
    void initializeGL() override;
    void paintGL() override;
    void resizeGL(int width, int height) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;

private:
    void uploadFrame(const VideoFrame &frame);
    void drawBlack();
    QRect computeVideoRect() const;
    void syncVideoRect();
    void cleanupGL();
    void resetRenderMeasurements();
    void collectGpuTimings();

    mutable std::mutex m_frameMutex;
    VideoFrameConstPtr m_pendingFrame;
    bool m_frameDirty = false;
    bool m_hasFrame = false;
    std::atomic<double> m_presentLatencyMs{-1.0};

    GLuint m_textures[3] = {0, 0, 0};
    GLuint m_vao = 0;
    GLuint m_vbo = 0;
    QOpenGLShaderProgram *m_program = nullptr;
    std::unique_ptr<GpuTextureInterop> m_gpuInterop;
    std::unique_ptr<QOpenGLFramebufferObject> m_scaledFrame;
    bool m_qualityPriority = false;
    int m_targetFps = 60;
    FrameBudgetController m_frameBudget;
    struct TimingSample {
        GLuint queries[2] = {};
        bool pending = false;
        quint64 revision = 0;
        double submitMs = 0.0;
        bool adaptive = false;
    };
    std::array<TimingSample, 4> m_timings{};
    bool m_gpuTimingSupported = false;
    quint64 m_renderRevision = 0;
    double m_gpuMs = -1.0;
    double m_submitMs = -1.0;
    qint64 m_gpuSampleAtMs = 0;
    qint64 m_submitAtMs = 0;
    VideoRenderDecision m_activeDecision;
    bool m_scalingUnavailable = false;
    bool m_glReady = false;
    bool m_allocated = false;
    int m_textureWidth = 0;
    int m_textureHeight = 0;

    EnhanceParams m_params;
    bool m_bypass = false;
    QRect m_videoRect;
    double m_frameAspect = 9.0 / 16.0; ///< 无帧时按竖屏比例占位
};

} // namespace ks::ui
