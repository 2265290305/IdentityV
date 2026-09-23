#ifndef A_NATIVE_WINDOW_CREATOR_H // !A_NATIVE_WINDOW_CREATOR_H
#define A_NATIVE_WINDOW_CREATOR_H

#include <android/native_window.h>
#include <android/log.h>
#include <dlfcn.h>
#include <sys/system_properties.h>

#include <cstddef>
#include <cstdio>
#include <climits>
#include <cstring>   // strncmp/strstr：dynsym 特征匹配用
#include <fcntl.h>   // 以下四个：读 libgui.so 的 .dynsym 做符号自愈
#include <sys/stat.h>
#include <sys/mman.h>
#include <unistd.h>
#include <elf.h>
#include <unordered_map>
#include <string>
#include <vector>

// 统一日志出口：logcat + stderr。
// main.cpp 已把 stderr 重定向到 /data/local/tmp/hack_stderr.log，
// 所以没有 adb 的机器也能直接取日志定位“窗口没出来”的原因。
#define ANWC_LOG_TAG "ImGui"
#define ANWC_LOGE(...)                                                          \
    do                                                                          \
    {                                                                           \
        __android_log_print(ANDROID_LOG_ERROR, ANWC_LOG_TAG, __VA_ARGS__);      \
        fprintf(stderr, "[ANWC][E] " __VA_ARGS__);                              \
        fputc('\n', stderr);                                                    \
    } while (0)
#define ANWC_LOGI(...)                                                          \
    do                                                                          \
    {                                                                           \
        __android_log_print(ANDROID_LOG_INFO, ANWC_LOG_TAG, __VA_ARGS__);       \
        fprintf(stderr, "[ANWC][I] " __VA_ARGS__);                              \
        fputc('\n', stderr);                                                    \
    } while (0)

