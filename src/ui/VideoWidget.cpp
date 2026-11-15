#include "ui/VideoWidget.h"
#include "ui/GpuTextureInterop.h"
#include "core/video/VideoColor.h"
#include <QOpenGLFramebufferObject>
#include <QOpenGLContext>
#include <QDebug>
#include <QElapsedTimer>
#include <algorithm>

#include <QMouseEvent>
#include <QOpenGLShaderProgram>

namespace ks::ui {
namespace {

constexpr const char *kVertexShader = R"(
#version 330 core
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aUv;
out vec2 vUv;
void main() {
    vUv = aUv;
    gl_Position = vec4(aPos, 0.0, 1.0);
}
)";

// CAS sharpen-only pass ported from AMD FidelityFX CAS (MIT).
// Copyright (c) 2017-2019 Advanced Micro Devices, Inc. See THIRD_PARTY_NOTICES.md.
constexpr const char *kFragmentShader = R"(
#version 330 core
in vec2 vUv;
out vec4 fragColor;
uniform sampler2D uTextureY;
uniform sampler2D uTextureU;
uniform sampler2D uTextureV;
uniform sampler2D uRgb;
uniform bool uRgbInput;
uniform bool uFlipRgb;
uniform bool uScalePass;
uniform int uScaling;
uniform mat3 uColorMatrix;
uniform vec3 uColorOffset;
uniform vec2 uTexel;
uniform vec2 uScale;
uniform float uBrightness;
uniform float uContrast;
uniform float uSaturation;
uniform float uShadow;
uniform float uSharpness;

vec3 sourceAt(vec2 uv) {
    if (uRgbInput) return texture(uRgb, vec2(uv.x, uFlipRgb ? 1.0 - uv.y : uv.y)).rgb;
    vec3 yuv = vec3(texture(uTextureY, uv).r, texture(uTextureU, uv).r,
                    texture(uTextureV, uv).r);
    return clamp(uColorMatrix * (yuv + uColorOffset), 0.0, 1.0);
}

float cubic(float x) {
    x = abs(x);
    if (x < 1.0) return (1.5*x - 2.5)*x*x + 1.0;
    if (x < 2.0) return ((-0.5*x + 2.5)*x - 4.0)*x + 2.0;
    return 0.0;
}

vec3 scaledAt(vec2 uv) {
    if (uScaling == 0) return sourceAt(uv);
    if (uScaling == 1) {
        // Catmull-Rom with the positive middle pair grouped into one bilinear
        // lookup per axis: nine lookups instead of sixteen for frame priority.
        vec2 pixel = uv / uTexel - 0.5;
        vec2 base = floor(pixel);
        vec2 t = pixel - base;
        vec2 t2 = t*t, t3 = t2*t;
        vec2 w0 = -0.5*t + t2 - 0.5*t3;
        vec2 w1 = vec2(1.0) - 2.5*t2 + 1.5*t3;
        vec2 w2 = 0.5*t + 2.0*t2 - 1.5*t3;
        vec2 w3 = -0.5*t2 + 0.5*t3;
        vec2 w12 = w1+w2;
        vec2 p0 = (base-0.5)*uTexel;
        vec2 p12 = (base+w2/w12+0.5)*uTexel;
        vec2 p3 = (base+2.5)*uTexel;
        return clamp(
            sourceAt(vec2(p0.x,p0.y))*w0.x*w0.y +
            sourceAt(vec2(p12.x,p0.y))*w12.x*w0.y +
            sourceAt(vec2(p3.x,p0.y))*w3.x*w0.y +
            sourceAt(vec2(p0.x,p12.y))*w0.x*w12.y +
            sourceAt(vec2(p12.x,p12.y))*w12.x*w12.y +
            sourceAt(vec2(p3.x,p12.y))*w3.x*w12.y +
            sourceAt(vec2(p0.x,p3.y))*w0.x*w3.y +
            sourceAt(vec2(p12.x,p3.y))*w12.x*w3.y +
            sourceAt(vec2(p3.x,p3.y))*w3.x*w3.y, 0.0, 1.0);
    }
    // Widen the sampling footprint when reducing a native-resolution stream.
    vec2 footprint = max(vec2(1.0), uScale);
    vec2 pixel = uv / uTexel - 0.5;
    vec2 base = floor(pixel);
    vec2 phase = (pixel - base) / footprint;
    vec3 value = vec3(0.0);
    float total = 0.0;
    for (int y = -1; y <= 2; ++y) {
        for (int x = -1; x <= 2; ++x) {
            vec2 delta = vec2(x,y) - phase;
            float weight = cubic(delta.x) * cubic(delta.y);
            vec2 pos = (base + vec2(x,y)*footprint + 0.5) * uTexel;
            value += sourceAt(pos) * weight;
            total += weight;
        }
    }
    return clamp(value / max(total, 0.001), 0.0, 1.0);
}

void main() {
    if (uScalePass) {
        fragColor = vec4(scaledAt(vUv), 1.0);
        return;
    }
    vec3 color = sourceAt(vUv);
    if (uSharpness > 0.001) {
        vec3 b = sourceAt(vUv - vec2(0.0, uTexel.y));
        vec3 d = sourceAt(vUv - vec2(uTexel.x, 0.0));
        vec3 f = sourceAt(vUv + vec2(uTexel.x, 0.0));
        vec3 h = sourceAt(vUv + vec2(0.0, uTexel.y));
        vec3 mn = min(min(min(b,d),min(f,h)),color);
        vec3 mx = max(max(max(b,d),max(f,h)),color);
        vec3 amp = sqrt(clamp(min(mn, vec3(1.0)-mx) / max(mx,vec3(0.00001)),0.0,1.0));
        // AMD CAS default fast path uses the green-channel coefficient.
        float w = -amp.g / mix(8.0, 5.0, uSharpness);
        vec3 sharpened = clamp(((b+d+f+h)*w+color)/(1.0+4.0*w),0.0,1.0);
        // Extend the original CAS range to a true zero-strength bypass.
        color = mix(color, sharpened, uSharpness);
    }
    color = (color - 0.5) * uContrast + 0.5 + uBrightness;
    float luma = dot(color, vec3(0.2126, 0.7152, 0.0722));
    color = mix(vec3(luma), color, uSaturation);
    if (uShadow > 0.001) {
        float l = clamp(dot(color, vec3(0.2126, 0.7152, 0.0722)), 0.0, 1.0);
        color += (vec3(1.0)-color) * (uShadow*0.45*(1.0-smoothstep(0.0,0.6,l)));
    }
    fragColor = vec4(clamp(color, 0.0, 1.0), 1.0);
}
)";

} // namespace

