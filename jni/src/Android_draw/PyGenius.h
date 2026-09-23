#ifndef IDV_PY_GENIUS_H
#define IDV_PY_GENIUS_H

// ============================================================================
// 天赋与辅助特质 —— 走游戏内嵌 CPython 的对象图读取
// ============================================================================
//
// 链路跟 PyProgress.h 完全一样(sys.modules -> game_kernel -> unit_mgr ->
// units_by_type)，只是取的是 1(ButcherUnit)、2(CivilianUnit)、236(梦之信徒，只读冷却) 三个键，
// 读的属性不同。CPython 3.11 的结构布局、防竞态读法、按名字查找+序号缓存
// 这些共通的东西在 PyProgress.h 文件头有完整说明，这里不重复。
//
// 属性：
//   genius_id_lv_lst   list[[天赋id, 等级], ...]
//   support_skill_id   list[id]，监管的辅助特质；求生者也有这个字段
//   model              -> +0x20 = 场景对象指针，用来跟绘制循环里的 obj 配对
//   position           -> +0x10/+0x14/+0x18 = X/高度/Y，model 为空时的兜底
//
// ！！两个必须记住的坑 ！！
//
// 1) **活体属性名和录像里的不一样**：录像快照里是 `genius_id_lvs`，
//    Python 层是 `genius_id_lv_lst`。照录像的名字去查会一个也找不到。
//
// 2) **监管和求生者共用同一套天赋 id 号段，同一个 id 在两边是不同天赋。**
//    尤其 id 26 在求生者侧是绝处逢生，在监管侧是另一个(还没查清的)天赋 ——
//    所以"有绝处逢生就标绿"这条只能对 阵营==2 生效，绝不能按 id 直接判。
//
// 3) **genius_id_lv_lst 经常读不出内容**(PC 侧脚本里表现成空列表或读不到)，
//    而同一时刻隔壁的 support_skill_id 每次都好。**原因还没定论。**
//    最合理的假设是冷热页差异：support_skill_id 是界面一直在用的热数据、结构也浅；
//    genius_id_lv_lst 是十几个嵌套 list，要碰的页多得多，而且开局读一次之后
//    再没人碰，是冷页 —— 内存压力下优先被换进 zram。
//
//    ⚠ 注意这里的读法：本文件的 read_genius() 自己读 ob_size，能分清"真的空"和"读不到"。
//    PC 侧那些脚本用的 pyw.list_items() 把这两种情况都返回 []，**不能拿它判断列表是不是空的**。
//
//    **做法：按 uid 增量累积 + 二次确认**(见 read_genius())。天赋一局之内不会变，所以每轮能读到几条
//    就合并几条进记忆，哪怕每一轮都只读到一部分，记忆也会逐渐补全：
//      - **一轮就读全**(n 条全部合法且 id 不重复)直接判完整 —— 天赋页是冷页，内存压力下
//        被压进 zram 后驱动读不回来，必须趁热一次拿下(2026-09-22 VmSwap 1.1G 时天赋全不显示)
//      - 没读全时，单条 [id, 等级] 要在**两个不同轮次读到相同值**才确认，挡住偶发垃圾(进记忆就是一整局)
//      - 列表长度 ob_size 本身读得稳，记为 n；已确认条数 == n 才算"完整"
//      - 只有"完整"时才能断言"没带某个天赋"；不完整时只能说"已确认带了哪些"
//    旧做法是"单次读到 >=2 个大天赋(求生者还要有 26)才锁存"，把"读全了没有"和"一般人怎么带"
//    混在一起判，导致少带大天赋/没带绝处逢生的人永远锁不上，而且"没带"和"读失败"分不开。
//
//    记忆的清理时机：**unit_mgr 地址变了才清**。uid 每局都从 1000001 重新开始，
//    不清的话下一局会套用上一局同号玩家的天赋，那是实打实的错误信息。
//    而 unit_mgr 实例每局重新分配(见 python-layer.md 的生命周期)，
//    地址一变就说明换局了，这个信号比"连续失败 N 轮"准得多 ——
//    后者在对局中途链路短暂中断(内存压力把 CPython 堆压进 zram)时会误清。
// ============================================================================

#include <cstdint>
#include <cstring>
#include <cmath>
#include <cstdio>
#include <thread>
#include <ctime>
#include <atomic>
#include <unistd.h>
#include "PyRoot.h"

