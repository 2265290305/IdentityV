#ifndef IDV_PY_PROGRESS_H
#define IDV_PY_PROGRESS_H

// ============================================================================
// 密码机破译进度 —— 走游戏内嵌 CPython 的对象图读取
// ============================================================================
//
// 为什么不能像坐标那样直接读结构体偏移：
//   数组里那 290 个场景对象是**渲染节点**(类名是 .gim 模型路径)，身上不带任何玩法状态。
//   对 7 台机器做过四档差分(0/25/80/100)，对象本体 0x400 字节 + 沿指针下钻两层，
//   f32/f64/u32/u16/u8 五种宽度全扫，零命中。另外 fix_process / hack_process /
//   get_generator_units 这些名字在 libclient.so 里**一个都搜不到**，说明它们不是
//   C++ 注册给 Python 的属性，而是纯 Python 脚本里定义的。进度只存在于 CPython 堆上。
//
// 完整链路(每一跳都实测验证过，跨对局重验通过)：
//   libbase + OFF_SYS_MODULES        -> sys.modules            (解释器状态是 .so 里的静态数据)
//     按名字找 "game_kernel"          -> module
//       module + 0x10                -> md_dict                (进程内稳定，不随对局变)
//         按名字找 "unit_mgr"         -> UnitManager 实例       (每局重新分配)
//           实例 - 0x18              -> managed dict
//             按名字找 "units_by_type"-> dict(int -> list)
//               键 int 3             -> list                   (3 = 密码机的 unit_type)
//                 list+0x10=元素数 +0x18=元素数组
//                   每个 GeneratorUnit 实例 - 0x18 -> dict
//                     按名字找 "fix_process" -> 0~100 的进度
//                     按名字找 "model"       -> +0x20 = **场景对象指针**(就是数组里那个 obj)
//                     按名字找 "position"    -> +0x10/+0x14/+0x18 = X/高度/Y
//
// 配对为什么用 model 而不是坐标：实测一局里 7 台有 4 台的 position 读不出/为 0，
// 靠坐标那几台就配不上 —— 表现是"明明破译完了却还画着机器"。
// model+0x20 是指针身份，同一局 7 台里 6 台立刻命中(剩下 1 台是 model 短暂为空)，
// 命中的场景对象 +0x240 全是 2，和场景侧判据完全一致。坐标只作为 model 为空时的兜底。
//
// 解释器版本是 CPython 3.11：dict 用 PyDictUnicodeEntry{key,value} 16 字节(不存 hash)，
// PyLong 仍是 ob_size 语义(不是 3.12 的 lv_tag)，managed-dict 的 dict 指针在对象前 0x18。
//
// 三个必须守住的坑：
//   1) entries 起始 = keys + 32 + (1<<dk_log2_index_bytes)，**必须逐实例算**。
//      同一个类的不同实例 dict 大小可以不同，把一个实例算出的偏移套到别的实例上会读到无关属性。
//   2) 值的类型在 int 和 float 之间来回变(0 和 100 通常是缓存的小整数对象，中间值是 float)，
//      两种都要认；绝不能只认一种，更不能把"认不出"当成 0。
//   3) 属性每被赋值一次就换一个新的 float 对象，旧的进 freelist 被复用。
//      "读指针 -> 再解引用"之间如果隔得久，会读到已经变成别人的内存(refcount 变 0 / 类型不对)。
//      所以解引用后要**再读一次指针确认没变**，并校验 refcount>0、类型对、数值在合理区间。
//      驱动读一次是微秒级，正常情况一次就过；PC 侧用 adb 读(一次 0.3 秒)则几乎必然读到垃圾。
//
// 序号(entry index)依赖类属性的插入顺序，同版本内稳定、热更后可能位移，
// 所以这里一律**按名字查找**，查到后把序号缓存下来，下次先试缓存并用名字校验，
// 对不上就自动回退全扫。这样热更后不会给出错误结果，最多慢一帧。
// ============================================================================

#include <cstdint>
#include <cstring>
#include <cmath>
#include <cstdio>
#include <thread>
#include <unistd.h>

