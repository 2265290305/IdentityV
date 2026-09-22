#ifndef IDV_PY_SELF_H
#define IDV_PY_SELF_H

// ============================================================================
// 自身锚点 + 废弃模型黑名单 —— 走游戏内嵌 CPython 的对象图读取
// ============================================================================
//
// 解决两件一直靠启发式凑的事：
//
// 一、"当前操控的是谁"
//   旧做法是 draw_Gui.cpp 里那套 `相机深度(10~40) + zy + 阵营是1或2`。
//   它的问题：任何一个求生者走进 10~40 这条深度带就会被误判成自身，而 +0xaa(zy)
//   按实测是通用状态位、**不区分是否自身**。自身锚错 -> Z 锚错 -> 全场距离全错，
//   而且那个对象会被 continue 掉不再绘制。
//
//   ★ 引擎自己持有答案：GK.g_cam_ctrl.unit 就是"当前视角/操控的单位"。
//     2026-09-22 受控实验，两角色两阵营各切换一次共 4 次读数，4/4 全部指对：
//       求生者机械师  本体 ⇄ 机械玩偶(MyCivilianPuppetUnit, 在 units_by_type[2])
//       监管梦之女巫  本体 ⇄ 梦之信徒(MyYidhraPuppetUnit,   在 units_by_type[236])
//
//   ！！`GK.g_unit` 不能用 ！！它是"我的主角色"，切到从属时纹丝不动(4/4 从不跟随)。
//   拿它做自身高亮，机械师操控玩偶 / 女巫操控信徒时会高亮错人。这是个陷阱，
//   曾经的记录"g_unit = 本机玩家"在能切换操控对象的角色上就是错的。
//   同理 `g_cam_ctrl.free_mode_unit` 恒为主角色，也不是我们要的。
//
//   另外这两个字段**不要碰**：
//     is_operating     机械师挂在本体上，语义跟名字相反("本体正在遥控玩偶中")，
//                      而且每个角色名字都不一样(女巫叫 yidhra_operating)，没通用性
//     bj_operating     所有单位(含 AI)恒为 True，完全没用
//
// 二、"废弃模型"
//   形态切换类角色(红蝶等)换形态**不是换操控对象**(cam.unit 不变)，是同一个 unit
//   换模型。旧形态的场景对象留在数组里、仍然被绘制循环捕获，这就是"废弃模型"。
//
//   ★ 判据：unit.another_model + 0x20 就是废弃形态的场景对象指针。
//     实测红蝶一局 model+0x20 = 0x7ACBDFFF80(在画的那个)、
//     another_model+0x20 = 0x7ACDDD0600，后者与独立抓到的废弃对象地址逐位相同。
//   这替掉了 ShouldSkipEntity 里那份红蝶/木偶师类名黑名单：精确、跟视角无关、不用维护名单。
//
//   ⚠️ 不要拿 `+0x73`/`+0x70`(= NeoX 模型对象的 visible) 当过滤器。
//   它是渲染剔除的结果：废弃模型恒不可见，但**远处的真实对象同样不可见**
//   (实测手持飞刀这种真道具 +0x70 也是 0)。用它过滤会重演"求生者只看得到身边的密码机"。
//
//   ⚠️ 只覆盖 another_model 这一类。实测另外还有 balloon_model(女巫气球)、
//   _fragrance_image(木偶师忘忧之香残影) 等同样指向非活跃模型的属性，它们目前靠
//   ShouldSkipEntity 的鬼魂判定挡住。要做全的话，应当取 unit.model 的 ob_type
//   拿到 world.model 类型指针，再扫整个实例 dict 把该类型的值全部拉黑(只留 model 那份)，
//   并把扫出来的序号按对局缓存住，否则每轮扫上千个条目开销太大。
//
// 链路、CPython 3.11 布局、三条铁律跟 PyProgress.h 完全同源，那边文件头有详细说明。
// 这里按同样风格自带一份 dict 工具，免得去改那两个已经验证过的文件。
// ============================================================================