VideoWidget::VideoWidget(QWidget *parent) : QOpenGLWidget(parent) {
    setObjectName(QStringLiteral("VideoSurface"));
    setMinimumSize(320, 180);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setFocusPolicy(Qt::StrongFocus);
}

VideoWidget::~VideoWidget() {
    if (context()) {
        makeCurrent(); cleanupGL();
        disconnect(context(), nullptr, this, nullptr);
        doneCurrent();
    }
}

void VideoWidget::cleanupGL() {
    if (!m_glReady) return;
    m_gpuInterop.reset();
    m_scaledFrame.reset();
    for (auto &sample : m_timings) {
        if (sample.queries[0]) glDeleteQueries(2, sample.queries);
        sample = TimingSample{};
    }
    m_gpuTimingSupported = false;
    resetRenderMeasurements();
    glDeleteTextures(3, m_textures);
    for (auto &texture : m_textures) texture = 0;
    if (m_vbo) glDeleteBuffers(1, &m_vbo);
    if (m_vao) glDeleteVertexArrays(1, &m_vao);
    m_vbo = m_vao = 0;
    delete m_program;
    m_program = nullptr;
    m_allocated = false;
    m_glReady = false;
    m_frameDirty = true;
}

void VideoWidget::setQualityPriority(bool enabled) {
    if (m_qualityPriority == enabled) return;
    m_qualityPriority = enabled;
    resetRenderMeasurements();
    update();
}

void VideoWidget::setTargetFps(int fps) {
    fps = std::clamp(fps, 30, 120);
    if (m_targetFps == fps) return;
    m_targetFps = fps;
    resetRenderMeasurements();
    update();
}

