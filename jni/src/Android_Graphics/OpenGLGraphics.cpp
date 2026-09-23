#include <GLES3/gl3.h>
#include <android/native_window.h>
#include <android/log.h>
#include <cstdio>
#include "OpenGLGraphics.h"
#include "imgui_impl_opengl3.h"
#if __has_include(<EGL/eglext.h>)
#include <EGL/eglext.h>
#endif

#define GL_LOG_TAG "IdentityV-OpenGL"
#define GL_LOGI(...)                                                        \
    do                                                                      \
    {                                                                       \
        __android_log_print(ANDROID_LOG_INFO, GL_LOG_TAG, __VA_ARGS__);      \
        fprintf(stderr, "[GL][I] " __VA_ARGS__);                             \
        fputc('\n', stderr);                                                 \
    } while (0)
#define GL_LOGE(...)                                                        \
    do                                                                      \
    {                                                                       \
        __android_log_print(ANDROID_LOG_ERROR, GL_LOG_TAG, __VA_ARGS__);     \
        fprintf(stderr, "[GL][E] " __VA_ARGS__);                             \
        fputc('\n', stderr);                                                 \
    } while (0)

// 兼容老 NDK / 老头文件：EGL 1.5 之前没有这个宏，KHR 扩展宏也可能缺失
#ifndef EGL_OPENGL_ES3_BIT
#define EGL_OPENGL_ES3_BIT 0x00000040
#endif
#ifndef EGL_OPENGL_ES3_BIT_KHR
#define EGL_OPENGL_ES3_BIT_KHR 0x00000040
#endif
// 如果需要，也可以顺手补上
#ifndef EGL_CONTEXT_CLIENT_VERSION
#define EGL_CONTEXT_CLIENT_VERSION 0x3098
#endif

bool OpenGLGraphics::Create() {
    const EGLint egl_attributes[] = {EGL_BLUE_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_RED_SIZE, 8,
                                     EGL_ALPHA_SIZE, 8, EGL_DEPTH_SIZE, 16,
                                     EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_SURFACE_TYPE,
                                     EGL_WINDOW_BIT, EGL_NONE};

    m_EglDisplay = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (EGL_NO_DISPLAY == m_EglDisplay) {
        GL_LOGE("eglGetDisplay failed: 0x%x", eglGetError());
        return false;
    }

    if (EGL_TRUE != eglInitialize(m_EglDisplay, nullptr, nullptr)) {
        GL_LOGE("eglInitialize failed: 0x%x", eglGetError());
        Cleanup();
        return false;
    }

    EGLint num_configs = 0;
    if (EGL_TRUE != eglChooseConfig(m_EglDisplay, egl_attributes, nullptr, 0, &num_configs) ||
        0 == num_configs) {
        GL_LOGE("eglChooseConfig found no config: 0x%x", eglGetError());
        Cleanup();
        return false;
    }

    EGLConfig egl_config;
    if (EGL_TRUE != eglChooseConfig(m_EglDisplay, egl_attributes, &egl_config, 1, &num_configs)) {
        GL_LOGE("eglChooseConfig failed: 0x%x", eglGetError());
        Cleanup();
        return false;
    }

    EGLint egl_format;
    eglGetConfigAttrib(m_EglDisplay, egl_config, EGL_NATIVE_VISUAL_ID, &egl_format);
    ANativeWindow_setBuffersGeometry(m_Window, 0, 0, egl_format);

    const EGLint egl_context_attributes[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    m_EglContext = eglCreateContext(m_EglDisplay, egl_config, EGL_NO_CONTEXT,
                                    egl_context_attributes);
    if (EGL_NO_CONTEXT == m_EglContext) {
        GL_LOGE("eglCreateContext failed: 0x%x", eglGetError());
        Cleanup();
        return false;
    }

    m_EglSurface = eglCreateWindowSurface(m_EglDisplay, egl_config, m_Window, nullptr);
    if (EGL_NO_SURFACE == m_EglSurface) {
        // EGL_BAD_NATIVE_WINDOW(0x300B) 说明这块 ANativeWindow 已经被别的 API 绑走（比如刚失败的 Vulkan）
        GL_LOGE("eglCreateWindowSurface failed: 0x%x", eglGetError());
        Cleanup();
        return false;
    }

    if (EGL_TRUE != eglMakeCurrent(m_EglDisplay, m_EglSurface, m_EglSurface, m_EglContext)) {
        GL_LOGE("eglMakeCurrent failed: 0x%x", eglGetError());
        Cleanup();
        return false;
    }

    glClearColor(0.0, 0.0, 0.0, 0.0);
    GL_LOGI("EGL ready: display=%p surface=%p context=%p, window=%dx%d", m_EglDisplay, m_EglSurface,
            m_EglContext, ANativeWindow_getWidth(m_Window), ANativeWindow_getHeight(m_Window));
    return true;
}

void OpenGLGraphics::Setup() {
    ImGui_ImplOpenGL3_Init("#version 300 es");
}

void OpenGLGraphics::PrepareFrame(bool resize) {
    ImGui_ImplOpenGL3_NewFrame();
}

void OpenGLGraphics::Render(ImDrawData *drawData) {
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(drawData);
    eglSwapBuffers(m_EglDisplay, m_EglSurface);
}

void OpenGLGraphics::PrepareShutdown() {
    ImGui_ImplOpenGL3_Shutdown();
}

void OpenGLGraphics::Cleanup() {
    eglMakeCurrent(m_EglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(m_EglDisplay, m_EglContext);
    eglDestroySurface(m_EglDisplay, m_EglSurface);
    eglTerminate(m_EglDisplay);
    m_EglDisplay = EGL_NO_DISPLAY;
    m_EglSurface = EGL_NO_SURFACE;
    m_EglContext = EGL_NO_CONTEXT;
}

BaseTexData *OpenGLGraphics::LoadTexture(BaseTexData *tex, void *pixel_data) {
    auto tex_data = new OpenglTextureData();
    tex_data->Width = tex->Width;
    tex_data->Height = tex->Height;
    tex_data->Channels = tex->Channels;

    GLuint textureId;
    glGenTextures(1, &textureId);
    glBindTexture(GL_TEXTURE_2D, textureId);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, tex_data->Width, tex_data->Height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixel_data);
    tex_data->DS = (void *) (intptr_t) textureId;
    return tex_data;
}

void OpenGLGraphics::RemoveTexture(BaseTexData *tex) {
    auto tex_data = (OpenglTextureData *) tex;
    auto textureId = (GLuint) (intptr_t) tex_data->DS;
    glDeleteTextures(1, &textureId);
    delete tex_data;
}