namespace 密码机进度 {

// ---- 模块相对偏移(2026-09-17 热更版实测)。热更后如果面板显示"链路失效"，从这里开始重验 ----
static const uintptr_t OFF_SYS_MODULES = 0xA7029E8;   // *(libbase+此) = sys.modules
static const uintptr_t OFF_FLOAT_TYPE  = 0xA0336F0;   // PyFloat_Type
static const uintptr_t OFF_LONG_TYPE   = 0xA026498;   // PyLong_Type
static const uintptr_t OFF_DICT_TYPE   = 0xA023BF0;   // PyDict_Type

// 布尔单例：CPython 的 True/False 是两个静态对象，地址永不移动。
// 2026-09-22 实测 libbase+0xA0261A0 = True、+0xA0261C0 = False —— 这是读 ob_size/digit
// 判出来的，**不是**按"False 在前"的直觉猜的(实际顺序正好相反，猜会全判错)。
static const uintptr_t OFF_TRUE  = 0xA0261A0;
static const uintptr_t OFF_FALSE = 0xA0261C0;

static const int 最大机器数 = 16;
static const int GENERATOR_UNIT_TYPE = 3;

// 大门。2026-09-22 实测 units_by_type[6] = DoorUnit，一局 2 个。
// 当时的属性序号：hack_process #220 hack_speed #221 can_open #217 has_open #218
//                is_opening #219 open_rate #226 model #63
// 这里仍然一律按名字查 + 序号缓存校验，热更后会自动重找，上面的数字只作参考。
//
// ！！大门进度只能走驱动读，不要试图用 adb/跨进程脚本验证 ！！
// hack_process 每帧都被重新赋值成一个新的 float 对象。实测 PC 侧一次 fetch 约 0.4 秒，
// 40 次取指针 40 次都已失效，命中率是 0；而驱动读"取指针->解引用->重读指针"是微秒级，
// 对 16ms 的重赋值有三四个数量级余量。下面 读数值() 的五道闸(指针重读/refcount/类型/
// 0~100 值域/重试三次)一条都不能省 —— 尤其值域那条，垃圾值里 0.16 这种"看着合理"的
// 也出现过，只靠指针稳定挡不住。
static const int 最大大门数 = 4;
static const int DOOR_UNIT_TYPE = 6;

struct 条目 { uint64_t 场景对象; float x, y, z; float 进度; };
struct 大门条目 { uint64_t 场景对象; float 进度; bool 可开, 已开, 开启中; };

// 双缓冲发布：写线程填非活跃缓冲，填完再翻转，绘制线程永远读到一份完整的数据
static 条目   g_缓冲[2][最大机器数];
static volatile int g_计数[2] = {0, 0};
static volatile int g_活跃 = 0;

// 大门跟密码机在同一次刷新里填、共用 g_活跃 的那一次翻转
static 大门条目 g_门缓冲[2][最大大门数];
static volatile int g_门计数[2] = {0, 0};

static uintptr_t g_libbase = 0;
static uint64_t  g_浮点类型 = 0, g_整数类型 = 0, g_字典类型 = 0;
static uint64_t  g_真 = 0, g_假 = 0;             // True/False 两个静态单例
static uint64_t  g_模块字典 = 0;                 // game_kernel 的 md_dict，进程内稳定
static int64_t   g_i_unit_mgr = -1, g_i_ubt = -1, g_i_fix = -1, g_i_pos = -1, g_i_model = -1;
// 大门自己的一套序号缓存：DoorUnit 和 GeneratorUnit 是两个类，插入顺序不同，
// 序号不能共用(共用的话每个单位都要重扫一遍，白白浪费)
static int64_t   g_i_hack = -1, g_i_门model = -1, g_i_可开 = -1, g_i_已开 = -1, g_i_开启中 = -1;
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
    uint64_t keys = getPtr64(d + 0x20);           // ma_keys
    if (!是对象(keys)) return false;
    uint8_t hdr[32];
    if (!vm_readv(keys, hdr, 32)) return false;
    uint8_t idxb = hdr[9];                        // dk_log2_index_bytes
    uint8_t kind = hdr[10];                       // dk_kind: 0=通用(带hash,24字节) 其它=全unicode(16字节)
    int64_t nent = 0; memcpy(&nent, hdr + 24, 8); // dk_nentries
    if (idxb > 30 || nent < 0 || nent > (1 << 22)) return false;
    out.条目起 = keys + 32 + ((uint64_t)1 << idxb);
    out.步长   = (kind == 0) ? 24 : 16;
    out.条数   = nent;
    return true;
}