void VideoWidget::resetRenderMeasurements() {
    ++m_renderRevision;
    m_frameBudget.reset();
    m_gpuMs = m_submitMs = -1.0;
    m_gpuSampleAtMs = m_submitAtMs = 0;
    m_scalingUnavailable = false;
    m_activeDecision = videoRenderDecision(m_qualityPriority, m_params.sharpness, 0, m_bypass);
}

VideoWidget::RenderStats VideoWidget::renderStats() const {
    RenderStats stats;
    const qint64 now = monotonicNowMs();
    stats.gpuMs = m_gpuSampleAtMs && now-m_gpuSampleAtMs <= 2000 ? m_gpuMs : -1.0;
    stats.submitMs = m_submitAtMs && now-m_submitAtMs <= 2000 ? m_submitMs : -1.0;
    stats.degradation = m_frameBudget.degradation();
    stats.overBudget = std::max(stats.gpuMs, stats.submitMs) > 1000.0/m_targetFps;
    const QString filter = m_activeDecision.filter == VideoScaleFilter::Linear
                               ? QStringLiteral("线性缩放")
                           : m_activeDecision.filter == VideoScaleFilter::FastCubic
                               ? QStringLiteral("快速双三次缩放")
                               : QStringLiteral("高质量双三次缩放");
    stats.status = QStringLiteral("%1 · %2 · CAS %3%")
                       .arg(m_qualityPriority ? QStringLiteral("画质优先") : QStringLiteral("帧率优先"), filter)
                       .arg(m_activeDecision.sharpness, 0, 'f', 0);
    if (stats.degradation > 0) {
        stats.status += QStringLiteral("\n性能保护：处理耗时持续接近或超出 %1 FPS 预算，%2。")
                            .arg(m_targetFps)
                            .arg(stats.degradation == 1 ? QStringLiteral("已减弱锐化")
                                 : stats.degradation == 2 ? QStringLiteral("已暂停锐化")
                                                         : QStringLiteral("已改用轻量缩放"));
    } else if (stats.overBudget) {
        stats.status += QStringLiteral("\n处理耗时超出 %1 FPS 的 %2 ms 帧预算。")
                            .arg(m_targetFps).arg(1000.0/m_targetFps, 0, 'f', 2);
    }
    if (m_scalingUnavailable)
        stats.status += QStringLiteral("\n缩放缓冲区创建失败，已回退线性缩放。分辨率设置未改变。");
    return stats;
}

void VideoWidget::collectGpuTimings() {
    for (auto &sample : m_timings) {
        if (!sample.pending) continue;
        GLint ready = GL_FALSE;
        glGetQueryObjectiv(sample.queries[1], GL_QUERY_RESULT_AVAILABLE, &ready);
        if (!ready) continue; // Never stall the GUI waiting for a GPU result.
        GLuint64 start = 0, end = 0;
        glGetQueryObjectui64v(sample.queries[0], GL_QUERY_RESULT, &start);
        glGetQueryObjectui64v(sample.queries[1], GL_QUERY_RESULT, &end);
        sample.pending = false;
        if (sample.revision != m_renderRevision || end < start) continue;
        const double ms = double(end-start)/1'000'000.0;
        m_gpuMs = m_gpuMs < 0.0 ? ms : m_gpuMs*0.8 + ms*0.2;
        m_gpuSampleAtMs = monotonicNowMs();
        if (sample.adaptive && m_frameBudget.observe(std::max(ms, sample.submitMs),
                                                     1000.0/m_targetFps, m_gpuSampleAtMs)) {
            ++m_renderRevision; // Do not feed measurements of the previous level back into the controller.
        }
    }
}

void VideoWidget::setFrame(const VideoFrameConstPtr &frame) {
    {
        std::lock_guard<std::mutex> lock(m_frameMutex);
        m_pendingFrame = frame;
        m_frameDirty = true;
        m_hasFrame = frame != nullptr;
    }
    update();
}

void VideoWidget::clearFrame() { resetRenderMeasurements(); setFrame(nullptr); }

bool VideoWidget::hasFrame() const {
    std::lock_guard<std::mutex> lock(m_frameMutex);
    return m_hasFrame;
}