#include <cstdint>
#include <cstring>
#include <cstdio>
#include <thread>
#include <unistd.h>

// 命名空间叫 本机 而不是 自身：draw_Gui.cpp 里已经有个全局变量 `uintptr_t 自身`，重名编不过
namespace 本机 {

// 模块相对偏移，跟 PyProgress.h / PyGenius.h 同源；热更后要一起改
static const uintptr_t OFF_SYS_MODULES = 0xA7029E8;
static const uintptr_t OFF_LONG_TYPE   = 0xA026498;
static const uintptr_t OFF_DICT_TYPE   = 0xA023BF0;

static const int 最大废弃数 = 32;

// 要扫 another_model 的单位类型：1=监管 2=求生者(机械玩偶也在这里) 236=梦之信徒
static const int 单位类型表[] = {1, 2, 236};
static const int 单位类型数 = sizeof(单位类型表) / sizeof(单位类型表[0]);

struct 快照 {
    uint64_t 锚点;                  // 当前操控单位的场景对象，0 = 没读到
    int      阵营;                  // 1=监管 2=求生者 其它=从属单位的 unit_type
    int64_t  uid;
    uint64_t 废弃[最大废弃数];
    int      废弃数;
};

// 双缓冲发布：写线程填非活跃缓冲，填完再翻转，绘制线程永远读到完整的一份
static 快照 g_缓冲[2];
static volatile int g_活跃 = 0;

static uintptr_t g_libbase = 0;
static uint64_t  g_整数类型 = 0, g_字典类型 = 0;
static uint64_t  g_模块字典 = 0;
static int64_t   g_i_cam = -1, g_i_cam_unit = -1;
static int64_t   g_i_unit_mgr = -1, g_i_ubt = -1;
static int64_t   g_i_model = -1, g_i_another = -1, g_i_utype = -1, g_i_uid = -1;
static volatile bool g_可用 = false;
static volatile int  g_连续失败 = 0;
static char g_状态[96] = "未启动";

static inline bool 是对象(uint64_t p) { return p > 0x7000000000ULL && p < 0x8000000000ULL; }

// PyASCIIObject: 长度在 +0x10，紧凑 ASCII 数据在 +0x30
static bool 字符串等于(uint64_t s, const char *want)
{
    if (!是对象(s)) return false;
    int64_t len = 0;
    if (!vm_readv(s + 0x10, &len, 8)) return false;
    size_t n = strlen(want);
    if (len != (int64_t)n || n >= 64) return false;
    char buf[64];
    if (!vm_readv(s + 0x30, buf, n)) return false;
    return memcmp(buf, want, n) == 0;
}

struct 字典 { uint64_t 条目起; int 步长; int64_t 条数; };

static bool 取字典(uint64_t d, 字典 &out)
{
    if (!是对象(d)) return false;
    uint64_t keys = getPtr64(d + 0x20);
    if (!是对象(keys)) return false;
    uint8_t hdr[32];
    if (!vm_readv(keys, hdr, 32)) return false;
    uint8_t idxb = hdr[9];
    uint8_t kind = hdr[10];
    int64_t nent = 0; memcpy(&nent, hdr + 24, 8);
    if (idxb > 30 || nent < 0 || nent > (1 << 22)) return false;
    out.条目起 = keys + 32 + ((uint64_t)1 << idxb);
    out.步长   = (kind == 0) ? 24 : 16;
    out.条数   = nent;
    return true;
}

static inline uint64_t 键槽(const 字典 &k, int64_t i) { return k.条目起 + i * k.步长 + (k.步长 == 24 ? 8 : 0); }
static inline uint64_t 值槽(const 字典 &k, int64_t i) { return k.条目起 + i * k.步长 + (k.步长 == 24 ? 16 : 8); }

static int64_t 查名(uint64_t d, const char *name, int64_t hint)
{
    字典 k;
    if (!取字典(d, k)) return -1;
    if (hint >= 0 && hint < k.条数 && 字符串等于(getPtr64(键槽(k, hint)), name)) return hint;
    for (int64_t i = 0; i < k.条数; i++)
        if (字符串等于(getPtr64(键槽(k, i)), name)) return i;
    return -1;
}

static uint64_t 取属性(uint64_t d, int64_t idx)
{
    字典 k;
    if (!取字典(d, k) || idx < 0 || idx >= k.条数) return 0;
    return getPtr64(值槽(k, idx));
}

static int64_t 校准序号(uint64_t d, const 字典 &dk, int64_t 缓存, const char *name)
{
    if (缓存 >= 0 && 缓存 < dk.条数 && 字符串等于(getPtr64(键槽(dk, 缓存)), name)) return 缓存;
    return 查名(d, name, -1);
}

// managed-dict 实例：dict 指针在对象前 0x18。多校验一次 ob_type 是 dict，
// 免得把别的布局的对象当实例用(g_cam_ctrl 不一定跟 unit 同布局)
static uint64_t 取实例字典(uint64_t obj)
{
    if (!是对象(obj)) return 0;
    uint64_t d = getPtr64(obj - 0x18);
    if (!是对象(d)) return 0;
    if (getPtr64(d + 8) != g_字典类型) return 0;
    return d;
}

static bool 读整数(uint64_t vp, int64_t &out)
{
    if (!是对象(vp)) return false;
    uint8_t b[32];
    if (!vm_readv(vp, b, 32)) return false;
    int64_t rc = 0; memcpy(&rc, b, 8);
    uint64_t tp = 0; memcpy(&tp, b + 8, 8); tp &= 0x00FFFFFFFFFFFFFFULL;
    if (rc <= 0 || tp != g_整数类型) return false;
    int64_t sz = 0; memcpy(&sz, b + 16, 8);
    uint32_t dg = 0; memcpy(&dg, b + 24, 4);
    out = (sz == 0) ? 0 : (int64_t)dg;
    if (sz < 0) out = -out;
    return true;
}

// Python 对象 -> 它持有的场景对象指针(world.model + 0x20)
static uint64_t 取场景对象(uint64_t 模型对象)
{
    if (!是对象(模型对象)) return 0;
    uint64_t sp = getPtr64(模型对象 + 0x20);
    return 是对象(sp) ? sp : 0;
}

static bool 解析模块()
{
    uint64_t sm = getPtr64(g_libbase + OFF_SYS_MODULES);
    if (!是对象(sm)) { snprintf(g_状态, sizeof(g_状态), "sys.modules 取不到"); return false; }
    if (getPtr64(sm + 8) != g_字典类型) { snprintf(g_状态, sizeof(g_状态), "sys.modules 不是dict(偏移已失效)"); return false; }
    int64_t i = 查名(sm, "game_kernel", -1);
    if (i < 0) { snprintf(g_状态, sizeof(g_状态), "找不到 game_kernel 模块"); return false; }
    uint64_t mod = 取属性(sm, i);
    if (!是对象(mod)) { snprintf(g_状态, sizeof(g_状态), "game_kernel 模块对象无效"); return false; }
    g_模块字典 = getPtr64(mod + 0x10);            // module.md_dict
    if (!是对象(g_模块字典)) { g_模块字典 = 0; snprintf(g_状态, sizeof(g_状态), "md_dict 无效"); return false; }
    return true;
}

// units_by_type 里找某个 int 键对应的 list
static uint64_t 取分类表(uint64_t ubt, int 类型)
{
    字典 k;
    if (!取字典(ubt, k)) return 0;
    for (int64_t i = 0; i < k.条数; i++) {
        uint64_t kp = getPtr64(键槽(k, i));
        if (!是对象(kp) || getPtr64(kp + 8) != g_整数类型) continue;
        int64_t sz = 0; uint32_t dg = 0;
        vm_readv(kp + 16, &sz, 8);
        vm_readv(kp + 24, &dg, 4);
        if (sz == 1 && (int)dg == 类型) return getPtr64(值槽(k, i));
    }
    return 0;
}

static bool 取列表(uint64_t L, int64_t &n, uint64_t &items)
{
    if (!是对象(L)) return false;
    if (!vm_readv(L + 0x10, &n, 8)) return false;
    if (n < 0 || n > 256) return false;
    if (n == 0) { items = 0; return true; }
    items = getPtr64(L + 0x18);
    return 是对象(items);
}

// 扫一类单位的 another_model，把废弃形态的场景对象收进黑名单
static void 收废弃(uint64_t ubt, int 类型, 快照 &出)
{
    uint64_t 列表 = 取分类表(ubt, 类型);
    int64_t n = 0; uint64_t items = 0;
    if (!取列表(列表, n, items) || n == 0) return;

    for (int64_t i = 0; i < n && 出.废弃数 < 最大废弃数; i++) {
        uint64_t inst = getPtr64(items + i * 8);
        uint64_t d = 取实例字典(inst);
        if (!d) continue;
        字典 dk;
        if (!取字典(d, dk)) continue;

        // another_model 只存在于有第二形态的角色身上，其余单位查不到就跳过(不是错误)
        g_i_another = 校准序号(d, dk, g_i_another, "another_model");
        if (g_i_another < 0) continue;
        uint64_t sp = 取场景对象(getPtr64(值槽(dk, g_i_another)));
        if (!sp) continue;

        bool 已有 = false;
        for (int j = 0; j < 出.废弃数; j++) if (出.废弃[j] == sp) { 已有 = true; break; }
        if (!已有) 出.废弃[出.废弃数++] = sp;
    }
}

static bool 尝试刷新()
{
    if (g_模块字典 == 0 && !解析模块()) return false;

    快照 出;
    memset(&出, 0, sizeof(出));

    // ---- 一、当前操控单位：g_cam_ctrl.unit ----
    g_i_cam = 查名(g_模块字典, "g_cam_ctrl", g_i_cam);
    uint64_t cam = (g_i_cam >= 0) ? 取属性(g_模块字典, g_i_cam) : 0;
    if (!是对象(cam)) { snprintf(g_状态, sizeof(g_状态), "g_cam_ctrl 无效(未在对局中?)"); return false; }

    uint64_t camd = 取实例字典(cam);
    if (!camd) { snprintf(g_状态, sizeof(g_状态), "g_cam_ctrl 不是 managed-dict 布局"); return false; }
    字典 camk;
    if (!取字典(camd, camk)) { snprintf(g_状态, sizeof(g_状态), "g_cam_ctrl 字典异常"); return false; }

    g_i_cam_unit = 校准序号(camd, camk, g_i_cam_unit, "unit");
    if (g_i_cam_unit < 0) { snprintf(g_状态, sizeof(g_状态), "g_cam_ctrl 里没有 unit"); return false; }
    uint64_t 我 = getPtr64(值槽(camk, g_i_cam_unit));
    if (!是对象(我)) { snprintf(g_状态, sizeof(g_状态), "cam.unit 为空"); return false; }

    uint64_t 我d = 取实例字典(我);
    if (!我d) { snprintf(g_状态, sizeof(g_状态), "cam.unit 没有实例字典"); return false; }
    字典 我k;
    if (!取字典(我d, 我k)) { snprintf(g_状态, sizeof(g_状态), "cam.unit 字典异常"); return false; }

    g_i_model = 校准序号(我d, 我k, g_i_model, "model");
    g_i_utype = 校准序号(我d, 我k, g_i_utype, "unit_type");
    g_i_uid   = 校准序号(我d, 我k, g_i_uid,   "uid");

    if (g_i_model >= 0) 出.锚点 = 取场景对象(getPtr64(值槽(我k, g_i_model)));
    if (g_i_utype >= 0) { int64_t v = 0; if (读整数(getPtr64(值槽(我k, g_i_utype)), v)) 出.阵营 = (int)v; }
    if (g_i_uid   >= 0) { int64_t v = 0; if (读整数(getPtr64(值槽(我k, g_i_uid)),   v)) 出.uid  = v; }

    if (出.锚点 == 0) { snprintf(g_状态, sizeof(g_状态), "cam.unit.model 读不到"); return false; }

    // ---- 二、废弃模型黑名单 ----
    g_i_unit_mgr = 查名(g_模块字典, "unit_mgr", g_i_unit_mgr);
    uint64_t um = (g_i_unit_mgr >= 0) ? 取属性(g_模块字典, g_i_unit_mgr) : 0;
    uint64_t ud = 取实例字典(um);
    if (ud) {
        g_i_ubt = 查名(ud, "units_by_type", g_i_ubt);
        uint64_t ubt = (g_i_ubt >= 0) ? 取属性(ud, g_i_ubt) : 0;
        if (是对象(ubt))
            for (int i = 0; i < 单位类型数; i++) 收废弃(ubt, 单位类型表[i], 出);
    }
    // 黑名单读不到不算失败：锚点已经拿到了，绘制照样能用

    int 写 = 1 - g_活跃;
    g_缓冲[写] = 出;
    g_活跃 = 写;                                   // 填完再翻转
    snprintf(g_状态, sizeof(g_状态), "正常 %s uid=%lld 废弃%d",
             出.阵营 == 1 ? "监管" : (出.阵营 == 2 ? "求生" : "从属"),
             (long long)出.uid, 出.废弃数);
    return true;
}

static void 刷新一次()
{
    if (g_libbase == 0) return;
    if (尝试刷新()) { g_可用 = true; g_连续失败 = 0; return; }
    g_可用 = false;
    if (++g_连续失败 >= 8) {                       // 连续失败就把序号缓存作废，下一轮按名字重找
        g_模块字典 = 0;
        g_i_cam = g_i_cam_unit = g_i_unit_mgr = g_i_ubt = -1;
        g_i_model = g_i_another = g_i_utype = g_i_uid = -1;
        g_连续失败 = 0;
    }
}

// 自身锚点每帧都要用，而且切换操控对象时要立刻跟上，所以比天赋刷得勤
static const int 刷新间隔毫秒 = 100;

static void 线程体()
{
    while (true) {
        刷新一次();
        usleep(刷新间隔毫秒 * 1000);
    }
}

static void 启动(uintptr_t libbase)
{
    if (g_libbase != 0) return;
    g_libbase   = libbase;
    g_整数类型 = libbase + OFF_LONG_TYPE;
    g_字典类型 = libbase + OFF_DICT_TYPE;
    snprintf(g_状态, sizeof(g_状态), "启动中");
    std::thread(线程体).detach();
    printf("[自身] 已启动 libbase=0x%lx\n", (unsigned long)libbase);
    fflush(stdout);
}

// ---------------- 给绘制侧用的只读接口 ----------------

static inline bool 可用() { return g_可用; }
static inline const char *状态文本() { return g_状态; }

// 当前操控单位的场景对象。返回 false 时绘制侧要退回相机深度启发式
static bool 锚点(uint64_t &obj)
{
    if (!g_可用) return false;
    uint64_t v = g_缓冲[g_活跃].锚点;
    if (v == 0) return false;
    obj = v;
    return true;
}

// 当前视角的阵营：1=监管 2=求生者。0=没读到。
// 注意操控从属单位时这里是从属的 unit_type(机械玩偶仍是 2、梦之信徒是 236)
static inline int 阵营() { return g_可用 ? g_缓冲[g_活跃].阵营 : 0; }
static inline int64_t 自身uid() { return g_可用 ? g_缓冲[g_活跃].uid : 0; }

// 形态切换留下的废弃模型 —— 替掉红蝶/木偶师类名黑名单
static bool 是废弃模型(uintptr_t obj)
{
    if (!g_可用) return false;
    const 快照 &s = g_缓冲[g_活跃];
    for (int i = 0; i < s.废弃数; i++) if (s.废弃[i] == (uint64_t)obj) return true;
    return false;
}

} // namespace 本机

#endif // IDV_PY_SELF_H