static inline uint64_t 键槽(const 字典 &k, int64_t i) { return k.条目起 + i * k.步长 + (k.步长 == 24 ? 8 : 0); }
static inline uint64_t 值槽(const 字典 &k, int64_t i) { return k.条目起 + i * k.步长 + (k.步长 == 24 ? 16 : 8); }

// 先试缓存的序号并用名字校验，对不上再全扫。返回序号，失败返回 -1
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

// 读一个 int/float 属性。带防竞态校验，见文件头第 3 条
static bool 读数值(uint64_t slot, float &out)
{
    for (int t = 0; t < 3; t++) {
        uint64_t vp = getPtr64(slot);
        if (!是对象(vp)) continue;
        uint8_t b[32];
        if (!vm_readv(vp, b, 32)) continue;
        if (getPtr64(slot) != vp) continue;       // 解引用期间指针变了 -> 对象可能已被释放复用
        int64_t rc = 0; memcpy(&rc, b, 8);
        uint64_t tp = 0; memcpy(&tp, b + 8, 8); tp &= 0x00FFFFFFFFFFFFFFULL;
        if (rc <= 0) continue;                    // refcount 0 = 已释放
        double v;
        if (tp == g_浮点类型) {
            memcpy(&v, b + 16, 8);
        } else if (tp == g_整数类型) {            // CPython 3.11: +0x10 是 ob_size, +0x18 是第一个 digit
            int64_t sz = 0; memcpy(&sz, b + 16, 8);
            uint32_t dg = 0; memcpy(&dg, b + 24, 4);
            v = (sz == 0) ? 0.0 : (double)dg;
            if (sz < 0) v = -v;
        } else {
            continue;
        }
        if (!(v >= -0.5 && v <= 100.5)) continue; // 进度只可能是 0~100
        out = (float)v;
        return true;
    }
    return false;
}