#define ResolveMethod(ClassName, MethodName, Handle, MethodSignature)                                                                    \
    ClassName##__##MethodName = reinterpret_cast<decltype(ClassName##__##MethodName)>(symbolMethod.Find(Handle, MethodSignature));       \
    if (nullptr == ClassName##__##MethodName)                                                                                            \
    {                                                                                                                                    \
        ANWC_LOGE("[-] Method not found: %s -> %s::%s", MethodSignature, #ClassName, #MethodName);                                       \
    }

// 可选符号：解析不到是正常的（例如老签名只存在于旧系统），不打错误日志。
#define ResolveOptionalMethod(ClassName, MethodName, Handle, MethodSignature)                                                             \
    ClassName##__##MethodName = reinterpret_cast<decltype(ClassName##__##MethodName)>(symbolMethod.Find(Handle, MethodSignature));

namespace android
{
    namespace detail
    {
        // -----------------------------------------------------------------------
        // 符号自愈：精确 mangled 名找不到时，直接扫 .so 的 .dynsym 找结构等价的符号。
        //
        // 为什么需要它：Google 每改一次参数类型（LayerMetadata 换命名空间、PixelFormat
        // 变成具名枚举…），mangled 名就变一次，而 ro.build.version.release 只能读到大版本号，
        // 追不完（真机 A17 实测：14+ 的 gui::createSurface 精确名已不存在）。
        // 这里只负责「发现名字」，取地址仍交给 dlsym，所以不用自己处理重定位。
        // -----------------------------------------------------------------------

        // 列出 .so 里 prefix 开头的符号名（可选：必须含全部子串、必须以某串结尾）
        static inline std::vector<std::string> ListDynSymbols(const char *libPath, const char *prefix,
                                                              const char *const *mustContain = nullptr,
                                                              size_t mustContainCount = 0,
                                                              const char *mustEndWith = nullptr)
        {
            std::vector<std::string> symbols;

            if (nullptr == libPath || nullptr == prefix)
                return symbols;

            int fd = open(libPath, O_RDONLY);
            if (0 > fd)
                return symbols;

            struct stat fileInfo{};
            if (0 != fstat(fd, &fileInfo) || fileInfo.st_size < static_cast<off_t>(sizeof(Elf64_Ehdr)))
            {
                close(fd);
                return symbols;
            }

            void *mapped = mmap(nullptr, fileInfo.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
            close(fd);
            if (MAP_FAILED == mapped)
                return symbols;

            do
            {
                auto *elfHeader = reinterpret_cast<const Elf64_Ehdr *>(mapped);
                if (0 != memcmp(elfHeader->e_ident, ELFMAG, SELFMAG) || ELFCLASS64 != elfHeader->e_ident[EI_CLASS])
                    break;
                if (0 == elfHeader->e_shoff || 0 == elfHeader->e_shnum)
                    break;

                auto *sectionHeaders = reinterpret_cast<const Elf64_Shdr *>(static_cast<const char *>(mapped) + elfHeader->e_shoff);
                const Elf64_Shdr *dynSym = nullptr;
                for (int i = 0; i < elfHeader->e_shnum; ++i)
                {
                    if (SHT_DYNSYM == sectionHeaders[i].sh_type)
                    {
                        dynSym = &sectionHeaders[i];
                        break;
                    }
                }

                if (nullptr == dynSym || dynSym->sh_link >= elfHeader->e_shnum)
                    break;

                const Elf64_Shdr *dynStr = &sectionHeaders[dynSym->sh_link];
                const char *stringTable = static_cast<const char *>(mapped) + dynStr->sh_offset;
                auto *entries = reinterpret_cast<const Elf64_Sym *>(static_cast<const char *>(mapped) + dynSym->sh_offset);
                const size_t entryCount = dynSym->sh_size / sizeof(Elf64_Sym);
                const size_t prefixLength = strlen(prefix);
                const size_t suffixLength = (nullptr == mustEndWith) ? 0 : strlen(mustEndWith);

                for (size_t i = 0; i < entryCount; ++i)
                {
                    if (0 == entries[i].st_name || 0 == entries[i].st_value)
                        continue;

                    const int type = ELF64_ST_TYPE(entries[i].st_info);
                    if (STT_FUNC != type && STT_NOTYPE != type)
                        continue;

                    const char *name = stringTable + entries[i].st_name;
                    if (0 != strncmp(name, prefix, prefixLength))
                        continue;

                    const size_t nameLength = strlen(name);
                    if (0 != suffixLength)
                    {
                        if (nameLength < suffixLength || 0 != strcmp(name + nameLength - suffixLength, mustEndWith))
                            continue;
                    }

                    bool matched = true;
                    for (size_t k = 0; k < mustContainCount && matched; ++k)
                    {
                        if (nullptr == strstr(name, mustContain[k]))
                            matched = false;
                    }

                    if (matched)
                        symbols.emplace_back(name);
                }
            } while (false);

            munmap(mapped, fileInfo.st_size);
            return symbols;
        }

        // 形参必须与 Functionals::SurfaceComposerClient__CreateSurface 完全一致：
        //   (this, const String8&, uint32_t, uint32_t, 4字节, 4字节,
        //    const sp<IBinder>&, LayerMetadata 按值或 const&, uint32_t*)
        // 只放过「类型编码变了但 ABI 没变」的情况（枚举↔整型、LayerMetadata 按值↔const&、
        // 命名空间挪动）；参数个数/顺序一变就拒绝，并把候选打出来，绝不拿错符号去调。
        static inline bool AcceptCreateSurfaceVariant(const char *name)
        {
            static const char kString8[] = "RKNS_7String8E";
            static const char kParent[] = "RKNS_2spINS_7IBinderEEE";
            static const char kMetaGui[] = "NS_3gui13LayerMetadataE";
            static const char kMetaPlain[] = "NS_13LayerMetadataE";

            const char *cursor = strstr(name, kString8);
            if (nullptr == cursor)
                return false;
            cursor += strlen(kString8);

            // w/h 必须是 uint32_t；format/flags 允许 int 或 uint
            if (!('j' == cursor[0] && 'j' == cursor[1]))
                return false;
            if (!(('i' == cursor[2] || 'j' == cursor[2]) && ('i' == cursor[3] || 'j' == cursor[3])))
                return false;
            cursor += 4;

            if (0 != strncmp(cursor, kParent, strlen(kParent)))
                return false;
            cursor += strlen(kParent);

            if ('R' == cursor[0] && 'K' == cursor[1])
                cursor += 2; // const LayerMetadata& 形式

            const size_t guiLength = strlen(kMetaGui);
            const size_t plainLength = strlen(kMetaPlain);
            if (0 == strncmp(cursor, kMetaGui, guiLength))
                cursor += guiLength;
            else if (0 == strncmp(cursor, kMetaPlain, plainLength))
                cursor += plainLength;
            else
                return false;

            return 0 == strcmp(cursor, "Pj");
        }

        // LayerMetadata 构造：android::LayerMetadata 与 android::gui::LayerMetadata
        // 的构造 ABI 完全相同（只收 this），两者都接受。
        static inline bool AcceptLayerMetadataCtor(const char *name)
        {
            return nullptr != strstr(name, "13LayerMetadataC2Ev");
        }

        static inline bool AcceptLayerMetadataSetInt32(const char *name)
        {
            return nullptr != strstr(name, "13LayerMetadata8setInt32Eji");
        }

        // 在 patch 表也失败/为空时，再按 ABI 等价特征从 .dynsym 里找符号。
        // 必须在此之前先 Close libutils，否则 Close libgui 会失败（关联的懒打开库尚未加载）。
        template <typename Fn, typename Method>
        static inline void ResolveByDynSymPattern(Fn &slot, const Method &symbolMethod,
                                                  void *libgui, const char *libPath, const char *prefix,
                                                  bool (*accept)(const char *) = nullptr,
                                                  const char *what = nullptr,
                                                  const char *mustContain = nullptr,
                                                  const char *mustEndWith = nullptr)
        {
            if (nullptr != slot)
                return;

            const char *const kMustContain[] = {mustContain};
            const size_t kMustContainCount = (nullptr != mustContain) ? 1 : 0;

            auto candidates = ListDynSymbols(libPath, prefix, kMustContain, kMustContainCount, mustEndWith);
            if (candidates.empty())
                return;

            for (const auto &candidate : candidates)
            {
                if (nullptr != accept && !accept(candidate.c_str()))
                    continue;

                slot = reinterpret_cast<Fn>(symbolMethod.Find(libgui, candidate.c_str()));
                if (nullptr != slot)
                    break;
            }

            if (nullptr != slot)
                ANWC_LOGI("[+] %s resolved via dynsym: %s", what, candidates[0].c_str());
            else
                ANWC_LOGE("[-] %s: no ABI-compatible symbol found in %s", what, libPath);
        }
    } // namespace detail

    using ::ANativeWindow;

    namespace detail
    {
        namespace ui
        {
            // A LayerStack identifies a Z-ordered group of layers. A layer can only be associated to a single
            // LayerStack, but a LayerStack can be associated to multiple displays, mirroring the same content.
            struct LayerStack
            {
                uint32_t id = UINT32_MAX;
            };

            enum class Rotation
            {
                Rotation0 = 0,
                Rotation90 = 1,
                Rotation180 = 2,
                Rotation270 = 3
            };

            // A simple value type representing a two-dimensional size.
            struct Size
            {
                int32_t width = -1;
                int32_t height = -1;
            };

            // Transactional state of physical or virtual display. Note that libgui defines
            // android::DisplayState as a superset of android::ui::DisplayState.
            struct DisplayState
            {
                LayerStack layerStack;
                Rotation orientation = Rotation::Rotation0;
                Size layerStackSpaceRect;
            };

            typedef int64_t nsecs_t; // nano-seconds
            struct DisplayInfo
            {
                uint32_t w{0};
                uint32_t h{0};
                float xdpi{0};
                float ydpi{0};
                float fps{0};
                float density{0};
                uint8_t orientation{0};
                bool secure{false};
                nsecs_t appVsyncOffset{0};
                nsecs_t presentationDeadline{0};
                uint32_t viewportW{0};
                uint32_t viewportH{0};
            };

            enum class DisplayType
            {
                DisplayIdMain = 0,
                DisplayIdHdmi = 1
            };

            struct PhysicalDisplayId
            {
                uint64_t value;
            };
        }

        struct String8;

        struct LayerMetadata;

        struct Surface;

        struct SurfaceControl;

        struct SurfaceComposerClientTransaction;

        struct SurfaceComposerClient;

        template <typename any_t>
        struct StrongPointer
        {
            union
            {
                any_t *pointer;
                char padding[sizeof(std::max_align_t)];
            };

            inline any_t *operator->() const { return pointer; }
            inline any_t *get() const { return pointer; }
            inline explicit operator bool() const { return nullptr != pointer; }
        };

        struct Functionals
        {
            struct SymbolMethod
            {
                void *(*Open)(const char *filename, int flag) = nullptr;
                void *(*Find)(void *handle, const char *symbol) = nullptr;
                int (*Close)(void *handle) = nullptr;
            };

            size_t systemVersion = 13;

            void (*RefBase__IncStrong)(void *thiz, void *id) = nullptr;
            void (*RefBase__DecStrong)(void *thiz, void *id) = nullptr;

            void (*String8__Constructor)(void *thiz, const char *const data) = nullptr;
            void (*String8__Destructor)(void *thiz) = nullptr;

            void (*LayerMetadata__Constructor)(void *thiz) = nullptr;
            void (*LayerMetadata__setInt32)(void *thiz, uint32_t key, int32_t value) = nullptr;

            void (*SurfaceComposerClient__Constructor)(void *thiz) = nullptr;
            void (*SurfaceComposerClient__Destructor)(void *thiz) = nullptr;
            StrongPointer<void> (*SurfaceComposerClient__CreateSurface)(void *thiz, void *name, uint32_t w, uint32_t h, int32_t format, uint32_t flags, void *parentHandle, void *layerMetadata, uint32_t *outTransformHint) = nullptr;
            StrongPointer<void> (*SurfaceComposerClient__CreateSurface_and9)(void *thiz, void *name, uint32_t w, uint32_t h, int32_t format, uint32_t flags, void *parentHandle, int32_t windowType, int32_t ownerUid) = nullptr;
            StrongPointer<void> (*SurfaceComposerClient__GetInternalDisplayToken)() = nullptr;
            StrongPointer<void> (*SurfaceComposerClient__GetBuiltInDisplay)(ui::DisplayType type) = nullptr;
            int32_t (*SurfaceComposerClient__GetDisplayState)(StrongPointer<void> &display, ui::DisplayState *displayState) = nullptr;
            int32_t (*SurfaceComposerClient__GetDisplayInfo)(StrongPointer<void> &display, ui::DisplayInfo *displayInfo) = nullptr;
            std::vector<ui::PhysicalDisplayId> (*SurfaceComposerClient__GetPhysicalDisplayIds)() = nullptr;
            StrongPointer<void> (*SurfaceComposerClient__GetPhysicalDisplayToken)(ui::PhysicalDisplayId displayId) = nullptr;

            void (*SurfaceComposerClient__Transaction__Constructor)(void *thiz) = nullptr;
            void *(*SurfaceComposerClient__Transaction__SetLayer)(void *thiz, StrongPointer<void> &surfaceControl, int32_t z) = nullptr;
            void *(*SurfaceComposerClient__Transaction__SetMetadata)(void *thiz, StrongPointer<void> &surfaceControl, uint32_t key, void *parcel) = nullptr;
            void *(*SurfaceComposerClient__Transaction__SetTrustedOverlay)(void *thiz, StrongPointer<void> &surfaceControl, bool isTrustedOverlay) = nullptr;

            void *(*SurfaceComposerClient__Transaction__SetFlags)(void *thiz, StrongPointer<void> &surfaceControl, uint32_t flags, uint32_t mask) = nullptr;

            int32_t (*SurfaceComposerClient__Transaction__Apply)(void *thiz, bool synchronous, bool oneWay) = nullptr;
            // Android 12 及以下的 apply(bool synchronous)，只有 13+ 用 apply(bool, bool)。
            // 可选解析：缺少它不算错误，只是说明系统在新版本上。
            int32_t (*SurfaceComposerClient__Transaction__ApplySync)(void *thiz, bool synchronous) = nullptr;

            int32_t (*SurfaceControl__Validate)(void *thiz) = nullptr;
            StrongPointer<Surface> (*SurfaceControl__GetSurface)(void *thiz) = nullptr;
            void (*SurfaceControl__DisConnect)(void *thiz) = nullptr;

            Functionals(const SymbolMethod &symbolMethod)
            {
                std::string systemVersionString(128, 0);

                systemVersionString.resize(__system_property_get("ro.build.version.release", systemVersionString.data()));
                if (!systemVersionString.empty())
                    systemVersion = std::stoi(systemVersionString);

                if (9 > systemVersion)
                {
                    ANWC_LOGE("[-] Unsupported system version: %zu", systemVersion);
                    return;
                }

                ANWC_LOGI("[*] Android %zu, resolving libgui/libutils symbols", systemVersion);

                static std::unordered_map<size_t, std::unordered_map<void **, const char *>> patchesTable = {
                    {
                        16,
                        {
                            {reinterpret_cast<void **>(&LayerMetadata__Constructor), "_ZN7android3gui13LayerMetadataC2Ev"},
                            {reinterpret_cast<void **>(&SurfaceComposerClient__CreateSurface), "_ZN7android21SurfaceComposerClient13createSurfaceERKNS_7String8EjjiiRKNS_2spINS_7IBinderEEENS_3gui13LayerMetadataEPj"},
                            {reinterpret_cast<void **>(&LayerMetadata__setInt32), "_ZN7android3gui13LayerMetadata8setInt32Eji"},

                        },
                    },
                    {
                        15,
                        {
                            {reinterpret_cast<void **>(&LayerMetadata__Constructor), "_ZN7android3gui13LayerMetadataC2Ev"},
                            {reinterpret_cast<void **>(&SurfaceComposerClient__CreateSurface), "_ZN7android21SurfaceComposerClient13createSurfaceERKNS_7String8EjjiiRKNS_2spINS_7IBinderEEENS_3gui13LayerMetadataEPj"},
                            {reinterpret_cast<void **>(&LayerMetadata__setInt32), "_ZN7android3gui13LayerMetadata8setInt32Eji"},

                        },
                    },
                    {
                        14,
                        {
                            {reinterpret_cast<void **>(&LayerMetadata__Constructor), "_ZN7android3gui13LayerMetadataC2Ev"},
                            {reinterpret_cast<void **>(&SurfaceComposerClient__CreateSurface), "_ZN7android21SurfaceComposerClient13createSurfaceERKNS_7String8EjjiiRKNS_2spINS_7IBinderEEENS_3gui13LayerMetadataEPj"},
                            {reinterpret_cast<void **>(&LayerMetadata__setInt32), "_ZN7android3gui13LayerMetadata8setInt32Eji"},
                        },
                    },
                    {
                        12,
                        {
                            {reinterpret_cast<void **>(&SurfaceComposerClient__Transaction__Apply), "_ZN7android21SurfaceComposerClient11Transaction5applyEb"},
                        },
                    },
                    {
                        11,
                        {
                            {reinterpret_cast<void **>(&SurfaceComposerClient__CreateSurface), "_ZN7android21SurfaceComposerClient13createSurfaceERKNS_7String8EjjijPNS_14SurfaceControlENS_13LayerMetadataEPj"},
                            {reinterpret_cast<void **>(&SurfaceControl__GetSurface), "_ZNK7android14SurfaceControl10getSurfaceEv"},
                        },
                    },
                    {
                        10,
                        {
                            {reinterpret_cast<void **>(&SurfaceComposerClient__CreateSurface), "_ZN7android21SurfaceComposerClient13createSurfaceERKNS_7String8EjjijPNS_14SurfaceControlENS_13LayerMetadataE"},
                            {reinterpret_cast<void **>(&SurfaceControl__GetSurface), "_ZNK7android14SurfaceControl10getSurfaceEv"},
                        },
                    },
                    {
                        9,
                        {
                            {reinterpret_cast<void **>(&SurfaceComposerClient__CreateSurface_and9), "_ZN7android21SurfaceComposerClient13createSurfaceERKNS_7String8EjjijPNS_14SurfaceControlEii"},
                            {reinterpret_cast<void **>(&SurfaceComposerClient__GetBuiltInDisplay), "_ZN7android21SurfaceComposerClient17getBuiltInDisplayEi"},
                            {reinterpret_cast<void **>(&SurfaceControl__GetSurface), "_ZNK7android14SurfaceControl10getSurfaceEv"},
                        },
                    },
                };

#ifdef __LP64__
                auto libgui = symbolMethod.Open("/system/lib64/libgui.so", RTLD_LAZY);
                auto libutils = symbolMethod.Open("/system/lib64/libutils.so", RTLD_LAZY);
#else
                auto libgui = symbolMethod.Open("/system/lib/libgui.so", RTLD_LAZY);
                auto libutils = symbolMethod.Open("/system/lib/libutils.so", RTLD_LAZY);
#endif

                ResolveMethod(RefBase, IncStrong, libutils, "_ZNK7android7RefBase9incStrongEPKv");
                ResolveMethod(RefBase, DecStrong, libutils, "_ZNK7android7RefBase9decStrongEPKv");

                ResolveMethod(String8, Constructor, libutils, "_ZN7android7String8C2EPKc");
                ResolveMethod(String8, Destructor, libutils, "_ZN7android7String8D2Ev");

                // 下面这 5 个是「老版本才用」的符号（14+/11+ 分别改了名），在新系统上
                // 找不到属于正常情况，用可选解析避免刷屏假告警；真正用到时调用点会各自报错。
                ResolveOptionalMethod(LayerMetadata, Constructor, libgui, "_ZN7android13LayerMetadataC2Ev");
                ResolveOptionalMethod(LayerMetadata, setInt32, libgui, "_ZN7android13LayerMetadata8setInt32Eji");

                ResolveMethod(SurfaceComposerClient, Constructor, libgui, "_ZN7android21SurfaceComposerClientC2Ev");
                ResolveOptionalMethod(SurfaceComposerClient, CreateSurface, libgui, "_ZN7android21SurfaceComposerClient13createSurfaceERKNS_7String8EjjijRKNS_2spINS_7IBinderEEENS_13LayerMetadataEPj");
                ResolveOptionalMethod(SurfaceComposerClient, GetInternalDisplayToken, libgui, "_ZN7android21SurfaceComposerClient23getInternalDisplayTokenEv");
                ResolveMethod(SurfaceComposerClient, GetDisplayState, libgui, "_ZN7android21SurfaceComposerClient15getDisplayStateERKNS_2spINS_7IBinderEEEPNS_2ui12DisplayStateE");
                ResolveOptionalMethod(SurfaceComposerClient, GetDisplayInfo, libgui, "_ZN7android21SurfaceComposerClient14getDisplayInfoERKNS_2spINS_7IBinderEEEPNS_11DisplayInfoE");
                ResolveMethod(SurfaceComposerClient, GetPhysicalDisplayIds, libgui, "_ZN7android21SurfaceComposerClient21getPhysicalDisplayIdsEv");
                ResolveMethod(SurfaceComposerClient, GetPhysicalDisplayToken, libgui, "_ZN7android21SurfaceComposerClient23getPhysicalDisplayTokenENS_17PhysicalDisplayIdE");

                ResolveMethod(SurfaceComposerClient__Transaction, Constructor, libgui, "_ZN7android21SurfaceComposerClient11TransactionC2Ev");
                ResolveMethod(SurfaceComposerClient__Transaction, SetLayer, libgui, "_ZN7android21SurfaceComposerClient11Transaction8setLayerERKNS_2spINS_14SurfaceControlEEEi");
                ResolveMethod(SurfaceComposerClient__Transaction, SetMetadata, libgui, "_ZN7android21SurfaceComposerClient11Transaction11setMetadataERKNS_2spINS_14SurfaceControlEEEjRKNS_6ParcelE");

                ResolveMethod(SurfaceComposerClient__Transaction, SetTrustedOverlay, libgui, "_ZN7android21SurfaceComposerClient11Transaction17setTrustedOverlayERKNS_2spINS_14SurfaceControlEEEb");
                ResolveMethod(SurfaceComposerClient__Transaction, SetFlags, libgui, "_ZN7android21SurfaceComposerClient11Transaction8setFlagsERKNS_2spINS_14SurfaceControlEEEjj");

                ResolveMethod(SurfaceComposerClient__Transaction, Apply, libgui, "_ZN7android21SurfaceComposerClient11Transaction5applyEbb");
                ResolveOptionalMethod(SurfaceComposerClient__Transaction, ApplySync, libgui, "_ZN7android21SurfaceComposerClient11Transaction5applyEb");

                ResolveMethod(SurfaceControl, Validate, libgui, "_ZNK7android14SurfaceControl8validateEv");
                ResolveMethod(SurfaceControl, GetSurface, libgui, "_ZN7android14SurfaceControl10getSurfaceEv");
                ResolveMethod(SurfaceControl, DisConnect, libgui, "_ZN7android14SurfaceControl10disconnectEv");

                // 版本 → patch 集选择：
                //  - 精确匹配优先（9/10/11/12/14/15/16 各有专用条目）
                //  - 比已知最高版本更新的系统（Android 17、18…）没有专用条目时，沿用最高版本的
                //    patch 集。Android 14 之后 LayerMetadata / createSurface 只是搬进了
                //    android::gui 命名空间，签名此后一直没再变，沿用是安全的；
                //    不这么做的话这些符号解析会失败 → 空指针调用 → 启动即 SIGSEGV。
                static constexpr size_t kLatestPatchVersion = 16;
                auto it = patchesTable.find(systemVersion);
                if (patchesTable.end() == it && systemVersion > kLatestPatchVersion)
                    it = patchesTable.find(kLatestPatchVersion);

                if (patchesTable.end() != it)
                {
                    if (it->first != systemVersion)
                        ANWC_LOGI("[*] Android %zu has no dedicated patch set, reuse Android %zu", systemVersion, it->first);

                    for (const auto &[patchTo, signature] : it->second)
                    {
                        *patchTo = symbolMethod.Find(libgui, signature);
                        if (nullptr != *patchTo)
                            continue;

                        ANWC_LOGE("[-] Patch method not found: %s", signature);
                    }
                }
                else
                {
                    ANWC_LOGI("[*] Android %zu, no patch set matched, use default symbols", systemVersion);
                }

                // 关键槽位一次性打出来：某个为 0 就能直接判断是“符号没解析到”还是“渲染失败”。
                ANWC_LOGI("[*] key symbols: LayerMetadataCtor=%p CreateSurface=%p GetSurface=%p TrustedOverlay=%p Apply=%p DisplayIds=%p",
                          reinterpret_cast<void *>(LayerMetadata__Constructor),
                          reinterpret_cast<void *>(SurfaceComposerClient__CreateSurface),
                          reinterpret_cast<void *>(SurfaceControl__GetSurface),
                          reinterpret_cast<void *>(SurfaceComposerClient__Transaction__SetTrustedOverlay),
                          reinterpret_cast<void *>(SurfaceComposerClient__Transaction__Apply),
                          reinterpret_cast<void *>(SurfaceComposerClient__GetPhysicalDisplayIds));

                // dynsym 自愈：patch 表都失效/空时，按 ABI 等价特征扫一遍 .dynsym。
                // 必须在此之前先 Close libutils，否则 Close libgui 会失败（关联的懒打开库尚未加载）。
                const char *dummyLibPaths[] = {"/system/lib64/libgui.so", "/system/lib/libgui.so", nullptr};
                for (const char *path : dummyLibPaths)
                {
                    if (0 == access(path, F_OK))
                    {
                        // 构造个命令的 std::string 避免参数求值顺序 UB
                        std::string libPath(path);
                        ANWC_LOGI("[*] trying dynsym fallback from %s", path);
                        ResolveByDynSymPattern<decltype(LayerMetadata__Constructor)>(LayerMetadata__Constructor, symbolMethod, libgui, libPath.c_str(), "_ZN7android", &AcceptLayerMetadataCtor, "LayerMetadata::Constructor");
                        ResolveByDynSymPattern<decltype(LayerMetadata__setInt32)>(LayerMetadata__setInt32, symbolMethod, libgui, libPath.c_str(), "_ZN7android", &AcceptLayerMetadataSetInt32, "LayerMetadata::setInt32");
                        ResolveByDynSymPattern<decltype(SurfaceComposerClient__CreateSurface)>(SurfaceComposerClient__CreateSurface, symbolMethod, libgui, libPath.c_str(), "_ZN7android21SurfaceComposerClient13createSurfaceE", &AcceptCreateSurfaceVariant, "SurfaceComposerClient::CreateSurface");
                        ResolveByDynSymPattern<decltype(SurfaceComposerClient__GetInternalDisplayToken)>(SurfaceComposerClient__GetInternalDisplayToken, symbolMethod, libgui, libPath.c_str(), "_ZN7android21SurfaceComposerClient", nullptr, "SurfaceComposerClient::GetInternalDisplayToken", nullptr, "Ev");
                        ResolveByDynSymPattern<decltype(SurfaceComposerClient__GetDisplayInfo)>(SurfaceComposerClient__GetDisplayInfo, symbolMethod, libgui, libPath.c_str(), "_ZN7android21SurfaceComposerClient", nullptr, "SurfaceComposerClient::GetDisplayInfo", nullptr, "EPNS_11DisplayInfoE");
                        break;
                    }
                }

                symbolMethod.Close(libutils);
                symbolMethod.Close(libgui);
            }

            static const Functionals &GetInstance(const SymbolMethod &symbolMethod = {.Open = dlopen, .Find = dlsym, .Close = dlclose})
            {
                static Functionals functionals(symbolMethod);
                return functionals;
            }
        };

        struct String8
        {
            char data[1024];
            bool valid = false;

            String8(const char *const string)
            {
                auto constructor = Functionals::GetInstance().String8__Constructor;
                if (nullptr == constructor)
                {
                    ANWC_LOGE("[-] String8 construction failed: symbol not resolved (libutils)");
                    return;
                }

                constructor(data, string);
                valid = true;
            }

            ~String8()
            {
                auto destructor = Functionals::GetInstance().String8__Destructor;
                if (nullptr == destructor || !valid)
                    return;

                destructor(data);
            }

            operator void *()
            {
                return reinterpret_cast<void *>(data);
            }
        };

        struct LayerMetadata
        {
            char data[1024];
            bool valid = false;

            LayerMetadata()
            {
                if (9 >= Functionals::GetInstance().systemVersion)
                    return;

                auto constructor = Functionals::GetInstance().LayerMetadata__Constructor;
                if (nullptr == constructor)
                {
                    // 不判空就会跳到 0 造成启动即 SIGSEGV（Android 17 上的原始故障）
                    ANWC_LOGE("[-] LayerMetadata construction failed: symbol not resolved (libgui)");
                    return;
                }

                constructor(data);
                valid = true;
            }

            void setInt32(uint32_t key, int32_t value)
            {
                auto setInt32 = Functionals::GetInstance().LayerMetadata__setInt32;
                if (nullptr == setInt32 || !valid)
                {
                    ANWC_LOGE("[-] LayerMetadata::setInt32 skipped: symbol not resolved or metadata invalid");
                    return;
                }

                setInt32(data, key, value);
            }

            operator void *()
            {
                if (9 < Functionals::GetInstance().systemVersion)
                    return reinterpret_cast<void *>(data);
                else
                    return nullptr;
            }
        };

        typedef int status_t;
        struct Parcel
        {
            char data[1024];                 // 存储数据的缓冲区
            mutable size_t dataSize = 0;     // 当前数据大小
            mutable size_t dataPosition = 0; // 当前数据位置，标记为 mutable 允许在 const 方法中修改

            // 构造函数
            Parcel()
            {
                memset(data, 0, sizeof(data));
            }

            // 写入 int32 值
            status_t writeInt32(int32_t val)
            {
                if (dataSize + sizeof(int32_t) > sizeof(data))
                {
                    return -1; // 缓冲区不足
                }

                *reinterpret_cast<int32_t *>(data + dataSize) = val;
                dataSize += sizeof(int32_t);
                return 0; // OK
            }

            // 读取 int32 值
            int32_t readInt32() const
            {
                if (dataPosition + sizeof(int32_t) > dataSize)
                {
                    return 0; // 数据不足，返回默认值
                }

                int32_t val = *reinterpret_cast<const int32_t *>(data + dataPosition);
                dataPosition += sizeof(int32_t); // 现在可以修改 mutable 变量
                return val;
            }

            // 设置数据位置
            void setDataPosition(size_t pos) const
            {
                if (pos <= dataSize)
                {
                    dataPosition = pos; // 现在可以修改 mutable 变量
                }
            }

            // 获取数据大小
            size_t dataAvail() const
            {
                return dataSize - dataPosition;
            }
        };

        struct Surface
        {
        };

        struct SurfaceControl
        {
            void *data;

            SurfaceControl() : data(nullptr) {}
            SurfaceControl(void *data) : data(data) {}

            int32_t Validate()
            {
                if (nullptr == data)
                    return 0;

                auto validate = Functionals::GetInstance().SurfaceControl__Validate;
                if (nullptr == validate)
                {
                    ANWC_LOGE("[-] SurfaceControl::validate: symbol not resolved");
                    return 0;
                }

                return validate(data);
            }

            Surface *GetSurface()
            {
                if (nullptr == data)
                    return nullptr;

                auto getSurface = Functionals::GetInstance().SurfaceControl__GetSurface;
                if (nullptr == getSurface)
                {
                    ANWC_LOGE("[-] SurfaceControl::getSurface: symbol not resolved");
                    return nullptr;
                }

                auto result = getSurface(data);
                if (nullptr == result.pointer)
                {
                    ANWC_LOGE("[-] SurfaceControl::getSurface returned null");
                    return nullptr;
                }

                // sp<Surface> 是 sret 返回，data[0] 就是 Surface*；
                // ANativeWindow 是 Surface 的第二个基类，偏移固定为 max_align_t/2（见反汇编 +0x10）。
                return reinterpret_cast<Surface *>(reinterpret_cast<size_t>(result.pointer) + sizeof(std::max_align_t) / 2);
            }

            void DisConnect()
            {
                if (nullptr == data)
                    return;

                auto disconnect = Functionals::GetInstance().SurfaceControl__DisConnect;
                if (nullptr == disconnect)
                {
                    ANWC_LOGE("[-] SurfaceControl::disconnect: symbol not resolved");
                    return;
                }

                disconnect(data);
            }

            void DestroySurface(Surface *surface)
            {
                if (nullptr == data || nullptr == surface)
                    return;

                DisConnect();

                auto decStrong = Functionals::GetInstance().RefBase__DecStrong;
                if (nullptr == decStrong)
                    return;

                decStrong(reinterpret_cast<Surface *>(reinterpret_cast<size_t>(surface) - sizeof(std::max_align_t) / 2), this);
                decStrong(data, this);
            }
        };

        struct SurfaceComposerClientTransaction
        {
            char data[1024];
            bool valid = false;

            SurfaceComposerClientTransaction()
            {
                auto constructor = Functionals::GetInstance().SurfaceComposerClient__Transaction__Constructor;
                if (nullptr == constructor)
                {
                    ANWC_LOGE("[-] Transaction construction failed: symbol not resolved (libgui)");
                    return;
                }

                constructor(data);
                valid = true;
            }

            void *SetLayer(StrongPointer<void> &surfaceControl, int32_t z)
            {
                auto setLayer = Functionals::GetInstance().SurfaceComposerClient__Transaction__SetLayer;
                if (nullptr == setLayer || !valid)
                {
                    ANWC_LOGE("[-] Transaction::setLayer skipped: symbol not resolved or transaction invalid");
                    return nullptr;
                }

                return setLayer(data, surfaceControl, z);
            }

            void *SetMetadata(StrongPointer<void> &surfaceControl, uint32_t key, android::detail::Parcel &parcel)
            {
                auto setMetadata = Functionals::GetInstance().SurfaceComposerClient__Transaction__SetMetadata;
                if (nullptr == setMetadata || !valid)
                    return nullptr;

                return setMetadata(data, surfaceControl, key, &parcel);
            }

            void *setMetadata(StrongPointer<void> &surfaceControl, uint32_t key, int32_t value)
            {
                if (nullptr == Functionals::GetInstance().SurfaceComposerClient__Transaction__SetMetadata)
                    return nullptr;

                android::detail::Parcel parcel;
                parcel.writeInt32(value);
                return SetMetadata(surfaceControl, key, parcel);
            }

            void *SetTrustedOverlay(StrongPointer<void> &surfaceControl, bool isTrustedOverlay)
            {
                auto setTrustedOverlay = Functionals::GetInstance().SurfaceComposerClient__Transaction__SetTrustedOverlay;
                if (nullptr == setTrustedOverlay || !valid)
                {
                    ANWC_LOGE("[-] Transaction::setTrustedOverlay skipped: symbol not resolved or transaction invalid");
                    return nullptr;
                }

                return setTrustedOverlay(data, surfaceControl, isTrustedOverlay);
            }

            void *SetFlags(StrongPointer<void> &surfaceControl, uint32_t flags, uint32_t mask)
            {
                auto setFlags = Functionals::GetInstance().SurfaceComposerClient__Transaction__SetFlags;
                if (nullptr == setFlags || !valid)
                    return nullptr;

                return setFlags(data, surfaceControl, flags, mask);
            }

            int32_t Apply(bool synchronous, bool oneWay)
            {
                auto &functionals = Functionals::GetInstance();

                // 12 及以下用 apply(bool)，13+ 才是 apply(bool, bool)。
                // 老写法是强转同一个槽位调用，槽位为 0 时直接段错误，这里按版本各自判空。
                if (12 >= functionals.systemVersion)
                {
                    if (nullptr != functionals.SurfaceComposerClient__Transaction__ApplySync)
                        return functionals.SurfaceComposerClient__Transaction__ApplySync(data, synchronous);

                    if (nullptr != functionals.SurfaceComposerClient__Transaction__Apply)
                        return reinterpret_cast<int32_t (*)(void *, bool)>(functionals.SurfaceComposerClient__Transaction__Apply)(data, synchronous);

                    ANWC_LOGE("[-] Transaction::apply: symbol not resolved");
                    return -1;
                }

                if (nullptr == functionals.SurfaceComposerClient__Transaction__Apply)
                {
                    ANWC_LOGE("[-] Transaction::apply(bool, bool): symbol not resolved");
                    return -1;
                }

                return functionals.SurfaceComposerClient__Transaction__Apply(data, synchronous, oneWay);
            }
        };

        struct SurfaceComposerClient
        {
            char data[1024];
            bool valid = false;

            SurfaceComposerClient()
            {
                auto constructor = Functionals::GetInstance().SurfaceComposerClient__Constructor;
                if (nullptr == constructor)
                {
                    ANWC_LOGE("[-] SurfaceComposerClient construction failed: symbol not resolved (libgui)");
                    return;
                }

                auto incStrong = Functionals::GetInstance().RefBase__IncStrong;
                if (nullptr == incStrong)
                {
                    ANWC_LOGE("[-] RefBase::incStrong: symbol not resolved (libutils)");
                    return;
                }

                constructor(data);
                incStrong(data, this);
                valid = true;
            }

            SurfaceControl CreateSurface(const char *name, int32_t width, int32_t height, bool skipScrenshot)
            {
                void *parentHandle = nullptr;
                String8 windowName(name);
                LayerMetadata layerMetadata;
                // printf("LayerMetadata__setInt32 函数地址: %p\n", Functionals::GetInstance().LayerMetadata__setInt32);
                if (skipScrenshot && (Functionals::GetInstance().systemVersion == 10 || Functionals::GetInstance().systemVersion == 11))
                {
                    layerMetadata.setInt32(2u, 441731);
                }
                uint32_t flags = 0;

                if (skipScrenshot && Functionals::GetInstance().systemVersion >= 12)
                {
                    // layerMetadata.setInt32(0x30000000, 2024); // 伪装为系统覆盖窗口类型
                    // layerMetadata.setInt32(1, 1000);          // 设置为 system UID
                    // layerMetadata.setInt32(9, 0);             // 通用元数据，由系统窗口使用
                    // layerMetadata.setInt32(0x40000001, 1);    // 标记为系统关键进程
                    // layerMetadata.setInt32(0x1a5e15a, 1);     // 隐藏窗口树遍历标识（需要root）

                    // layerMetadata.setInt32(0x30000002, 0); // 禁用任何调试标志
                    // layerMetadata.setInt32(0x40000003, 1); // 标记为"不可检测"
                    // layerMetadata.setInt32(0x50000000, 1); // 特定系统服务标识
                    // layerMetadata.setInt32(0x60000000, 0); // 禁用所有调试特性

                    // flags |= 0x100; // FLAG_NOT_FOCUSABLE (禁止获取焦点)
                    // flags |= 0x200; // FLAG_NOT_TOUCHABLE (禁止触摸事件)
                    // // flags |= 0x800;      // FLAG_NOT_VISIBLE   (窗口不可见，但需权衡显示需求)
                    // //  flags |= 0x00000004; // FLAG_HARDWARE_ACCELERATED (避免触发软件渲染检测)
                    // flags |= 0x00080000; // FLAG_DIM_BEHIND (混淆窗口用途)
                    // flags |= 0x40000000; // SYSTEM_FLAG_HIDE_NON_SYSTEM_OVERLAY_WINDOWS (需要root)
                    // flags |= 0x00001000; // FLAG_SLIPPERY (防止触摸穿透检测)
                    flags |= 0x40;

                    // printf("flags: %x\n", flags);
                }
                if (Functionals::GetInstance().systemVersion >= 12)
                {
                    static void *fakeParentHandleForBinder = nullptr;
                    parentHandle = &fakeParentHandleForBinder;
                    // printf("使用备用parentHandle: %p\n", parentHandle);
                }

                // printf("parentHandle: %p\n", parentHandle);

                if (!valid)
                {
                    ANWC_LOGE("[-] CreateSurface(%s) failed: SurfaceComposerClient invalid", name);
                    return {};
                }

                if (!windowName.valid)
                {
                    ANWC_LOGE("[-] CreateSurface(%s) failed: String8 invalid", name);
                    return {};
                }

                StrongPointer<void> result{};
                const size_t version = Functionals::GetInstance().systemVersion;
                if (9 == version)
                {
                    int32_t windowType = -1;
                    int32_t ownerUid = -1;
                    if (skipScrenshot)
                    {
                        windowType = 441731;
                    }

                    if (nullptr == Functionals::GetInstance().SurfaceComposerClient__CreateSurface_and9)
                    {
                        ANWC_LOGE("[-] CreateSurface(%s) failed: createSurface(Android 9) symbol not resolved", name);
                        return {};
                    }

                    result = Functionals::GetInstance().SurfaceComposerClient__CreateSurface_and9(data, windowName, width, height, 1, flags, parentHandle, windowType, ownerUid);
                }
                else if (10 <= version)
                {
                    if (nullptr == Functionals::GetInstance().SurfaceComposerClient__CreateSurface)
                    {
                        ANWC_LOGE("[-] CreateSurface(%s) failed: createSurface symbol not resolved (Android %zu)", name, version);
                        return {};
                    }

                    result = Functionals::GetInstance().SurfaceComposerClient__CreateSurface(data, windowName, width, height, 1, flags, parentHandle, layerMetadata, nullptr);
                }
                else
                {
                    ANWC_LOGE("[-] CreateSurface(%s) failed: unsupported Android %zu", name, version);
                    return {};
                }

                if (nullptr == result.get())
                {
                    ANWC_LOGE("[-] CreateSurface(%s) failed: createSurface returned null (Android %zu, %dx%d)", name, version, width, height);
                    return {};
                }

                if (12 <= version)
                {
                    static SurfaceComposerClientTransaction transaction;
                    transaction.SetTrustedOverlay(result, true);
                    // 置顶。原先写 19：在部分新版本上会被应用层/系统层压住，
                    // 表现就是“进程活着、帧也在画，但屏幕上看不到窗口”。
                    transaction.SetLayer(result, INT_MAX);
                    transaction.setMetadata(result, 1, 0); // METADATA_OWNER_UID = 0 (root)
                    transaction.Apply(false, true);
                }

                ANWC_LOGI("[+] CreateSurface(%s) ok: surfaceControl=%p (%dx%d, Android %zu, skipScrenshot=%d)",
                          name, result.get(), width, height, version, skipScrenshot ? 1 : 0);
                return {result.get()};
            }

            bool GetDisplayInfo(ui::DisplayState *displayInfo)
            {
                auto &functionals = Functionals::GetInstance();
                StrongPointer<void> defaultDisplay;

                if (!valid)
                {
                    ANWC_LOGE("[-] GetDisplayInfo failed: SurfaceComposerClient invalid");
                    return false;
                }

                if (9 >= functionals.systemVersion)
                {
                    if (nullptr == functionals.SurfaceComposerClient__GetBuiltInDisplay)
                    {
                        ANWC_LOGE("[-] GetDisplayInfo failed: getBuiltInDisplay symbol not resolved");
                        return false;
                    }

                    defaultDisplay = functionals.SurfaceComposerClient__GetBuiltInDisplay(ui::DisplayType::DisplayIdMain);
                }
                else
                {
                    if (14 > functionals.systemVersion)
                    {
                        if (nullptr == functionals.SurfaceComposerClient__GetInternalDisplayToken)
                        {
                            ANWC_LOGE("[-] GetDisplayInfo failed: getInternalDisplayToken symbol not resolved");
                            return false;
                        }

                        defaultDisplay = functionals.SurfaceComposerClient__GetInternalDisplayToken();
                    }
                    else
                    {
                        if (nullptr == functionals.SurfaceComposerClient__GetPhysicalDisplayIds ||
                            nullptr == functionals.SurfaceComposerClient__GetPhysicalDisplayToken)
                        {
                            ANWC_LOGE("[-] GetDisplayInfo failed: getPhysicalDisplayIds/Token symbol not resolved");
                            return false;
                        }

                        auto displayIds = functionals.SurfaceComposerClient__GetPhysicalDisplayIds();
                        if (displayIds.empty())
                        {
                            ANWC_LOGE("[-] GetDisplayInfo failed: no physical display id");
                            return false;
                        }

                        defaultDisplay = functionals.SurfaceComposerClient__GetPhysicalDisplayToken(displayIds[0]);
                    }
                }

                if (nullptr == defaultDisplay.get())
                {
                    ANWC_LOGE("[-] GetDisplayInfo failed: display token is null");
                    return false;
                }

                if (11 <= functionals.systemVersion)
                {
                    if (nullptr == functionals.SurfaceComposerClient__GetDisplayState)
                    {
                        ANWC_LOGE("[-] GetDisplayInfo failed: getDisplayState symbol not resolved");
                        return false;
                    }

                    return 0 == functionals.SurfaceComposerClient__GetDisplayState(defaultDisplay, displayInfo);
                }
                else
                {
                    if (nullptr == functionals.SurfaceComposerClient__GetDisplayInfo)
                    {
                        ANWC_LOGE("[-] GetDisplayInfo failed: getDisplayInfo symbol not resolved");
                        return false;
                    }

                    ui::DisplayInfo realDisplayInfo{};
                    if (0 != functionals.SurfaceComposerClient__GetDisplayInfo(defaultDisplay, &realDisplayInfo))
                        return false;

                    displayInfo->layerStackSpaceRect.width = realDisplayInfo.w;
                    displayInfo->layerStackSpaceRect.height = realDisplayInfo.h;
                    displayInfo->orientation = static_cast<ui::Rotation>(realDisplayInfo.orientation);

                    return true;
                }
            }
        };

    }

    class ANativeWindowCreator
    {
    public:
        struct DisplayInfo
        {
            int32_t orientation;
            int32_t width;
            int32_t height;
        };

    public:
        static detail::SurfaceComposerClient &GetComposerInstance()
        {
            static detail::SurfaceComposerClient surfaceComposerClient;

            return surfaceComposerClient;
        }

        static DisplayInfo GetDisplayInfo()
        {
            auto &surfaceComposerClient = GetComposerInstance();
            detail::ui::DisplayState displayInfo{};

            if (!surfaceComposerClient.GetDisplayInfo(&displayInfo))
                return {};

            DisplayInfo local_displayInfo{0};
            int32_t local_orientation = static_cast<int32_t>(displayInfo.orientation);
            int32_t local_abs_x = (displayInfo.layerStackSpaceRect.width > displayInfo.layerStackSpaceRect.height ? displayInfo.layerStackSpaceRect.width : displayInfo.layerStackSpaceRect.height);
            int32_t local_abs_y = (displayInfo.layerStackSpaceRect.width < displayInfo.layerStackSpaceRect.height ? displayInfo.layerStackSpaceRect.width : displayInfo.layerStackSpaceRect.height);
            if (local_orientation == 1 || local_orientation == 3)
            {
                local_displayInfo.width = local_abs_x;
                local_displayInfo.height = local_abs_y;
            }
            else
            {
                local_displayInfo.width = local_abs_y;
                local_displayInfo.height = local_abs_x;
            }
            local_displayInfo.orientation = local_orientation;
            return local_displayInfo;
        }

        static ANativeWindow *Create(const char *name, int32_t width = -1, int32_t height = -1, bool skipScrenshot_ = false)
        {
            auto &surfaceComposerClient = GetComposerInstance();

            while (-1 == width || -1 == height)
            {
                detail::ui::DisplayState displayInfo{};

                if (!surfaceComposerClient.GetDisplayInfo(&displayInfo))
                    break;

                width = displayInfo.layerStackSpaceRect.width;
                height = displayInfo.layerStackSpaceRect.height;

                break;
            }

            ANWC_LOGI("[*] Create(%s): request %dx%d (skipScrenshot=%d)", name, width, height, skipScrenshot_ ? 1 : 0);

            auto surfaceControl = surfaceComposerClient.CreateSurface(name, width, height, skipScrenshot_);
            auto nativeWindow = reinterpret_cast<ANativeWindow *>(surfaceControl.GetSurface());
            if (nullptr == nativeWindow)
            {
                ANWC_LOGE("[-] Create(%s) failed: ANativeWindow is null", name);
                return nullptr;
            }

            m_cachedSurfaceControl.emplace(nativeWindow, std::move(surfaceControl));
            ANWC_LOGI("[+] Create(%s) ok: ANativeWindow=%p", name, nativeWindow);
            return nativeWindow;
        }

        static bool Destroy(ANativeWindow *nativeWindow)
        {
            if (nullptr == nativeWindow)
                return false;

            // if (!m_cachedSurfaceControl.contains(nativeWindow))
            auto it = m_cachedSurfaceControl.find(nativeWindow);
            if (it == m_cachedSurfaceControl.end())
                return false;

            it->second.DestroySurface(reinterpret_cast<detail::Surface *>(nativeWindow));
            m_cachedSurfaceControl.erase(it);
            return true;
        }

    private:
        inline static std::unordered_map<ANativeWindow *, detail::SurfaceControl> m_cachedSurfaceControl;
    };
}

#undef ResolveMethod
#undef ResolveOptionalMethod

#endif // !A_NATIVE_WINDOW_CREATOR_H
