#include "draw.h"
#include <thread>
#include <cstdint>
#include <stdio.h>
#include "My_font/wrg_font.h"
#include "kerneldriver-qxqd.hpp"
#include "DrawTool.h"
#include "Name.h"
#include "SoHookIntegration.h"
#include "PyProgress.h"
#include "PyGenius.h"
#include "PySelf.h"
#include <linux/input.h>
#include <sstream>
#include <iomanip>
std::string 过滤类名,类名;
char gwd1[25];
char gwd2[25];
float 距离比例=11.886;
float 红夫人X, 红夫人Y, 红夫人Z;
float 红夫人镜像X, 红夫人镜像Y, 红夫人镜像Z;
typedef struct {
    uintptr_t obj;
    uintptr_t objcoor;
    int 阵营;
    char str[256];//翻译名
    char 类名[256];//类名
}DataStruct;
DataStruct data[1000];

bool permeate_record = false;
bool permeate_record_ini = false;
struct Last_ImRect LastCoordinate = {0, 0, 0, 0};
static uint32_t orientation = -1;
ANativeWindow *window; 
// 屏幕信息
android::ANativeWindowCreator::DisplayInfo displayInfo;
// 窗口信息
ImGuiWindow *g_window;
// 绝对屏幕X _ Y
int abs_ScreenX, abs_ScreenY;
int native_window_screen_x, native_window_screen_y;
std::unique_ptr<AndroidImgui>  graphics;
ImFont* zh_font = NULL;
bool niexi;
float 矩阵视野距离;
float 孽蜥距离,孽蜥按住距离;
/*定义*/
bool DrawIo[50];
float 孽蜥触摸X,孽蜥触摸Y;
bool M_Android_LoadFont(float SizePixels) {
    ImGuiIO &io = ImGui::GetIO();
    
    ImFontConfig config;
    config.FontDataOwnedByAtlas = false;
    config.OversampleH = 1;
    config.SizePixels = SizePixels;
    ::zh_font = io.Fonts->AddFontFromMemoryTTF((void *)WRG_Font, WRG_Font_size, SizePixels, &config, io.Fonts->GetGlyphRangesChineseFull());

    return zh_font != nullptr;
}
void init_My_drawdata() {
    M_Android_LoadFont(25.0f); //加载内存字体(含中文TTF+图标)
}


void screen_config() {
    ::displayInfo = android::ANativeWindowCreator::GetDisplayInfo();
}

void drawBegin() {
    if (::permeate_record_ini) {
        LastCoordinate.Pos_x = ::g_window->Pos.x;
        LastCoordinate.Pos_y = ::g_window->Pos.y;
        LastCoordinate.Size_x = ::g_window->Size.x;
        LastCoordinate.Size_y = ::g_window->Size.y;

        graphics->Shutdown();
        android::ANativeWindowCreator::Destroy(::window);
        ::window = android::ANativeWindowCreator::Create("逆天改命", native_window_screen_x, native_window_screen_y, permeate_record);
        graphics->Init_Render(::window, native_window_screen_x, native_window_screen_y);
        ::init_My_drawdata(); //初始化绘制数据
    } 

    screen_config();
    if (::orientation != displayInfo.orientation) {
        ::orientation = displayInfo.orientation;
        Touch::setOrientation(displayInfo.orientation);
        if (g_window != NULL) {
            g_window->Pos.x = 100;
            g_window->Pos.y = 125;        
        }        
        //cout << " width:" << displayInfo.width << " height:" << displayInfo.height << " orientation:" << displayInfo.orientation << endl;
    }
}

struct Vector3A
{
	float X;
	float Y;
	float Z;

	  Vector3A()
	{
		this->X = 0;
		this->Y = 0;
		this->Z = 0;
	}

	Vector3A(float x, float y, float z)
	{
		this->X = x;
		this->Y = y;
		this->Z = z;
	}

};

float xs_prime_mirror, ys_prime_mirror;
float xs_final, ys_final;
void calculate_mirror_reflection(float x1, float y1, float x2, float y2, float xs, float ys, float *xs_prime, float *ys_prime) {
    float xm = (x1 + x2) / 2.0;
    float ym = (y1 + y2) / 2.0;

    *xs_prime = 2.0 * xm - xs;
    *ys_prime = 2.0 * ym - ys;
}

void calculate_line_reflection(float x1, float y1, float x2, float y2, float xs, float ys, float *xs_prime, float *ys_prime) {
    float A = y2 - y1;
    float B = x1 - x2;
    float C = x2 * y1 - x1 * y2;

    float D = A * xs + B * ys + C;
    float denom = A * A + B * B;

    *xs_prime = xs - 2.0 * A * D / denom;
    *ys_prime = ys - 2.0 * B * D / denom;
}


uintptr_t libbase;
uintptr_t Arrayaddr, Count, Matrix;
uintptr_t 对象,对象阵营,自身,自身阵营,namezfcz,namezfc;
uintptr_t 红夫人,红夫人镜像,镜子,捏镜子,镜子预览;
float 镜线X1, 镜线Y1, 镜线X2, 镜线Y2;   // 本帧镜面在水平面上的直线(两点)，mirror==true 时有效
int 数量,zfcz,zfc;
float 过滤矩阵[17];
float matrix[16];
float angle;

static bool show_draw_Rect = true;//方框
static bool show_draw_Line = true;//射线
static bool show_draw_Camera = false;//相机
static bool show_draw_Door = true;//门(开门进度，走 Python 层单独绘制，不依赖 getscene)
static bool show_draw_Box = false;//盒子
static bool show_draw_Name = true;//名字
static bool show_draw_Distance = true;//距离
static bool show_draw_Cellar = true;//地窖
static bool show_draw_Chair = false;//椅子
static bool show_draw_Prop = false;//道具
static bool show_draw_Genius = true;//天赋/辅助特质(走 CPython 链路，见 PyGenius.h)
static bool show_draw_prophet = true;//预知监管者
static bool redqueenmod = false;//红夫人模式
static bool show_draw_secret_mechine = true;//密码机(含破译进度/进度条/最后一台)
static bool show_draw_Role = false;//角色
static bool show_draw_touch = false;//孽蜥
static bool show_draw_ClassName = false;//类名
static bool Debugging = false;//调试
static bool mirror = false;//镜子状态
static bool show_demo_window = false;
static bool show_another_window = false;
static bool show_window = true;  // 音量键控制：音量下=隐藏，音量上=显示
static bool voice = true;
static bool inform_ghost = false; // 显示鬼魂
static bool show_sohook = false;  // 骨骼与进度覆盖层

float z_x, z_y, z_z, d_x, d_y, d_z, camera, r_x, r_y, r_w;
float X1,Y1,X2,Y2,W,H,MIDDLE,TOP,BOTTOM;
int 距离;	
char objtext[256];
//char content[1024];
char Team[1024];
char Name[1024];
char 监管者预知[1024];

float px,py;
Vector3A D,Z,M;


void AimBotAuto()
{   
    bool 触摸状态 = false;
    // 是否按下触摸


    float SpeedMin = 2.0f;
    // 临时触摸速度

    double w = 0.0f, h = 0.0f, cmp = 0.0f;
    // 宽度 高度 正切

    /*double ScreenX = displayInfo.width, ScreenY = displayInfo.height;
    const Vector2 P;
    P.x=displayInfo.width;
    P.y=displayInfo.height;
    const Vector2 P(ScreenX, ScreenY); */

    //const ImVec2 P(ScreenX, ScreenY); 
    double ScreenX , ScreenY;
    if (displayInfo.width>displayInfo.height)
    {
    ScreenX = displayInfo.height;
    ScreenY = displayInfo.width;
    }
    else
    {
    
    ScreenX = displayInfo.width;
    ScreenY = displayInfo.height;
    }
    const Vector2 P(ScreenX, ScreenY);
    Touch::Init(P,false);	//初始化触摸

    // 分辨率(竖屏)PS:滑屏用的坐标是竖屏状态下的

    double ScrXH = ScreenX / 2.0f;
    // 一半屏幕X

    double ScrYH = ScreenY / 2.0f;
    // 一半屏幕X

    static float TargetX = 0;
    static float TargetY = 0;
    // 触摸目标位置
    //Vector3A obj;   
   
	
	
    while (1)
    {
    /*if (niexi)
    {
    Touch::Down(450,2500);
    触摸状态 = true;
    usleep(1000*100);
    niexi=false;
    }
    if (触摸状态)
    {
    Touch::Up();
    触摸状态 = false;
    usleep(1000*100);
    }*/
    Touch::Down(孽蜥触摸X,孽蜥触摸Y);
    usleep(1000*10);
    for (int i = 0; i < 50; i++)
    {
    Touch::Move(孽蜥触摸X+i*5,孽蜥触摸Y+i*5);
    usleep(1000*10);
    }
    //usleep(1000*10);
    Touch::Up();
    //Touch::Close;
    usleep(1000*1000);
    usleep(1000*100);
    }
}
ImColor 红色 = ImColor(255,0,0,255);
ImColor 绿色 = ImColor(0,255,0,255);
ImColor 蓝色 = ImColor(0,0,255,255);
ImColor 黄色 = ImColor(255,255,0,255);
ImColor 紫色 = ImColor(255,0,255,255);
ImColor 黑色 = ImColor(0,0,0,255);
ImColor BoneColor = ImColor(255,0,0,255);
ImColor BotBoneColor = ImColor(255,255,255,255);
int 状态 = 0;
int 数据获取状态 = 0;
int 遍历次数=0;
bool 首帧打印 = false;
bool 首帧矩阵 = false;
char extractedString[64];
long int MatrixOffset = 0,ArrayaddrOffset = 0;
typedef struct {
    unsigned long addr;
    unsigned long taddr;
} ModuleBssInfo;


