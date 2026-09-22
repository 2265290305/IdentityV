#ifndef IDV_PY_GENIUS_H
#define IDV_PY_GENIUS_H

// ============================================================================
// 天赋与辅助特质 —— 走游戏内嵌 CPython 的对象图读取
// ============================================================================
//
// 链路跟 PyProgress.h 完全一样(sys.modules -> game_kernel -> unit_mgr ->
// units_by_type)，只是取的是 1(ButcherUnit) 和 2(CivilianUnit) 两个键，
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
//    ⚠ 注意这里的读法：本文件的 读天赋() 自己读 ob_size，能分清"真的空"和"读不到"。
//    PC 侧那些脚本用的 pyw.list_items() 把这两种情况都返回 []，**不能拿它判断列表是不是空的**。
//
//    **做法：按 uid 增量累积 + 二次确认**(见 读天赋())。天赋一局之内不会变，所以每轮能读到几条
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
#include <unistd.h>

namespace 天赋 {

// 模块相对偏移，跟 PyProgress.h 同源；热更后要一起改
static const uintptr_t OFF_SYS_MODULES = 0xA7029E8;
static const uintptr_t OFF_LONG_TYPE   = 0xA026498;
static const uintptr_t OFF_FLOAT_TYPE  = 0xA0336F0;
static const uintptr_t OFF_DICT_TYPE   = 0xA023BF0;
static const uintptr_t OFF_TRUE        = 0xA0261A0;   // True  单例(实测，顺序与直觉相反)
static const uintptr_t OFF_FALSE       = 0xA0261C0;   // False 单例

static const int 最大单位数 = 16;
static const int BUTCHER_UNIT_TYPE  = 1;
static const int CIVILIAN_UNIT_TYPE = 2;

// 绝处逢生的**被动技能 id**(不是天赋 id)。天赋 id 是 26，落地成被动是 102。
// unit.ability_used 是 dict{被动id: bool}，True = 本局已消耗。
// 2026-09-22 实测：非本机的 CivilianUnit 上读到 {102: True}，
// 说明服务器**会把别人的消耗状态下发给本机**，不是只有自己的能看。
// 这个属性只存在于求生者类上，监管(MyButcherUnit)身上根本没有。
static const int 被动_绝处逢生 = 102;

// 天赋 id -> 位号。只关心四个分支终端 + 26(求生者的绝处逢生)，其余忽略。
// 两个阵营共用这张 id->位 的映射，名字在格式化时才按阵营区分。
enum {
    位_八  = 1 << 0,   // id 8 : 求生=飞轮效应  监管=紧闭空间
    位_十六 = 1 << 1,  // id 16: 求生=回光返照  监管=底牌
    位_廿四 = 1 << 2,  // id 24: 求生=化险为夷  监管=挽留
    位_廿六 = 1 << 3,  // id 26: 求生=绝处逢生  监管=未知
    位_卅二 = 1 << 4,  // id 32: 求生=膝跳反射  监管=张狂
};

struct 信息 {
    int      阵营;       // 1=监管 2=求生
    uint32_t 天赋位;     // 只含**已确认**的条目
    int      辅助特质;   // 1..8，0=没读到
    int      天赋状态;   // 天赋_未知 / 天赋_部分 / 天赋_完整，见 读天赋()
    uint8_t  天赋等级[41]; // 已确认的 {天赋id: 等级}，0 = 未确认或没带。下标 1~40
    bool     绝处已用;   // ability_used[102] == True，本局已经触发过绝处逢生
    // 监管辅助特质的冷却(求生者侧全为 0)
    bool     有冷却;     // false = 没读到技能对象
    float    冷却剩余;   // 秒，0 = 就绪。**读到那一刻的值**，显示时用 推算剩余() 往下推
    int64_t  读到时刻;   // 现在毫秒() 时钟，配合 冷却剩余 做推算
    float    冷却速率;   // skill_mgr.cd_rate，实测恒 1；推算时乘上它
    float    冷却总长;   // cd_time，画进度环用
    int      充能最大;   // power_num，非充能型为 0
    int      充能当前;   // _cur_power_num
};

struct 条目 {
    uint64_t 场景对象;
    float    x, y;
    信息     info;
};

// 双缓冲发布：写线程填非活跃缓冲，填完再翻转，绘制线程永远读到完整的一份
static 条目 g_缓冲[2][最大单位数];
static volatile int g_计数[2] = {0, 0};
static volatile int g_活跃 = 0;

// 天赋锁存：genius_id_lv_lst 会间歇性读空(见文件头第 3 条)，按 uid 记住最后一次非空的结果
enum { 天赋_未知 = 0, 天赋_部分 = 1, 天赋_完整 = 2 };

// 每个玩家的天赋记忆(按 uid)，增量累积，见 读天赋()
struct 记忆项 {
    int64_t uid;
    int     n;              // genius_id_lv_lst 的长度(ob_size)，0 = 还没读到过
    int     确认数;
    uint8_t 等级[41];       // 已确认的等级，0 = 未确认
    uint8_t 候选[41];       // 读到过一次、等待第二次确认的等级
};
static 记忆项 g_记忆[最大单位数];
static int      g_记忆数 = 0;
static uint64_t g_上次unit_mgr = 0;    // 地址一变就说明换局了，此时才清记忆

static uintptr_t g_libbase = 0;
static uint64_t  g_整数类型 = 0, g_浮点类型 = 0, g_字典类型 = 0;
static uint64_t  g_真 = 0, g_假 = 0;
static uint64_t  g_模块字典 = 0;
static int64_t   g_i_unit_mgr = -1, g_i_ubt = -1;
static int64_t   g_i_genius = -1, g_i_support = -1, g_i_model = -1, g_i_pos = -1, g_i_uid = -1;
static int64_t   g_i_ability = -1;   // ability_used，只有求生者类上有
// 监管技能冷却链：unit.skill_mgr -> skill_dict{id: Skill} / leader_skills(set)
static int64_t   g_i_skillmgr = -1, g_i_skilldict = -1, g_i_cdrate = -1;
// Skill 对象上的属性(223 条)，各自一套缓存
static int64_t   g_i_cd = -1, g_i_cdtime = -1, g_i_power = -1, g_i_curpower = -1;

static void 清空记忆项(记忆项 &m, int64_t uid)
{
    memset(&m, 0, sizeof(m));
    m.uid = uid;
}

// 取某个 uid 的记忆，没有就新建；表满了返回 nullptr
static 记忆项 *取记忆(int64_t uid)
{
    for (int i = 0; i < g_记忆数; i++)
        if (g_记忆[i].uid == uid) return &g_记忆[i];
    if (g_记忆数 >= 最大单位数) return nullptr;
    清空记忆项(g_记忆[g_记忆数], uid);
    return &g_记忆[g_记忆数++];
}
static volatile bool g_可用 = false;
static volatile int  g_连续失败 = 0;
static char g_状态[96] = "未启动";

static inline bool 是对象(uint64_t p) { return p > 0x7000000000ULL && p < 0x8000000000ULL; }

// 读线程和绘制线程共用的单调时钟，冷却推算用
static inline int64_t 现在毫秒()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

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
    out.条目起 = keys + 32 + ((uint64_t)1 << idxb);   // 必须逐实例算，不能跨实例套用
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

// 读一个 PyLong。天赋 id / 等级 / 特质 id 都是小整数，恒为 int，不会变成 float
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

// 读 int/float 属性，带防竞态校验。值域上下界由调用方给：
// 冷却是 0~600 秒级(闪现 csv_cd_time 就有 150)，不能套进度那条 0~100 的闸。
// _cd_delta 每秒都在减，float 对象被频繁重新分配，所以"重读指针确认没变"这步不能省。
static bool 读浮点(uint64_t slot, float &out, float 下界, float 上界)
{
    for (int t = 0; t < 3; t++) {
        uint64_t vp = getPtr64(slot);
        if (!是对象(vp)) continue;
        uint8_t b[32];
        if (!vm_readv(vp, b, 32)) continue;
        if (getPtr64(slot) != vp) continue;       // 解引用期间被重新赋值了
        int64_t rc = 0; memcpy(&rc, b, 8);
        if (rc <= 0) continue;
        uint64_t tp = 0; memcpy(&tp, b + 8, 8); tp &= 0x00FFFFFFFFFFFFFFULL;
        double v;
        if (tp == g_浮点类型) {
            memcpy(&v, b + 16, 8);
        } else if (tp == g_整数类型) {
            int64_t sz = 0; memcpy(&sz, b + 16, 8);
            uint32_t dg = 0; memcpy(&dg, b + 24, 4);
            v = (sz == 0) ? 0.0 : (double)dg;
            if (sz < 0) v = -v;
        } else {
            continue;
        }
        if (!(v >= 下界 && v <= 上界)) continue;
        out = (float)v;
        return true;
    }
    return false;
}

// 取 list 的 (元素数, 元素数组)。空 list 也算成功(n=0)，因为"空"是有意义的状态
static bool 取列表(uint64_t L, int64_t &n, uint64_t &items)
{
    if (!是对象(L)) return false;
    if (!vm_readv(L + 0x10, &n, 8)) return false;
    if (n < 0 || n > 256) return false;
    if (n == 0) { items = 0; return true; }
    items = getPtr64(L + 0x18);
    return 是对象(items);
}

// 天赋 id 的合法等级。来自配表(genius-table.md 的 lv 列)，求生者和监管这组 id 完全相同：
// 层 2.2 和 3.1/3.2/3.3 的节点可加 1~3 级，其余(分支根、2.1/2.3、终端、33~40)只有 1 级。
// 用来挡垃圾：读到的 [id, 等级] 不在配表允许范围里就不认。
static bool 等级合法(int64_t id, int64_t lv)
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
static bool 读天赋(uint64_t 槽, 记忆项 &记)
{
    uint64_t lp = getPtr64(槽);
    int64_t n = 0; uint64_t items = 0;
    if (!取列表(lp, n, items)) return false;
    if (getPtr64(槽) != lp) return false;             // 解引用期间被重新赋值了
    if (n == 0) return true;
    if (n > 40) return false;                         // 天赋一共就 40 个，再多一定是垃圾

    if (记.n != 0 && 记.n != (int)n) 清空记忆项(记, 记.uid);
    记.n = (int)n;

    // 先把这一轮读到的合法条目收齐，再决定怎么合并
    uint8_t 本轮[41] = {0};
    int     本轮条数 = 0;
    for (int64_t i = 0; i < n; i++) {
        uint64_t pair = getPtr64(items + i * 8);
        int64_t m = 0; uint64_t pit = 0;
        if (!取列表(pair, m, pit) || m != 2) continue;  // 单个条目读不到就跳过，下一轮再补
        int64_t id = 0, lv = 0;
        if (!读整数(getPtr64(pit), id)) continue;
        if (!读整数(getPtr64(pit + 8), lv)) continue;
        if (!等级合法(id, lv)) continue;
        if (本轮[id]) continue;                         // 同一轮里重复出现，不算两次
        本轮[id] = (uint8_t)lv;
        本轮条数++;
    }

    // 一轮就读全(合法且不重复的条数 == n)：直接判完整，不等第二轮。
    // 天赋页是冷页，开局读一次后游戏不再碰，内存压力下会被压进 zram 且再也换不回来；
    // 页还热的时候一次拿下，比等两轮确认可靠得多。垃圾值要同时满足
    // "n 条全部是合法 [id, 等级]、id 互不重复"几乎不可能。
    // 跟已确认的记忆冲突，说明有一方是垃圾：整份重来，这一轮的值只当候选。
    if (本轮条数 == (int)n) {
        bool 冲突 = false;
        for (int id = 1; id <= 40; id++)
            if (记.等级[id] != 0 && 记.等级[id] != 本轮[id]) { 冲突 = true; break; }
        if (冲突) {
            清空记忆项(记, 记.uid);
            记.n = (int)n;
            memcpy(记.候选, 本轮, sizeof(记.候选));
        } else {
            memcpy(记.等级, 本轮, sizeof(记.等级));
            记.确认数 = (int)n;
        }
        return true;
    }

    // 没读全：逐条增量合并，两个不同轮次读到相同值才确认
    for (int id = 1; id <= 40; id++) {
        int lv = 本轮[id];
        if (lv == 0) continue;
        if (记.等级[id] != 0) continue;                 // 已确认，天赋一局不变
        if (记.候选[id] == (uint8_t)lv) {              // 两个不同轮次读到相同值 -> 确认
            记.等级[id] = (uint8_t)lv;
            记.确认数++;
        } else {
            记.候选[id] = (uint8_t)lv;
        }
    }
    // 确认的比列表还多，说明有垃圾混进来了，整份重来
    if (记.确认数 > 记.n) 清空记忆项(记, 记.uid);
    return true;
}

// 记忆 -> 显示用的位掩码 / 状态
static uint32_t 记忆位(const 记忆项 &记)
{
    uint32_t 位 = 0;
    if (记.等级[8])  位 |= 位_八;
    if (记.等级[16]) 位 |= 位_十六;
    if (记.等级[24]) 位 |= 位_廿四;
    if (记.等级[26]) 位 |= 位_廿六;
    if (记.等级[32]) 位 |= 位_卅二;
    return 位;
}

static int 记忆状态(const 记忆项 &记)
{
    if (记.确认数 == 0) return 天赋_未知;
    return (记.n > 0 && 记.确认数 == 记.n) ? 天赋_完整 : 天赋_部分;
}

// ability_used = dict{被动技能id: bool}。找键 102(绝处逢生)，值为 True 就是本局已消耗。
// 键不在表里 = 还没用过(实测没用过的人这个 dict 是空的 {})，所以"找不到"要返回 false 而不是失败。
static bool 读绝处已用(uint64_t 槽)
{
    uint64_t dp = getPtr64(槽);
    if (!是对象(dp) || getPtr64(dp + 8) != g_字典类型) return false;
    字典 k;
    if (!取字典(dp, k)) return false;
    for (int64_t i = 0; i < k.条数; i++) {
        int64_t id = 0;
        if (!读整数(getPtr64(键槽(k, i)), id) || id != 被动_绝处逢生) continue;
        uint64_t vp = getPtr64(值槽(k, i));
        if (vp == g_真) return true;
        if (vp == g_假) return false;
        // 单例比不上就按 PyLong 布局退化解，热更挪了单例地址也还能用
        uint8_t b[32];
        if (!vm_readv(vp, b, 32)) return false;
        int64_t sz = 0; memcpy(&sz, b + 16, 8);
        uint32_t dg = 0; memcpy(&dg, b + 24, 4);
        return sz != 0 && dg != 0;
    }
    return false;
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
    g_模块字典 = getPtr64(mod + 0x10);
    if (!是对象(g_模块字典)) { g_模块字典 = 0; snprintf(g_状态, sizeof(g_状态), "md_dict 无效"); return false; }
    return true;
}

// 在 units_by_type(int -> list) 里按键取表
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
static const int32_t 主技能_聆听[]   = {701};
static const int32_t 主技能_失常[]   = {751};
static const int32_t 主技能_兴奋[]   = {722};
static const int32_t 主技能_巡视者[] = {761, 763, 765, 3850, 9761, 10761, 11761, 12650, 13450, 13724, 13761, 14050,
                                      14559, 14738, 15159, 15359, 15569, 17761, 21761, 22761, 23761, 24761, 25761,
                                      26761, 27761, 28761, 29761, 30761, 31761, 32761, 34761, 35761, 125750, 128750,
                                      136850, 143750};
static const int32_t 主技能_传送[]   = {741, 3741, 3844, 8741, 9741, 10741, 12644, 13444, 13718, 13741, 14044, 14553,
                                      14732, 15153, 15353, 15563, 17741, 21741, 22741, 23741, 24741, 25741, 26741,
                                      27741, 28741, 29741, 30741, 31741, 32741, 34741, 35741, 125744, 128744, 136844,
                                      143744};
static const int32_t 主技能_窥视者[] = {732, 735, 738, 3841, 9732, 10732, 11732, 12641, 13441, 13715, 13732, 14041,
                                      14550, 14729, 15150, 15350, 15560, 17732, 21732, 22732, 23732, 24732, 25732,
                                      26732, 27732, 28732, 29732, 30732, 31732, 32732, 34732, 35732, 125741, 128741,
                                      136841, 143741};
static const int32_t 主技能_闪现[]   = {711};
static const int32_t 主技能_移形[]   = {8802, 8805, 8808, 8811, 8814, 8817, 8820, 8823, 8826, 8829, 8832, 8835, 8838,
                                      8841, 8844, 8847, 8850, 8853, 8856, 8859, 8862, 8865, 8868, 8871, 8874, 8877,
                                      8880, 8883, 8886, 8890, 8893, 8896, 14562, 14742, 15162, 15362, 15572, 143753};

#define 取表(a) do { p = a; n = (int)(sizeof(a) / sizeof(a[0])); } while (0)
static bool 是特质主技能(int 特质, int64_t sid)
{
    const int32_t *p = nullptr; int n = 0;
    switch (特质) {
        case 1: 取表(主技能_聆听);   break;
        case 2: 取表(主技能_失常);   break;
        case 3: 取表(主技能_兴奋);   break;
        case 4: 取表(主技能_巡视者); break;
        case 5: 取表(主技能_传送);   break;
        case 6: 取表(主技能_窥视者); break;
        case 7: 取表(主技能_闪现);   break;
        case 8: 取表(主技能_移形);   break;
        default: return false;
    }
    for (int i = 0; i < n; i++) if (p[i] == sid) return true;
    return false;
}

static void 收监管技能(uint64_t d, const 字典 &dk, int 特质, 信息 &出)
{
    if (特质 < 1 || 特质 > 8) return;
    g_i_skillmgr = 校准序号(d, dk, g_i_skillmgr, "skill_mgr");
    if (g_i_skillmgr < 0) return;
    uint64_t sm = getPtr64(值槽(dk, g_i_skillmgr));
    if (!是对象(sm)) return;
    uint64_t smd = getPtr64(sm - 0x18);
    字典 smk;
    if (!取字典(smd, smk)) return;

    g_i_skilldict = 校准序号(smd, smk, g_i_skilldict, "skill_dict");
    if (g_i_skilldict < 0) return;
    // cd_rate 实测恒为 int 1，但名字说明它能变(加速冷却类效果)。读不到就按 1
    float 速率 = 1.f;
    g_i_cdrate = 校准序号(smd, smk, g_i_cdrate, "cd_rate");
    if (g_i_cdrate >= 0 && !读浮点(值槽(smk, g_i_cdrate), 速率, 0.01f, 10.f)) 速率 = 1.f;
    uint64_t sd = getPtr64(值槽(smk, g_i_skilldict));
    字典 sdk;
    if (!是对象(sd) || getPtr64(sd + 8) != g_字典类型 || !取字典(sd, sdk)) return;

    for (int64_t i = 0; i < sdk.条数; i++) {
        int64_t sid = 0;
        if (!读整数(getPtr64(键槽(sdk, i)), sid)) continue;
        if (!是特质主技能(特质, sid)) continue;          // 先按 id 筛，只解那一个技能对象
        uint64_t sk = getPtr64(值槽(sdk, i));
        if (!是对象(sk)) continue;
        uint64_t skd = getPtr64(sk - 0x18);
        字典 skk;
        if (!取字典(skd, skk)) continue;

        g_i_cd       = 校准序号(skd, skk, g_i_cd,       "_cd_delta");
        g_i_cdtime   = 校准序号(skd, skk, g_i_cdtime,   "cd_time");
        g_i_power    = 校准序号(skd, skk, g_i_power,    "power_num");
        g_i_curpower = 校准序号(skd, skk, g_i_curpower, "_cur_power_num");
        if (g_i_cd < 0) return;

        float cd = 0.f;
        if (!读浮点(值槽(skk, g_i_cd), cd, -0.5f, 600.0f)) return;   // 读失败就不显示冷却，不猜
        出.冷却剩余 = cd;
        出.读到时刻 = 现在毫秒();
        出.冷却速率 = 速率;
        出.有冷却   = true;
        if (g_i_cdtime >= 0) 读浮点(值槽(skk, g_i_cdtime), 出.冷却总长, 0.f, 600.f);
        int64_t v = 0;
        if (g_i_power    >= 0 && 读整数(getPtr64(值槽(skk, g_i_power)),    v) && v > 0 && v < 16) 出.充能最大 = (int)v;
        if (g_i_curpower >= 0 && 读整数(getPtr64(值槽(skk, g_i_curpower)), v) && v >= 0 && v < 16) 出.充能当前 = (int)v;
        return;                                        // 一个监管只挂一个主技能
    }
}

static int 收一类(uint64_t ubt, int 类型, int 写, int 个数)
{
    uint64_t 列表 = 取分类表(ubt, 类型);
    int64_t n = 0; uint64_t items = 0;
    if (!取列表(列表, n, items) || n == 0) return 个数;

    for (int64_t i = 0; i < n && 个数 < 最大单位数; i++) {
        uint64_t inst = getPtr64(items + i * 8);
        if (!是对象(inst)) continue;
        uint64_t d = getPtr64(inst - 0x18);           // managed-dict 在对象前 0x18
        字典 dk;
        if (!取字典(d, dk)) continue;

        g_i_genius  = 校准序号(d, dk, g_i_genius,  "genius_id_lv_lst");
        g_i_support = 校准序号(d, dk, g_i_support, "support_skill_id");
        g_i_model   = 校准序号(d, dk, g_i_model,   "model");
        g_i_pos     = 校准序号(d, dk, g_i_pos,     "position");
        g_i_uid     = 校准序号(d, dk, g_i_uid,     "uid");
        // 只在求生者类上找 ability_used：监管类上没这个属性，每个单位白扫一遍上千条不值
        if (类型 == CIVILIAN_UNIT_TYPE)
            g_i_ability = 校准序号(d, dk, g_i_ability, "ability_used");
        if (g_i_genius < 0) continue;

        int64_t uid = 0;
        bool 有uid = (g_i_uid >= 0) && 读整数(getPtr64(值槽(dk, g_i_uid)), uid);

        // 天赋按 uid 增量累积(见 读天赋())。这一轮 uid 读不到时天赋状态记为未知(只影响这一轮的显示)，
        // 记忆本身不动，下一轮读到 uid 会接着用
        记忆项 *记 = 有uid ? 取记忆(uid) : nullptr;
        if (记) 读天赋(值槽(dk, g_i_genius), *记);

        int 特质 = 0;
        if (g_i_support >= 0) {
            uint64_t sp = getPtr64(值槽(dk, g_i_support));
            int64_t m = 0; uint64_t sit = 0;
            if (取列表(sp, m, sit) && m >= 1) {
                int64_t v = 0;
                if (读整数(getPtr64(sit), v) && v >= 1 && v <= 8) 特质 = (int)v;
            }
        }

        uint64_t 场景 = 0;
        if (g_i_model >= 0) {
            uint64_t mo = getPtr64(值槽(dk, g_i_model));
            if (是对象(mo)) {
                uint64_t sp2 = getPtr64(mo + 0x20);
                if (是对象(sp2)) 场景 = sp2;
            }
        }
        float xyz[3] = {0, 0, 0};
        if (g_i_pos >= 0) {
            uint64_t pos = getPtr64(值槽(dk, g_i_pos));
            if (是对象(pos)) vm_readv(pos + 0x10, xyz, 12);
        }

        g_缓冲[写][个数].场景对象 = 场景;
        g_缓冲[写][个数].x = xyz[0];
        g_缓冲[写][个数].y = xyz[2];
        bool 已用 = false;
        if (类型 == CIVILIAN_UNIT_TYPE && g_i_ability >= 0)
            已用 = 读绝处已用(值槽(dk, g_i_ability));

        信息 inf;
        memset(&inf, 0, sizeof(inf));              // 冷却那几项求生者侧不填，必须先清零
        inf.阵营     = 类型;
        inf.辅助特质 = 特质;
        inf.绝处已用 = 已用;
        if (记) {
            inf.天赋位   = 记忆位(*记);
            inf.天赋状态 = 记忆状态(*记);
            memcpy(inf.天赋等级, 记->等级, sizeof(inf.天赋等级));
        }
        if (类型 == BUTCHER_UNIT_TYPE) 收监管技能(d, dk, 特质, inf);

        g_缓冲[写][个数].info = inf;
        个数++;
    }
    return 个数;
}

static bool 尝试刷新()
{
    if (g_模块字典 == 0 && !解析模块()) return false;

    g_i_unit_mgr = 查名(g_模块字典, "unit_mgr", g_i_unit_mgr);
    uint64_t um = (g_i_unit_mgr >= 0) ? 取属性(g_模块字典, g_i_unit_mgr) : 0;
    if (!是对象(um)) { snprintf(g_状态, sizeof(g_状态), "unit_mgr 无效(未在对局中?)"); return false; }

    // 换局了：uid 会从 1000001 重新开始，上一局的天赋记忆必须作废，否则会张冠李戴。
    // 注意只在**拿到有效新地址**时比较；um 无效时不动记忆，免得对局中途的
    // 短暂读失败把记忆误清。
    if (g_上次unit_mgr != 0 && um != g_上次unit_mgr) g_记忆数 = 0;
    g_上次unit_mgr = um;

    uint64_t ud = getPtr64(um - 0x18);
    g_i_ubt = 查名(ud, "units_by_type", g_i_ubt);
    uint64_t ubt = (g_i_ubt >= 0) ? 取属性(ud, g_i_ubt) : 0;
    if (!是对象(ubt)) { snprintf(g_状态, sizeof(g_状态), "units_by_type 无效"); return false; }

    int 写 = 1 - g_活跃, 个数 = 0;
    // 监管和求生者的属性序号缓存是共用的；两个类的插入顺序不同，
    // 所以每个单位都会走一次"按名字校验，不对就重扫"，多花的时间可以忽略
    个数 = 收一类(ubt, BUTCHER_UNIT_TYPE,  写, 个数);
    个数 = 收一类(ubt, CIVILIAN_UNIT_TYPE, 写, 个数);

    if (个数 == 0) { snprintf(g_状态, sizeof(g_状态), "一个单位也没读出"); return false; }
    g_计数[写] = 个数;
    g_活跃 = 写;
    snprintf(g_状态, sizeof(g_状态), "正常 %d 人", 个数);
    return true;
}

static void 刷新一次()
{
    if (g_libbase == 0) return;
    if (尝试刷新()) { g_可用 = true; g_连续失败 = 0; return; }
    g_可用 = false;
    if (++g_连续失败 >= 8) {                       // 连续失败就把序号缓存全部作废，下一轮重新按名字找
        g_模块字典 = 0;
        g_i_unit_mgr = g_i_ubt = -1;
        g_i_genius = g_i_support = g_i_model = g_i_pos = g_i_uid = g_i_ability = -1;
        g_i_skillmgr = g_i_skilldict = g_i_cdrate = -1;
        g_i_cd = g_i_cdtime = g_i_power = g_i_curpower = -1;
        g_连续失败 = 0;
        // 这里**不清天赋记忆**：对局中途链路短暂中断也会走到这里，清了会让天赋行白白消失。
        // 记忆只在 unit_mgr 地址变化(换局)时清，见 尝试刷新()
    }
}

// 天赋一局之内基本不变(只有监管带底牌换特质时会动)，不用像进度那样 100ms 一轮
static const int 刷新间隔毫秒 = 500;

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
    g_浮点类型 = libbase + OFF_FLOAT_TYPE;
    g_字典类型 = libbase + OFF_DICT_TYPE;
    g_真       = libbase + OFF_TRUE;
    g_假       = libbase + OFF_FALSE;
    snprintf(g_状态, sizeof(g_状态), "启动中");
    std::thread(线程体).detach();
    printf("[天赋] 已启动 libbase=0x%lx\n", (unsigned long)libbase);
    fflush(stdout);
}

