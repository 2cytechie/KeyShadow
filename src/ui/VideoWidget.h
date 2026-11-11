#pragma once

#include <QOpenGLFunctions_3_3_Core>
#include <QOpenGLWidget>
#include <QRect>
#include <mutex>

#include "core/video/VideoFrame.h"

class QOpenGLShaderProgram;

namespace ks::ui {

/// GPU 渲染画布：YUV420P 三纹理上传 + 单 pass 画面增强 shader。
class VideoWidget : public QOpenGLWidget, protected QOpenGLFunctions_3_3_Core {
    Q_OBJECT

public:
    struct EnhanceParams {
        float brightness = 0.0f;  ///< -100..100
        float contrast = 0.0f;    ///< -100..100
        float saturation = 0.0f;  ///< -100..100
        float gamma = 1.0f;       ///< 0.5..2.0
        float highlight = 0.0f;   ///< 0..200（高光压缩强度）
        float casSharpness = 0.0f; ///< 0..100
    };

    explicit VideoWidget(QWidget *parent = nullptr);
    ~VideoWidget() override;

    void setFrame(const VideoFrameConstPtr &frame);
    void clearFrame();
    bool hasFrame() const;

    void setEnhanceParams(const EnhanceParams &params);
    EnhanceParams enhanceParams() const { return m_params; }

    /// 长按「对比原图」时临时旁路全部增强参数。
    void setBypassEnhance(bool bypass);

    /// 画面（含信箱黑边）在控件内的实际显示矩形。
    QRect videoRect() const;

    QSize minimumSizeHint() const override;
    QSize sizeHint() const override { return {960, 540}; }

signals:
    void framePresented();
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

    mutable std::mutex m_frameMutex;
    VideoFrameConstPtr m_pendingFrame;
    bool m_frameDirty = false;
    bool m_hasFrame = false;

    GLuint m_textures[3] = {0, 0, 0};
    GLuint m_vao = 0;
    GLuint m_vbo = 0;
    QOpenGLShaderProgram *m_program = nullptr;
    bool m_allocated = false;
    int m_textureWidth = 0;
    int m_textureHeight = 0;

    EnhanceParams m_params;
    bool m_bypass = false;
    QRect m_videoRect;
    double m_frameAspect = 9.0 / 16.0; ///< 无帧时按竖屏比例占位
};

} // namespace ks::ui