ModuleBssInfo get_module_bss(int pid, const char *module_name) {
    FILE *fp;
    ModuleBssInfo info = {0, 0};
    char filename[64];
    char line[1024];

    // 生成文件名
    snprintf(filename, sizeof(filename), "/proc/%d/maps", pid);

    // 打开文件
    fp = fopen(filename, "r");

    bool found_module = false;

    if (fp!= NULL) {
        while (fgets(line, sizeof(line), fp)) {
            // 先判断是否包含模块名
            if (strstr(line, module_name)!= NULL) {
                found_module = true;
            }

            if (found_module) {
                // 检查是否满足rw权限且行长度符合要求
                long addr,taddr;
                sscanf(line, "%lx-%lx", &addr, &taddr);
                if (strstr(line, "rw")!= NULL && strlen(line) < 86 &&(taddr-addr)/4096>=2800) {
                //printf("%d", (taddr-addr)/4096);
                
                    // 将行按空格分割成字符串数组（这里简单示意，实际可能需要更完善的分割函数）
                    char *words[10];
                    int numWords = 0;
                    char *token = strtok(line, " ");
                    while (token!= NULL && numWords < 10) {
                        words[numWords++] = token;
                        token = strtok(NULL, " ");
                    }

                    // 遍历分割后的字符串数组，查找地址范围并转换
                    for (int i = 0; i < numWords; i++) {
                        if (sscanf(words[i], "%lx-%lx", &info.addr, &info.taddr) == 2) {
                            fclose(fp);
                            return info;
                        }
                    }

                    // 如果未找到正确格式的地址范围，设置为0并返回
                    info.addr = 0;
                    info.taddr = 0;
                    fclose(fp);
                    return info;
                }
            }
        }

        fclose(fp);
    }

    return info;
}

ModuleBssInfo get_module_bssgjf(int pid, const char *module_name) {
    FILE *fp;
    ModuleBssInfo info = {0, 0};
    long addr,taddr;
    char *pch;
    char filename[64];
    char line[1024];
    snprintf(filename, sizeof(filename), "/proc/%d/maps", pid);
    fp = fopen(filename, "r");
    bool is = false;
    if (fp!= NULL) {
        while (fgets(line, sizeof(line), fp)) {
        sscanf(line, "%lx-%lx", &addr, &taddr);
            if (strstr(line, module_name) &&strstr(line, "r-xp")!= NULL &&(taddr-addr)== 114982912) {
                is = true;
            }
            if (is) {
                if (strstr(line, "rw")!= NULL &&!feof(fp) && (strlen(line) < 86)) {
                long addr,taddr;
                sscanf(line, "%lx-%lx", &addr, &taddr);
                if ((taddr-addr)/4096<=3000)
                continue;
                    if (sscanf(line, "%lx-%lx", &info.addr, &info.taddr)!= 2) {
                        // 处理转换失败的情况
                        info.addr = 0;
                        info.taddr = 0;
                        break;
                    }
                    break;
                }
            }
        }
        fclose(fp);
    }
    return info;
}
int get_name_pid1(const char *packageName) {
    int id = -1;
    DIR *dir;
    FILE *fp;
    char filename[64];
    char cmdline[64] = {};
    struct dirent *entry;
    dir = opendir("/proc");
    if (dir == NULL) {
        return -1;
    }
    while ((entry = readdir(dir))!= NULL) {
        id = atoi(entry->d_name);
        if (id!= 0) {
            sprintf(filename, "/proc/%d/cmdline", id);
            fp = fopen(filename, "r");
            if (fp) {
                char *readResult = fgets(cmdline, sizeof(cmdline), fp);
                fclose(fp);
                if (readResult != NULL &&
                    (strstr(cmdline, packageName) != NULL || strstr(cmdline, "com.netease.idv") != NULL) &&
                    strstr(cmdline, "com") != NULL && strstr(cmdline, "PushService") == NULL &&
                    strstr(cmdline, "gcsdk") == NULL) {
                    sprintf(extractedString, "%s", cmdline);
                    closedir(dir);
                    return id;
                }
            }
        }
    }
    closedir(dir);
    return -1;
}
long getModuleBasegjf(int pid, const char *module_name) {
    FILE *fp;
    long addr,taddr;
    char *pch;
    char filename[64];
    char line[1024];
    snprintf(filename, sizeof(filename), "/proc/%d/maps", pid);
    fp = fopen(filename, "r");
    bool is = false;
    if (fp!= NULL) {
        while (fgets(line, sizeof(line), fp)) {
                if (strstr(line, "r-xp")!= NULL &&!feof(fp) && strstr(line, module_name)) {
                sscanf(line, "%lx-%lx", &addr, &taddr);
                    if ((taddr-addr)== 114982912) {
                        // 处理转换失败的情况
                        fclose(fp);
                        return addr;
                        break;
                    }
                    //break;
                }
            
        }
        fclose(fp);
    }
    return 0;
}

int c;
char libso[256] = {"libclient.so"};

// ==================== 过滤表 ====================
static const char* g_filter_keywords[] = {
    "creature",
    "dm65_survivor_girl_page",
    "skill_hudie",
    "h55_joseph_camera",
    "burke_console",
    "redqueen_e_heijin_yizi",
    "qiutu_box",
    "weapon",
    "nvyao"
};
static constexpr int g_filter_count = sizeof(g_filter_keywords) / sizeof(g_filter_keywords[0]);
inline bool should_filter(const std::string& name) {
    for (int i = 0; i < g_filter_count; ++i) {
        if (name.find(g_filter_keywords[i]) != std::string::npos)
            return true;
    }
    return false;
}

// ================== 幽灵/隐身状态判定(合并原有特殊场景排除) ==================
// +0x70 == 0x1000000 且 +0x1a0 == 450.0 才是"正常在场"的真实角色/道具;
// 其余取值(包括65150鬼魂视角等)统一视为幽灵态, 由 inform_ghost 决定是否仍然显示。
bool ShouldSkipEntity(const DataStruct& obj) {
    // 这几类名字命中即直接跳过, 与幽灵判定无关(原本散落在渲染循环里, 现在收拢到一处)
    if (strstr(obj.类名, "h55_joseph_camera") != NULL) return true;   // 约瑟夫相机
    if (strstr(obj.类名, "redqueen_mirror") != NULL) return true;      // 红夫人镜子
    if (strstr(obj.类名, "burke_console") != NULL) return true;        // 疯眼场景
    if (strstr(obj.类名, "chr\\guajian") != NULL) return true;
    if (strstr(obj.类名, "girl_e_sj_zuoyi") != NULL) return true;
    if (strstr(obj.类名, "h55_survivor_w_shangren_tiaoban") != NULL) return true; // 商人跳板

    // 废弃模型(形态切换后留在数组里的旧形态)。这条取代了原来那份
    // "红蝶/无常/歌剧/破轮/木偶/冒险家" 的类名黑名单 —— 那份是按角色名猜的,
    // 漏一个角色就漏一个, 而且只在 inform_ghost 打开时才生效。
    // 现在直接问引擎: unit.another_model + 0x20 就是废弃形态的场景对象指针,
    // 精确、跟视角无关、不用维护名单。详见 PySelf.h 文件头。
    if (本机::是废弃模型(obj.obj)) return true;

    int checkVal = getDword(obj.obj + 0x70);
    float checkFloat = getFloat(obj.obj + 0x1a0);
    bool is_ghost_obj = (checkVal != 0x1000000 || checkFloat != 450.0f);

    // 注意 +0x70 就是 NeoX 模型对象的 visible, 是**渲染剔除的结果**:
    // 废弃模型恒不可见, 但远处的真实对象同样不可见。所以它只能当"要不要按幽灵显示"的开关,
    // 绝不能当存在性/真假判据 —— 那会重演"求生者只看得到身边的密码机"。
    if (is_ghost_obj && !inform_ghost) return true;
    return false;
}