void VideoWidget::setEnhanceParams(const EnhanceParams &params) {
    if (m_params.brightness == params.brightness && m_params.contrast == params.contrast &&
        m_params.saturation == params.saturation && m_params.shadow == params.shadow &&
        m_params.sharpness == params.sharpness) return;
    m_params = params;
    resetRenderMeasurements();
    update();
}

void VideoWidget::setBypassEnhance(bool bypass) {
    if (m_bypass == bypass) return;
    m_bypass = bypass;
    ++m_renderRevision; // Comparison samples must not influence adaptive quality.
    update();
}

void VideoWidget::setFallbackAspect(double aspect) {
    if (aspect <= 0.01 || hasFrame() || m_frameAspect == aspect) {
        return;
    }
    m_frameAspect = aspect;
    syncVideoRect();
    update();
}

void VideoWidget::initializeGL() {
    initializeOpenGLFunctions();
    qInfo() << "video OpenGL context:" << context()->format()
            << "requested swap interval:" << QSurfaceFormat::defaultFormat().swapInterval();
    glClearColor(0.043f, 0.051f, 0.063f, 1.0f);

    connect(context(), &QOpenGLContext::aboutToBeDestroyed, this, [this] {
        makeCurrent(); cleanupGL(); doneCurrent();
    }, Qt::DirectConnection);
    m_glReady = true;
    GLint timestampBits = 0;
    glGetQueryiv(GL_TIMESTAMP, GL_QUERY_COUNTER_BITS, &timestampBits);
    m_gpuTimingSupported = timestampBits > 0;
    if (m_gpuTimingSupported)
        for (auto &sample : m_timings) glGenQueries(2, sample.queries);
    m_gpuInterop = std::make_unique<GpuTextureInterop>();
    m_program = new QOpenGLShaderProgram();
    if (!m_program->addShaderFromSourceCode(QOpenGLShader::Vertex, kVertexShader) ||
        !m_program->addShaderFromSourceCode(QOpenGLShader::Fragment, kFragmentShader) ||
        !m_program->link()) {
        qWarning() << "video shader initialization failed:" << m_program->log();
        return;
    }

    static const float vertices[] = {
        // x, y, u, v
        -1.0f, -1.0f, 0.0f, 1.0f,
        1.0f,  -1.0f, 1.0f, 1.0f,
        -1.0f, 1.0f,  0.0f, 0.0f,
        1.0f,  1.0f,  1.0f, 0.0f,
    };

    glGenVertexArrays(1, &m_vao);
    glGenBuffers(1, &m_vbo);
    glBindVertexArray(m_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), nullptr);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                          reinterpret_cast<void *>(2 * sizeof(float)));
    glBindVertexArray(0);

    glGenTextures(3, m_textures);
    for (GLuint texture : m_textures) {
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    glBindTexture(GL_TEXTURE_2D, 0);
}

void VideoWidget::resizeGL(int width, int height) {
    (void)width;
    (void)height;
    resetRenderMeasurements();
    syncVideoRect();
    update();
}

QRect VideoWidget::computeVideoRect() const {
    const float widgetW = static_cast<float>(width());
    const float widgetH = static_cast<float>(height());
    if (widgetW <= 0.0f || widgetH <= 0.0f) {
        return {};
    }
    const float videoAspect = static_cast<float>(m_frameAspect);
    const float widgetAspect = widgetW / widgetH;

    float viewW = widgetW;
    float viewH = widgetH;
    if (widgetAspect > videoAspect) {
        viewW = widgetH * videoAspect;
    } else {
        viewH = widgetW / videoAspect;
    }
    const float viewX = (widgetW - viewW) * 0.5f;
    const float viewY = (widgetH - viewH) * 0.5f;
    return QRect(static_cast<int>(viewX), static_cast<int>(viewY), static_cast<int>(viewW),
                 static_cast<int>(viewH));
}

void VideoWidget::syncVideoRect() {
    const QRect rect = computeVideoRect();
    if (rect != m_videoRect) {
        m_videoRect = rect;
        emit videoRectChanged(rect);
    }
}

