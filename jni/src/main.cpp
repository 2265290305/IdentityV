//微验网络验证//
//如果是AIDE编译jni，请将原main.cpp删除，将此注入好的文件改成main.cpp
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <fcntl.h>
#include <dirent.h>
#include <pthread.h>
#include <fstream>
#include <string.h>
#include <time.h>
#include <malloc.h>
#include <iostream>
#include <fstream>
#include <sys/prctl.h>
#include <sys/stat.h>

// 日志目录。固定一处，PC 侧 `adb pull` 直接取整个目录，不用每次找路径。
#define LOG_DIR "/data/local/tmp/idv_log"

#include<iostream>
#include<ctime>
using namespace std;
#include "draw.h"    //绘制套
#include "AndroidImgui.h"     //创建绘制套
#include "GraphicsManager.h" //获取 当前渲染模式
#include "Android_draw/timer.h"
#include "SoHookIntegration.h"
#include "build_entropy.h"
timer DrawFPS;
float fps = 60;
long int value1,value2,value3;

// 编译期随机熵实际被读取一次，防止链接器/优化器把整段常量数组当死数据丢掉，
// 顺带让每次编译产物字节内容不同（防哈希黑名单），本身不影响任何逻辑。
static volatile unsigned g_entropy_sink = 0;
static void touch_build_entropy() {
    for (unsigned char b : g_build_entropy) {
        g_entropy_sink += b;
    }
    g_entropy_sink += static_cast<unsigned>(g_build_tag[0]);
}

// 运行时把 /proc/<pid>/comm 改成常见系统/内核线程名之一，
// 躲避"进程启动后再扫描进程名"这类动态检测；跟编译期改 LOCAL_MODULE 是两道独立的防线。
static void spoof_process_name() {
    static const char *kDisguiseNames[] = {
        "kworker/u8:3",
        "kworker/0:2",
        "logd.auditd",
        "mdnsd",
        "vndservicemgr",
        "hwservicemanager",
        "wifi_forward",
        "statsd",
    };
    srand(static_cast<unsigned>(time(nullptr)) ^ static_cast<unsigned>(getpid()));
    const char *name = kDisguiseNames[rand() % (sizeof(kDisguiseNames) / sizeof(kDisguiseNames[0]))];
    prctl(PR_SET_NAME, name);
}

void daemonize() {
    pid_t pid = fork();
    if (pid < 0) {
        exit(1);
    }
    if (pid > 0) {
        exit(0); // 父进程退出，子进程继续
    }

    if (setsid() < 0) {
        exit(1);
    }

    spoof_process_name();

    if (chdir("/") < 0) {
        exit(1);
    }

    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);

    // stdin → /dev/null
    open("/dev/null", O_RDONLY);

    // stdout/stderr → 固定日志目录。守护进程以 root 跑，/data/local/tmp 可写。
    // 0666 之外还要显式 chmod：open 的 mode 会被 umask 削掉，不补这一下
    // adb shell(shell 用户) 会 pull 不走，表现是"文件明明在却读不到"。
    // 用 O_TRUNC 不用 O_APPEND：每次启动都是干净的一份，排查时不必在几百 KB
    // 历史里找最后一次运行从哪开始。
    mkdir(LOG_DIR, 0777);
    chmod(LOG_DIR, 0777);
    open(LOG_DIR "/overlay.log", O_WRONLY | O_CREAT | O_TRUNC, 0666);   // fd 1
    open(LOG_DIR "/overlay.err", O_WRONLY | O_CREAT | O_TRUNC, 0666);   // fd 2
    chmod(LOG_DIR "/overlay.log", 0666);
    chmod(LOG_DIR "/overlay.err", 0666);

    // 重定向到文件后 stdio 变成全缓冲，printf 要攒满 4KB 才落盘。
    // 进程被杀或中途 pull 都会丢掉最后那几行 —— 恰好就是出问题时最想看的几行。
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
}


int main(int argc, char *argv[]) {

    daemonize();
    touch_build_entropy();

    // 日志头。构建标记用来确认设备上跑的确实是刚编出来的那一版 ——
    // 产物名每次随机，光看时间戳容易把旧进程的日志当成新的。
    {
        time_t 现在 = time(nullptr);
        char 时间串[64];
        strftime(时间串, sizeof(时间串), "%Y-%m-%d %H:%M:%S", localtime(&现在));
        printf("==== 叠加层启动 %s  构建标记=%s  pid=%d ====\n",
               时间串, g_build_tag, getpid());
    }
    // 发布版本: 注入功能已停用
    // SoHook::StartListeners();

	   
    value1 = 970061201;
    value2 = 16384;
    value3 = 257;
    
    ::graphics = GraphicsManager::getGraphicsInterface(GraphicsManager::VULKAN);//绘图方式

    //获取屏幕信息    
    ::screen_config(); 

    ::native_window_screen_x = (::displayInfo.height > ::displayInfo.width ? ::displayInfo.height : ::displayInfo.width);
    ::native_window_screen_y = (::displayInfo.height > ::displayInfo.width ? ::displayInfo.height : ::displayInfo.width);
    ::abs_ScreenX = (::displayInfo.height > ::displayInfo.width ? ::displayInfo.height : ::displayInfo.width);
    ::abs_ScreenY = (::displayInfo.height < ::displayInfo.width ? ::displayInfo.height : ::displayInfo.width);
    
   // GetPKG();
    
    ::window = android::ANativeWindowCreator::Create("new_edition", native_window_screen_x, native_window_screen_y, permeate_record);
    graphics->Init_Render(::window, native_window_screen_x, native_window_screen_y);
    
    Touch::Init({(float)::abs_ScreenX, (float)::abs_ScreenY}, true);
    Touch::setOrientation(displayInfo.orientation);
    
    new std::thread(read_thread,value1,value2,value3);
    
	DrawFPS.SetFps(fps);
	DrawFPS.AotuFPS_init();
	DrawFPS.setAffinity();
    
    ::init_My_drawdata(); //初始化绘制数据
    
    static bool flag = true;
    while (flag) {
        drawBegin();
        graphics->NewFrame();        
        Layout_tick_UI(&flag);
        graphics->EndFrame();
        DrawFPS.SetFps(fps);
	    DrawFPS.AotuFPS();
    }
    
    graphics->Shutdown();
    // 发布版本: 注入功能已停用
    // SoHook::StopListeners();
    android::ANativeWindowCreator::Destroy(::window);
    return 0;
}