void read_thread(long int PD1,long int PD2,long int PD3)
{
    bool waitingLogged = false;
    while (pid <= 0) {
        pid = get_name_pid1("dwrg");
        if (pid > 0) break;
        if (!waitingLogged) {
            printf("[进程] 等待游戏进程启动\n");
            waitingLogged = true;
        }
        sleep(1);
    }
    driver->initialize(pid);
    printf("[进程] 已获取游戏进程 PID=%d 包名=%s\n", pid, extractedString);

    ModuleBssInfo result;
    // libbase 统一从maps取, 不用内核ioctl
    while (libbase == 0) {
        char mappath[64];
        snprintf(mappath, sizeof(mappath), "/proc/%d/maps", pid);
        FILE *fp = fopen(mappath, "r");
        if (fp == NULL) {
            printf("[进程] 无法打开 %s，重新等待游戏进程\n", mappath);
            pid = -1;
            while (pid <= 0) {
                sleep(1);
                pid = get_name_pid1("dwrg");
            }
            driver->initialize(pid);
            continue;
        }
        char line[1024];
        bool 官方 = strstr(extractedString, "com.netease.idv") != NULL;
        while (fgets(line, sizeof(line), fp)){
            long a, t;
            if (sscanf(line, "%lx-%lx", &a, &t) != 2) continue;
            if (libbase == 0 && strstr(line, "r-xp")){
                if (官方 && strstr(line, "."))    libbase = a;
                if (!官方 && strstr(line, libso)) libbase = a;
            }
        }
        fclose(fp);
        if (libbase == 0) {
            printf("[基址] 暂未找到游戏模块，1秒后重试\n");
            sleep(1);
        }
    }
    if (strstr(extractedString, "com.netease.idv") != NULL)
        result = get_module_bssgjf(pid, ".");
    else
        result = get_module_bss(pid, libso);
    printf("[基址] libbase=0x%llX BSS=0x%llX-0x%llX\n",
           (unsigned long long)libbase, (unsigned long long)result.addr, (unsigned long long)result.taddr);
    if (libbase < 0x5000000000){
        printf("[错误] libbase无效, 无法继续\n");
        sleep(9999);
    }
    c = (result.taddr-result.addr)/4096;
    long buff[512];
    while (MatrixOffset==0||ArrayaddrOffset==0)
    {
    	for (int i = 0; i < c; i++){
        	vm_readv(result.addr+(i*4096), &buff, 0x1000);
        	for (int ii=0;ii<512;ii+=1){

        	    if (MatrixOffset == 0 && *(long long*)(&buff[ii]) == 0x656A624F72655028LL){
        	        uint64_t 签名地址 = result.addr + i*4096 + ii*8;
        	        // 2026-09-17 热更后 +0x430 恒为0。该处是一组步长0x88的指针，三个元素都能走到
        	        // 同一个矩阵对象，这次更新只是让它整体位移了0x10。写死任何一个下次还会废，
        	        // 改成在窗口内找"整条链都走得通"的槽位(p1有效 且 p1+0xa58 也有效)，自愈。
        	        // 注意上界：只判 >0x5000000000 会让浮点垃圾(如0x400674954293DF)也蒙混过关，
        	        // 本机用户态指针都在 0x77xx/0x79xx 段，统一卡 <0x8000000000。
        	        for (int off = 0x300; off <= 0x500; off += 8){
        	            uint64_t 候选 = 签名地址 + off;
        	            uint64_t p1 = getPtr64(候选);            // getPtr64 已做 0xB4 掩码
        	            if (p1 <= 0x5000000000 || p1 >= 0x8000000000) continue;
        	            uint64_t p2 = getPtr64(p1 + 0xa58);
        	            if (p2 <= 0x5000000000 || p2 >= 0x8000000000) continue;
        	            MatrixOffset = 候选 - libbase;
        	            printf("[矩阵命中] 签名=0x%llX 槽位=+0x%X 根=0x%llX MatrixOffset=0x%lx\n",
        	                   (unsigned long long)签名地址, off, (unsigned long long)候选, MatrixOffset);
        	            break;
        	        }
        	    }

        	    if (ArrayaddrOffset == 0 && buff[ii] == 16384){
                    if (getDword(result.addr + 4096*i + 8*ii - 0x8) == 257 &&
                        getFloat(result.addr + 4096*i + 8*ii - 16) == 1.0f){
                        ArrayaddrOffset = result.addr - libbase + i*4096 + ii*8 + 56;
                        printf("[数组命中] 偏移=0x%llX\n", (unsigned long long)ArrayaddrOffset);
                    }
                }
    	    }
        }
        if (MatrixOffset!=0 && ArrayaddrOffset!=0){
            uint64_t tmpArr = getPtr64(libbase + ArrayaddrOffset);
            uint64_t tmpEnd = getPtr64(libbase + ArrayaddrOffset + 8);
            if (tmpArr > 0x5000000000 && tmpEnd > tmpArr) break;
            printf("[数组无效] 重新扫描 Array=0x%llX End=0x%llX\n", (unsigned long long)tmpArr, (unsigned long long)tmpEnd);
            ArrayaddrOffset = 0;
        }
        sleep(5);
    }
    状态 = 2;

    // 密码机破译进度：走 CPython 对象图，自带后台线程(400ms 一轮)，跟这里的 3 秒大循环解耦
    密码机进度::启动(libbase);
    // 天赋与辅助特质：同一条 CPython 链路，独立线程 500ms 一轮(一局内基本不变，不用刷那么勤)
    天赋::启动(libbase);
    // 自身锚点(g_cam_ctrl.unit)与废弃模型黑名单：100ms 一轮，切换操控对象时要立刻跟上
    本机::启动(libbase);

    Arrayaddr = getPtr64(libbase + ArrayaddrOffset);
    uint64_t ArrayEnd = getPtr64(libbase + ArrayaddrOffset + 8);
    Count = (ArrayEnd - Arrayaddr) / 8;
    if (Count <= 0 || Count > 10000) Count = 3000;
    printf("[数组] Arrayaddr=0x%llX End=0x%llX Count=%d\n", (unsigned long long)Arrayaddr, (unsigned long long)ArrayEnd, (int)Count);

    while (true)
    {        
    	uint64_t curArray = getPtr64(libbase + ArrayaddrOffset);
        uint64_t curArrayEnd = getPtr64(libbase + ArrayaddrOffset + 8);
        uint64_t curCount = curArrayEnd > curArray ? (curArrayEnd - curArray) / 8 : 0;
	    if (curArray < 0x5000000000 || curCount == 0 || curCount > 10000) {
            数量 = 0;
            状态 = 1;
            sleep(1);
            continue;
        }
        Arrayaddr = curArray;
        Count = curCount;
        状态 = 2;
    	int 指针数量=0;
        红夫人 = 0;      // 每轮清零，防止跨局/跨帧残留
        红夫人镜像 = 0;
        镜子 = 0;
        镜子预览 = 0;
        for (int ii = 0; ii < Count && 指针数量 < 1000; ii++){
            对象 = getPtr64(curArray+0x8 * ii);	// 遍历数量次数            
                
    		if (对象 == 0)   			
        		continue;    			    			
    		
    	    uint64_t 类名对象 = getPtr64(getPtr64(getPtr64(getPtr64(getPtr64(对象 + 0xf8)+0x0)+0x8)+0x20)+0x20)+0x0;
            int len = getDword(类名对象 + 0x10);
            if (len >= 256 || len == 0 || len < 0)
                continue;

            过滤类名.resize(len);
            vm_readv(getPtr64(类名对象 + 0x8), &过滤类名[0], len);
        		
			int 过滤重复指针=0;
			float pd1 = getFloat(对象 + 0x1a0);
			float pd2 = getFloat(对象 + 0x298);
			for (int i = 0; i < 指针数量; i++){
        		if(对象 == data[i].obj){
        		    过滤重复指针=1;
        		}        		    
        	}
        	if (过滤重复指针 == 1){
    		    continue;
    		}
        	if (should_filter(过滤类名)) {
        		continue;//过滤随从等无关对象
        	}
        	std::string s;
        	//预知监管者
            if (show_draw_prophet){//预知开始
                if (strstr(过滤类名.c_str(), "burke_console") == NULL&&strstr(过滤类名.c_str(), "h55_joseph_camera") == NULL&&strstr(过滤类名.c_str(), "redqueen_e_heijin_yizi") == NULL&&strstr(过滤类名.c_str(), "_lod") == NULL){
                    if (strstr(过滤类名.c_str(), "boss") != NULL){
                        s += getboss(过滤类名.c_str());
                        sprintf(监管者预知, "%s", s.c_str());
                    }       
                }
            }//预知结束
    		
			if (strstr(过滤类名.c_str(), "player") != NULL||strstr(过滤类名.c_str(), "boss") != NULL || pd1 == 450 || strstr(过滤类名.c_str(), "scene") != NULL || strstr(过滤类名.c_str(), "prop") != NULL || strstr(过滤类名.c_str(), "mirror") != NULL || Debugging )
			{
    			data[指针数量].obj = 对象;
    			// 阵营/str 必须先清零：下面那串 if-else 只有五个分支，而入口条件里的
    			// `pd1 == 450` 和 `Debugging` 能让对象进来却一个分支都不命中。
    			// data[] 是全局数组、原地复用，不清就会**沿用上一帧同下标那个对象的
    			// 阵营和名字** —— 表现是装饰物被当成角色画出来、还顶着别人的名字，
    			// 并且 内核人物数量 虚高。正常游玩时类名基本都能命中 player/boss，
    			// 所以这个洞主要在开「绘制调试」时发作。
    			data[指针数量].阵营 = 0;
    			data[指针数量].str[0] = '\0';
    			if (strstr(过滤类名.c_str(), "boss") != NULL){
    			//data[指针数量].str=getboss(过滤类名.c_str());
    			strcpy(data[指针数量].str, getboss(过滤类名.c_str()));
    			data[指针数量].阵营=1;
    			}
    			else if (strstr(过滤类名.c_str(), "player") != NULL||strstr(类名.c_str(), "npc_deluosi_dress_ghost") != NULL||strstr(类名.c_str(), "h55_pendant_huojian") != NULL){
    			//data[指针数量].str=getplayer(过滤类名.c_str());
    			strcpy(data[指针数量].str, getplayer(过滤类名.c_str()));
    			data[指针数量].阵营=2;
    			}
    			else if (strstr(过滤类名.c_str(), "scene") != NULL){
    			const char* scene_result = getscene(过滤类名.c_str());
    			if (scene_result == NULL) continue;
    			strcpy(data[指针数量].str, scene_result);
    			data[指针数量].阵营=3;
    			}
    			else if (strstr(过滤类名.c_str(), "prop") != NULL){
    			const char* prop_result = getprop(过滤类名.c_str());
    			if (prop_result == NULL) continue;
    			strcpy(data[指针数量].str, prop_result);
    			data[指针数量].阵营=4;
    			}

    			else if (strstr(过滤类名.c_str(), "redqueen") != NULL&&strstr(过滤类名.c_str(), "mirror") != NULL&&strstr(过滤类名.c_str(), "model") != NULL){
    			// fx/model/redqueen_mirror_model_obj_001.gim：准备放镜时的预览镜子(PlaceIndicator.rtc_model)，
    			// 常驻复用同一个对象，只在准备阶段可见。用法见 Draw_Main 里的镜线计算
    			data[指针数量].阵营=5;
    			镜子预览 = 对象;
    			}
    			//sprintf(data[指针数量].类名, "%s", 过滤类名.c_str());
    			strcpy(data[指针数量].类名, 过滤类名.c_str());
    			data[指针数量].objcoor=getPtr64(对象+0x28);
    			if (!首帧打印){
        			printf("[实体] %s 地址=0x%llX 坐标地址=0x%llX 坐标=(%.1f,%.1f,%.1f) 类名=%s\n",
        			       data[指针数量].str, (unsigned long long)对象,
        			       (unsigned long long)data[指针数量].objcoor,
        			       getFloat(data[指针数量].objcoor + 0xa0),
        			       getFloat(data[指针数量].objcoor + 0xa4),
        			       getFloat(data[指针数量].objcoor + 0xa8),
        			       data[指针数量].类名);
        		}
    			指针数量++;
			}
    			
			//红夫人模式：本体和镜中红夫人的类名都是 redqueen.gim，只能靠对象字段区分。
			// 2026-09-22 热更后旧判据失效：镜中红夫人的 +0x70 不再是 65150(鬼魂)，也在地面上，
			// 于是被当成本体，镜面算错，求生者镜像跟着红夫人跑。
			// 现在用 +0x6D 实体种类位(实测，镜子放出状态)：
			//   本体       0x50 = 0x40(角色实体) | 0x10
			//   镜中红夫人 0x90 = 0x80(不占玩家槽位的角色) | 0x10
			// 整字节会跳变(见过 0x50->0xD0)，但 0x40 位稳定，所以只看位不看整字节。
			// 镜面 = 本体与镜中红夫人连线的垂直平分线，已用 Python 侧 MaryMirrorUnit.position/direction 验证。
			// 镜子没放出时镜中红夫人停在 y≈-1000，下面坐标读取处的 Z>=-300 会让 mirror=false。
			if (pd1==450){
			    uintptr_t coorPtr = getPtr64(对象 + 0x28);
			    if (strstr(过滤类名.c_str(), "boss") != NULL && strstr(过滤类名.c_str(), "redqueen") != NULL && strstr(过滤类名.c_str(), "mirror") == NULL
			        && getFloat(coorPtr + 0xa0) != 0 && getFloat(coorPtr + 0xa8) != 0) {
			        uint8_t 种类 = 0;
			        vm_readv(对象 + 0x6D, &种类, 1);
			        if (种类 & 0x40)      红夫人 = 对象;       // 本体
			        else if (种类 & 0x80) 红夫人镜像 = 对象;   // 镜中红夫人
			    }
    			if (strstr(过滤类名.c_str(), "boss") != NULL && strstr(过滤类名.c_str(), "mirror") != NULL
    			    && getFloat(coorPtr + 0xa0) != 0 && getFloat(coorPtr + 0xa8) != 0)
    			{
    		    	镜子=对象;
    			}    			
			}    						
        }
        if (!首帧打印){
            首帧打印 = true;
            printf("[首帧调试] 矩阵16值已打印 实体列表已打印\n");
        }
        数量 = 指针数量;
        sleep(3);
    }
}