// ---------------- 给绘制侧用的只读接口 ----------------

static inline bool 可用() { return g_可用; }
static inline const char *状态文本() { return g_状态; }

// 配对规则跟 PyProgress.h 一致：先用 model+0x20 的指针身份，读不到再退回坐标近邻。
// 人物是移动的，坐标兜底给的阈值比密码机那边紧，宁可配不上也不要配错。
static bool 查询(uintptr_t obj, float x, float y, 信息 &out)
{
    if (!g_可用) return false;
    int b = g_活跃, n = g_计数[b];
    for (int i = 0; i < n; i++) {
        if (g_缓冲[b][i].场景对象 == (uint64_t)obj) { out = g_缓冲[b][i].info; return true; }
    }
    int 最近 = -1; float 最小 = 1.0f;
    for (int i = 0; i < n; i++) {
        if (g_缓冲[b][i].场景对象 != 0) continue;
        if (g_缓冲[b][i].x == 0.f && g_缓冲[b][i].y == 0.f) continue;
        float d = fabsf(g_缓冲[b][i].x - x) + fabsf(g_缓冲[b][i].y - y);
        if (d < 最小) { 最小 = d; 最近 = i; }
    }
    if (最近 < 0) return false;
    out = g_缓冲[b][最近].info;
    return true;
}

