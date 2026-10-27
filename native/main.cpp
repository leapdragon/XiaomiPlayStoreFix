// XiaomiPlayStoreFix: Zygisk module that disables Play Store's Cronet path (no Xposed framework needed).
//
// preAppSpecialize (still zygote context): for Play Store processes only, load the embedded
// LSPlant library from a memfd and initialise it against libart.
// postAppSpecialize: load the embedded dex and let its Java code install the hooks.

#include <android/dlext.h>
#include <android/log.h>
#include <dlfcn.h>
#include <elf.h>
#include <fcntl.h>
#include <link.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstring>
#include <string>
#include <string_view>
#include <unordered_map>

#include "third_party/And64InlineHook.hpp"
#include "third_party/lsplant.hpp"
#include "third_party/zygisk.hpp"

#define TAG "XiaomiPlayStoreFix"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

// Embedded payloads (paths supplied by the build script).
#define INCBIN(name, path)                                                     \
    __asm__(".section .rodata\n.balign 16\n.global " #name "_start\n" #name   \
            "_start:\n.incbin \"" path "\"\n.global " #name "_end\n" #name    \
            "_end:\n.byte 0\n.previous\n");                                    \
    extern "C" const unsigned char name##_start[];                             \
    extern "C" const unsigned char name##_end[];

INCBIN(lsplant_so, LSPLANT_SO_PATH)
INCBIN(classes_dex, CLASSES_DEX_PATH)

namespace {

constexpr std::string_view kTargetPackage = "com.android.vending";

using InitFn = bool (*)(JNIEnv *, const lsplant::InitInfo &);
using HookFn = jobject (*)(JNIEnv *, jobject, jobject, jobject);

HookFn g_hook = nullptr;

// ---------- libart symbol resolution (.symtab and .dynsym from the file on disk) ----------

class ElfSymbols {
public:
    bool Open() {
        dl_iterate_phdr(
            [](dl_phdr_info *info, size_t, void *self) -> int {
                std::string_view name = info->dlpi_name ? info->dlpi_name : "";
                if (name.size() >= 10 && name.substr(name.size() - 10) == "/libart.so") {
                    auto *s = static_cast<ElfSymbols *>(self);
                    s->bias_ = info->dlpi_addr;
                    s->path_ = name;
                    return 1;
                }
                return 0;
            },
            this);
        if (path_.empty()) return false;

        int fd = open(path_.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) return false;
        struct stat st {};
        fstat(fd, &st);
        size_ = st.st_size;
        map_ = mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd, 0);
        close(fd);
        if (map_ == MAP_FAILED) {
            map_ = nullptr;
            return false;
        }

        auto *base = static_cast<const char *>(map_);
        auto *ehdr = reinterpret_cast<const Elf64_Ehdr *>(base);
        auto *shdr = reinterpret_cast<const Elf64_Shdr *>(base + ehdr->e_shoff);
        for (int i = 0; i < ehdr->e_shnum; i++) {
            if (shdr[i].sh_type != SHT_SYMTAB && shdr[i].sh_type != SHT_DYNSYM) continue;
            auto *syms = reinterpret_cast<const Elf64_Sym *>(base + shdr[i].sh_offset);
            const char *strtab = base + shdr[shdr[i].sh_link].sh_offset;
            size_t count = shdr[i].sh_size / sizeof(Elf64_Sym);
            for (size_t j = 0; j < count; j++) {
                if (syms[j].st_value == 0 || syms[j].st_shndx == SHN_UNDEF) continue;
                symbols_.emplace(std::string_view(strtab + syms[j].st_name),
                                 bias_ + syms[j].st_value);
            }
        }
        return !symbols_.empty();
    }

    void *Find(std::string_view name) const {
        auto it = symbols_.find(name);
        return it == symbols_.end() ? nullptr : reinterpret_cast<void *>(it->second);
    }