// 读一个 bool 属性。先比两个静态单例(最稳)，比不上再按 PyLong 布局的 ob_size/digit 退化解，
// 这样热更挪了单例地址也还能用
static bool 读布尔(uint64_t slot, bool &out)
{
    uint64_t vp = getPtr64(slot);
    if (!是对象(vp)) return false;
    if (vp == g_真) { out = true;  return true; }
    if (vp == g_假) { out = false; return true; }
    uint8_t b[32];
    if (!vm_readv(vp, b, 32)) return false;
    int64_t rc = 0; memcpy(&rc, b, 8);
    if (rc <= 0) return false;
    int64_t sz = 0; memcpy(&sz, b + 16, 8);
    uint32_t dg = 0; memcpy(&dg, b + 24, 4);
    out = (sz != 0 && dg != 0);
    return true;
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

// 这个类的某个属性序号：先用缓存的并按名字校验，不对就全扫重找
static int64_t 校准序号(uint64_t d, const 字典 &dk, int64_t 缓存, const char *name)
{
    if (缓存 >= 0 && 缓存 < dk.条数 && 字符串等于(getPtr64(键槽(dk, 缓存)), name)) return 缓存;
    return 查名(d, name, -1);
}

// units_by_type 是 dict(int -> list)，按 int 键取出那条 list
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

// 大门：units_by_type[6]。读不到不算整轮失败 —— 门和密码机互不影响
static void 收大门(uint64_t ubt, int 写)
{
    g_门计数[写] = 0;
    uint64_t 列表 = 取分类表(ubt, DOOR_UNIT_TYPE);
    if (!是对象(列表)) return;

    int64_t n = 0;
    vm_readv(列表 + 0x10, &n, 8);                 // ob_size
    uint64_t items = getPtr64(列表 + 0x18);       // ob_item
    if (n <= 0 || n > 16 || !是对象(items)) return;
    if (n > 最大大门数) n = 最大大门数;

    int 个数 = 0;
    for (int64_t i = 0; i < n; i++) {
        uint64_t inst = getPtr64(items + i * 8);
        if (!是对象(inst)) continue;
        uint64_t d = getPtr64(inst - 0x18);
        字典 dk;
        if (!取字典(d, dk)) continue;

        g_i_hack    = 校准序号(d, dk, g_i_hack,    "hack_process");
        g_i_门model = 校准序号(d, dk, g_i_门model, "model");
        g_i_可开    = 校准序号(d, dk, g_i_可开,    "can_open");
        g_i_已开    = 校准序号(d, dk, g_i_已开,    "has_open");
        g_i_开启中  = 校准序号(d, dk, g_i_开启中,  "is_opening");
        if (g_i_hack < 0) continue;

        float 进度 = 0.f;
        if (!读数值(值槽(dk, g_i_hack), 进度)) continue;   // 读不稳就整扇门跳过，不发布垃圾

        // 配对只能用 model 的指针身份。大门**不能**走场景侧判据：
        // 实测两扇门的场景类名还不一样(dm65_scene_prop_30 / dm65_scene_wooddoor01a)，
        // 而且两扇的 +0x240 都是 0 —— 那条"最可靠的存在性判据"在大门上直接失效。
        uint64_t 场景 = 0;
        if (g_i_门model >= 0) {
            uint64_t mo = getPtr64(值槽(dk, g_i_门model));
            if (是对象(mo)) {
                uint64_t sp = getPtr64(mo + 0x20);
                if (是对象(sp)) 场景 = sp;
            }
        }
        if (场景 == 0) continue;                  // 没有身份就画不到屏幕上，收了也没用

        bool 可开 = false, 已开 = false, 开中 = false;
        if (g_i_可开   >= 0) 读布尔(值槽(dk, g_i_可开),   可开);
        if (g_i_已开   >= 0) 读布尔(值槽(dk, g_i_已开),   已开);
        if (g_i_开启中 >= 0) 读布尔(值槽(dk, g_i_开启中), 开中);

        g_门缓冲[写][个数].场景对象 = 场景;
        g_门缓冲[写][个数].进度     = 进度;
        g_门缓冲[写][个数].可开     = 可开;
        g_门缓冲[写][个数].已开     = 已开;
        g_门缓冲[写][个数].开启中   = 开中;
        个数++;
    }
    g_门计数[写] = 个数;
}

static bool 尝试刷新()
{
    if (g_模块字典 == 0 && !解析模块()) return false;

    g_i_unit_mgr = 查名(g_模块字典, "unit_mgr", g_i_unit_mgr);
    uint64_t um = (g_i_unit_mgr >= 0) ? 取属性(g_模块字典, g_i_unit_mgr) : 0;
    if (!是对象(um)) { snprintf(g_状态, sizeof(g_状态), "unit_mgr 无效(未在对局中?)"); return false; }

    uint64_t ud = getPtr64(um - 0x18);            // managed-dict 预头
    g_i_ubt = 查名(ud, "units_by_type", g_i_ubt);
    uint64_t ubt = (g_i_ubt >= 0) ? 取属性(ud, g_i_ubt) : 0;
    if (!是对象(ubt)) { snprintf(g_状态, sizeof(g_状态), "units_by_type 无效"); return false; }

    // 大门先收：放在密码机那几个 return false 之前，免得"没有密码机列表"把门也带下水
    int 写 = 1 - g_活跃;
    收大门(ubt, 写);

    // 在 int->list 的表里找键 3
    字典 k;
    if (!取字典(ubt, k)) { snprintf(g_状态, sizeof(g_状态), "units_by_type 结构异常"); return false; }
    uint64_t 列表 = 0;
    for (int64_t i = 0; i < k.条数; i++) {
        uint64_t kp = getPtr64(键槽(k, i));
        if (!是对象(kp) || getPtr64(kp + 8) != g_整数类型) continue;
        int64_t sz = 0; uint32_t dg = 0;
        vm_readv(kp + 16, &sz, 8);
        vm_readv(kp + 24, &dg, 4);
        if (sz == 1 && (int)dg == GENERATOR_UNIT_TYPE) { 列表 = getPtr64(值槽(k, i)); break; }
    }
    if (!是对象(列表)) { snprintf(g_状态, sizeof(g_状态), "没有密码机列表"); return false; }

    int64_t n = 0;
    vm_readv(列表 + 0x10, &n, 8);                 // ob_size
    uint64_t items = getPtr64(列表 + 0x18);       // ob_item
    if (n <= 0 || n > 64 || !是对象(items)) { snprintf(g_状态, sizeof(g_状态), "密码机列表异常"); return false; }
    if (n > 最大机器数) n = 最大机器数;

    int 个数 = 0;                                  // 写缓冲下标在上面收大门时已经取好
    for (int64_t i = 0; i < n; i++) {
        uint64_t inst = getPtr64(items + i * 8);
        if (!是对象(inst)) continue;
        uint64_t d = getPtr64(inst - 0x18);
        字典 dk;
        if (!取字典(d, dk)) continue;

        g_i_fix   = 校准序号(d, dk, g_i_fix,   "fix_process");
        g_i_model = 校准序号(d, dk, g_i_model, "model");
        g_i_pos   = 校准序号(d, dk, g_i_pos,   "position");
        if (g_i_fix < 0) continue;

        float 进度 = 0.f;
        if (!读数值(值槽(dk, g_i_fix), 进度)) continue;

        uint64_t 场景 = 0;
        if (g_i_model >= 0) {
            uint64_t mo = getPtr64(值槽(dk, g_i_model));
            if (是对象(mo)) {
                uint64_t sp = getPtr64(mo + 0x20);
                if (是对象(sp)) 场景 = sp;
            }
        }
        float xyz[3] = {0, 0, 0};
        if (g_i_pos >= 0) {
            uint64_t pos = getPtr64(值槽(dk, g_i_pos));
            if (是对象(pos)) vm_readv(pos + 0x10, xyz, 12);   // +0x10/+0x14/+0x18 = X/高度/Y
        }
        g_缓冲[写][个数].场景对象 = 场景;
        g_缓冲[写][个数].x = xyz[0];
        g_缓冲[写][个数].z = xyz[1];
        g_缓冲[写][个数].y = xyz[2];
        g_缓冲[写][个数].进度 = 进度;
        个数++;
    }

    if (个数 == 0) { snprintf(g_状态, sizeof(g_状态), "一台也没读出"); return false; }
    g_计数[写] = 个数;
    g_活跃 = 写;                                   // 填完再翻转，绘制线程永远看到完整的一份
    snprintf(g_状态, sizeof(g_状态), "正常 %d 台 门%d", 个数, g_门计数[写]);
    return true;
}

static void 刷新一次()
{
    if (g_libbase == 0) return;
    if (尝试刷新()) {
        g_可用 = true;
        g_连续失败 = 0;
        return;
    }
    g_可用 = false;
    if (++g_连续失败 >= 8) {                       // 连续失败就把缓存的序号全部作废，下一轮重新按名字找
        g_模块字典 = 0;
        g_i_unit_mgr = g_i_ubt = g_i_fix = g_i_pos = g_i_model = -1;
        g_i_hack = g_i_门model = g_i_可开 = g_i_已开 = g_i_开启中 = -1;
        g_连续失败 = 0;
    }
}

// 一轮大约几十到两百次驱动读(微秒级)，100ms 一轮的开销可以忽略，换来 10Hz 的刷新
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
    g_浮点类型 = libbase + OFF_FLOAT_TYPE;
    g_整数类型 = libbase + OFF_LONG_TYPE;
    g_字典类型 = libbase + OFF_DICT_TYPE;
    g_真       = libbase + OFF_TRUE;
    g_假       = libbase + OFF_FALSE;
    snprintf(g_状态, sizeof(g_状态), "启动中");
    std::thread(线程体).detach();
    printf("[密码机进度] 已启动 libbase=0x%lx\n", (unsigned long)libbase);
    fflush(stdout);
}