// 绝处逢生只在求生者侧成立：id 26 在监管侧是另一个天赋，按 id 直接判会误标
static inline bool 有绝处逢生(const 信息 &g)
{
    return g.阵营 == CIVILIAN_UNIT_TYPE && (g.天赋位 & 位_廿六);
}

// 绝处逢生四态：没带 / 带了还没用 / 带了已经用掉 / 还不知道
// **"没带"只有在天赋表完整时才能断言**；不完整又还没确认到 26，就是"未知"，不能当成没带
enum { 绝处_无 = 0, 绝处_可用 = 1, 绝处_已用 = 2, 绝处_未知 = 3 };
static inline int 绝处状态(const 信息 &g)
{
    if (有绝处逢生(g)) return g.绝处已用 ? 绝处_已用 : 绝处_可用;
    return g.天赋状态 == 天赋_完整 ? 绝处_无 : 绝处_未知;
}

// 天赋简称行。求生者大心脏排最后，监管挽留排最后。写不出内容时 buf 为空串
//   未知：空串(不画)
//   部分：已确认的大天赋 + "?"，表示可能还有没读到的
//   完整：全部大天赋；一个大天赋都没带就写"无"
static void 天赋文本(const 信息 &g, char *buf, size_t cap)
{
    buf[0] = '\0';
    if (g.天赋状态 == 天赋_未知) return;
    char tmp[64]; tmp[0] = '\0';
    if (g.阵营 == CIVILIAN_UNIT_TYPE) {
        if (g.天赋位 & 位_卅二) strncat(tmp, "双弹", sizeof(tmp) - strlen(tmp) - 1);
        if (g.天赋位 & 位_八)   strncat(tmp, "飞轮", sizeof(tmp) - strlen(tmp) - 1);
        if (g.天赋位 & 位_廿四) strncat(tmp, "搏命", sizeof(tmp) - strlen(tmp) - 1);
        if (g.天赋位 & 位_十六) strncat(tmp, "大心脏", sizeof(tmp) - strlen(tmp) - 1);  // 固定排最后
    } else {
        if (g.天赋位 & 位_八)   strncat(tmp, "封窗", sizeof(tmp) - strlen(tmp) - 1);
        if (g.天赋位 & 位_十六) strncat(tmp, "底牌", sizeof(tmp) - strlen(tmp) - 1);
        if (g.天赋位 & 位_卅二) strncat(tmp, "张狂", sizeof(tmp) - strlen(tmp) - 1);
        if (g.天赋位 & 位_廿四) strncat(tmp, "挽留", sizeof(tmp) - strlen(tmp) - 1);    // 固定排最后
    }
    if (g.天赋状态 == 天赋_部分)      strncat(tmp, "?", sizeof(tmp) - strlen(tmp) - 1);
    else if (tmp[0] == '\0')          snprintf(tmp, sizeof(tmp), "无");
    snprintf(buf, cap, "%s", tmp);
}