// ---- 场景对象"是否本局真实存在"判定 ----
// 游戏把所有候选刷新点、以及角色的每种形态都实例化成对象放进数组，本局/当前只激活其中一部分。
// 未激活的那些类名、坐标全都正常，光看类名/坐标区分不出来。
//
// **判据是 +0x70（代码里原本叫 jxpd/checkVal）**，它是三态的：
//     0          = 本局根本不存在 / 当前不是活跃形态
//     0x1000000  = 正常存在
//     其它非0    = 存在，但处于鬼魂/特殊状态（旧记录里的 65150 属于这一档）
//
// 怎么确认的：红蝶本体与 opposite 形态做变身前后差分，两个对象在 0x300 字节里
// **各自只有 +0x70 这一列发生变化，且方向相反**（本体 0x1000000->0，opposite 0->0x1000000），
// 同时对照组(9个约瑟夫相机 + 15个密码机)零变化。语义非常干净。
// 横向验证：约瑟夫相机(每局默认加载但不存在) 4/4 全为 0；求生者 8 个对象里 +0x70!=0 的正好 4 个(实际人数)。
//
// 注意 +0x1a0 是"类型"字段(450=角色 500=可交互物)，**不是存在性**：
// 曾经误用它单独做判据，结果一个不存在的密码机 +0x1a0 恰好就是 500，照样被画出来。
//
// 曾经用过 +0x30(实例化节点指针)，**已废弃**：它不稳定，同样是约瑟夫相机，
// 一局里测到 13个只有1个非空，另一局 9个全部非空，不能用。
// +0x6D 与 +0x73 都是单字节，分别藏在 +0x6C / +0x70 这两个 dword 里。
//   +0x73 = 1  -> 当前活跃/在场（常见的 0x1000000 就是这个字节的 dword 形式）
//   +0x6D      -> 实体种类位域: 0x40=角色/生物  0x10=玩家阵营相关  0x00=纯场景装饰
static inline unsigned 实体活跃位(uintptr_t obj) { return (getDword(obj + 0x70) >> 24) & 0xFF; }
static inline unsigned 实体种类位(uintptr_t obj) { return (getDword(obj + 0x6C) >> 8)  & 0xFF; }

// **千万不要拿活跃位当"本局是否存在"用**：它是"当前对本机客户端可见"的意思，跟视角走。
// 实测同一张图，监管视角下 12 台密码机活跃位全是 1，换成求生者视角只剩 1 台是 1。
// 早先用它做密码机判据，导致求生者只能看到身边那一台，绕了一大圈才发现。
//
// 真正视角无关的存在性标记是 +0x240：
//   实测 约瑟夫相机 7/7 = 0，破轮台 4/4 = 0（这两类都是每局默认加载、本局并不存在的装饰）
//        密码机 真的 7 个 = 2 / 假的 5 个 = 0，箱子、求生者、监管 全部 = 2
static inline bool 实体本局存在(uintptr_t obj)
{
    return getDword(obj + 0x240) == 2;
}
// 破译进度配色：<20 绿、20~60 黄、>60 红
static inline ImColor 进度颜色(float v)
{
    if (v < 20.0f)  return 绿色;
    if (v <= 60.0f) return 黄色;
    return 红色;
}

static inline bool 是真实密码机(uintptr_t obj)
{
    // 活跃位 + 一个密码机专用的辅助值。单用任何一个都不够：
    //   只判 +0x1a0 -> 未激活的候选机器该值恰好也可能是 500
    //   只判活跃位  -> 会混进一个位于原点(0,0,0)的占位对象
    return getFloat(obj + 0x1a0) == 500.0f && getDword(obj + 0x240) == 2;
}