// ---------------- 给绘制侧用的只读接口 ----------------

static inline bool 可用() { return g_可用; }
static inline const char *状态文本() { return g_状态; }

// 把场景对象配到对应的 GeneratorUnit。
// 首选指针身份(model+0x20 就是场景对象本身)；model 为空时才退回坐标近邻。
// 坐标兜底不能单独用: 实测一局 7 台里有 4 台的 position 读不出，那几台会一直配不上。
static bool 查询(uintptr_t obj, float x, float y, float &进度)
{
    if (!g_可用) return false;
    int b = g_活跃, n = g_计数[b];
    for (int i = 0; i < n; i++) {
        if (g_缓冲[b][i].场景对象 == (uint64_t)obj) { 进度 = g_缓冲[b][i].进度; return true; }
    }
    int 最近 = -1; float 最小 = 2.0f;
    for (int i = 0; i < n; i++) {
        if (g_缓冲[b][i].场景对象 != 0) continue;                        // 有身份的已经在上面比过了
        if (g_缓冲[b][i].x == 0.f && g_缓冲[b][i].y == 0.f) continue;    // 坐标没写入的跳过，避免误配
        float d = fabsf(g_缓冲[b][i].x - x) + fabsf(g_缓冲[b][i].y - y);
        if (d < 最小) { 最小 = d; 最近 = i; }
    }
    if (最近 < 0) return false;
    进度 = g_缓冲[b][最近].进度;
    return true;
}

