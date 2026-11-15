#include "ui/GpuTextureInterop.h"

#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <qt_windows.h>

namespace ks::ui {
struct GpuTextureInterop::Impl {
    using Open = HANDLE (WINAPI *)(void *);
    using Close = BOOL (WINAPI *)(HANDLE);
    using Register = HANDLE (WINAPI *)(HANDLE, void *, GLuint, GLenum, GLenum);
    using Unregister = BOOL (WINAPI *)(HANDLE, HANDLE);
    using Lock = BOOL (WINAPI *)(HANDLE, GLint, HANDLE *);
    Open open = nullptr;
    Close close = nullptr;
    Register registerObject = nullptr;
    Unregister unregisterObject = nullptr;
    Lock lock = nullptr;
    Lock unlock = nullptr;
    HANDLE device = nullptr;
    ID3D11Device *resourceDevice = nullptr;
    HANDLE object = nullptr;
    GLuint texture = 0;
    bool locked = false;
    std::shared_ptr<GpuVideoTexture> frame;
};

GpuTextureInterop::GpuTextureInterop() : m_impl(std::make_unique<Impl>()) {}
GpuTextureInterop::~GpuTextureInterop() { reset(); }

unsigned GpuTextureInterop::acquire(const std::shared_ptr<GpuVideoTexture> &frame) {
    auto *context = QOpenGLContext::currentContext();
    if (!context || !frame) return 0;
    auto &p = *m_impl;
    if (!p.open) {
        p.open = reinterpret_cast<Impl::Open>(context->getProcAddress("wglDXOpenDeviceNV"));
        p.close = reinterpret_cast<Impl::Close>(context->getProcAddress("wglDXCloseDeviceNV"));
        p.registerObject = reinterpret_cast<Impl::Register>(context->getProcAddress("wglDXRegisterObjectNV"));
        p.unregisterObject = reinterpret_cast<Impl::Unregister>(context->getProcAddress("wglDXUnregisterObjectNV"));
        p.lock = reinterpret_cast<Impl::Lock>(context->getProcAddress("wglDXLockObjectsNV"));
        p.unlock = reinterpret_cast<Impl::Lock>(context->getProcAddress("wglDXUnlockObjectsNV"));
    }
    if (!p.open || !p.close || !p.registerObject || !p.unregisterObject || !p.lock || !p.unlock)
        return 0;
    if (p.frame != frame) {
        release();
        if (p.object) p.unregisterObject(p.device, p.object);
        p.object = nullptr;
        if (p.texture) context->functions()->glDeleteTextures(1, &p.texture);
        p.texture = 0;
        // Keep the old frame/device alive until its registrations are closed.
        if (p.resourceDevice != frame->device) {
            if (p.device) p.close(p.device);
            p.device = p.open(frame->device);
            p.resourceDevice = frame->device;
        }
        p.frame.reset();
        if (!p.device) return 0;
        context->functions()->glGenTextures(1, &p.texture);
        constexpr GLenum accessReadOnly = 0x0000;
        p.object = p.registerObject(p.device, frame->texture, p.texture, GL_TEXTURE_2D, accessReadOnly);
        if (!p.object) { reset(); return 0; }
        p.frame = frame;
    }
    if (!p.locked) p.locked = p.lock(p.device, 1, &p.object);
    if (!p.locked) { reset(); return 0; }
    auto *gl = context->functions();
    gl->glBindTexture(GL_TEXTURE_2D, p.texture);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return p.texture;
}

void GpuTextureInterop::release() {
    auto &p = *m_impl;
    if (p.locked) {
        p.unlock(p.device, 1, &p.object);
        p.locked = false;
    }
}

void GpuTextureInterop::reset() {
    auto &p = *m_impl;
    release();
    if (p.object) p.unregisterObject(p.device, p.object);
    p.object = nullptr;
    if (p.texture && QOpenGLContext::currentContext())
        QOpenGLContext::currentContext()->functions()->glDeleteTextures(1, &p.texture);
    p.texture = 0;
    if (p.device) p.close(p.device);
    p.device = nullptr;
    p.resourceDevice = nullptr;
    p.frame.reset();
}
} // namespace ks::ui