// 相机坐标偏移自愈。旧版写死 Matrix-0x290，2026-09-17 热更后该处恒为 (0,0,0)。
// VP矩阵本身能反解出相机世界坐标(实测与真实字段差约6单位/0.5游戏米)，精度不足以直接用，
// 但足够当"校验器"：在窗口内扫，谁最接近反解值谁就是真正的相机字段。
// 这样以后引擎再挪这个字段，不用人工重新找。
static int g_相机偏移 = 0;
static int g_相机尝试 = 0;
static int 取相机偏移(uint64_t Matrix)
{
    if (g_相机偏移 != 0) return g_相机偏移;
    if (++g_相机尝试 > 60) { g_相机偏移 = -0x290; return g_相机偏移; }   // 找不到就退回旧值，不卡绘制

    float m[16];
    for (int i = 0; i < 16; i++) m[i] = getFloat(Matrix + i*4);
    // 行向量约定 clip = world * M：左上3x3记为A，第3行为平移t，相机世界坐标 = -t * A^-1
    float a=m[0], b=m[1], c=m[2];
    float d=m[4], e=m[5], f=m[6];
    float g=m[8], h=m[9], k=m[10];
    float det = a*(e*k-f*h) - b*(d*k-f*g) + c*(d*h-e*g);
    if (!(det > 1e-6f || det < -1e-6f)) return -0x290;
    float ai0=(e*k-f*h)/det, ai1=(c*h-b*k)/det, ai2=(b*f-c*e)/det;
    float ai3=(f*g-d*k)/det, ai4=(a*k-c*g)/det, ai5=(c*d-a*f)/det;
    float ai6=(d*h-e*g)/det, ai7=(b*g-a*h)/det, ai8=(a*e-b*d)/det;
    float t0=m[12], t1=m[13], t2=m[14];
    float cx = -(t0*ai0 + t1*ai3 + t2*ai6);
    float cy = -(t0*ai1 + t1*ai4 + t2*ai7);
    float cz = -(t0*ai2 + t1*ai5 + t2*ai8);
    if (!(cx > -1e5f && cx < 1e5f)) return -0x290;   // 同时挡住 NaN/Inf

    int best = 0; float bestErr = 1e9f;
    for (int off = -0x400; off <= -0x40; off += 4){
        float x = getFloat(Matrix + off);
        float y = getFloat(Matrix + off + 4);
        float z = getFloat(Matrix + off + 8);
        if (!(x > -1e5f && x < 1e5f)) continue;
        if (!(y > -1e5f && y < 1e5f)) continue;
        if (!(z > -1e5f && z < 1e5f)) continue;
        float err = fabsf(x-cx) + fabsf(y-cy) + fabsf(z-cz);
        if (err < bestErr){ bestErr = err; best = off; }
    }
    if (best != 0 && bestErr < 60.0f){
        g_相机偏移 = best;
        printf("[相机命中] 偏移=%d(-0x%X) 反解=(%.1f,%.1f,%.1f) 实读=(%.1f,%.1f,%.1f) 误差=%.2f\n",
               best, -best, cx, cy, cz,
               getFloat(Matrix+best), getFloat(Matrix+best+4), getFloat(Matrix+best+8), bestErr);
        return best;
    }
    return -0x290;
}

