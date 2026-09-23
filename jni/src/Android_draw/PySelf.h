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
#include <atomic>
#include <unistd.h>
#include "PyRoot.h"

// 命名空间跟文件同名(PySelf)；draw_Gui.cpp 里另有全局变量 `uintptr_t self_obj`，别起成 self_obj 之类重名
namespace PySelf {

// sys.modules 与 dict/int/True 由 PyRoot.h 运行时找出，这里不再写死模块偏移

static const int MAX_STALE = 32;
static const int MAX_BODIES = 32;

// 要扫的单位类型：1=监管(巡视者也在这里) 2=求生者(机械玩偶、幻灯师分身也在这里) 236=梦之信徒
// 1065=伊斯人幻影(yith_ghost03，2026-09-22 实测；漏了它时幻影被本体集合挡掉)。
// 局中召出来的从属单位常有独立的键，某个召唤物被误挡时先用 _scripts\cmd_phase.py 查它在哪个键下
static const int UNIT_TYPES[] = {1, 2, 236, 1065};
static const int UNIT_TYPE_COUNT = sizeof(UNIT_TYPES) / sizeof(UNIT_TYPES[0]);

// ---- 本体集合：哪些场景对象是"角色本体"(2026-09-22 实测) ----
// 场景数组里跟角色同名、或者挂在角色身上的对象很多：_fragrance_image(每个求生者一份、同名、恒不可见)、
// another_model(另一形态)、balloon_model/umbrella_model(挂件)、item_lst[i].model(手持道具)、
// 时装挂件、约瑟夫相机……类名和 +0x240/+0x6D 都分不开(另一形态、气球跟本体一样是 2 / 0x50)。
// 唯一干净的定义：单位类型表里各键下每个单位的 unit.model + 0x20。
//   - 魔术师分身是 CloneUnit，在独立的键 [17] 里，不在这张表里，自然排除
//   - 幻灯师分身 SlideManCloneUnit 却在 [2] 里，靠 is_civilian_puppet == True 排除
//     (它 is_clone=False、is_puppet=False，这两个名字都骗人，别用)
//   - 机械玩偶 MyCivilianPuppetUnit 在 [2] 里、is_civilian_puppet=False，**保留**(用户要画它)
//   - owner_uid 别用：机械玩偶实测为 None，可能只在被操控时才有值
struct Snapshot {
    uint64_t anchor;                  // 当前操控单位的场景对象，0 = 没读到
    int      camp;                  // 1=监管 2=求生者 其它=从属单位的 unit_type
    int64_t  uid;
    uint64_t stale[MAX_STALE];
    int      stale_count;
    uint64_t body[MAX_BODIES];      // 本体集合，见上
    int      body_count;
    uint64_t hunter_body;              // [1] 里第一个类名不含 Puppet 的单位(跳过巡视者)，0 = 没有
};

// 双缓冲发布：写线程填非活跃缓冲，填完再翻转，绘制线程永远读到完整的一份
static Snapshot g_buf[2];
static std::atomic<int> g_active{0};      // 见 PyProgress.h 同名变量的注释(volatile 不保证发布顺序)

static uintptr_t g_libbase = 0;
static uint64_t  g_int_type = 0, g_dict_type = 0, g_true = 0;
static uint64_t  g_module_dict = 0;
static int64_t   g_i_cam = -1, g_i_cam_unit = -1;
static int64_t   g_i_unit_mgr = -1, g_i_ubt = -1;
static int64_t   g_i_model = -1, g_i_another = -1, g_i_utype = -1, g_i_uid = -1;
// another_model 的序号缓存**按类分开存**(键 = 实例的 ob_type)。
// 各玩家类(ButcherUnit / CivilianUnit / MyCivilianUnit / 各种 Puppet ...)实例字典大小不同，
// another_model 的序号也不同；共用一个缓存时，遍历每换一个类就失效一次、从头按名字扫到它
// (字典上千条，每条 2 次读取)，一轮要扫 2~4 次，占了这个线程九成的读取量。
// 分开存之后每个类只在第一次出现时扫一次。缓存从不被直接信任：resolve_index() 每次都按名字核对，
// 核对不上就重扫，所以最坏情况就是退回共用缓存那种多扫几遍，不会读错属性。
// 表满了(类比这个多)就退回共用的 g_i_*。model / is_civilian_puppet 同理，一起按类存。
struct ClassIndexCache { uint64_t type; int64_t i_another; int64_t i_model; int64_t i_cpuppet; bool is_puppet; };
static const int MAX_CLASSES = 8;
static ClassIndexCache g_class_cache[MAX_CLASSES];
static int     g_class_cache_count = 0;
static int64_t g_i_unit_model = -1, g_i_cpuppet = -1;   // 表满时的共用缓存

static volatile bool g_available = false;
// 局外(大厅/准备阶段)：units_by_type 完整读出来了、而且没有键 1/2。
// 准备阶段 cam.unit 是 MyCityUnit，它的 model+0x20 读不出场景指针(2026-09-22 实测)，锚点那步会失败，
// 所以这个标志不挂在 快照/g_available 上，在锚点之前单独算、单独发布。读不全一律算"不是局外"
static volatile bool g_in_lobby = false;
static volatile int  g_fail_streak = 0;
static char g_status[96] = "未启动";

static inline bool is_obj(uint64_t p) { return p > 0x7000000000ULL && p < 0x8000000000ULL; }

// PyASCIIObject: 长度在 +0x10，紧凑 ASCII 数据在 +0x30
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
    out.entries = keys + 32 + ((uint64_t)1 << idxb);
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

// managed-dict 实例：dict 指针在对象前 0x18。多校验一次 ob_type 是 dict，
// 免得把别的布局的对象当实例用(g_cam_ctrl 不一定跟 unit 同布局)
static uint64_t get_inst_dict(uint64_t obj)
{
    if (!is_obj(obj)) return 0;
    uint64_t d = getPtr64(obj - 0x18);
    if (!is_obj(d)) return 0;
    if (getPtr64(d + 8) != g_dict_type) return 0;
    return d;
}

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

// Python 对象 -> 它持有的场景对象指针(world.model + 0x20)
static uint64_t get_scene_obj(uint64_t model_obj)
{
    if (!is_obj(model_obj)) return 0;
    uint64_t sp = getPtr64(model_obj + 0x20);
    return is_obj(sp) ? sp : 0;
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
    g_module_dict = getPtr64(mod + 0x10);            // module.md_dict
    if (!is_obj(g_module_dict)) { g_module_dict = 0; snprintf(g_status, sizeof(g_status), "md_dict 无效"); return false; }
    return true;
}

// units_by_type 里找某个 int 键对应的 list
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

// 判断是不是局外。units_by_type 的每个键都要读成功才下结论 —— get_type_list() 返回 0 分不清
// "没有这个键"和"键对象所在页读不到"，拿它判会在对局中途的读失败里误判成局外、把人全藏掉。
// 局外 = 没有键 1(监管) 也没有键 2(求生者)。2026-09-22 准备阶段实测键只有 53/59/74/100/1009。
static bool check_lobby(uint64_t ubt, bool &in_lobby)
{
    Dict k;
    if (!read_dict(ubt, k) || k.n_entries <= 0) return false;
    bool has_player = false;
    for (int64_t i = 0; i < k.n_entries; i++) {
        uint64_t kp = getPtr64(key_slot(k, i));
        if (kp == 0) continue;                         // 已删除的条目
        if (!is_obj(kp) || getPtr64(kp + 8) != g_int_type) return false;
        int64_t sz = 0; uint32_t dg = 0;
        if (!vm_readv(kp + 16, &sz, 8) || !vm_readv(kp + 24, &dg, 4)) return false;
        if (sz == 1 && (dg == 1 || dg == 2)) has_player = true;
    }
    in_lobby = !has_player;
    return true;
}

static bool read_list(uint64_t L, int64_t &n, uint64_t &items)
{
    if (!is_obj(L)) return false;
    if (!vm_readv(L + 0x10, &n, 8)) return false;
    if (n < 0 || n > 256) return false;
    if (n == 0) { items = 0; return true; }
    items = getPtr64(L + 0x18);
    return is_obj(items);
}

// 类名(PyTypeObject.tp_name，+0x18 是 char*)里含不含 Puppet。
// 实测 [1] 里的巡视者是 MyButcherPatrolPuppetUnit，挑"监管本体"时要跳过它
static bool class_name_has_puppet(uint64_t type)
{
    uint64_t np = getPtr64(type + 0x18);
    if (!is_obj(np)) return false;
    char buf[48] = {0};                                // 最长的 MyButcherPatrolPuppetUnit 也才 25 字节
    if (!vm_readv(np, buf, sizeof(buf) - 1)) return false;
    return strstr(buf, "Puppet") != nullptr;
}

static inline void add_unique(uint64_t *tbl, int &cnt, int cap, uint64_t sp)
{
    for (int j = 0; j < cnt; j++) if (tbl[j] == sp) return;
    if (cnt < cap) tbl[cnt++] = sp;
}

// 扫一类单位：
//   another_model -> 废弃模型黑名单
//   model         -> 本体集合(幻灯师分身 is_civilian_puppet==True 不收)
//   [1] 里第一个类名不含 Puppet 的 -> 监管本体(预知监管用)
static void collect_units(uint64_t ubt, int type, Snapshot &res)
{
    uint64_t lst = get_type_list(ubt, type);
    int64_t n = 0; uint64_t items = 0;
    if (!read_list(lst, n, items) || n == 0) return;

    for (int64_t i = 0; i < n; i++) {
        uint64_t inst = getPtr64(items + i * 8);
        uint64_t d = get_inst_dict(inst);
        if (!d) continue;
        Dict dk;
        if (!read_dict(d, dk)) continue;

        // 按实例的类取序号缓存(见 g_class_cache)。类型指针读不到、或表满了就用共用缓存
        // 别叫 type：会遮住参数 type(单位类型键)，导致下面 type == 1 永远不成立、hunter_body 永远拿不到
        uint64_t cls = getPtr64(inst + 8);
        ClassIndexCache *slot = nullptr;
        if (is_obj(cls)) {
            for (int j = 0; j < g_class_cache_count; j++)
                if (g_class_cache[j].type == cls) { slot = &g_class_cache[j]; break; }
            if (!slot && g_class_cache_count < MAX_CLASSES) {
                slot = &g_class_cache[g_class_cache_count++];
                slot->type = cls;
                slot->i_another = slot->i_model = slot->i_cpuppet = -1;
                slot->is_puppet = class_name_has_puppet(cls);    // 类名一个类只读一次
            }
        }
        int64_t &i_another = slot ? slot->i_another : g_i_another;
        int64_t &i_model = slot ? slot->i_model : g_i_unit_model;
        int64_t &i_cpuppet = slot ? slot->i_cpuppet : g_i_cpuppet;

        // 实测所有玩家类都有 another_model(没有第二形态时值是 None)，查不到就跳过(不是错误)
        i_another = resolve_index(d, dk, i_another, "another_model");
        if (i_another >= 0) {
            uint64_t sp = get_scene_obj(getPtr64(value_slot(dk, i_another)));
            if (sp) add_unique(res.stale, res.stale_count, MAX_STALE, sp);
        }

        i_model = resolve_index(d, dk, i_model, "model");
        if (i_model < 0) continue;
        uint64_t body = get_scene_obj(getPtr64(value_slot(dk, i_model)));
        if (!body) continue;

        // 幻灯师分身：is_civilian_puppet 是 True 单例。属性不存在(监管类)或读不到都按"不是分身"
        i_cpuppet = resolve_index(d, dk, i_cpuppet, "is_civilian_puppet");
        if (i_cpuppet >= 0 && getPtr64(value_slot(dk, i_cpuppet)) == g_true) continue;

        add_unique(res.body, res.body_count, MAX_BODIES, body);
        if (type == 1 && res.hunter_body == 0 && !(slot ? slot->is_puppet : class_name_has_puppet(cls)))
            res.hunter_body = body;
    }
}

static bool try_refresh()
{
    if (g_module_dict == 0 && !resolve_module()) { g_in_lobby = false; return false; }

    Snapshot res;
    memset(&res, 0, sizeof(res));

    // ---- 零、units_by_type + 局外判定(必须在锚点之前，见 g_in_lobby) ----
    g_i_unit_mgr = find_key(g_module_dict, "unit_mgr", g_i_unit_mgr);
    uint64_t um = (g_i_unit_mgr >= 0) ? get_attr(g_module_dict, g_i_unit_mgr) : 0;
    uint64_t ud = get_inst_dict(um);
    uint64_t ubt = 0;
    if (ud) {
        g_i_ubt = find_key(ud, "units_by_type", g_i_ubt);
        ubt = (g_i_ubt >= 0) ? get_attr(ud, g_i_ubt) : 0;
    }
    bool in_lobby = false;
    g_in_lobby = is_obj(ubt) && check_lobby(ubt, in_lobby) && in_lobby;

    // ---- 一、当前操控单位：g_cam_ctrl.unit ----
    g_i_cam = find_key(g_module_dict, "g_cam_ctrl", g_i_cam);
    uint64_t cam = (g_i_cam >= 0) ? get_attr(g_module_dict, g_i_cam) : 0;
    if (!is_obj(cam)) { snprintf(g_status, sizeof(g_status), "g_cam_ctrl 无效(未在对局中?)"); return false; }

    uint64_t camd = get_inst_dict(cam);
    if (!camd) { snprintf(g_status, sizeof(g_status), "g_cam_ctrl 不是 managed-dict 布局"); return false; }
    Dict camk;
    if (!read_dict(camd, camk)) { snprintf(g_status, sizeof(g_status), "g_cam_ctrl 字典异常"); return false; }

    g_i_cam_unit = resolve_index(camd, camk, g_i_cam_unit, "unit");
    if (g_i_cam_unit < 0) { snprintf(g_status, sizeof(g_status), "g_cam_ctrl 里没有 unit"); return false; }
    uint64_t me = getPtr64(value_slot(camk, g_i_cam_unit));
    if (!is_obj(me)) { snprintf(g_status, sizeof(g_status), "cam.unit 为空"); return false; }

    uint64_t me_d = get_inst_dict(me);
    if (!me_d) { snprintf(g_status, sizeof(g_status), "cam.unit 没有实例字典"); return false; }
    Dict me_k;
    if (!read_dict(me_d, me_k)) { snprintf(g_status, sizeof(g_status), "cam.unit 字典异常"); return false; }

    g_i_model = resolve_index(me_d, me_k, g_i_model, "model");
    g_i_utype = resolve_index(me_d, me_k, g_i_utype, "unit_type");
    g_i_uid   = resolve_index(me_d, me_k, g_i_uid,   "uid");

    if (g_i_model >= 0) res.anchor = get_scene_obj(getPtr64(value_slot(me_k, g_i_model)));
    if (g_i_utype >= 0) { int64_t v = 0; if (read_int(getPtr64(value_slot(me_k, g_i_utype)), v)) res.camp = (int)v; }
    if (g_i_uid   >= 0) { int64_t v = 0; if (read_int(getPtr64(value_slot(me_k, g_i_uid)),   v)) res.uid  = v; }

    if (res.anchor == 0) { snprintf(g_status, sizeof(g_status), "cam.unit.model 读不到"); return false; }

    // ---- 二、废弃模型黑名单 + 本体集合 + 监管本体 ----
    if (is_obj(ubt))
        for (int i = 0; i < UNIT_TYPE_COUNT; i++) collect_units(ubt, UNIT_TYPES[i], res);
    // 这一段读不到不算失败：锚点已经拿到了。本体数为 0 时绘制侧会退回按类名画

    int write_idx = 1 - g_active;
    g_buf[write_idx] = res;
    g_active = write_idx;                                   // 填完再翻转
    snprintf(g_status, sizeof(g_status), "正常 %s uid=%lld 本体%d 废弃%d",
             res.camp == 1 ? "监管" : (res.camp == 2 ? "求生" : "从属"),
             (long long)res.uid, res.body_count, res.stale_count);
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
    g_int_type = PyRoot::g_int_type; g_dict_type = PyRoot::g_dict_type; g_true = PyRoot::g_true;
    if (try_refresh()) { g_available = true; g_fail_streak = 0; return; }
    g_available = false;
    if (++g_fail_streak >= 8) {                       // 连续失败就把序号缓存作废，下一轮按名字重找
        g_module_dict = 0;
        g_i_cam = g_i_cam_unit = g_i_unit_mgr = g_i_ubt = -1;
        g_i_model = g_i_another = g_i_utype = g_i_uid = -1;
        g_i_unit_model = g_i_cpuppet = -1;
        g_class_cache_count = 0;
        g_fail_streak = 0;
    }
}

// 自身锚点每帧都要用，而且切换操控对象时要立刻跟上，所以比天赋刷得勤(约一帧 60fps)
static const int REFRESH_INTERVAL_MS = 16;

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
    printf("[自身] 已启动 libbase=0x%lx\n", (unsigned long)libbase);
    fflush(stdout);
}