namespace PyGenius {

// sys.modules 与 dict/int/float/True/False 由 PyRoot.h 运行时找出，这里不再写死模块偏移

static const int MAX_UNITS = 24;   // 1 监管 + 4 求生者 + 梦之信徒(实测一局 5 个) + 余量
static const int BUTCHER_UNIT_TYPE  = 1;
static const int CIVILIAN_UNIT_TYPE = 2;
// 梦之女巫的信徒(YidhraPuppetUnit)。2026-09-22 实测(求生者视角读对面女巫)：每个信徒有**自己的**
// skill_mgr / skill_dict / 711 闪现技能对象，_cd_delta 各自独立递减；support_skill_id 与本体相同([7])。
// 所以按特质找主技能那套逻辑原样适用。信徒不读天赋/绝处逢生，只要冷却
static const int YIDHRA_PUPPET_UNIT_TYPE = 236;

// 绝处逢生的**被动技能 id**(不是天赋 id)。天赋 id 是 26，落地成被动是 102。
// unit.ability_used 是 dict{被动id: bool}，True = 本局已消耗。
// 2026-09-22 实测：非本机的 CivilianUnit 上读到 {102: True}，
// 说明服务器**会把别人的消耗状态下发给本机**，不是只有自己的能看。
// 这个属性只存在于求生者类上，监管(MyButcherUnit)身上根本没有。
static const int PASSIVE_DESPERATE = 102;

// 飞轮效应(求生者天赋 8，被动 207)的**技能 id**。被动配表
// passive_skill_data[(207,1)]['active_skills'] = (11111,)，冷却挂在 skill_dict[11111] 上：
// SkillFlywheelSprint，cd_time 135、game_start_cd 50，_cd_delta 每秒 -1(2026-09-22 注入实测)。
// ⚠ 配表里那个 skill_cd = 10.0 不是飞轮冷却。
// ⚠ 只在本机玩家身上验过；非本机求生者的 skill_dict 里有没有它还没验(那局没人带)。
static const int SKILL_FLYWHEEL = 11111;

// 天赋 id -> 位号。只关心四个分支终端 + 26(求生者的绝处逢生)，其余忽略。
// 两个阵营共用这张 id->位 的映射，名字在格式化时才按阵营区分。
enum {
    BIT_ID_8  = 1 << 0,   // id 8 : 求生=飞轮效应  监管=紧闭空间
    BIT_ID_16 = 1 << 1,  // id 16: 求生=回光返照  监管=底牌
    BIT_ID_24 = 1 << 2,  // id 24: 求生=化险为夷  监管=挽留
    BIT_ID_26 = 1 << 3,  // id 26: 求生=绝处逢生  监管=未知
    BIT_ID_32 = 1 << 4,  // id 32: 求生=膝跳反射  监管=张狂
};

struct Info {
    int      camp;       // 1=监管 2=求生 236=梦之信徒(只有特质冷却，天赋状态恒为未知)
    uint32_t genius_bits;     // 只含**已确认**的条目
    int      support_trait;   // 1..8，0=没读到
    int      genius_state;   // GENIUS_UNKNOWN / GENIUS_PARTIAL / GENIUS_COMPLETE，见 read_genius()
    uint8_t  genius_levels[41]; // 已确认的 {天赋id: 等级}，0 = 未确认或没带。下标 1~40
    bool     desperate_used;   // ability_used[102] == True，本局已经触发过绝处逢生
    // 监管辅助特质的冷却(求生者侧全为 0)
    bool     has_cd;     // false = 没读到技能对象
    float    cd_remain;   // 秒，0 = 就绪。**读到那一刻的值**，显示时用 cd_remain_now() 往下推
    int64_t  cd_read_ms;   // now_ms() 时钟，配合 冷却剩余 做推算
    float    cd_rate;   // skill_mgr.cd_rate，实测恒 1；推算时乘上它
    float    cd_total;   // cd_time，画进度环用
    int      charge_max;   // power_num，非充能型为 0
    int      charge_cur;   // _cur_power_num
    // 求生者飞轮效应的冷却(skill_dict[SKILL_FLYWHEEL])，监管侧全为 0
    bool     has_flywheel;     // skill_dict 里有飞轮技能且读到了 _cd_delta
    float    flywheel_remain;   // 秒，0 = 就绪。读到那一刻的值
    int64_t  flywheel_read_ms;   // now_ms() 时钟
    float    flywheel_rate;   // skill_mgr.cd_rate
};

struct Entry {
    uint64_t scene_obj;
    float    x, y;
    Info     info;
};

// 双缓冲发布：写线程填非活跃缓冲，填完再翻转，绘制线程永远读到完整的一份
static Entry g_buf[2][MAX_UNITS];
static volatile int g_count[2] = {0, 0};
static std::atomic<int> g_active{0};      // 见 PyProgress.h 同名变量的注释(volatile 不保证发布顺序)

// 天赋锁存：genius_id_lv_lst 会间歇性读空(见文件头第 3 条)，按 uid 记住最后一次非空的结果
enum { GENIUS_UNKNOWN = 0, GENIUS_PARTIAL = 1, GENIUS_COMPLETE = 2 };

// 每个玩家的天赋记忆(按 uid)，增量累积，见 read_genius()
struct GeniusMemory {
    int64_t uid;
    int     n;              // genius_id_lv_lst 的长度(ob_size)，0 = 还没读到过
    int     confirmed;
    uint8_t levels[41];       // 已确认的等级，0 = 未确认
    uint8_t candidates[41];       // 读到过一次、等待第二次确认的等级
};
static GeniusMemory g_mem[MAX_UNITS];
static int      g_mem_count = 0;
static uint64_t g_last_unit_mgr = 0;    // 地址一变就说明换局了，此时才清记忆

static uintptr_t g_libbase = 0;
static uint64_t  g_int_type = 0, g_float_type = 0, g_dict_type = 0;
static uint64_t  g_true = 0, g_false = 0;
static uint64_t  g_module_dict = 0;
static int64_t   g_i_unit_mgr = -1, g_i_ubt = -1;
static int64_t   g_i_genius = -1, g_i_support = -1, g_i_model = -1, g_i_pos = -1, g_i_uid = -1;
static int64_t   g_i_ability = -1;   // ability_used，只有求生者类上有
// 监管技能冷却链：unit.skill_mgr -> skill_dict{id: Skill} / leader_skills(set)
static int64_t   g_i_skillmgr = -1, g_i_skilldict = -1, g_i_cdrate = -1;
// Skill 对象上的属性(223 条)，各自一套缓存
static int64_t   g_i_cd = -1, g_i_cdtime = -1, g_i_power = -1, g_i_curpower = -1;

static void clear_mem(GeniusMemory &m, int64_t uid)
{
    memset(&m, 0, sizeof(m));
    m.uid = uid;
}

// 取某个 uid 的记忆，没有就新建；表满了返回 nullptr
static GeniusMemory *get_mem(int64_t uid)
{
    for (int i = 0; i < g_mem_count; i++)
        if (g_mem[i].uid == uid) return &g_mem[i];
    if (g_mem_count >= MAX_UNITS) return nullptr;
    clear_mem(g_mem[g_mem_count], uid);
    return &g_mem[g_mem_count++];
}
static volatile bool g_available = false;
static volatile int  g_fail_streak = 0;
static char g_status[96] = "未启动";

static inline bool is_obj(uint64_t p) { return p > 0x7000000000ULL && p < 0x8000000000ULL; }

// 读线程和绘制线程共用的单调时钟，冷却推算用
static inline int64_t now_ms()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static bool str_equals(uint64_t s, const char *want)
{
    if (!is_obj(s)) return false;
    int64_t len = 0;
    if (!vm_readv(s + 0x10, &len, 8)) return false;
    size_t n = strlen(want);
    if (len != (int64_t)n || n >= 64) return false;
    char buf[64];
    if (!vm_readv(s + 0x30, buf, n)) return false;
    return memcmp(buf, want, n) == 0;
}

struct Dict { uint64_t entries; int stride; int64_t n_entries; };

static bool read_dict(uint64_t d, Dict &out)
{
    if (!is_obj(d)) return false;
    uint64_t keys = getPtr64(d + 0x20);
    if (!is_obj(keys)) return false;
    uint8_t hdr[32];
    if (!vm_readv(keys, hdr, 32)) return false;
    uint8_t idxb = hdr[9];
    uint8_t kind = hdr[10];
    int64_t nent = 0; memcpy(&nent, hdr + 24, 8);
    if (idxb > 30 || nent < 0 || nent > (1 << 22)) return false;
    out.entries = keys + 32 + ((uint64_t)1 << idxb);   // 必须逐实例算，不能跨实例套用
    out.stride   = (kind == 0) ? 24 : 16;
    out.n_entries   = nent;
    return true;
}

static inline uint64_t key_slot(const Dict &k, int64_t i) { return k.entries + i * k.stride + (k.stride == 24 ? 8 : 0); }
static inline uint64_t value_slot(const Dict &k, int64_t i) { return k.entries + i * k.stride + (k.stride == 24 ? 16 : 8); }

// ---- 字典查找的两项优化(2026-09-23) ----
// 原来每比一个键要 3 次驱动读(取键指针 + 读长度 + 读内容)，全扫上千条的实例字典就是几千次读。
//   a) 整块读条目：entries 是连续内存，一次 vm_readv 读完(1300 条 * 24 字节 = 31KB)，
//      之后在本地取键指针，省掉"每条一次取指针"。
//   b) 驻留键指针：CPython 会驻留标识符形式的属性名，所以同一个属性名在进程里就是同一个
//      字符串对象。第一次按名字找到后记下键对象地址，以后只比指针，连字符串都不用读。
//      这个假设不成立时会自动退回按名字比较，只是慢一点，不会读错。
// 效果：命中缓存的校验从 3 次读降到 1 次；全扫从几千次降到 1 次(整块) + 本地比较。
static uint8_t g_entry_block[48 * 1024];
struct InternedName { const char *name; uint64_t key_obj; };
static InternedName g_interned[32];
static int g_interned_n = 0;

static uint64_t interned_of(const char *name)
{
    for (int i = 0; i < g_interned_n; i++)
        if (strcmp(g_interned[i].name, name) == 0) return g_interned[i].key_obj;
    return 0;
}

static void remember_interned(const char *name, uint64_t kp)
{
    for (int i = 0; i < g_interned_n; i++)
        if (strcmp(g_interned[i].name, name) == 0) { g_interned[i].key_obj = kp; return; }
    if (g_interned_n < (int)(sizeof(g_interned) / sizeof(g_interned[0]))) {
        g_interned[g_interned_n].name = name;          // 调用方传的都是字符串常量，生命周期同进程
        g_interned[g_interned_n].key_obj = kp;
        g_interned_n++;
    }
}

static int64_t find_key(uint64_t d, const char *name, int64_t hint)
{
    Dict k;
    if (!read_dict(d, k)) return -1;
    const uint64_t want = interned_of(name);
    if (hint >= 0 && hint < k.n_entries) {
        uint64_t kp = getPtr64(key_slot(k, hint));
        if (want && kp == want) return hint;                        // 命中缓存：只花 1 次读
        if (str_equals(kp, name)) { remember_interned(name, kp); return hint; }
    }
    size_t bytes = (size_t)k.n_entries * k.stride;
    int key_off = (k.stride == 24) ? 8 : 0;
    if (bytes > 0 && bytes <= sizeof(g_entry_block) && vm_readv(k.entries, g_entry_block, bytes)) {
        for (int pass = 0; pass < 2; pass++) {
            if (pass == 0 && !want) continue;                       // 还不知道键地址，直接进第二遍
            for (int64_t i = 0; i < k.n_entries; i++) {
                uint64_t kp = 0;
                memcpy(&kp, g_entry_block + (size_t)i * k.stride + key_off, 8);
                kp &= 0x00FFFFFFFFFFFFFFULL;
                if (pass == 0) { if (kp == want) return i; }
                else if (str_equals(kp, name)) { remember_interned(name, kp); return i; }
            }
        }
        return -1;
    }
    // 整块读失败(条目跨到换出的页等)：退回逐条读，行为和以前完全一样
    for (int64_t i = 0; i < k.n_entries; i++)
        if (str_equals(getPtr64(key_slot(k, i)), name)) return i;
    return -1;
}

static uint64_t get_attr(uint64_t d, int64_t idx)
{
    Dict k;
    if (!read_dict(d, k) || idx < 0 || idx >= k.n_entries) return 0;
    return getPtr64(value_slot(k, idx));
}

static int64_t resolve_index(uint64_t d, const Dict &dk, int64_t cached, const char *name)
{
    if (cached >= 0 && cached < dk.n_entries) {
        uint64_t kp = getPtr64(key_slot(dk, cached));
        uint64_t want = interned_of(name);
        if (want && kp == want) return cached;                      // 驻留键指针：校验只要 1 次读
        if (str_equals(kp, name)) { remember_interned(name, kp); return cached; }
    }
    return find_key(d, name, -1);
}

// 读一个 PyLong。天赋 id / 等级 / 特质 id 都是小整数，恒为 int，不会变成 float
static bool read_int(uint64_t vp, int64_t &out)
{
    if (!is_obj(vp)) return false;
    uint8_t b[32];
    if (!vm_readv(vp, b, 32)) return false;
    int64_t rc = 0; memcpy(&rc, b, 8);
    uint64_t tp = 0; memcpy(&tp, b + 8, 8); tp &= 0x00FFFFFFFFFFFFFFULL;
    if (rc <= 0 || tp != g_int_type) return false;
    int64_t sz = 0; memcpy(&sz, b + 16, 8);
    uint32_t dg = 0; memcpy(&dg, b + 24, 4);
    out = (sz == 0) ? 0 : (int64_t)dg;
    if (sz < 0) out = -out;
    return true;
}

// 读 int/float 属性，带防竞态校验。值域上下界由调用方给：
// 冷却是 0~600 秒级(闪现 csv_cd_time 就有 150)，不能套进度那条 0~100 的闸。
// _cd_delta 每秒都在减，float 对象被频繁重新分配，所以"重读指针确认没变"这步不能省。
static bool read_float(uint64_t slot, float &out, float lo, float hi)
{
    for (int t = 0; t < 3; t++) {
        uint64_t vp = getPtr64(slot);
        if (!is_obj(vp)) continue;
        uint8_t b[32];
        if (!vm_readv(vp, b, 32)) continue;
        if (getPtr64(slot) != vp) continue;       // 解引用期间被重新赋值了
        int64_t rc = 0; memcpy(&rc, b, 8);
        if (rc <= 0) continue;
        uint64_t tp = 0; memcpy(&tp, b + 8, 8); tp &= 0x00FFFFFFFFFFFFFFULL;
        double v;
        if (tp == g_float_type) {
            memcpy(&v, b + 16, 8);
        } else if (tp == g_int_type) {
            int64_t sz = 0; memcpy(&sz, b + 16, 8);
            uint32_t dg = 0; memcpy(&dg, b + 24, 4);
            v = (sz == 0) ? 0.0 : (double)dg;
            if (sz < 0) v = -v;
        } else {
            continue;
        }
        if (!(v >= lo && v <= hi)) continue;
        out = (float)v;
        return true;
    }
    return false;
}

// 取 list 的 (元素数, 元素数组)。空 list 也算成功(n=0)，因为"空"是有意义的状态
static bool read_list(uint64_t L, int64_t &n, uint64_t &items)
{
    if (!is_obj(L)) return false;
    if (!vm_readv(L + 0x10, &n, 8)) return false;
    if (n < 0 || n > 256) return false;
    if (n == 0) { items = 0; return true; }
    items = getPtr64(L + 0x18);
    return is_obj(items);
}

// 天赋 id 的合法等级。来自配表(genius-table.md 的 lv 列)，求生者和监管这组 id 完全相同：
// 层 2.2 和 3.1/3.2/3.3 的节点可加 1~3 级，其余(分支根、2.1/2.3、终端、33~40)只有 1 级。
// 用来挡垃圾：读到的 [id, 等级] 不在配表允许范围里就不认。
static bool level_valid(int64_t id, int64_t lv)
{
    if (id < 1 || id > 40) return false;
    if (lv == 1) return true;
    if (lv < 1 || lv > 3) return false;
    if (id > 32) return false;                        // 33~40 只有 1 级
    int r = (int)(id % 8);
    return r == 3 || r == 5 || r == 6 || r == 7;      // 3/5/6/7、11/13/14/15、… 可加到 3 级
}

// genius_id_lv_lst 是 list[list[id, lv]]。每轮读一次，**增量合并**进这个玩家的记忆：
//   - 某一轮一次读全(合法不重复条数 == n)就直接判完整，见函数里的说明。
//   - 没读全时：每条 [id, 等级] 第一次读到进"候选"，之后某一轮读到相同值才"确认"(挡偶发垃圾)。
//     候选和再读到的值不一致，就用新值重新当候选，不确认。
//   - 列表长度 n 记在记忆里；已确认条数 == n 就是完整。
//   - n 变了(同一局同一个人不该变)说明记忆不可信，整份重来。
//   - 读到空列表(n==0)不动记忆：天赋一局不会变，空只可能是还没同步或已被清理。
// 返回 false = 这一轮连列表头都没读到；记忆不受影响。
static bool read_genius(uint64_t slot, GeniusMemory &mem)
{
    uint64_t lp = getPtr64(slot);
    int64_t n = 0; uint64_t items = 0;
    if (!read_list(lp, n, items)) return false;
    if (getPtr64(slot) != lp) return false;             // 解引用期间被重新赋值了
    if (n == 0) return true;
    if (n > 40) return false;                         // 天赋一共就 40 个，再多一定是垃圾

    if (mem.n != 0 && mem.n != (int)n) clear_mem(mem, mem.uid);
    mem.n = (int)n;

    // 先把这一轮读到的合法条目收齐，再决定怎么合并
    uint8_t this_round[41] = {0};
    int     this_round_n = 0;
    for (int64_t i = 0; i < n; i++) {
        uint64_t pair = getPtr64(items + i * 8);
        int64_t m = 0; uint64_t pit = 0;
        if (!read_list(pair, m, pit) || m != 2) continue;  // 单个条目读不到就跳过，下一轮再补
        int64_t id = 0, lv = 0;
        if (!read_int(getPtr64(pit), id)) continue;
        if (!read_int(getPtr64(pit + 8), lv)) continue;
        if (!level_valid(id, lv)) continue;
        if (this_round[id]) continue;                         // 同一轮里重复出现，不算两次
        this_round[id] = (uint8_t)lv;
        this_round_n++;
    }

    // 一轮就读全(合法且不重复的条数 == n)：直接判完整，不等第二轮。
    // 天赋页是冷页，开局读一次后游戏不再碰，内存压力下会被压进 zram 且再也换不回来；
    // 页还热的时候一次拿下，比等两轮确认可靠得多。垃圾值要同时满足
    // "n 条全部是合法 [id, 等级]、id 互不重复"几乎不可能。
    // 跟已确认的记忆冲突，说明有一方是垃圾：整份重来，这一轮的值只当候选。
    if (this_round_n == (int)n) {
        bool conflict = false;
        for (int id = 1; id <= 40; id++)
            if (mem.levels[id] != 0 && mem.levels[id] != this_round[id]) { conflict = true; break; }
        if (conflict) {
            clear_mem(mem, mem.uid);
            mem.n = (int)n;
            memcpy(mem.candidates, this_round, sizeof(mem.candidates));
        } else {
            memcpy(mem.levels, this_round, sizeof(mem.levels));
            mem.confirmed = (int)n;
        }
        return true;
    }

    // 没读全：逐条增量合并，两个不同轮次读到相同值才确认
    for (int id = 1; id <= 40; id++) {
        int lv = this_round[id];
        if (lv == 0) continue;
        if (mem.levels[id] != 0) continue;                 // 已确认，天赋一局不变
        if (mem.candidates[id] == (uint8_t)lv) {              // 两个不同轮次读到相同值 -> 确认
            mem.levels[id] = (uint8_t)lv;
            mem.confirmed++;
        } else {
            mem.candidates[id] = (uint8_t)lv;
        }
    }
    // 确认的比列表还多，说明有垃圾混进来了，整份重来
    if (mem.confirmed > mem.n) clear_mem(mem, mem.uid);
    return true;
}

// 记忆 -> 显示用的位掩码 / 状态
static uint32_t mem_bits(const GeniusMemory &mem)
{
    uint32_t bits = 0;
    if (mem.levels[8])  bits |= BIT_ID_8;
    if (mem.levels[16]) bits |= BIT_ID_16;
    if (mem.levels[24]) bits |= BIT_ID_24;
    if (mem.levels[26]) bits |= BIT_ID_26;
    if (mem.levels[32]) bits |= BIT_ID_32;
    return bits;
}

static int mem_state(const GeniusMemory &mem)
{
    if (mem.confirmed == 0) return GENIUS_UNKNOWN;
    return (mem.n > 0 && mem.confirmed == mem.n) ? GENIUS_COMPLETE : GENIUS_PARTIAL;
}

// ability_used = dict{被动技能id: bool}。找键 102(绝处逢生)，值为 True 就是本局已消耗。
// 键不在表里 = 还没用过(实测没用过的人这个 dict 是空的 {})，所以"找不到"要返回 false 而不是失败。
static bool read_desperate_used(uint64_t slot)
{
    uint64_t dp = getPtr64(slot);
    if (!is_obj(dp) || getPtr64(dp + 8) != g_dict_type) return false;
    Dict k;
    if (!read_dict(dp, k)) return false;
    for (int64_t i = 0; i < k.n_entries; i++) {
        int64_t id = 0;
        if (!read_int(getPtr64(key_slot(k, i)), id) || id != PASSIVE_DESPERATE) continue;
        uint64_t vp = getPtr64(value_slot(k, i));
        if (vp == g_true) return true;
        if (vp == g_false) return false;
        // 单例比不上就按 PyLong 布局退化解，热更挪了单例地址也还能用
        uint8_t b[32];
        if (!vm_readv(vp, b, 32)) return false;
        int64_t sz = 0; memcpy(&sz, b + 16, 8);
        uint32_t dg = 0; memcpy(&dg, b + 24, 4);
        return sz != 0 && dg != 0;
    }
    return false;
}

static bool resolve_module()
{
    uint64_t sm = getPtr64(PyRoot::g_modules_slot);
    if (!is_obj(sm)) { snprintf(g_status, sizeof(g_status), "sys.modules 取不到"); return false; }
    if (getPtr64(sm + 8) != g_dict_type) { snprintf(g_status, sizeof(g_status), "sys.modules 不是dict(偏移已失效)"); return false; }
    int64_t i = find_key(sm, "game_kernel", -1);
    if (i < 0) { snprintf(g_status, sizeof(g_status), "找不到 game_kernel 模块"); return false; }
    uint64_t mod = get_attr(sm, i);
    if (!is_obj(mod)) { snprintf(g_status, sizeof(g_status), "game_kernel 模块对象无效"); return false; }
    g_module_dict = getPtr64(mod + 0x10);
    if (!is_obj(g_module_dict)) { g_module_dict = 0; snprintf(g_status, sizeof(g_status), "md_dict 无效"); return false; }
    return true;
}

// 在 units_by_type(int -> list) 里按键取表
static uint64_t get_type_list(uint64_t ubt, int type)
{
    Dict k;
    if (!read_dict(ubt, k)) return 0;
    for (int64_t i = 0; i < k.n_entries; i++) {
        uint64_t kp = getPtr64(key_slot(k, i));
        if (!is_obj(kp) || getPtr64(kp + 8) != g_int_type) continue;
        int64_t sz = 0; uint32_t dg = 0;
        vm_readv(kp + 16, &sz, 8);
        vm_readv(kp + 24, &dg, 4);
        if (sz == 1 && (int)dg == type) return getPtr64(value_slot(k, i));
    }
    return 0;
}

// 监管当前辅助特质的剩余冷却。链路见 idv-replay-genius 第四节：
//   unit.skill_mgr -> skill_dict{技能id: Skill}
//   Skill._cd_delta  = 剩余秒数，**不在冷却中时恒为 0**(所以 0 就是"就绪")
//   充能型(窥视者)：power_num=最大层数 _cur_power_num=当前层数 _cd_delta=距下一层回满
//
// ！！不能用 leader_skills + is_leader_skill 筛！！(旧实现就是这么写的，结果永远"就绪")
// leader_skills 里混着角色自身技能(红夫人 2601 普攻 lead=True、_cd_delta 恒 0)，
// is_leader_skill=True 的也不止一个(窥视者 25732/25733 都是，但只有 25732 在走)。
//
// 特质 -> 技能 id 来自游戏配表 GK.DM.support_skill_data[特质][handheld_id]['skill_id']
// (2026-09-22 注入 dump，原始数据存档在 逆向存档\2026.0917_b6f53566\captures\support_skill_data.txt)。
// 同一特质按监管角色(handheld_id)有不同的技能 id，而且**没有统一规律**：
//   传送 多数 x741/x743，但也有 12644、13718、3844……；移形每个角色都不同(8802、8811、8883……)
// 每组的**第一个 id 是主技能**，已实测：窥视者 732(733 的 _cd_delta 恒 30)、闪现 711、
// 传送 741(与 743 共享冷却)。这里把每个特质下所有角色的主技能 id 收成一个集合，
// 在 skill_dict 里找落在集合里的那个 —— 一个监管身上只挂自己那一版，不会撞。
// support_skill_id 是实时值(底牌换完立刻变)，旧特质那些冻结的技能对象不在当前集合里，自然不会被选中。
// ⚠️ 热更上新监管时这张表会缺新角色的 id：表现是该角色只显示特质名、不显示冷却，不会给错值。
//    重新 dump：注入 probe2，命令通道里读 GK.DM.support_skill_data。
static const int32_t MAIN_SKILL_LISTEN[]   = {701};
static const int32_t MAIN_SKILL_ABNORMAL[]   = {751};
static const int32_t MAIN_SKILL_EXCITEMENT[]   = {722};
static const int32_t MAIN_SKILL_PATROLLER[] = {761, 763, 765, 3850, 9761, 10761, 11761, 12650, 13450, 13724, 13761, 14050,
                                      14559, 14738, 15159, 15359, 15569, 17761, 21761, 22761, 23761, 24761, 25761,
                                      26761, 27761, 28761, 29761, 30761, 31761, 32761, 34761, 35761, 125750, 128750,
                                      136850, 143750};
static const int32_t MAIN_SKILL_TELEPORT[]   = {741, 3741, 3844, 8741, 9741, 10741, 12644, 13444, 13718, 13741, 14044, 14553,
                                      14732, 15153, 15353, 15563, 17741, 21741, 22741, 23741, 24741, 25741, 26741,
                                      27741, 28741, 29741, 30741, 31741, 32741, 34741, 35741, 125744, 128744, 136844,
                                      143744};
static const int32_t MAIN_SKILL_PEEPER[] = {732, 735, 738, 3841, 9732, 10732, 11732, 12641, 13441, 13715, 13732, 14041,
                                      14550, 14729, 15150, 15350, 15560, 17732, 21732, 22732, 23732, 24732, 25732,
                                      26732, 27732, 28732, 29732, 30732, 31732, 32732, 34732, 35732, 125741, 128741,
                                      136841, 143741};
static const int32_t MAIN_SKILL_BLINK[]   = {711};
static const int32_t MAIN_SKILL_SHIFT[]   = {8802, 8805, 8808, 8811, 8814, 8817, 8820, 8823, 8826, 8829, 8832, 8835, 8838,
                                      8841, 8844, 8847, 8850, 8853, 8856, 8859, 8862, 8865, 8868, 8871, 8874, 8877,
                                      8880, 8883, 8886, 8890, 8893, 8896, 14562, 14742, 15162, 15362, 15572, 143753};

#define PICK_TABLE(a) do { p = a; n = (int)(sizeof(a) / sizeof(a[0])); } while (0)
static bool is_trait_main_skill(int trait, int64_t sid)
{
    const int32_t *p = nullptr; int n = 0;
    switch (trait) {
        case 1: PICK_TABLE(MAIN_SKILL_LISTEN);   break;
        case 2: PICK_TABLE(MAIN_SKILL_ABNORMAL);   break;
        case 3: PICK_TABLE(MAIN_SKILL_EXCITEMENT);   break;
        case 4: PICK_TABLE(MAIN_SKILL_PATROLLER); break;
        case 5: PICK_TABLE(MAIN_SKILL_TELEPORT);   break;
        case 6: PICK_TABLE(MAIN_SKILL_PEEPER); break;
        case 7: PICK_TABLE(MAIN_SKILL_BLINK);   break;
        case 8: PICK_TABLE(MAIN_SKILL_SHIFT);   break;
        default: return false;
    }
    for (int i = 0; i < n; i++) if (p[i] == sid) return true;
    return false;
}

// unit.skill_mgr -> skill_dict 的条目表。监管和求生者共用。
// 速率 = skill_mgr.cd_rate：实测恒为 int 1，但名字说明它能变(加速冷却类效果)，读不到就按 1
static bool get_skill_dict(uint64_t d, const Dict &dk, Dict &sdk, float &rate)
{
    rate = 1.f;
    g_i_skillmgr = resolve_index(d, dk, g_i_skillmgr, "skill_mgr");
    if (g_i_skillmgr < 0) return false;
    uint64_t sm = getPtr64(value_slot(dk, g_i_skillmgr));
    if (!is_obj(sm)) return false;
    uint64_t smd = getPtr64(sm - 0x18);
    Dict smk;
    if (!read_dict(smd, smk)) return false;

    g_i_skilldict = resolve_index(smd, smk, g_i_skilldict, "skill_dict");
    if (g_i_skilldict < 0) return false;
    g_i_cdrate = resolve_index(smd, smk, g_i_cdrate, "cd_rate");
    if (g_i_cdrate >= 0 && !read_float(value_slot(smk, g_i_cdrate), rate, 0.01f, 10.f)) rate = 1.f;
    uint64_t sd = getPtr64(value_slot(smk, g_i_skilldict));
    return is_obj(sd) && getPtr64(sd + 8) == g_dict_type && read_dict(sd, sdk);
}

// 读一个 Skill 对象的冷却。_cd_delta 读不到返回 false(不显示冷却，不猜)，其余几项读不到就保持原值
static bool read_skill_cd(uint64_t sk, float &remain, float &total, int &charge_max, int &charge_cur)
{
    if (!is_obj(sk)) return false;
    uint64_t skd = getPtr64(sk - 0x18);
    Dict skk;
    if (!read_dict(skd, skk)) return false;

    g_i_cd       = resolve_index(skd, skk, g_i_cd,       "_cd_delta");
    g_i_cdtime   = resolve_index(skd, skk, g_i_cdtime,   "cd_time");
    g_i_power    = resolve_index(skd, skk, g_i_power,    "power_num");
    g_i_curpower = resolve_index(skd, skk, g_i_curpower, "_cur_power_num");
    if (g_i_cd < 0) return false;
    if (!read_float(value_slot(skk, g_i_cd), remain, -0.5f, 600.0f)) return false;

    if (g_i_cdtime >= 0) read_float(value_slot(skk, g_i_cdtime), total, 0.f, 600.f);
    int64_t v = 0;
    if (g_i_power    >= 0 && read_int(getPtr64(value_slot(skk, g_i_power)),    v) && v > 0 && v < 16) charge_max = (int)v;
    if (g_i_curpower >= 0 && read_int(getPtr64(value_slot(skk, g_i_curpower)), v) && v >= 0 && v < 16) charge_cur = (int)v;
    return true;
}

static void collect_hunter_skill(uint64_t d, const Dict &dk, int trait, Info &res)
{
    if (trait < 1 || trait > 8) return;
    Dict sdk; float rate;
    if (!get_skill_dict(d, dk, sdk, rate)) return;

    for (int64_t i = 0; i < sdk.n_entries; i++) {
        int64_t sid = 0;
        if (!read_int(getPtr64(key_slot(sdk, i)), sid)) continue;
        if (!is_trait_main_skill(trait, sid)) continue;          // 先按 id 筛，只解那一个技能对象
        float cd = 0.f;
        if (!read_skill_cd(getPtr64(value_slot(sdk, i)), cd, res.cd_total, res.charge_max, res.charge_cur)) return;
        res.cd_remain = cd;
        res.cd_read_ms = now_ms();
        res.cd_rate = rate;
        res.has_cd   = true;
        return;                                        // 一个监管只挂一个主技能
    }
}

// 求生者的飞轮效应冷却。skill_dict 里没有 11111 = 没带飞轮(或这一轮没读到)，什么都不填
static void collect_flywheel(uint64_t d, const Dict &dk, Info &res)
{
    Dict sdk; float rate;
    if (!get_skill_dict(d, dk, sdk, rate)) return;

    for (int64_t i = 0; i < sdk.n_entries; i++) {
        int64_t sid = 0;
        if (!read_int(getPtr64(key_slot(sdk, i)), sid) || sid != SKILL_FLYWHEEL) continue;
        float cd = 0.f, total = 0.f; int full = 0, cur = 0;
        if (!read_skill_cd(getPtr64(value_slot(sdk, i)), cd, total, full, cur)) return;
        res.flywheel_remain = cd;
        res.flywheel_read_ms = now_ms();
        res.flywheel_rate = rate;
        res.has_flywheel   = true;
        return;
    }
}

static int collect_type(uint64_t ubt, int type, int write_idx, int count)
{
    uint64_t lst = get_type_list(ubt, type);
    int64_t n = 0; uint64_t items = 0;
    if (!read_list(lst, n, items) || n == 0) return count;

    for (int64_t i = 0; i < n && count < MAX_UNITS; i++) {
        uint64_t inst = getPtr64(items + i * 8);
        if (!is_obj(inst)) continue;
        uint64_t d = getPtr64(inst - 0x18);           // managed-dict 在对象前 0x18
        Dict dk;
        if (!read_dict(d, dk)) continue;

        const bool is_follower = (type == YIDHRA_PUPPET_UNIT_TYPE);
        g_i_support = resolve_index(d, dk, g_i_support, "support_skill_id");
        g_i_model   = resolve_index(d, dk, g_i_model,   "model");
        g_i_pos     = resolve_index(d, dk, g_i_pos,     "position");

        GeniusMemory *mem = nullptr;
        if (!is_follower) {                                   // 信徒不读天赋，也不占天赋记忆
            g_i_genius = resolve_index(d, dk, g_i_genius, "genius_id_lv_lst");
            g_i_uid    = resolve_index(d, dk, g_i_uid,    "uid");
            // 只在求生者类上找 ability_used：监管类上没这个属性，每个单位白扫一遍上千条不值
            if (type == CIVILIAN_UNIT_TYPE)
                g_i_ability = resolve_index(d, dk, g_i_ability, "ability_used");
            if (g_i_genius < 0) continue;

            int64_t uid = 0;
            bool has_uid = (g_i_uid >= 0) && read_int(getPtr64(value_slot(dk, g_i_uid)), uid);

            // 天赋按 uid 增量累积(见 read_genius())。这一轮 uid 读不到时天赋状态记为未知(只影响这一轮的显示)，
            // 记忆本身不动，下一轮读到 uid 会接着用
            mem = has_uid ? get_mem(uid) : nullptr;
            if (mem) read_genius(value_slot(dk, g_i_genius), *mem);
        }

        int trait = 0;
        if (g_i_support >= 0) {
            uint64_t sp = getPtr64(value_slot(dk, g_i_support));
            int64_t m = 0; uint64_t sit = 0;
            if (read_list(sp, m, sit) && m >= 1) {
                int64_t v = 0;
                if (read_int(getPtr64(sit), v) && v >= 1 && v <= 8) trait = (int)v;
            }
        }

        uint64_t scene = 0;
        if (g_i_model >= 0) {
            uint64_t mo = getPtr64(value_slot(dk, g_i_model));
            if (is_obj(mo)) {
                uint64_t sp2 = getPtr64(mo + 0x20);
                if (is_obj(sp2)) scene = sp2;
            }
        }
        float xyz[3] = {0, 0, 0};
        if (g_i_pos >= 0) {
            uint64_t pos = getPtr64(value_slot(dk, g_i_pos));
            if (is_obj(pos)) vm_readv(pos + 0x10, xyz, 12);
        }

        g_buf[write_idx][count].scene_obj = scene;
        g_buf[write_idx][count].x = xyz[0];
        g_buf[write_idx][count].y = xyz[2];
        bool used = false;
        if (type == CIVILIAN_UNIT_TYPE && g_i_ability >= 0)
            used = read_desperate_used(value_slot(dk, g_i_ability));

        Info inf;
        memset(&inf, 0, sizeof(inf));              // 冷却那几项求生者侧不填，必须先清零
        inf.camp     = type;
        inf.support_trait = trait;
        inf.desperate_used = used;
        if (mem) {
            inf.genius_bits   = mem_bits(*mem);
            inf.genius_state = mem_state(*mem);
            memcpy(inf.genius_levels, mem->levels, sizeof(inf.genius_levels));
        }
        if (type == BUTCHER_UNIT_TYPE || is_follower) collect_hunter_skill(d, dk, trait, inf);
        else                                  collect_flywheel(d, dk, inf);

        g_buf[write_idx][count].info = inf;
        count++;
    }
    return count;
}

static bool try_refresh()
{
    if (g_module_dict == 0 && !resolve_module()) return false;

    g_i_unit_mgr = find_key(g_module_dict, "unit_mgr", g_i_unit_mgr);
    uint64_t um = (g_i_unit_mgr >= 0) ? get_attr(g_module_dict, g_i_unit_mgr) : 0;
    if (!is_obj(um)) { snprintf(g_status, sizeof(g_status), "unit_mgr 无效(未在对局中?)"); return false; }

    // 换局了：uid 会从 1000001 重新开始，上一局的天赋记忆必须作废，否则会张冠李戴。
    // 注意只在**拿到有效新地址**时比较；um 无效时不动记忆，免得对局中途的
    // 短暂读失败把记忆误清。
    if (g_last_unit_mgr != 0 && um != g_last_unit_mgr) g_mem_count = 0;
    g_last_unit_mgr = um;

    uint64_t ud = getPtr64(um - 0x18);
    g_i_ubt = find_key(ud, "units_by_type", g_i_ubt);
    uint64_t ubt = (g_i_ubt >= 0) ? get_attr(ud, g_i_ubt) : 0;
    if (!is_obj(ubt)) { snprintf(g_status, sizeof(g_status), "units_by_type 无效"); return false; }

    int write_idx = 1 - g_active, count = 0;
    // 监管和求生者的属性序号缓存是共用的；两个类的插入顺序不同，
    // 所以每个单位都会走一次"按名字校验，不对就重扫"，多花的时间可以忽略
    count = collect_type(ubt, BUTCHER_UNIT_TYPE,  write_idx, count);
    count = collect_type(ubt, CIVILIAN_UNIT_TYPE, write_idx, count);
    count = collect_type(ubt, YIDHRA_PUPPET_UNIT_TYPE, write_idx, count);   // 没有女巫时这个键不存在，直接返回

    if (count == 0) { snprintf(g_status, sizeof(g_status), "一个单位也没读出"); return false; }
    g_count[write_idx] = count;
    g_active = write_idx;
    snprintf(g_status, sizeof(g_status), "正常 %d 人", count);
    return true;
}

static void refresh_once()
{
    if (g_libbase == 0) return;
    if (!PyRoot::ensure()) {
        g_available = false;
        snprintf(g_status, sizeof(g_status), "%s", PyRoot::status_text());
        return;
    }
    g_int_type = PyRoot::g_int_type; g_float_type = PyRoot::g_float_type; g_dict_type = PyRoot::g_dict_type;
    g_true = PyRoot::g_true; g_false = PyRoot::g_false;
    if (try_refresh()) { g_available = true; g_fail_streak = 0; return; }
    g_available = false;
    if (++g_fail_streak >= 8) {                       // 连续失败就把序号缓存全部作废，下一轮重新按名字找
        g_module_dict = 0;
        g_i_unit_mgr = g_i_ubt = -1;
        g_i_genius = g_i_support = g_i_model = g_i_pos = g_i_uid = g_i_ability = -1;
        g_i_skillmgr = g_i_skilldict = g_i_cdrate = -1;
        g_i_cd = g_i_cdtime = g_i_power = g_i_curpower = -1;
        g_fail_streak = 0;
        // 这里**不清天赋记忆**：对局中途链路短暂中断也会走到这里，清了会让天赋行白白消失。
        // 记忆只在 unit_mgr 地址变化(换局)时清，见 try_refresh()
    }
}

// 天赋一局之内基本不变(只有监管带底牌换特质时会动)，不用像进度那样 100ms 一轮
static const int REFRESH_INTERVAL_MS = 500;

static void thread_main()
{
    while (true) {
        refresh_once();
        usleep(REFRESH_INTERVAL_MS * 1000);
    }
}

static void start(uintptr_t libbase)
{
    if (g_libbase != 0) return;
    g_libbase   = libbase;
    snprintf(g_status, sizeof(g_status), "启动中");
    std::thread(thread_main).detach();
    printf("[天赋] 已启动 libbase=0x%lx\n", (unsigned long)libbase);
    fflush(stdout);
}

// ---------------- 给绘制侧用的只读接口 ----------------

static inline bool available() { return g_available; }
static inline const char *status_text() { return g_status; }

// 配对规则跟 PyProgress.h 一致：先用 model+0x20 的指针身份，读不到再退回坐标近邻。
// 人物是移动的，坐标兜底给的阈值比密码机那边紧，宁可配不上也不要配错。
static bool lookup(uintptr_t obj, float x, float y, Info &out)
{
    if (!g_available) return false;
    int b = g_active, n = g_count[b];
    for (int i = 0; i < n; i++) {
        if (g_buf[b][i].scene_obj == (uint64_t)obj) { out = g_buf[b][i].info; return true; }
    }
    int nearest = -1; float min_dist = 1.0f;
    for (int i = 0; i < n; i++) {
        if (g_buf[b][i].scene_obj != 0) continue;
        if (g_buf[b][i].x == 0.f && g_buf[b][i].y == 0.f) continue;
        float d = fabsf(g_buf[b][i].x - x) + fabsf(g_buf[b][i].y - y);
        if (d < min_dist) { min_dist = d; nearest = i; }
    }
    if (nearest < 0) return false;
    out = g_buf[b][nearest].info;
    return true;
}

// 绝处逢生只在求生者侧成立：id 26 在监管侧是另一个天赋，按 id 直接判会误标
static inline bool has_desperate(const Info &g)
{
    return g.camp == CIVILIAN_UNIT_TYPE && (g.genius_bits & BIT_ID_26);
}

// 绝处逢生四态：没带 / 带了还没用 / 带了已经用掉 / 还不知道
// **"没带"只有在天赋表完整时才能断言**；不完整又还没确认到 26，就是"未知"，不能当成没带
enum { DESPERATE_NONE = 0, DESPERATE_AVAILABLE = 1, DESPERATE_USED = 2, DESPERATE_UNKNOWN = 3 };
static inline int desperate_state(const Info &g)
{
    if (has_desperate(g)) return g.desperate_used ? DESPERATE_USED : DESPERATE_AVAILABLE;
    return g.genius_state == GENIUS_COMPLETE ? DESPERATE_NONE : DESPERATE_UNKNOWN;
}

// 监管辅助特质那一行的完整文本：特质名 + 冷却/充能。
//   普通技能   "闪现 12.7s" / "闪现 就绪"
//   充能型技能 "窥视者 2/3 20.9s"(还在攒下一层) / "窥视者 3/3"(满层)
// _cd_delta 不在冷却中时恒为 0，所以 0 直接当"就绪"用，不需要额外的标志位。
static void trait_line_text(const Info &g, const char *name, char *buf, size_t cap);

// 辅助特质名。1~8 连号，全表已确认
static const char *trait_name(int id)
{
    switch (id) {
        case 1: return "聆听";
        case 2: return "失常";
        case 3: return "兴奋";
        case 4: return "巡视者";
        case 5: return "传送";
        case 6: return "窥视者";
        case 7: return "闪现";
        case 8: return "移形";
        default: return "";
    }
}

// 读线程 500ms 才读一次 _cd_delta，直接显示会一卡一卡地跳 0.5。
// _cd_delta 实测严格按每秒 -1(乘 cd_rate)递减，所以绘制时从"读到的值"按经过的时间往下推。
// 推到 0 以下只停在 0.0，**不自己判就绪** —— 就绪以真正读到的 0 为准(见 cd_ready())，
// 否则底牌改写冷却、或者推算略快时会误报。下一轮读取会覆盖推算值，误差不超过一个读取周期。
static float extrapolate(float read_val, int64_t at_ms, float rate)
{
    if (read_val <= 0.05f) return 0.f;
    if (rate <= 0.f) rate = 1.f;
    float r = read_val - (float)(now_ms() - at_ms) / 1000.f * rate;
    return r > 0.f ? r : 0.f;
}
static float cd_remain_now(const Info &g)
{
    return g.has_cd ? extrapolate(g.cd_remain, g.cd_read_ms, g.cd_rate) : 0.f;
}

// 就绪 = 真正读到 0(不是推算到 0)
static inline bool cd_ready(const Info &g) { return g.has_cd && g.cd_remain <= 0.05f; }
static inline bool flywheel_ready(const Info &g) { return g.has_flywheel && g.flywheel_remain <= 0.05f; }

// 天赋简称行，拆成 前 / 飞轮 / 后 三段，飞轮单独一段好让绘制侧单独上色。
// 求生者：双弹 [飞轮] 搏命 大心脏(固定最后)；监管全部在"前"段：封窗 底牌 张狂 挽留(固定最后)。
//   未知：三段全空(不画)
//   部分：已确认的大天赋 + "?"，表示可能还有没读到的
//   完整：全部大天赋；一个大天赋都没带就写"无"
// 飞轮段：就绪 "飞轮"(飞轮就绪=true，画红) / 冷却中 "飞轮13s" / 没读到冷却 "飞轮"。
// skill_dict 里有飞轮技能本身就证明带了飞轮，所以天赋表读不全(未知/部分)时也照样显示它。
struct GeniusLine { char pre[48]; char flywheel[24]; char post[48]; bool flywheel_ready; };
static void genius_segments(const Info &g, GeniusLine &o)
{
    memset(&o, 0, sizeof(o));
    bool is_survivor = (g.camp == CIVILIAN_UNIT_TYPE);
    bool carries_flywheel = is_survivor && ((g.genius_bits & BIT_ID_8) || g.has_flywheel);
    if (g.genius_state == GENIUS_UNKNOWN && !carries_flywheel) return;
    if (is_survivor) {
        if (g.genius_bits & BIT_ID_32) strncat(o.pre, "双弹", sizeof(o.pre) - strlen(o.pre) - 1);
        if (carries_flywheel) {
            if (flywheel_ready(g)) { snprintf(o.flywheel, sizeof(o.flywheel), "飞轮"); o.flywheel_ready = true; }
            else if (g.has_flywheel) snprintf(o.flywheel, sizeof(o.flywheel), "飞轮%.0fs", extrapolate(g.flywheel_remain, g.flywheel_read_ms, g.flywheel_rate));
            else               snprintf(o.flywheel, sizeof(o.flywheel), "飞轮");
        }
        if (g.genius_bits & BIT_ID_24) strncat(o.post, "搏命", sizeof(o.post) - strlen(o.post) - 1);
        if (g.genius_bits & BIT_ID_16) strncat(o.post, "大心脏", sizeof(o.post) - strlen(o.post) - 1);
    } else {
        if (g.genius_bits & BIT_ID_8)   strncat(o.pre, "封窗", sizeof(o.pre) - strlen(o.pre) - 1);
        if (g.genius_bits & BIT_ID_16) strncat(o.pre, "底牌", sizeof(o.pre) - strlen(o.pre) - 1);
        if (g.genius_bits & BIT_ID_32) strncat(o.pre, "张狂", sizeof(o.pre) - strlen(o.pre) - 1);
        if (g.genius_bits & BIT_ID_24) strncat(o.pre, "挽留", sizeof(o.pre) - strlen(o.pre) - 1);
    }
    if (g.genius_state != GENIUS_COMPLETE) strncat(o.post, "?", sizeof(o.post) - strlen(o.post) - 1);
    else if (!o.pre[0] && !o.flywheel[0] && !o.post[0]) snprintf(o.pre, sizeof(o.pre), "无");
}

static void trait_line_text(const Info &g, const char *name, char *buf, size_t cap)
{
    if (!name || !name[0]) { buf[0] = '\0'; return; }
    if (!g.has_cd) { snprintf(buf, cap, "%s", name); return; }   // 读不到冷却就只写名字
    if (g.charge_max > 0) {                                      // 充能型
        // 满层时 _cd_delta 的表现没验过，按读到的层数判满层；层数本身不推算，等下一轮读取
        if (g.charge_cur >= g.charge_max) snprintf(buf, cap, "%s %d/%d", name, g.charge_cur, g.charge_max);
        else snprintf(buf, cap, "%s %d/%d %.1fs", name, g.charge_cur, g.charge_max, cd_remain_now(g));
        return;
    }
    if (cd_ready(g)) snprintf(buf, cap, "%s 就绪", name);
    else           snprintf(buf, cap, "%s %.1fs", name, cd_remain_now(g));
}

} // namespace 天赋

#endif // IDV_PY_GENIUS_H