static inline bool 已破译(float 进度) { return 进度 >= 99.95f; }

// ---------------- 大门 ----------------
// 大门不进 data[] 数组(getscene() 只认 prop_76/sender，大门那条分支收不到)，
// 所以绘制侧不走实体循环，直接遍历这里拿到场景对象再自己投影。
static inline int 大门数() { return g_可用 ? g_门计数[g_活跃] : 0; }

static bool 取大门(int i, 大门条目 &out)
{
    if (!g_可用) return false;
    int b = g_活跃;
    if (i < 0 || i >= g_门计数[b]) return false;
    out = g_门缓冲[b][i];
    return true;
}

// 大门开启完成。hack_process 是 0~100 的百分比(跟 fix_process 同构，
// 作者载荷里也是按 "进度:{:.1f}%" 格式化的)
static inline bool 已开启(const 大门条目 &d) { return d.已开 || d.进度 >= 99.95f; }

static int 已破译数()
{
    if (!g_可用) return -1;
    int b = g_活跃, n = g_计数[b], c = 0;
    for (int i = 0; i < n; i++) if (已破译(g_缓冲[b][i].进度)) c++;
    return c;
}

// 一局要破译 5 台。已破译 完成 台 -> 还需 需要 = 5 - 完成 台 -> 在还没破译、且已经有进度的
// 机器里按进度降序取第 需要 名，那台就是最后修完的（前面 需要-1 台都会比它先完成）。
// 有进度的机器不足 需要 台时返回 false：剩下的名额会落在某台 0% 的机器上，无从判断是哪台。
// 只在 完成 == 4（即 需要 == 1）时才给结果：此时进度最高的那台就是最后一台，是确定的。
// 完成 < 4 时的第 需要 名只是按当前进度的预测，中途换人修就会变，所以不显示。
static bool 最后一台(float &进度)
{
    if (!g_可用) return false;
    int b = g_活跃, n = g_计数[b];
    int 完成 = 0;
    float 有进度的[最大机器数];
    int m = 0;
    for (int i = 0; i < n; i++) {
        float v = g_缓冲[b][i].进度;
        if (已破译(v)) { 完成++; continue; }
        if (v > 0.05f) 有进度的[m++] = v;
    }
    if (完成 < 4) return false;                    // 不满 4 台时不显示：名次还会变
    int 需要 = 5 - 完成;
    if (需要 < 1 || m < 需要) return false;         // 已破译满 5 台则没有"最后一台"可言
    for (int i = 0; i < m - 1; i++)                 // 只有几个元素，插入排序足够
        for (int j = i + 1; j < m; j++)
            if (有进度的[j] > 有进度的[i]) { float t = 有进度的[i]; 有进度的[i] = 有进度的[j]; 有进度的[j] = t; }
    进度 = 有进度的[需要 - 1];                       // 降序第 需要 名
    return true;
}

} // namespace 密码机进度

#endif // IDV_PY_PROGRESS_H