void Draw_Main(ImDrawList *Draw){
    if (libbase == 0 || 状态 == 0) return;  // 数据未就绪，跳过本帧绘制
    int 内核人物数量 = 0;
    const bool 模仿者绘制中 = false; // 发布版本: 注入功能已停用, SoHook::IsCopycatDrawingActive() 不再调用

    // ---- 自身锚点：引擎自己持有的答案。相机深度启发式兜底已停用，这是唯一来源 ----
    // g_cam_ctrl.unit 就是"当前视角/操控的单位"，切到机械玩偶/梦之信徒时会跟着换，
    // 所以这里每帧重取：一旦换了操控对象，自身锚点立刻跟上。
    // (不能用 g_unit —— 那是"我的主角色"，切从属时纹丝不动，会高亮错人。见 PySelf.h)
    uint64_t 权威自身 = 0;
    const bool 有权威自身 = 本机::锚点(权威自身);
    if (有权威自身) {
        自身 = (uintptr_t)权威自身;
        int c = 本机::阵营();
        if (c == 1 || c == 2) 自身阵营 = (uintptr_t)c;
    }

    Matrix = getPtr64(getPtr64(libbase + MatrixOffset) + 0xa58) + 0x2c0; //矩阵
    int 相机偏移 = 取相机偏移(Matrix);
    M.X = getFloat(Matrix + 相机偏移);
    M.Z = getFloat(Matrix + 相机偏移 + 4);
    M.Y = getFloat(Matrix + 相机偏移 + 8);
    
    // ---- 红夫人镜面：两个来源，先放下的镜子，其次准备阶段的预览镜子 ----
    // (1) 镜子已放下：本体与镜中红夫人连线的垂直平分线(已用 Python 侧 MaryMirrorUnit 验证)。
    //     镜子没放时镜中红夫人停在 y≈-1000/-2000，Z>=-300 挡掉。
    // (2) 准备放镜(SkillMaryPlaceMirrorPrepare)：镜中红夫人还在地下，但预览镜子已经可见。
    //     预览镜子的 objcoor 是 3x3 旋转(+0x78 起、每行 12 字节) + 位置(+0xa0)：
    //       +0x90/+0x98 = 局部 Z 轴 = 投掷方向 = 镜面法向(与 Python rtc_model.world_transformation 第 3 行一致)
    //     可见位 +0x73 只在准备阶段为 1。
    mirror = false;
    bool 本体有效 = false, 镜像有效 = false;
    if (红夫人 != 0) {
        uintptr_t 红夫人坐标指针 = getPtr64(红夫人 + 0x28);
        if (红夫人坐标指针 != 0) {
            红夫人X = getFloat(红夫人坐标指针 + 0xa0);
            红夫人Z = getFloat(红夫人坐标指针 + 0xa4);
            红夫人Y = getFloat(红夫人坐标指针 + 0xa8);
            本体有效 = 红夫人Z >= -300 && 红夫人X != 0 && 红夫人Y != 0;
        }
    }
    if (红夫人镜像 != 0) {
        uintptr_t 镜像坐标指针 = getPtr64(红夫人镜像 + 0x28);
        if (镜像坐标指针 != 0) {
            红夫人镜像X = getFloat(镜像坐标指针 + 0xa0);
            红夫人镜像Z = getFloat(镜像坐标指针 + 0xa4);
            红夫人镜像Y = getFloat(镜像坐标指针 + 0xa8);
            镜像有效 = 红夫人镜像Z >= -300 && 红夫人镜像X != 0 && 红夫人镜像Y != 0;
        }
    }
    if (本体有效 && 镜像有效) {
        float mx = (红夫人X + 红夫人镜像X) / 2.0f, my = (红夫人Y + 红夫人镜像Y) / 2.0f;
        float dx = 红夫人镜像X - 红夫人X,      dy = 红夫人镜像Y - 红夫人Y;   // 法向
        if (dx * dx + dy * dy > 1e-4f) {
            镜线X1 = mx;      镜线Y1 = my;
            镜线X2 = mx - dy; 镜线Y2 = my + dx;                              // 沿镜面方向
            mirror = true;
        }
    }
    if (!mirror && 镜子预览 != 0) {
        uint8_t 可见 = 0;
        vm_readv(镜子预览 + 0x73, &可见, 1);
        uintptr_t cp = getPtr64(镜子预览 + 0x28);
        if (可见 == 1 && cp != 0) {
            float px0 = getFloat(cp + 0xa0), pz0 = getFloat(cp + 0xa4), py0 = getFloat(cp + 0xa8);
            float nx = getFloat(cp + 0x90), ny = getFloat(cp + 0x98);
            float 模 = nx * nx + ny * ny;
            if (px0 != 0 && py0 != 0 && pz0 >= -300 && 模 > 0.9f && 模 < 1.1f) {
                镜线X1 = px0;      镜线Y1 = py0;
                镜线X2 = px0 - ny; 镜线Y2 = py0 + nx;
                mirror = true;
            }
        }
    }
    vm_readv(Matrix, matrix, 64);
    if (!首帧矩阵){
        首帧矩阵 = true;
        printf("[矩阵]");
        for (int i = 0; i < 16; i++) printf(" %.4f", matrix[i]);
        printf("\n");
    }  // 直接从Matrix读16个float
    if (show_draw_prophet){
        auto textSize = ImGui::CalcTextSize(监管者预知, 0, 25);
        Draw->AddText({px-(textSize.x/2),130}, 红色, 监管者预知);
    }

    // 已破译 4 台后，剩下那台在修的就是最后一台 —— 单独用进度条标出来
    {
        float 最后进度 = 0.f;
        if (show_draw_secret_mechine && 密码机进度::最后一台(最后进度)){
            char 文字[64];
            snprintf(文字, sizeof(文字), "最后一台 %.1f%%", 最后进度);
            auto ts = ImGui::CalcTextSize(文字, 0, 25);
            const float 条宽 = 160.0f, 条高 = 14.0f, 间隙 = 8.0f;
            float x0 = px - (条宽 + 间隙 + ts.x) / 2.0f;
            float y0 = 158.0f;                                  // 预知监管那行(y=130)的下一行
            ImColor c = 进度颜色(最后进度);
            float 填充 = 条宽 * (最后进度 / 100.0f);
            if (填充 > 0.0f)
                Draw->AddRectFilled({x0, y0}, {x0 + 填充, y0 + 条高}, c);
            Draw->AddRect({x0, y0}, {x0 + 条宽, y0 + 条高}, ImColor(255,255,255,255));
            Draw->AddText({x0 + 条宽 + 间隙, y0 + 条高/2.0f - ts.y/2.0f}, c, 文字);
        }
    }

    // ---- 大门开门进度 ----
    // 大门**不走下面那个实体循环**：getscene() 只认 prop_76/sender，大门那条分支收不到东西
    // (原版认 6 个类名，Name.h 重构时缩成 2 个，门/箱/椅三条分支就成了孤儿)。
    // 所以这里直接拿 Python 侧的 model+0x20 场景对象自己投影。
    // 也不能靠场景侧判据补救：实测两扇门的类名都不一样(prop_30 / wooddoor01a)，
    // 而且 +0x240 两扇都是 0 —— "最可靠的存在性判据"在大门上失效。
    if (show_draw_Door){
        for (int i = 0; i < 密码机进度::大门数(); i++){
            密码机进度::大门条目 门;
            if (!密码机进度::取大门(i, 门)) continue;
            if (密码机进度::已开启(门)) continue;          // 已经开了的不用再画
            if (!门.可开) continue;                         // 还没通电(不可开)的不画

            uintptr_t 坐标指针 = getPtr64((uintptr_t)门.场景对象 + 0x28);
            if (坐标指针 == 0) continue;
            float mx = getFloat(坐标指针 + 0xa0);
            float mz = getFloat(坐标指针 + 0xa4);          // +0xa4 是高度
            float my = getFloat(坐标指针 + 0xa8);
            if (mx == 0 && my == 0) continue;

            float cam = matrix[3]*mx + matrix[7]*mz + matrix[11]*my + matrix[15];
            if (cam <= 0.01f) continue;                    // 在相机背后
            float sx = px + (matrix[0]*mx + matrix[4]*mz + matrix[8]*my + matrix[12]) / cam * px;
            float sy = py - (matrix[1]*mx + matrix[5]*(mz+8.5f) + matrix[9]*my + matrix[13]) / cam * py;

            int 米 = (int)(sqrt(pow(mx - Z.X, 2) + pow(my - Z.Y, 2) + pow(mz - Z.Z, 2)) / 距离比例);
            char 文字[64];
            if (门.开启中)      snprintf(文字, sizeof(文字), "[%.1f%%↑]", 门.进度);
            else                snprintf(文字, sizeof(文字), "[%.1f%%]",  门.进度);   // 不可开时按灰色画，见下

            // 有进度就在文字上方画一条，跟密码机那套一致
            if (门.进度 > 0.05f){
                const float 条宽 = 120.0f, 条高 = 10.0f;
                float bx = sx - 条宽 / 2.0f, by = sy - 条高 - 3.0f;
                Draw->AddRectFilled({bx, by}, {bx + 条宽 * (门.进度 / 100.0f), by + 条高}, 进度颜色(门.进度));
                Draw->AddRect({bx, by}, {bx + 条宽, by + 条高}, ImColor(255,255,255,255));
            }
            auto ts = ImGui::CalcTextSize(文字, 0, 25);
            Draw->AddText({sx - ts.x/2.0f, sy}, 门.可开 ? 进度颜色(门.进度) : ImColor(180,180,180,255), 文字);
        }
    }

    for (int i = 0; i < 数量; i++){
    
        if (strstr(data[i].类名, "buzz") != NULL)
            continue;//跳过不知所谓的东西
        if (strstr(data[i].类名, "nvyao.gim") != NULL)
            continue;//跳过女妖蜡烛
        D.X = getFloat(data[i].objcoor + 0xa0);
        D.Z = getFloat(data[i].objcoor + 0xa4);
        D.Y = getFloat(data[i].objcoor + 0xa8);

        // 已锁定的自身: 只负责判断"还活着没"+持续更新坐标, 不重新参与后面的识别/绘制逻辑
        if (自身 != 0 && data[i].obj == 自身) {
            if (!ShouldSkipEntity(data[i]) && !(D.X==0 && D.Y==0) && D.Z>-300) {
                Z.X = D.X; Z.Z = D.Z; Z.Y = D.Y;
            }
            continue; // 不管有效无效, 自身都不需要再走下面的常规实体流程
        }

        if (D.X==0 || D.Y==0){
		    continue;//跳过xy0
		}
		if (D.Z<=-300){
		    continue;//跳过地下
		}
		if (data[i].阵营 == 1 || data[i].阵营 == 2) 内核人物数量++;
		// 本体是监管时，游戏自己就会显示监管者个体，这里不再重复画任何监管者。
		// 只认 CPython 锚点给的阵营：兜底的相机深度启发式可能把求生者误判成监管，
		// 那样会让求生者视角下的监管整个消失，代价远大于多画一个。
		if (有权威自身 && 自身阵营 == 1 && data[i].阵营 == 1) continue;
		int jxpd = getDword(data[i].obj + 0x70);
		camera = matrix[3] * D.X + matrix[7] * D.Z + matrix[11] * D.Y + matrix[15];
        距离 = sqrt(pow(D.X - Z.X, 2) + pow(D.Y - Z.Y, 2) + pow(D.Z - Z.Z, 2)) / 距离比例;
        矩阵视野距离 = sqrt(pow(D.X - M.X, 2) + pow(D.Y - M.Y, 2) + pow(D.Z - M.Z, 2)) / 距离比例;
		孽蜥距离 = sqrt(pow(D.X - Z.X, 2) + pow(D.Y - Z.Y, 2)) / 距离比例;
		r_x = px + (matrix[0] * D.X + matrix[4] * D.Z + matrix[8] * D.Y + matrix[12]) / camera * px;
        r_y = py - (matrix[1] * D.X + matrix[5] * (D.Z+ 8.5) + matrix[9] * (D.Y) + matrix[13]) / camera * py;
        r_w = py - (matrix[1] * D.X + matrix[5] * (D.Z+ 28.5) + matrix[9] * (D.Y) + matrix[13]) / camera * py;
												
		W = (r_y - r_w) / 2;	// 宽度
		H = r_y - r_w;		// 高度
		X1 = r_x - (r_y - r_w) / 4;	// X1
		Y1 = r_y - H / 2;	// Y1
		X2 = X1 + W;		// X2
		Y2 = Y1 + H;		// Y2
		if (距离>=300){
            continue;
        }
        if (W>0){
            if (Debugging){
                // 调试绘制是独立分支，不走上面任何类别过滤，所以会把每局默认加载的装饰对象
                // 一起画出来(表现为"同一个位置两份不同地址"、约瑟夫相机等)。这里统一挡掉。
                // 判据用 +0x240 而不是活跃位：活跃位是"当前对本机可见"，跟视角走，
                // 求生者视角下大量真实对象的活跃位也是 0，用它会把真东西一起挡掉。
                // 想看全部对象(比如排查新偏移时)，把下面这行注释掉即可。
                if (!实体本局存在(data[i].obj)) continue;
                std::string test;
                sprintf(objtext, "%lx", data[i].obj);
                test += " [";
                test += std::to_string((int) 距离);    
                test += " 米]  0x";
                test += objtext;    
                test += " [类名] ";
                test += data[i].类名;
                auto textSize = ImGui::CalcTextSize(test.c_str(), 0, 25);
                Draw->AddText({r_x-(textSize.x/2),r_y}, ImColor(255,200,0,255), test.c_str());
            }
        
            if (strstr(data[i].类名, "camera") != NULL && 距离 < 38){
                // 约瑟夫的相机每局都会默认加载十几个，跟这局有没有约瑟夫无关。
                // 实测那些默认加载的 +0x240 全是 0（7/7），本局真实存在的物件是 2。
                if (!实体本局存在(data[i].obj)) continue;
                if (getDword(data[i].obj + 0xa8)==256){
		            continue;//跳过使用过的椅子
		        }
                std::string s;
			    if (show_draw_Camera){                          
                    s += "[摄影机]";
                }
                auto textSize = ImGui::CalcTextSize(s.c_str(), 0, 25);
                Draw->AddText({r_x-(textSize.x/2),r_y}, ImColor(255,200,0,255), s.c_str());
            }
            

		
    		if (data[i].阵营==3)
    		{
    		    if (strstr(data[i].类名, "dm65_scene_prop_30") != NULL){
    			    std::string s;
    		        if (show_draw_Door){
                        s += "[大门]";
                    }
                    auto textSize = ImGui::CalcTextSize(s.c_str(), 0, 25);
                    Draw->AddText({r_x-(textSize.x/2),r_y}, ImColor(255,200,0,255), s.c_str());
    		    }
    		
    		    else if (strstr(data[i].类名, "dm65_scene_prop_01") != NULL&&距离<38){
    			    std::string s;
    		        if (show_draw_Box){                          
    		            if (getDword(data[i].obj + 0x148)==0){
    		                continue;//跳过使用过的箱子
    		            }
                        s += "[道具箱]";
                    }    
                    auto textSize = ImGui::CalcTextSize(s.c_str(), 0, 25);
                    Draw->AddText({r_x-(textSize.x/2),r_y}, 红色, s.c_str());
    		    }

    		    else if (strstr(data[i].类名, "dm65_scene_gallow") != NULL&&strstr(data[i].类名, "bashou") == NULL&&距离<38){
    			    std::string s;
    		        if (show_draw_Chair){                          
    		            if (getDword(data[i].obj + 0xa8)==256){
    		                continue;//跳过使用过的椅子
    		            }
                        s += "[狂欢之椅]";
                    }
                    auto textSize = ImGui::CalcTextSize(s.c_str(), 0, 25);
                    Draw->AddText({r_x-(textSize.x/2),r_y}, 红色, s.c_str());
    		    }
    		
    		    else if (strstr(data[i].类名, "dm65_scene_prop_76") != NULL){
    			    std::string s;
    		        if (show_draw_Cellar){                                                  
                            s += "[地窖] ";
                            s += std::to_string((int) 距离);    
                            s += " 米 ";
                    }
                    auto textSize = ImGui::CalcTextSize(s.c_str(), 0, 25);
                    Draw->AddText({r_x-(textSize.x/2),r_y}, 紫色, s.c_str());
    		    }

    	    else if (strstr(data[i].类名, "sender") != NULL){
    	        // 判据见上面 是真实密码机() 的注释。若发现密码机被破译完成后从叠加层消失，改那里。
    	        if (show_draw_secret_mechine && 是真实密码机(data[i].obj)){
    	            // 进度来自 Python 侧的 GeneratorUnit，靠 model 指针身份配对(见 PyProgress.h)。
    	            // 配不上时(model 为空/链路失效)退化成原来的 "[密码机] X.X 米"。
    	            float 进度 = 0.f;
    	            bool 有进度 = 密码机进度::查询(data[i].obj, D.X, D.Y, 进度);
    	            if (!(有进度 && 密码机进度::已破译(进度))){      // 破译完的机器本身和进度都不画
    	                std::ostringstream oss;
    	                oss << std::fixed << std::setprecision(1) << 距离;
    	                std::string 距离文本 = " " + oss.str() + " 米";
    	                char 头[24];
    	                if (有进度) snprintf(头, sizeof(头), "[%.1f%%]", 进度);
    	                else        snprintf(头, sizeof(头), "[密码机]");

    	                // 两段分开上色：头用进度色，距离沿用原来 61~63 米变绿的规则
    	                auto hs = ImGui::CalcTextSize(头, 0, 25);
    	                auto ds = ImGui::CalcTextSize(距离文本.c_str(), 0, 25);
    	                float x0 = r_x - (hs.x + ds.x) / 2.0f;
    	                Draw->AddText({x0, r_y}, 有进度 ? 进度颜色(进度) : ImColor(255,255,255,255), 头);
    	                Draw->AddText({x0 + hs.x, r_y},
    	                    (距离 >= 61 && 距离 <= 63) ? 绿色 : ImColor(255, 255, 255, 255),
    	                    距离文本.c_str());

    	                // 进度条画在文字上方；进度为 0 时没有意义，不画
    	                if (有进度 && 进度 > 0.05f){
    	                    const float 条宽 = 120.0f, 条高 = 10.0f;
    	                    float bx = r_x - 条宽 / 2.0f, by = r_y - 条高 - 3.0f;
    	                    Draw->AddRectFilled({bx, by}, {bx + 条宽 * (进度 / 100.0f), by + 条高}, 进度颜色(进度));
    	                    Draw->AddRect({bx, by}, {bx + 条宽, by + 条高}, ImColor(255,255,255,255));
    	                }
    	            }
    	        }
    	    }
    		}
	
		    if (show_draw_Prop&&data[i].阵营==4){
                // 自己身上的道具按距离剔除。必须只算**水平**距离:
                // 道具是挂在角色骨骼上的(手/胸口)，挂点比角色原点(脚底)高一截，
                // 实测 h55_pendant_glim(手电筒) 高度差固定 +0.73 米、水平只差 0.36 米。
                // 三维距离因此恒有 0.7+ 米的底噪，亚米阈值永远不成立 —— 挂件类道具靠
                // 三维距离**不可能**滤掉，跟站位无关，是结构性偏移。
                // 另注: 全局的 距离 是 int(见文件头 `int 距离;`)，不到1米会被截断成0，
                // 所以这里必须用浮点重算，不能直接拿 距离 比。
                float 道具水平距离 = sqrt(pow(D.X - Z.X, 2) + pow(D.Y - Z.Y, 2)) / 距离比例;
                // 阈值 1.5 米：手电筒实测水平只差 0.36 米，但 1.0 米实战仍有漏网，
                // 说明别的挂件(火箭/橄榄球等)挂点更靠外。代价是贴身队友手里的道具
                // 也会被隐藏，1.5 米内的地面道具本来也在视野里，可以接受。
                if (道具水平距离 >= 1.5f) {
                    const char* propName = getprop(data[i].类名);
                    if (propName) {
                        std::string s = propName;
                        s += std::to_string((int) 距离);
                        s += " 米";
                        auto textSize = ImGui::CalcTextSize(s.c_str(), 0, 25);
                        Draw->AddText({r_x-(textSize.x/2),r_y}, ImColor(255,200,0,255), s.c_str());
                    }
                }
            }

            int zy;//=getbool(data[i].obj + 0xaa);
            vm_readv(data[i].obj + 0xaa, &zy, 1);

            if (!ShouldSkipEntity(data[i])){
                if (show_draw_Role&&strstr(data[i].类名, "chr") != NULL){
                    std::string test;
                    test += " [";
                    test += std::to_string((int) 距离);    
                    test += " 米]  0x";
                    test += objtext;    
                    test += " [类名] ";
                    test += data[i].类名;
                    auto textSize = ImGui::CalcTextSize(test.c_str(), 0, 25);
                    Draw->AddText({r_x-(textSize.x/2),r_y}, ImColor(255,200,0,255), test.c_str());
                }
                			            
                // 旧的自身判定：相机深度落在 10~40 + zy + 阵营是1或2。**已停用**(见下)。
                // 原本只在 CPython 链路拿不到锚点时兜底，它的问题见 PySelf.h 文件头：
                //   - 任何一个求生者走进 10~40 这条深度带都会被误判成自身
                //   - zy 是 +0xaa，实测是通用状态位，**不区分是否自身**，挡不住
                //   - 自身锚错 -> Z 锚错 -> 全场距离全错，而且那个人会被 continue 掉不再绘制
                // 2026-09-22 停用：暂时只走 CPython 锚点(PySelf.h)，锚点拿不到时宁可没有自身也不猜。
                // if (!有权威自身 && camera < 40 && camera > 10 && zy&&(data[i].阵营==1||data[i].阵营==2)){
                //     自身 = data[i].obj;
                //     Z.X = D.X;
                //     Z.Z = D.Z;
                //     Z.Y = D.Y;
                //     自身阵营=对象阵营;
                //        continue;
                // }
               
                std::string s;


                if (!模仿者绘制中 && (data[i].阵营==1||data[i].阵营==2)){
                    s+=data[i].str;
                    auto textSize = ImGui::CalcTextSize(s.c_str(), 0, 25);
                    Draw->AddText({X1 + W/2-(textSize.x/2),Y1-45}, ImColor(255,200,0,255), s.c_str());
                    if (show_draw_Rect){
        			    if (jxpd==65150)
        			        ImGui::GetForegroundDrawList()->AddRect({X1, Y1},{X2, Y2}, BotBoneColor,3, 0,1.8);			        	        
        			    else if (data[i].阵营==1)
        			        ImGui::GetForegroundDrawList()->AddRect({X1, Y1},{X2, Y2}, BoneColor,3, 0,1.8f);
        			    else if (data[i].阵营==2)
        			        ImGui::GetForegroundDrawList()->AddRect({X1, Y1},{X2, Y2}, 绿色,3, 0,1.8f);
        			}
                    float 下一行 = Y2 + 10;            // 距离、天赋、辅助特质依次往下排
                    const float 行高 = ImGui::GetFontSize() + 2.0f;
                    if (show_draw_Distance){
                        std::string 人物距离;
                        人物距离 += std::to_string((int) 距离);
                        人物距离 += " 米";
                        auto textSize = ImGui::CalcTextSize(人物距离.c_str(), 0, 25);
                        Draw->AddText({X1 + W/2-(textSize.x/2),下一行}, ImColor(255,200,0,255), 人物距离.c_str());
                        下一行 += 行高;
                    }

                    // 天赋简称：求生者大心脏排最后、监管挽留排最后，格式化在 PyGenius.h 里
                    if (show_draw_Genius){
                        天赋::信息 gi;
                        if (天赋::查询(data[i].obj, D.X, D.Y, gi)){
                            char 天赋行[64];
                            天赋::天赋文本(gi, 天赋行, sizeof(天赋行));
                            if (天赋行[0]){
                                // 绝处逢生三态：没带=白 / 带了还没用=绿 / 带了已经用掉=灰。
                                // 消耗标志是 unit.ability_used[102]，2026-09-22 实测**别人的也读得到**
                                // (服务器会下发非本机玩家的消耗状态)，所以监管看四个人都准。
                                ImColor c;
                                switch (天赋::绝处状态(gi)) {
                                    case 天赋::绝处_可用: c = 绿色; break;
                                    case 天赋::绝处_已用: c = ImColor(140,140,140,255); break;
                                    // 绝处_无(表完整且没带) 和 绝处_未知(表还没读全) 都画白色；
                                    // 后者的文本末尾带 "?"，靠它区分
                                    case 天赋::绝处_未知:
                                    default:              c = ImColor(255,255,255,255); break;
                                }
                                auto ts = ImGui::CalcTextSize(天赋行, 0, 25);
                                Draw->AddText({X1 + W/2-(ts.x/2),下一行}, c, 天赋行);
                                下一行 += 行高;
                            }
                            // 监管再单独一行写当前辅助特质 + 剩余冷却
                            // (带底牌会局中换特质，所以读的是实时值；冷却来自 skill_mgr，见 PyGenius.h)
                            if (gi.阵营 == 1 && gi.辅助特质 != 0){
                                char 特质行[48];
                                天赋::特质行文本(gi, 天赋::特质名(gi.辅助特质), 特质行, sizeof(特质行));
                                if (特质行[0]){
                                    // 就绪=绿，冷却中=白；读不到冷却时按白显示(只有名字)
                                    ImColor cc = 天赋::已就绪(gi) ? 绿色 : ImColor(255,255,255,255);
                                    auto ts2 = ImGui::CalcTextSize(特质行, 0, 25);
                                    Draw->AddText({X1 + W/2-(ts2.x/2),下一行}, cc, 特质行);
                                    下一行 += 行高;
                                }
                            }
                        }
                    }

                    if (show_draw_Line){
                        ImGui::GetForegroundDrawList()->AddLine({px, 160},{X1 + W/2, Y1}, ImColor(255, 255, 255),2);
                    }                                          
                }
            }                                                          			
    	}//判断w    
	     
	                
	   //红夫人镜像                                   
	    if (mirror&&redqueenmod){
            if (getFloat(data[i].obj+0x1a0)==450&&data[i].阵营==2){
                std::string ss;
                // 镜线在 Draw_Main 开头算好(放下的镜子 / 准备阶段预览镜子二选一)，这里只做一次关于直线的对称
                float 原X = D.X, 原Y = D.Y;
                calculate_line_reflection(镜线X1, 镜线Y1, 镜线X2, 镜线Y2, 原X, 原Y, &D.X, &D.Y);
                camera = matrix[3] * D.X + matrix[7] * D.Z + matrix[11] * D.Y + matrix[15];
                距离 = sqrt(pow(D.X - Z.X, 2) + pow(D.Y - Z.Y, 2) + pow(D.Z - Z.Z, 2)) / 距离比例;
        		r_x = px + (matrix[0] * D.X + matrix[4] * D.Z + matrix[8] * D.Y + matrix[12]) / camera * px;
                r_y = py - (matrix[1] * D.X + matrix[5] * (D.Z+ 8.5) + matrix[9] * (D.Y) + matrix[13]) / camera * py;
                r_w = py - (matrix[1] * D.X + matrix[5] * (D.Z+ 28.5) + matrix[9] * (D.Y) + matrix[13]) / camera * py;
												
        		W = (r_y - r_w) / 2;	// 宽度
        		H = r_y - r_w;		// 高度
        		X1 = r_x - (r_y - r_w) / 4;	// X1
        		Y1 = r_y - H / 2;	// Y1
        		X2 = X1 + W;		// X2
        		Y2 = Y1 + H;		// Y2
        
                if (W>0){

                    ss += data[i].str;
                    auto textSize = ImGui::CalcTextSize(ss.c_str(), 0, 25);
                    Draw->AddText({X1 + W/2-(textSize.x/2),Y1-45}, BotBoneColor, ss.c_str());
                        
                    if (show_draw_Rect){
                        ImGui::GetForegroundDrawList()->AddRect({X1, Y1},{X2, Y2}, BotBoneColor,3, 0,1.8f);
    			    }
    			        
                    if (show_draw_Distance){
                        std::string 镜像距离;
                        镜像距离 += std::to_string((int) 距离);
                        镜像距离 += " 米";
                        auto textSize = ImGui::CalcTextSize(镜像距离.c_str(), 0, 25);
                        Draw->AddText({X1 + W/2-(textSize.x/2),Y2+10}, BotBoneColor, 镜像距离.c_str());
                    }
                        
                    if (show_draw_Line){
                        ImGui::GetForegroundDrawList()->AddLine({px, 160},{X1 + W/2, Y1}, ImColor(255, 255, 255),2);
                    }            
                }//判断W
            }                
        }
    }
    // 发布版本: 注入功能已停用
    // if (show_sohook)
    //     SoHook::DrawOverlay(Draw, matrix, px, py, 内核人物数量,
    //                         Z.X, Z.Z, Z.Y, 距离比例);
    show_draw_ClassName = 0;
}
        