    void *FindPrefix(std::string_view prefix) const {
        for (auto &[name, addr] : symbols_) {
            if (name.substr(0, prefix.size()) == prefix) return reinterpret_cast<void *>(addr);
        }
        return nullptr;
    }

private:
    std::string path_;
    uintptr_t bias_ = 0;
    void *map_ = nullptr;
    size_t size_ = 0;
    std::unordered_map<std::string_view, uintptr_t> symbols_;
};

// ---------- LSPlant loading ----------

bool LoadLsplant(JNIEnv *env) {
    size_t len = lsplant_so_end - lsplant_so_start;
    int fd = memfd_create("xiaomiplaystorefix-lsplant", MFD_CLOEXEC);
    if (fd < 0 || write(fd, lsplant_so_start, len) != static_cast<ssize_t>(len)) {
        LOGE("memfd for lsplant failed: %s", strerror(errno));
        if (fd >= 0) close(fd);
        return false;
    }
    android_dlextinfo ext{};
    ext.flags = ANDROID_DLEXT_USE_LIBRARY_FD;
    ext.library_fd = fd;
    void *handle = android_dlopen_ext("libxiaomiplaystorefix_lsplant.so", RTLD_NOW, &ext);
    close(fd);
    if (!handle) {
        LOGE("dlopen lsplant failed: %s", dlerror());
        return false;
    }

    auto init = reinterpret_cast<InitFn>(
        dlsym(handle, "_ZN7lsplant2v24InitEP7_JNIEnvRKNS0_8InitInfoE"));
    g_hook = reinterpret_cast<HookFn>(
        dlsym(handle, "_ZN7lsplant2v24HookEP7_JNIEnvP8_jobjectS4_S4_"));
    if (!init || !g_hook) {
        LOGE("lsplant symbols missing");
        return false;
    }

    // LSPlant may resolve symbols after Init returns, so the table lives for the process.
    static ElfSymbols art;
    if (!art.Open()) {
        LOGE("cannot read libart symbols");
        return false;
    }
    lsplant::InitInfo info{
        .inline_hooker =
            [](void *target, void *hooker) -> void * {
                void *backup = nullptr;
                A64HookFunction(target, hooker, &backup);
                return backup;
            },
        .inline_unhooker = [](void *) { return false; },
        .art_symbol_resolver = [](std::string_view name) { return art.Find(name); },
        .art_symbol_prefix_resolver = [](std::string_view p) { return art.FindPrefix(p); },
        .generated_class_name = "XiaomiPlayStoreFixHooker_",
        .generated_source_name = "XiaomiPlayStoreFix",
    };
    bool ok = init(env, info);
    if (!ok) LOGE("lsplant init failed");
    return ok;
}

// ---------- JNI bridge for xiaomiplaystorefix.Main ----------

jobject JNICALL NativeHook(JNIEnv *env, jclass, jobject target, jobject hooker, jobject callback) {
    if (!target || !hooker || !callback) return nullptr;
    return g_hook(env, target, hooker, callback);
}

bool ClearException(JNIEnv *env) {
    if (!env->ExceptionCheck()) return false;
    env->ExceptionDescribe();
    env->ExceptionClear();
    return true;
}

class XiaomiPlayStoreFix : public zygisk::ModuleBase {
public:
    void onLoad(zygisk::Api *api, JNIEnv *env) override {
        api_ = api;
        env_ = env;
    }

    void preAppSpecialize(zygisk::AppSpecializeArgs *args) override {
        if (!IsTarget(args->nice_name)) {
            api_->setOption(zygisk::DLCLOSE_MODULE_LIBRARY);
            return;
        }
        CacheHiddenApiExemption();
        active_ = LoadLsplant(env_);
        LOGI("pre-specialize: lsplant %s", active_ ? "ready" : "unavailable");
    }

    void postAppSpecialize(const zygisk::AppSpecializeArgs *) override {
        if (!active_) return;
        ExemptHiddenApis();
        LoadDexAndInit();
    }

private:
    bool IsTarget(jstring nice_name) {
        if (!nice_name) return false;
        const char *name = env_->GetStringUTFChars(nice_name, nullptr);
        if (!name) return false;
        std::string_view n(name);
        bool match = n == kTargetPackage ||
                     (n.size() > kTargetPackage.size() && n.substr(0, kTargetPackage.size()) ==
                          kTargetPackage && n[kTargetPackage.size()] == ':');
        env_->ReleaseStringUTFChars(nice_name, name);
        return match;
    }