void VideoWidget::drawBlack() {
    glClearColor(0.043f, 0.051f, 0.063f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
}

void VideoWidget::uploadFrame(const VideoFrame &frame) {
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    const int chromaWidth = (frame.width + 1) / 2;
    const int chromaHeight = (frame.height + 1) / 2;
    const bool sizeChanged = !m_allocated || m_textureWidth != frame.width ||
                             m_textureHeight != frame.height;

    glBindTexture(GL_TEXTURE_2D, m_textures[0]);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, frame.yStride);
    if (sizeChanged) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, frame.width, frame.height, 0, GL_RED,
                     GL_UNSIGNED_BYTE, frame.y.constData());
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, frame.width, frame.height, GL_RED,
                        GL_UNSIGNED_BYTE, frame.y.constData());
    }

    glBindTexture(GL_TEXTURE_2D, m_textures[1]);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, frame.uStride);
    if (sizeChanged) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, chromaWidth, chromaHeight, 0, GL_RED,
                     GL_UNSIGNED_BYTE, frame.u.constData());
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, chromaWidth, chromaHeight, GL_RED,
                        GL_UNSIGNED_BYTE, frame.u.constData());
    }

    glBindTexture(GL_TEXTURE_2D, m_textures[2]);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, frame.vStride);
    if (sizeChanged) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, chromaWidth, chromaHeight, 0, GL_RED,
                     GL_UNSIGNED_BYTE, frame.v.constData());
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, chromaWidth, chromaHeight, GL_RED,
                        GL_UNSIGNED_BYTE, frame.v.constData());
    }

    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    m_allocated = true;
    m_textureWidth = frame.width;
    m_textureHeight = frame.height;
}