size_t get_memory_usage_kb() {
    FILE* file = fopen("/proc/self/statm", "r");
    if (!file) return 0;
    size_t size = 0, resident = 0;
    fscanf(file, "%zu %zu", &size, &resident);
    fclose(file);
    return resident * 4;
}

int GetInputDeviceCount() {
    DIR *dir = opendir("/dev/input/");
    if (!dir) return -1;
    dirent *ptr = NULL;
    int count = 0;
    while ((ptr = readdir(dir)) != NULL) {
        if (strstr(ptr->d_name, "event"))
            count++;
    }
    closedir(dir);
    return count ? count : -1;
}

void VolumeKeyHide() {
    int EventCount = GetInputDeviceCount();
    if (EventCount <= 0) return;

    int *fdArray = (int *)malloc(EventCount * sizeof(int));
    if (!fdArray) return;

    for (int i = 0; i < EventCount; i++) {
        char temp[128];
        sprintf(temp, "/dev/input/event%d", i);
        fdArray[i] = open(temp, O_RDWR | O_NONBLOCK);
    }

    input_event ev;
    while (1) {
        for (int i = 0; i < EventCount; i++) {
            if (fdArray[i] < 0) continue;
            memset(&ev, 0, sizeof(ev));
            while (read(fdArray[i], &ev, sizeof(ev)) == sizeof(ev)) {
                if (ev.type == EV_KEY && ev.code == KEY_VOLUMEDOWN && ev.value == 1)
                    voice = false;
                if (ev.type == EV_KEY && ev.code == KEY_VOLUMEUP && ev.value == 1)
                    voice = true;
            }
        }
        show_window = voice;
        usleep(10000);
    }
    free(fdArray);
}