// 监管辅助特质那一行的完整文本：特质名 + 冷却/充能。
//   普通技能   "闪现 12.7s" / "闪现 就绪"
//   充能型技能 "窥视者 2/3 20.9s"(还在攒下一层) / "窥视者 3/3"(满层)
// _cd_delta 不在冷却中时恒为 0，所以 0 直接当"就绪"用，不需要额外的标志位。
static void 特质行文本(const 信息 &g, const char *名, char *buf, size_t cap);

// 辅助特质名。1~8 连号，全表已确认
static const char *特质名(int id)
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
// 推到 0 以下只停在 0.0，**不自己判就绪** —— 就绪以真正读到的 0 为准(见 已就绪())，
// 否则底牌改写冷却、或者推算略快时会误报。下一轮读取会覆盖推算值，误差不超过一个读取周期。
static float 推算剩余(const 信息 &g)
{
    if (!g.有冷却 || g.冷却剩余 <= 0.05f) return 0.f;
    float 速率 = g.冷却速率 > 0.f ? g.冷却速率 : 1.f;
    float r = g.冷却剩余 - (float)(现在毫秒() - g.读到时刻) / 1000.f * 速率;
    return r > 0.f ? r : 0.f;
}

// 就绪 = 真正读到 0(不是推算到 0)
static inline bool 已就绪(const 信息 &g) { return g.有冷却 && g.冷却剩余 <= 0.05f; }

static void 特质行文本(const 信息 &g, const char *名, char *buf, size_t cap)
{
    if (!名 || !名[0]) { buf[0] = '\0'; return; }
    if (!g.有冷却) { snprintf(buf, cap, "%s", 名); return; }   // 读不到冷却就只写名字
    if (g.充能最大 > 0) {                                      // 充能型
        // 满层时 _cd_delta 的表现没验过，按读到的层数判满层；层数本身不推算，等下一轮读取
        if (g.充能当前 >= g.充能最大) snprintf(buf, cap, "%s %d/%d", 名, g.充能当前, g.充能最大);
        else snprintf(buf, cap, "%s %d/%d %.1fs", 名, g.充能当前, g.充能最大, 推算剩余(g));
        return;
    }
    if (已就绪(g)) snprintf(buf, cap, "%s 就绪", 名);
    else           snprintf(buf, cap, "%s %.1fs", 名, 推算剩余(g));
}

} // namespace 天赋

#endif // IDV_PY_GENIUS_H