void VideoWidget::paintGL() {
    QElapsedTimer submitTimer;
    submitTimer.start();
    collectGpuTimings();
    VideoFrameConstPtr frame;
    bool dirty = false;
    {
        std::lock_guard<std::mutex> lock(m_frameMutex);
        frame = m_pendingFrame;
        dirty = m_frameDirty;
        m_frameDirty = false;
    }

    if (!frame) {
        if (m_gpuInterop) m_gpuInterop->reset();
        m_scaledFrame.reset();
        m_presentLatencyMs.store(-1.0);
        drawBlack();
        return;
    }
    if (!m_program || !m_program->isLinked()) { drawBlack(); return; }
    GLuint gpuTexture = 0;
    if (frame->gpu) {
        gpuTexture = m_gpuInterop->acquire(frame->gpu);
        if (!gpuTexture) {
            drawBlack();
            if (dirty) emit gpuFramesUnavailable();
            return;
        }
    } else {
        m_gpuInterop->reset();
        if (dirty || !m_allocated) uploadFrame(*frame);
    }
    m_frameAspect = double(frame->width) / frame->height;
    syncVideoRect();
    const QRect rect = m_videoRect;
    const qreal dpr = devicePixelRatioF();
    const QSize target(std::max(1, qRound(rect.width()*dpr)),
                       std::max(1, qRound(rect.height()*dpr)));
    auto decision = videoRenderDecision(m_qualityPriority, m_params.sharpness,
                                         m_frameBudget.degradation(), m_bypass);
    TimingSample *timing = nullptr;
    if (m_gpuTimingSupported) {
        for (auto &sample : m_timings) if (!sample.pending) { timing = &sample; break; }
    }
    if (timing) {
        timing->revision = m_renderRevision;
        timing->adaptive = !m_qualityPriority && !m_bypass;
        glQueryCounter(timing->queries[0], GL_TIMESTAMP);
    }
    m_program->bind();
    m_program->setUniformValue("uTextureY", 0);
    m_program->setUniformValue("uTextureU", 1);
    m_program->setUniformValue("uTextureV", 2);
    m_program->setUniformValue("uRgb", 3);
    m_program->setUniformValue("uRgbInput", gpuTexture != 0);
    m_program->setUniformValue("uFlipRgb", false);
    m_program->setUniformValue("uColorMatrix", yuvToRgbMatrix(frame->colorSpace, frame->fullRange));
    m_program->setUniformValue("uColorOffset", yuvOffset(frame->fullRange));
    m_program->setUniformValue("uTexel", QVector2D(1.0f/frame->width, 1.0f/frame->height));
    m_program->setUniformValue("uScale", QVector2D(float(frame->width)/target.width(),
                                                 float(frame->height)/target.height()));
    for (int i=0; i<3; ++i) {
        glActiveTexture(GL_TEXTURE0+i);
        glBindTexture(GL_TEXTURE_2D, m_textures[i]);
    }
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, gpuTexture);
    glBindVertexArray(m_vao);

    // Scale/color-convert once, then apply CAS at output resolution. This avoids
    // repeating the expensive scale kernel for every CAS neighbor.
    bool scaled = false;
    m_scalingUnavailable = false;
    if (decision.filter != VideoScaleFilter::Linear) {
        if (!m_scaledFrame || m_scaledFrame->size() != target)
            m_scaledFrame = std::make_unique<QOpenGLFramebufferObject>(target);
        if (m_scaledFrame->isValid()) {
            m_scaledFrame->bind();
            // FBO creation can change texture bindings on the active unit.
            glActiveTexture(GL_TEXTURE3);
            glBindTexture(GL_TEXTURE_2D, gpuTexture);
            glViewport(0, 0, target.width(), target.height());
            m_program->setUniformValue("uScalePass", true);
            m_program->setUniformValue("uScaling", decision.filter == VideoScaleFilter::FastCubic ? 1 : 2);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            glBindFramebuffer(GL_FRAMEBUFFER, defaultFramebufferObject());
            glActiveTexture(GL_TEXTURE3);
            glBindTexture(GL_TEXTURE_2D, m_scaledFrame->texture());
            m_program->setUniformValue("uRgbInput", true);
            m_program->setUniformValue("uFlipRgb", true);
            m_program->setUniformValue("uTexel", QVector2D(1.0f/target.width(), 1.0f/target.height()));
            scaled = true;
        } else {
            m_scalingUnavailable = true;
            decision.filter = VideoScaleFilter::Linear;
        }
    }
    if (!scaled) m_scaledFrame.reset();
    glBindFramebuffer(GL_FRAMEBUFFER, defaultFramebufferObject());
    glViewport(0, 0, qRound(width()*dpr), qRound(height()*dpr));
    drawBlack();
    glViewport(qRound(rect.x()*dpr), qRound((height()-rect.bottom()-1)*dpr),
               target.width(), target.height());
    m_program->setUniformValue("uScalePass", false);
    m_program->setUniformValue("uBrightness", m_bypass ? 0.0f : m_params.brightness/500.0f);
    m_program->setUniformValue("uContrast", m_bypass ? 1.0f : 1.0f+m_params.contrast/100.0f);
    m_program->setUniformValue("uSaturation", m_bypass ? 1.0f : 1.0f+m_params.saturation/100.0f);
    m_program->setUniformValue("uShadow", m_bypass ? 0.0f : m_params.shadow/100.0f);
    m_program->setUniformValue("uSharpness", decision.sharpness/100.0f);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    if (timing) glQueryCounter(timing->queries[1], GL_TIMESTAMP);
    glBindVertexArray(0);
    m_program->release();
    if (frame->gpu) m_gpuInterop->release();
    m_activeDecision = decision;
    const double submitMs = submitTimer.nsecsElapsed()/1'000'000.0;
    m_submitMs = m_submitMs < 0.0 ? submitMs : m_submitMs*0.8 + submitMs*0.2;
    m_submitAtMs = monotonicNowMs();
    if (timing) {
        timing->submitMs = submitMs;
        timing->pending = true;
    } else if (!m_gpuTimingSupported && !m_qualityPriority && !m_bypass) {
        m_frameBudget.observe(submitMs, 1000.0/m_targetFps, m_submitAtMs);
    }
    if (dirty && frame->decodedAtMs > 0)
        m_presentLatencyMs.store(double(monotonicNowMs()-frame->decodedAtMs));
    if (dirty) emit framePresented();
}

QRect VideoWidget::videoRect() const { return m_videoRect; }

QSize VideoWidget::minimumSizeHint() const { return {480, 270}; }

void VideoWidget::mouseDoubleClickEvent(QMouseEvent *event) {
    (void)event;
    emit doubleClicked();
}

} // namespace ks::ui