void Layout_tick_UI(bool *main_thread_flag) {
    static bool volume_thread_started = false;
    if (!volume_thread_started) {
        std::thread(VolumeKeyHide).detach();
        volume_thread_started = true;
    }

    px = static_cast<float>(displayInfo.width) / 2;
    py = static_cast<float>(displayInfo.height) / 2;
    // 发布版本: 注入功能已停用
    // SoHook::Update(pid);

    Draw_Main(ImGui::GetForegroundDrawList());

    if (show_window) {
        ImGui::Begin("New_Edition", nullptr, ImGuiWindowFlags_AlwaysAutoResize);

        if (::permeate_record_ini) {
            ImGui::SetWindowPos({LastCoordinate.Pos_x, LastCoordinate.Pos_y});
            ImGui::SetWindowSize({LastCoordinate.Size_x, LastCoordinate.Size_y});
            permeate_record_ini = false;
        }

        ImGui::Text("渲染模式 : %s, gui版本 : %s", graphics->RenderName, IMGUI_VERSION);
        ImGui::TextColored(ImVec4(1.0f, 0.0f, 1.0f, 1.0f), "帧率 %.1f FPS", ImGui::GetIO().Framerate);

        size_t mem_kb = get_memory_usage_kb();
        if (mem_kb >= 1024)
            ImGui::Text("内存占用: %.2f MB", mem_kb / 1024.0f);
        else
            ImGui::Text("内存占用: %zu KB", mem_kb);

        ImGui::Text("数据状态:");
        if (状态 == 2)
            ImGui::TextColored(ImVec4(0.0f, 205.0f, 0.0f, 100.0f), "已获取到游戏数据");
        else if (状态 == 1)
            ImGui::TextColored(ImVec4(255.0f, 0.0f, 0.0f, 100.0f), "正在获取游戏数据");

        if (ImGui::CollapsingHeader("基础信息")) {
            ImGui::Text("游戏进程:%d", pid);
            ImGui::Text("模块入口:%lx", libbase);
            ImGui::Text("游戏包名:%s", extractedString);
            ImGui::Text("矩阵地址:%lx", Matrix);
            ImGui::Text("数组地址:%lx", Arrayaddr);
            ImGui::Text("矩阵偏移:%lx", MatrixOffset);
            ImGui::Text("相机偏移:%d", g_相机偏移);
            ImGui::Text("模块页数:%d", c);
            ImGui::Text("数组偏移:%lx", ArrayaddrOffset);
            ImGui::Text("数据获取状态:%d", 数据获取状态);
            ImGui::Text("监管者:%s", 监管者预知);
            ImGui::Text("密码机进度:%s (已破译%d)", 密码机进度::状态文本(), 密码机进度::已破译数());
            ImGui::Text("天赋:%s", 天赋::状态文本());
            // 自身锚点：链路一旦失效这里会写明原因，绘制自动退回相机深度启发式
            ImGui::Text("自身:%s", 本机::状态文本());
        }

        ImGui::SetNextItemOpen(true, ImGuiCond_Once);
        if (ImGui::CollapsingHeader("绘制设置")) {
            ImGui::Checkbox("显示鬼魂", &inform_ghost);
            ImGui::SameLine();
            ImGui::Checkbox("预知监管", &show_draw_prophet);

            ImGui::Checkbox("绘制道具", &show_draw_Prop);
            ImGui::SameLine();
            ImGui::Checkbox("夫人模式", &redqueenmod);

            ImGui::Checkbox("绘制调试", &Debugging);
            ImGui::SameLine();
            ImGui::Checkbox("显示密码机", &show_draw_secret_mechine);   // 进度/进度条/最后一台 都跟着它

            ImGui::Checkbox("显示天赋", &show_draw_Genius);             // 天赋行 + 监管辅助特质行
            ImGui::SameLine();
            ImGui::Checkbox("显示大门", &show_draw_Door);

            // 发布版本: 注入功能已停用
            // ImGui::Checkbox("骨骼与进度", &show_sohook);

            ImGui::Text("");
            if (ImGui::Button("结束进程"))
                exit(0);
        }

        // 发布版本: 注入功能已停用
        // if (show_sohook) {
        //     ImGui::SetNextItemOpen(true, ImGuiCond_Once);
        //     if (ImGui::CollapsingHeader("骨骼与进度")) {
        //         SoHook::RenderPanel(pid);
        //     }
        // }

        g_window = ImGui::GetCurrentWindow();
        ImGui::End();
    }
}