// ---------------- 给绘制侧用的只读接口 ----------------

static inline bool available() { return g_available; }
static inline const char *status_text() { return g_status; }

// 当前操控单位的场景对象。返回 false 时绘制侧要退回相机深度启发式
static bool anchor(uint64_t &obj)
{
    if (!g_available) return false;
    uint64_t v = g_buf[g_active].anchor;
    if (v == 0) return false;
    obj = v;
    return true;
}

// 当前视角的阵营：1=监管 2=求生者。0=没读到。
// 注意操控从属单位时这里是从属的 unit_type(机械玩偶仍是 2、梦之信徒是 236)
static inline int camp() { return g_available ? g_buf[g_active].camp : 0; }
static inline int64_t self_uid() { return g_available ? g_buf[g_active].uid : 0; }

// 形态切换留下的废弃模型 —— 替掉红蝶/木偶师类名黑名单
static bool is_stale_model(uintptr_t obj)
{
    if (!g_available) return false;
    const Snapshot &s = g_buf[g_active];
    for (int i = 0; i < s.stale_count; i++) if (s.stale[i] == (uint64_t)obj) return true;
    return false;
}

// 大厅/准备阶段：Python 侧还没有任何玩家单位，场景里的 chr/player、chr/boss 对象(时装挂件、头饰、袖子)
// 全都没有宿主，绘制侧据此整类不画。读不全时是 false，照旧按类名画
static inline bool in_lobby() { return g_in_lobby; }

// 本体集合能不能用。准备阶段/大厅里 units_by_type 没有 [1]/[2](实测两次)，这里就是 false，
// 绘制侧据此退回按类名画(读取失败时同样是 false，所以"是否局外"要看 in_lobby()，别用它)
static inline bool body_set_ready() { return g_available && g_buf[g_active].body_count > 0; }

static bool is_body(uintptr_t obj)
{
    if (!g_available) return false;
    const Snapshot &s = g_buf[g_active];
    for (int i = 0; i < s.body_count; i++) if (s.body[i] == (uint64_t)obj) return true;
    return false;
}

// 局内监管本体的场景对象(跳过巡视者)。准备阶段没有 [1] 单位，返回 false
static bool hunter_body(uint64_t &obj)
{
    if (!g_available) return false;
    uint64_t v = g_buf[g_active].hunter_body;
    if (v == 0) return false;
    obj = v;
    return true;
}

} // namespace PySelf

#endif // IDV_PY_SELF_H
