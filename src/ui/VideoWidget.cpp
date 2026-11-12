#include "ui/VideoWidget.h"

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

constexpr const char *kFragmentShader = R"(
#version 330 core
in vec2 vUv;
out vec4 fragColor;

uniform sampler2D uTextureY;
uniform sampler2D uTextureU;
uniform sampler2D uTextureV;

uniform float uBrightness;   // -1..1
uniform float uContrast;     // 0..2
uniform float uSaturation;   // 0..2
uniform float uGamma;        // 0.5..2
uniform float uHighlight;    // 0..2
uniform float uCas;          // 0..1
uniform vec2  uTexel;

float sampleY(vec2 uv) {
    return texture(uTextureY, uv).r;
}

void main() {
    float y = texture(uTextureY, vUv).r;
    float u = texture(uTextureU, vUv).r - 0.5;
    float v = texture(uTextureV, vUv).r - 0.5;

    // BT.709 limited range → RGB
    y = (y - 16.0 / 255.0) * 1.164383;
    vec3 color;
    color.r = y + 1.5748 * v;
    color.g = y - 0.1873 * u - 0.4681 * v;
    color.b = y + 1.8556 * u;

    // CAS 风格自适应锐化（基于亮度局部对比）
    if (uCas > 0.001) {
        float yc = sampleY(vUv);
        float yl = sampleY(vUv - vec2(uTexel.x, 0.0));
        float yr = sampleY(vUv + vec2(uTexel.x, 0.0));
        float yt = sampleY(vUv + vec2(0.0, uTexel.y));
        float yb = sampleY(vUv - vec2(0.0, uTexel.y));
        float mn = min(min(yl, yr), min(yt, yb));
        float mx = max(max(yl, yr), max(yt, yb));
        float amp = clamp(min(mn, 1.0 - mx) / max(mx, 0.0001), 0.0, 1.0);
        float w = uCas * sqrt(amp);
        float avg = (yl + yr + yt + yb) * 0.25;
        color += (yc - avg) * w;
    }

    // Gamma
    color = pow(max(color, vec3(0.0)), vec3(1.0 / max(uGamma, 0.01)));

    // 对比度（围绕中灰）
    color = (color - 0.5) * uContrast + 0.5;

    // 亮度
    color += uBrightness;

    // 饱和度
    float luma = dot(color, vec3(0.2126, 0.7152, 0.0722));
    color = mix(vec3(luma), color, uSaturation);

    // 高光压缩（soft-clip tone mapping）
    if (uHighlight > 0.001) {
        vec3 compressed = color * (1.0 + uHighlight) / (1.0 + uHighlight * max(color, vec3(0.0)));
        color = mix(color, compressed, 1.0);
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
    makeCurrent();
    for (GLuint &texture : m_textures) {
        if (texture) {
            glDeleteTextures(1, &texture);
            texture = 0;
        }
    }
    if (m_vbo) {
        glDeleteBuffers(1, &m_vbo);
    }
    if (m_vao) {
        glDeleteVertexArrays(1, &m_vao);
    }
    delete m_program;
    m_program = nullptr;
    doneCurrent();
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

void VideoWidget::clearFrame() { setFrame(nullptr); }

bool VideoWidget::hasFrame() const {
    std::lock_guard<std::mutex> lock(m_frameMutex);
    return m_hasFrame;
}

void VideoWidget::setEnhanceParams(const EnhanceParams &params) {
    m_params = params;
    update();
}

void VideoWidget::setBypassEnhance(bool bypass) {
    m_bypass = bypass;
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
    glClearColor(0.043f, 0.051f, 0.063f, 1.0f);

    m_program = new QOpenGLShaderProgram();
    m_program->addShaderFromSourceCode(QOpenGLShader::Vertex, kVertexShader);
    m_program->addShaderFromSourceCode(QOpenGLShader::Fragment, kFragmentShader);
    m_program->link();

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
    VideoFrameConstPtr frame;
    bool dirty = false;
    {
        std::lock_guard<std::mutex> lock(m_frameMutex);
        frame = m_pendingFrame;
        dirty = m_frameDirty;
        m_frameDirty = false;
    }

    if (!frame) {
        drawBlack();
        return;
    }
    if (dirty) {
        uploadFrame(*frame);
        // 解码完成 → 真正上传呈现：含帧槽排队时间与上传耗时，反映「看到的画面有多旧」。
        if (frame->decodedAtMs > 0) {
            m_presentLatencyMs.store(static_cast<double>(monotonicNowMs() - frame->decodedAtMs),
                                     std::memory_order_relaxed);
        }
    }

    // 计算信箱区域：保持原始宽高比，不拉伸不裁切
    m_frameAspect = static_cast<double>(frame->width) / static_cast<double>(frame->height);
    const QRect videoRect = computeVideoRect();
    if (videoRect != m_videoRect) {
        m_videoRect = videoRect;
        emit videoRectChanged(videoRect);
    }

    const qreal dpr = devicePixelRatioF();
    // 先按整幅控件清屏（避免信箱区残留脏像素）
    glViewport(0, 0, static_cast<GLsizei>(width() * dpr), static_cast<GLsizei>(height() * dpr));
    drawBlack();
    glViewport(static_cast<GLint>(videoRect.x() * dpr),
               static_cast<GLint>((height() - videoRect.y() - videoRect.height()) * dpr),
               static_cast<GLsizei>(videoRect.width() * dpr),
               static_cast<GLsizei>(videoRect.height() * dpr));

    m_program->bind();
    m_program->setUniformValue("uTextureY", 0);
    m_program->setUniformValue("uTextureU", 1);
    m_program->setUniformValue("uTextureV", 2);

    const bool bypass = m_bypass;
    m_program->setUniformValue("uBrightness", bypass ? 0.0f : m_params.brightness / 500.0f);
    m_program->setUniformValue("uContrast", bypass ? 1.0f : 1.0f + m_params.contrast / 100.0f);
    m_program->setUniformValue("uSaturation", bypass ? 1.0f : 1.0f + m_params.saturation / 100.0f);
    m_program->setUniformValue("uGamma", bypass ? 1.0f : std::max(0.2f, m_params.gamma));
    m_program->setUniformValue("uHighlight", bypass ? 0.0f : m_params.highlight / 100.0f);
    m_program->setUniformValue("uCas", bypass ? 0.0f : m_params.casSharpness / 100.0f);
    m_program->setUniformValue("uTexel", QVector2D(1.0f / frame->width, 1.0f / frame->height));

    for (int i = 0; i < 3; ++i) {
        glActiveTexture(GL_TEXTURE0 + i);
        glBindTexture(GL_TEXTURE_2D, m_textures[i]);
    }

    glBindVertexArray(m_vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);
    m_program->release();

    emit framePresented();
}

QRect VideoWidget::videoRect() const { return m_videoRect; }

QSize VideoWidget::minimumSizeHint() const { return {480, 270}; }

void VideoWidget::mouseDoubleClickEvent(QMouseEvent *event) {
    (void)event;
    emit doubleClicked();
}

} // namespace ks::ui
