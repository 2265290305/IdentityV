#include <android/native_window.h>
#include "AndroidImgui.h"
#include "imgui.h"
#include "my_imgui_impl_android.h"
#include "stb_image.h"
#include <android/log.h>
#include <algorithm>
#include <cstdio>

#define IMGUI_LOG_TAG "IdentityV-Init"
#define IMGUI_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, IMGUI_LOG_TAG, __VA_ARGS__)
// 同时写 logcat 和 stderr（main.cpp 已把 stderr 重定向到 /data/local/tmp/hack_stderr.log）
#define IMGUI_LOGI(...)                                                          \
    do                                                                           \
    {                                                                            \
        __android_log_print(ANDROID_LOG_INFO, IMGUI_LOG_TAG, __VA_ARGS__);        \
        fprintf(stderr, "[ImGui] " __VA_ARGS__);                                  \
        fputc('\n', stderr);                                                      \
    } while (0)

bool AndroidImgui::Init_Render(ANativeWindow *window, float width, float height) {
    if (window == nullptr) {
        IMGUI_LOGE("Init_Render failed: ANativeWindow is null");
        return false;
    }

    m_Window = window;
    m_Width = width;
    m_Height = height;

    IMGUI_LOGI("Init_Render: backend=%s window=%p request=%.0fx%.0f", RenderName, window, width, height);

    ANativeWindow_acquire(window);
    if (!Create()) {
        IMGUI_LOGE("Init_Render failed: %s renderer Create() returned false", RenderName);
        ANativeWindow_release(m_Window);
        m_Window = nullptr;
        return false;
    }

    IMGUI_LOGI("Init_Render: surface ready, window size=%dx%d", ANativeWindow_getWidth(window),
               ANativeWindow_getHeight(window));

    // Setup Dear ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();

    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.DisplaySize = {width, height};
    io.FontGlobalScale = 1.3f;
    // Setup Dear ImGui style
    //ImGui::StyleColorsDark();
    ImGui::StyleColorsLight();
    ImGuiStyle &style = ImGui::GetStyle();
    style.ScaleAllSizes(3);
    style.WindowRounding = 3.f;
    My_ImGui_ImplAndroid_Init(window);

    Setup();
    return true;
}

void AndroidImgui::NewFrame(bool resize) {
    PrepareFrame(resize);
    My_ImGui_ImplAndroid_NewFrame(resize);
    ImGui::NewFrame();
}

void AndroidImgui::EndFrame() {
    ImGui::Render();
    Render(ImGui::GetDrawData());

    // 心跳日志：证明渲染循环真的在跑。
    // 没有这条日志时，“看不到窗口”分不清是初始化失败、还是根本没进主循环。
    static unsigned long frameCount = 0;
    if (0 == (++frameCount % 300))
        IMGUI_LOGI("rendering: backend=%s frames=%lu", RenderName, frameCount);
}

void AndroidImgui::Shutdown() {
    for (auto &texture: m_Textures) {
        RemoveTexture(texture);
    }
    m_Textures.clear();
    PrepareShutdown();
    My_ImGui_ImplAndroid_Shutdown();
    ImGui::DestroyContext();
    Cleanup();
    ANativeWindow_release(m_Window);
}

BaseTexData *AndroidImgui::LoadTextureData(const std::function<unsigned char *(BaseTexData *)> &loadFunc) {
    BaseTexData tex_data{};

    tex_data.Channels = 4;
    unsigned char *image_data = loadFunc(&tex_data);
    if (image_data == nullptr)
        return nullptr;

    auto result = LoadTexture(&tex_data, image_data);

    stbi_image_free(image_data);

    if (result) {
        m_Textures.push_back(result);
    }
    return result;
}

BaseTexData *AndroidImgui::LoadTextureFromFile(const char *filepath) {
    return LoadTextureData([filepath](BaseTexData *tex_data) {
        return stbi_load(filepath, &tex_data->Width, &tex_data->Height, nullptr, tex_data->Channels);
    });
}

BaseTexData *AndroidImgui::LoadTextureFromMemory(void *data, int len) {
    return LoadTextureData([data, len](BaseTexData *tex_data) {
        return stbi_load_from_memory((const stbi_uc *) data, len, &tex_data->Width, &tex_data->Height, nullptr, tex_data->Channels);
    });
}

void AndroidImgui::DeleteTexture(BaseTexData *tex_data) {
    RemoveTexture(tex_data);
    auto it = std::find(m_Textures.begin(), m_Textures.end(), tex_data);
    if (it != m_Textures.end()) {
        m_Textures.erase(it);
    }
}