    // Hidden API checks happen at lookup time, so resolve VMRuntime before the app policy applies.
    void CacheHiddenApiExemption() {
        jclass cls = env_->FindClass("dalvik/system/VMRuntime");
        if (ClearException(env_) || !cls) return;
        vm_runtime_ = static_cast<jclass>(env_->NewGlobalRef(cls));
        get_runtime_ = env_->GetStaticMethodID(cls, "getRuntime", "()Ldalvik/system/VMRuntime;");
        set_exemptions_ =
            env_->GetMethodID(cls, "setHiddenApiExemptions", "([Ljava/lang/String;)V");
        ClearException(env_);
    }

    void ExemptHiddenApis() {
        if (!vm_runtime_ || !get_runtime_ || !set_exemptions_) return;
        jobject runtime = env_->CallStaticObjectMethod(vm_runtime_, get_runtime_);
        if (ClearException(env_) || !runtime) return;
        jobjectArray prefixes =
            env_->NewObjectArray(1, env_->FindClass("java/lang/String"), env_->NewStringUTF("L"));
        env_->CallVoidMethod(runtime, set_exemptions_, prefixes);
        ClearException(env_);
    }

    void LoadDexAndInit() {
        JNIEnv *env = env_;
        size_t len = classes_dex_end - classes_dex_start;
        // ART copies the dex out of the buffer; the embedded data stays valid regardless.
        jobject buffer =
            env->NewDirectByteBuffer(const_cast<unsigned char *>(classes_dex_start), len);

        jclass cl_class = env->FindClass("java/lang/ClassLoader");
        jobject system_cl = env->CallStaticObjectMethod(
            cl_class,
            env->GetStaticMethodID(cl_class, "getSystemClassLoader", "()Ljava/lang/ClassLoader;"));
        jclass imcl_class = env->FindClass("dalvik/system/InMemoryDexClassLoader");
        jobject loader = env->NewObject(
            imcl_class,
            env->GetMethodID(imcl_class, "<init>", "(Ljava/nio/ByteBuffer;Ljava/lang/ClassLoader;)V"),
            buffer, system_cl);
        if (ClearException(env) || !loader) {
            LOGE("cannot create dex class loader");
            return;
        }

        auto main_class = static_cast<jclass>(env->CallObjectMethod(
            loader,
            env->GetMethodID(cl_class, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;"),
            env->NewStringUTF("xiaomiplaystorefix.Main")));
        if (ClearException(env) || !main_class) {
            LOGE("cannot load xiaomiplaystorefix.Main");
            return;
        }

        JNINativeMethod natives[] = {
            {"hook",
             "(Ljava/lang/reflect/Member;Ljava/lang/Object;Ljava/lang/reflect/Method;)"
             "Ljava/lang/reflect/Method;",
             reinterpret_cast<void *>(NativeHook)},
        };
        if (env->RegisterNatives(main_class, natives, 1) != JNI_OK) {
            ClearException(env);
            LOGE("RegisterNatives failed");
            return;
        }

        env->CallStaticVoidMethod(main_class, env->GetStaticMethodID(main_class, "init", "()V"));
        ClearException(env);
        // Keep the loader (and with it the hook classes) alive for the life of the process.
        env->NewGlobalRef(loader);
        LOGI("hooks installed");
    }

    zygisk::Api *api_ = nullptr;
    JNIEnv *env_ = nullptr;
    bool active_ = false;
    jclass vm_runtime_ = nullptr;
    jmethodID get_runtime_ = nullptr;
    jmethodID set_exemptions_ = nullptr;
};

}  // namespace

REGISTER_ZYGISK_MODULE(XiaomiPlayStoreFix)
