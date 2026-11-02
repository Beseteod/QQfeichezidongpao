// R10 loader — zygisk module (Enhanced with multi-tap sequence)
// loads the original payload from module dir into an anonymous memfd and
// hands it to the app process without ptrace, without a helper process and
// without touching any file on /data.
#include <jni.h>
#include <android/log.h>
#include <android/dlext.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <link.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>


#include "zygisk.hpp"

#ifndef __NR_memfd_create
#define __NR_memfd_create 279
#endif
#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif

#define LTAG "jit"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LTAG, __VA_ARGS__)

static const char *kPkg = "com.tencent.tmgp.speedmobile";

// ---------------------------------------------------------------- config ---
// 新增：支持多个点击点的序列
#define MAX_TAP_POINTS 10

struct TapPoint {
    float x;
    float y;
    int delay_after_ms;  // 点击后等待多久再进行下一个点击
    int hold_ms;         // 按压时长
    int enabled;         // 是否启用这个点击点
};

struct Config {
    int   delay_ms      = 2500;
    int   auto_enable   = 1;
    float tap_x         = 0.50f;
    float tap_y         = 0.86f;
    int   tap_interval  = 8000;   // ms between attempts
    int   tap_hold      = 60;     // ms of contact
    int   wait_libs_ms  = 60000;  // how long to wait for unity/il2cpp
    int   rename        = 1;      // scrub identifying strings in memory

    // 新增：多点击序列配置
    int   use_sequence  = 0;      // 是否使用序列模式（0=单点循环，1=序列模式）
    int   sequence_count = 0;     // 序列中有几个点击点
    TapPoint sequence[MAX_TAP_POINTS];  // 点击序列
    int   sequence_loop_delay = 8000;   // 整个序列完成后等待多久再重复
};

static Config g_cfg;
static unsigned char *g_img = nullptr;
static size_t g_imgsz = 0;
static int g_loaded = 0;
static int g_want = 0;
static JavaVM *g_vm = nullptr;

static int parse_int(const char *v, int dflt) {
    if (!v) return dflt;
    char *end = nullptr;
    long r = strtol(v, &end, 10);
    if (end == v) return dflt;
    return (int) r;
}

static float parse_float(const char *v, float dflt) {
    if (!v) return dflt;
    char *end = nullptr;
    float r = strtof(v, &end);
    if (end == v) return dflt;
    return r;
}

static void load_config(int dirfd) {
    int fd = openat(dirfd, "autostart.conf", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return;
    char buf[4096];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return;
    buf[n] = 0;

    char *save = nullptr;
    for (char *line = strtok_r(buf, "\n", &save); line; line = strtok_r(nullptr, "\n", &save)) {
        char *hash = strchr(line, '#');
        if (hash) *hash = 0;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *k = line;
        char *v = eq + 1;
        while (*k == ' ' || *k == '\t') k++;
        while (*v == ' ' || *v == '\t') v++;
        char *ke = k + strlen(k);
        while (ke > k && (ke[-1] == ' ' || ke[-1] == '\t' || ke[-1] == '\r')) *--ke = 0;

        if (!strcmp(k, "delay_ms"))          g_cfg.delay_ms = parse_int(v, g_cfg.delay_ms);
        else if (!strcmp(k, "auto"))         g_cfg.auto_enable = parse_int(v, g_cfg.auto_enable);
        else if (!strcmp(k, "x"))            g_cfg.tap_x = parse_float(v, g_cfg.tap_x);
        else if (!strcmp(k, "y"))            g_cfg.tap_y = parse_float(v, g_cfg.tap_y);
        else if (!strcmp(k, "interval_ms"))  g_cfg.tap_interval = parse_int(v, g_cfg.tap_interval);
        else if (!strcmp(k, "hold_ms"))      g_cfg.tap_hold = parse_int(v, g_cfg.tap_hold);
        else if (!strcmp(k, "rename"))       g_cfg.rename = parse_int(v, g_cfg.rename);
        // 新增：序列模式配置
        else if (!strcmp(k, "use_sequence")) g_cfg.use_sequence = parse_int(v, g_cfg.use_sequence);
        else if (!strcmp(k, "sequence_loop_delay")) g_cfg.sequence_loop_delay = parse_int(v, g_cfg.sequence_loop_delay);
        // 解析序列点: tap1_x, tap1_y, tap1_delay, tap1_hold, tap1_enabled
        else if (strncmp(k, "tap", 3) == 0) {
            int idx = -1;
            sscanf(k + 3, "%d", &idx);
            if (idx >= 1 && idx <= MAX_TAP_POINTS) {
                idx--; // 转为0-based索引
                const char *suffix = strchr(k + 3, '_');
                if (suffix) {
                    suffix++;
                    if (!strcmp(suffix, "x")) {
                        g_cfg.sequence[idx].x = parse_float(v, 0.5f);
                        g_cfg.sequence[idx].enabled = 1;
                        if (idx >= g_cfg.sequence_count) g_cfg.sequence_count = idx + 1;
                    }
                    else if (!strcmp(suffix, "y")) g_cfg.sequence[idx].y = parse_float(v, 0.5f);
                    else if (!strcmp(suffix, "delay")) g_cfg.sequence[idx].delay_after_ms = parse_int(v, 1000);
                    else if (!strcmp(suffix, "hold")) g_cfg.sequence[idx].hold_ms = parse_int(v, 60);
                    else if (!strcmp(suffix, "enabled")) g_cfg.sequence[idx].enabled = parse_int(v, 1);
                }
            }
        }
    }
    if (g_cfg.tap_interval < 1000) g_cfg.tap_interval = 1000;
    if (g_cfg.tap_hold < 20) g_cfg.tap_hold = 20;
    if (g_cfg.delay_ms < 0) g_cfg.delay_ms = 0;
    if (g_cfg.sequence_loop_delay < 1000) g_cfg.sequence_loop_delay = 1000;

    // 设置默认序列（如果配置中没有）
    if (g_cfg.use_sequence && g_cfg.sequence_count == 0) {
        // 默认3个点击点：游戏中 -> 结算跳过 -> 开始下一局
        g_cfg.sequence_count = 3;
        // 点1: 游戏中点击（原位置）
        g_cfg.sequence[0].x = g_cfg.tap_x;
        g_cfg.sequence[0].y = g_cfg.tap_y;
        g_cfg.sequence[0].hold_ms = g_cfg.tap_hold;
        g_cfg.sequence[0].delay_after_ms = 3000;  // 点击后等3秒
        g_cfg.sequence[0].enabled = 1;
        // 点2: 结算页面跳过按钮（假设在右上角）
        g_cfg.sequence[1].x = 0.85f;
        g_cfg.sequence[1].y = 0.15f;
        g_cfg.sequence[1].hold_ms = 60;
        g_cfg.sequence[1].delay_after_ms = 2000;  // 点击后等2秒
        g_cfg.sequence[1].enabled = 1;
        // 点3: 开始下一局按钮（假设在屏幕中间偏下）
        g_cfg.sequence[2].x = 0.50f;
        g_cfg.sequence[2].y = 0.75f;
        g_cfg.sequence[2].hold_ms = 60;
        g_cfg.sequence[2].delay_after_ms = 1000;  // 点击后等1秒
        g_cfg.sequence[2].enabled = 1;
    }
}

// ------------------------------------------------------------------ misc ---
// plain C scanning: keeps a small overlap so a name split across two reads is
// still found
static bool maps_has(const char *needle) {
    int fd = open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    static char buf[65536 + 256 + 1];
    const size_t overlap = 256;
    size_t have = 0;
    bool found = false;
    while (true) {
        ssize_t n = read(fd, buf + have, 65536);
        if (n <= 0) break;
        have += (size_t) n;
        buf[have] = 0;
        if (strstr(buf, needle)) { found = true; break; }
        if (have > overlap) {
            memmove(buf, buf + have - overlap, overlap);
            have = overlap;
        }
    }
    close(fd);
    return found;
}

static bool has_thread(const char *name) {
    DIR *d = opendir("/proc/self/task");
    if (!d) return false;
    bool found = false;
    struct dirent *e;
    char path[80], buf[64];
    while ((e = readdir(d)) != nullptr) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        snprintf(path, sizeof(path), "/proc/self/task/%s/comm", e->d_name);
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) continue;
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n <= 0) continue;
        buf[n] = 0;
        char *nl = strchr(buf, '\n');
        if (nl) *nl = 0;
        if (!strcmp(buf, name)) { found = true; break; }
    }
    closedir(d);
    return found;
}

static bool wait_maps(const char *needle, int timeout_ms) {
    int waited = 0;
    while (!maps_has(needle)) {
        if (waited >= timeout_ms) return false;
        usleep(100 * 1000);
        waited += 100;
    }
    return true;
}

// --------------------------------------------------------------- scrubber ---
struct Patch { const char *from; const char *to; };

static const Patch kPatches[] = {
    // outer library identity
    { "libnbyydsop_jshook_r13.so",   "libil2cpp_gc.so" },
    { "libnbyydsop_payload_r13.so",  "jit-cache-1.so" },
    { "nbyydsop_r13_payload",        "jit-cache" },
    { "JuanReplayR13",               "jit" },
    // inner library identity
    { "libqqspeed_juan_unpacked.so", "libunity_aux.so" },
};

static int apply_patches(unsigned char *buf, size_t size) {
    int hits = 0;
    for (const auto &p : kPatches) {
        size_t fl = strlen(p.from), tl = strlen(p.to);
        if (tl > fl) continue;
        size_t pos = 0;
        while (pos + fl <= size) {
            void *at = memmem(buf + pos, size - pos, p.from, fl);
            if (!at) break;
            auto *q = (unsigned char *) at;
            memcpy(q, p.to, tl);
            memset(q + tl, 0, fl - tl);
            hits++;
            pos = (size_t)(q - buf) + fl;
        }
    }
    return hits;
}

// ------------------------------------------------------------------ main ---
static bool load_payload(JavaVM *vm) {
    if (!g_img || !g_imgsz) { LOGI("init: no image"); return false; }

    if (g_cfg.rename) {
        int n = apply_patches(g_img, g_imgsz);
        LOGI("init: scrub %d", n);
    }

    int fd = (int) syscall(__NR_memfd_create, "jit-cache", MFD_CLOEXEC);
    if (fd < 0) { LOGI("init: cache %d", errno); return false; }

    size_t off = 0;
    while (off < g_imgsz) {
        ssize_t w = write(fd, g_img + off, g_imgsz - off);
        if (w <= 0) {
            if (w < 0 && errno == EINTR) continue;
            LOGI("init: write %d", errno);
            close(fd);
            return false;
        }
        off += (size_t) w;
    }
    // drop our private copy, the fd owns the pages now
    munmap(g_img, g_imgsz);
    g_img = nullptr;

    android_dlextinfo info;
    memset(&info, 0, sizeof(info));
    info.flags = ANDROID_DLEXT_USE_LIBRARY_FD;
    info.library_fd = fd;

    void *h = android_dlopen_ext("libil2cpp_gc.so", RTLD_NOW, &info);
    if (!h) { LOGI("map: %s", dlerror()); close(fd); return false; }
    close(fd);

    auto onload = (jint (*)(JavaVM *, void *)) dlsym(h, "JNI_OnLoad");
    if (!onload) { LOGI("map: entry"); return false; }

    jint r = onload(vm, nullptr);
    LOGI("map: ok %x", (unsigned) r);
    return true;
}

// --------------------------------------------------------------- autostart ---
struct TapCtx { JavaVM *vm; };

static void release(JNIEnv *env, jobject o) { if (o) env->DeleteLocalRef(o); }

// fallback: walk ActivityThread.mActivities -> record.activity -> window -> decor
static jobject root_view_alt(JNIEnv *env) {
    jobject decor = nullptr;
    jclass at = env->FindClass("android/app/ActivityThread");
    if (!at) { env->ExceptionClear(); return nullptr; }
    jmethodID cur = env->GetStaticMethodID(at, "currentActivityThread",
                                           "()Landroid/app/ActivityThread;");
    jfieldID f_map = env->GetFieldID(at, "mActivities", "Landroid/util/ArrayMap;");
    if (env->ExceptionCheck() || !cur || !f_map) { env->ExceptionClear(); return nullptr; }
    jobject ato = env->CallStaticObjectMethod(at, cur);
    if (env->ExceptionCheck() || !ato) { env->ExceptionClear(); return nullptr; }
    jobject map = env->GetObjectField(ato, f_map);
    if (env->ExceptionCheck() || !map) { env->ExceptionClear(); return nullptr; }

    jclass mc = env->FindClass("java/util/Map");
    jmethodID values = env->GetMethodID(mc, "values", "()Ljava/util/Collection;");
    jclass cc = env->FindClass("java/util/Collection");
    jmethodID to_array = env->GetMethodID(cc, "toArray", "()[Ljava/lang/Object;");
    if (env->ExceptionCheck() || !values || !to_array) { env->ExceptionClear(); return nullptr; }
    jobject coll = env->CallObjectMethod(map, values);
    if (env->ExceptionCheck() || !coll) { env->ExceptionClear(); return nullptr; }
    auto arr = (jobjectArray) env->CallObjectMethod(coll, to_array);
    if (env->ExceptionCheck() || !arr) { env->ExceptionClear(); return nullptr; }

    jsize n = env->GetArrayLength(arr);
    for (jsize i = 0; i < n && !decor; i++) {
        jobject rec = env->GetObjectArrayElement(arr, i);
        if (!rec) continue;
        jclass rc = env->GetObjectClass(rec);
        jfieldID fa = env->GetFieldID(rc, "activity", "Landroid/app/Activity;");
        if (env->ExceptionCheck()) { env->ExceptionClear(); env->DeleteLocalRef(rc); env->DeleteLocalRef(rec); continue; }
        jobject act = fa ? env->GetObjectField(rec, fa) : nullptr;
        env->DeleteLocalRef(rc);
        env->DeleteLocalRef(rec);
        if (!act) continue;
        jclass ac = env->GetObjectClass(act);
        jmethodID gw = env->GetMethodID(ac, "getWindow", "()Landroid/view/Window;");
        jobject win = gw ? env->CallObjectMethod(act, gw) : nullptr;
        if (env->ExceptionCheck()) env->ExceptionClear();
        if (win) {
            jclass wc = env->GetObjectClass(win);
            jmethodID gd = env->GetMethodID(wc, "getDecorView", "()Landroid/view/View;");
            jobject dv = gd ? env->CallObjectMethod(win, gd) : nullptr;
            if (env->ExceptionCheck()) env->ExceptionClear();
            if (dv) decor = dv;
            env->DeleteLocalRef(wc);
            env->DeleteLocalRef(win);
        }
        env->DeleteLocalRef(ac);
        env->DeleteLocalRef(act);
    }
    return decor;
}

// returns the decor view of the top activity, or null
static jobject root_view(JNIEnv *env) {
    jobject result = nullptr;
    jclass wmg = env->FindClass("android/view/WindowManagerGlobal");
    if (!wmg) { env->ExceptionClear(); return nullptr; }

    jmethodID get_instance = env->GetStaticMethodID(
        wmg, "getInstance", "()Landroid/view/WindowManagerGlobal;");
    jmethodID get_root = env->GetMethodID(
        wmg, "getRootView", "(I)Landroid/view/View;");
    if (env->ExceptionCheck()) { env->ExceptionClear(); return nullptr; }
    if (!get_instance || !get_root) return nullptr;

    jobject inst = env->CallStaticObjectMethod(wmg, get_instance);
    if (env->ExceptionCheck() || !inst) { env->ExceptionClear(); return nullptr; }

    for (jint i = 0; i < 4 && !result; i++) {
        jobject v = env->CallObjectMethod(inst, get_root, i);
        if (env->ExceptionCheck()) { env->ExceptionClear(); break; }
        if (v) {
            jclass vc = env->GetObjectClass(v);
            jmethodID shown = env->GetMethodID(vc, "isShown", "()Z");
            jboolean ok = shown ? env->CallBooleanMethod(v, shown) : JNI_TRUE;
            if (env->ExceptionCheck()) { env->ExceptionClear(); ok = JNI_TRUE; }
            jmethodID gvw = env->GetMethodID(vc, "getWidth", "()I");
            jint w = gvw ? env->CallIntMethod(v, gvw) : 0;
            env->DeleteLocalRef(vc);
            if (ok && w > 0) result = v;
            else release(env, v);
        }
    }
    release(env, inst);
    if (!result) result = root_view_alt(env);
    return result;
}

static void tap_impl(JNIEnv *env, float fx, float fy, int hold_ms) {
    jobject view = root_view(env);
    if (!view) return;

    jclass vc = env->GetObjectClass(view);
    jmethodID gw = env->GetMethodID(vc, "getWidth", "()I");
    jmethodID gh = env->GetMethodID(vc, "getHeight", "()I");
    jmethodID dispatch = env->GetMethodID(vc, "dispatchTouchEvent",
                                         "(Landroid/motion/MotionEvent;)Z");
    if (env->ExceptionCheck() || !gw || !gh || !dispatch) {
        env->ExceptionClear();
        env->DeleteLocalRef(vc);
        release(env, view);
        return;
    }
    jint w = env->CallIntMethod(view, gw);
    jint h = env->CallIntMethod(view, gh);
    if (w <= 0 || h <= 0) {
        env->DeleteLocalRef(vc);
        release(env, view);
        return;
    }
    if (fx < 0.f) fx = 0.f; if (fx > 1.f) fx = 1.f;
    if (fy < 0.f) fy = 0.f; if (fy > 1.f) fy = 1.f;
    float x = (float) w * fx;
    float y = (float) h * fy;

    jclass mec = env->FindClass("android/motion/MotionEvent");
    jmethodID obtain = env->GetStaticMethodID(
        mec, "obtain", "(JJIFFI)Landroid/motion/MotionEvent;");
    jmethodID set_source = env->GetMethodID(mec, "setSource", "(I)V");
    jmethodID recycle = env->GetMethodID(mec, "recycle", "()V");
    long long down = 0, up = 0;
    {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        down = (long long) ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
        up = down + hold_ms;
    }
    if (env->ExceptionCheck() || !obtain) { env->ExceptionClear(); goto done; }

    for (int phase = 0; phase < 2; phase++) {
        jint action = phase == 0 ? 0 /*DOWN*/ : 1 /*UP*/;
        long long when = phase == 0 ? down : up;
        jobject ev = env->CallStaticObjectMethod(mec, obtain,
                                                 (jlong) down, (jlong) when,
                                                 action, x, y, (jint) 0);
        if (env->ExceptionCheck() || !ev) { env->ExceptionClear(); break; }
        if (set_source) env->CallVoidMethod(ev, set_source, 4098 /*TOUCHSCREEN*/);
        if (env->ExceptionCheck()) env->ExceptionClear();
        env->CallBooleanMethod(view, dispatch, ev);
        if (env->ExceptionCheck()) env->ExceptionClear();
        if (recycle) env->CallVoidMethod(ev, recycle);
        if (env->ExceptionCheck()) env->ExceptionClear();
        release(env, ev);
        if (phase == 0) usleep((useconds_t) hold_ms * 1000);
    }

done:
    if (mec) env->DeleteLocalRef(mec);
    env->DeleteLocalRef(vc);
    release(env, view);
}

static void tap(JNIEnv *env, float fx, float fy, int hold_ms) {
    if (env->PushLocalFrame(64) != JNI_OK) { env->ExceptionClear(); return; }
    tap_impl(env, fx, fy, hold_ms);
    if (env->ExceptionCheck()) env->ExceptionClear();
    env->PopLocalFrame(nullptr);
}

// 新增：序列点击工作线程
static void *auto_worker_sequence(void *arg) {
    TapCtx *ctx = (TapCtx *) arg;
    JNIEnv *env = nullptr;
    if (ctx->vm->GetEnv((void **) &env, JNI_VERSION_1_6) != JNI_OK) {
        if (ctx->vm->AttachCurrentThread(&env, nullptr) != JNI_OK) return nullptr;
    }
    LOGI("attach: sequence mode");

    while (true) {
        // 执行序列中的每个点击点
        for (int i = 0; i < g_cfg.sequence_count; i++) {
            if (!g_cfg.sequence[i].enabled) continue;

            LOGI("tap: seq[%d] x=%.2f y=%.2f hold=%d",
                 i, g_cfg.sequence[i].x, g_cfg.sequence[i].y, g_cfg.sequence[i].hold_ms);

            tap(env, g_cfg.sequence[i].x, g_cfg.sequence[i].y, g_cfg.sequence[i].hold_ms);

            // 点击后等待
            if (g_cfg.sequence[i].delay_after_ms > 0) {
                usleep((useconds_t) g_cfg.sequence[i].delay_after_ms * 1000);
            }
        }

        // 整个序列完成后的循环等待
        usleep((useconds_t) g_cfg.sequence_loop_delay * 1000);
    }
    return nullptr;
}

// 原有的单点循环模式
static void *auto_worker(void *arg) {
    TapCtx *ctx = (TapCtx *) arg;
    JNIEnv *env = nullptr;
    if (ctx->vm->GetEnv((void **) &env, JNI_VERSION_1_6) != JNI_OK) {
        if (ctx->vm->AttachCurrentThread(&env, nullptr) != JNI_OK) return nullptr;
    }
    LOGI("attach: single tap mode");
    while (true) {
        usleep((useconds_t) g_cfg.tap_interval * 1000);
        tap(env, g_cfg.tap_x, g_cfg.tap_y, g_cfg.tap_hold);
    }
    return nullptr;
}

// ---------------------------------------------------------------- worker ---
static void *loader_worker(void *arg) {
    JavaVM *vm = (JavaVM *) arg;

    if (!wait_maps("libil2cpp.so", g_cfg.wait_libs_ms)) {
        LOGI("wait: il2cpp timeout");
    } else {
        LOGI("wait: il2cpp mapped");
    }
    // Unity spins up its own main thread while the engine initialises; the
    // patches only line up once that is running
    int waited = 0;
    while (!has_thread("UnityMain")) {
        if (waited >= 30000) { LOGI("wait: unity thread timeout"); break; }
        usleep(100 * 1000);
        waited += 100;
    }
    if (g_cfg.delay_ms > 0) usleep((useconds_t) g_cfg.delay_ms * 1000);

    if (!load_payload(vm)) {
        LOGI("init: aborted");
        return nullptr;
    }
    g_loaded = 1;

    if (g_cfg.auto_enable) {
        static TapCtx ctx;
        ctx.vm = vm;
        pthread_t t;
        pthread_attr_t a;
        pthread_attr_init(&a);
        pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);

        // 根据配置选择单点模式或序列模式
        if (g_cfg.use_sequence) {
            LOGI("start: sequence mode with %d points", g_cfg.sequence_count);
            pthread_create(&t, &a, auto_worker_sequence, &ctx);
        } else {
            LOGI("start: single tap mode");
            pthread_create(&t, &a, auto_worker, &ctx);
        }

        pthread_attr_destroy(&a);
    }
    return nullptr;
}

// ----------------------------------------------------------------- zygisk ---
class R10 : public zygisk::ModuleBase {
public:
    void onLoad(zygisk::Api *api, JNIEnv *env) override {
        this->api = api;
        this->env = env;
        if (env && env->GetJavaVM(&g_vm) != JNI_OK) g_vm = nullptr;
        LOGI("load: ok");
    }

    void preAppSpecialize(zygisk::AppSpecializeArgs *args) override {
        const char *name = nullptr;
        if (args->nice_name) name = env->GetStringUTFChars(args->nice_name, nullptr);
        bool match = name && strcmp(name, kPkg) == 0;
        if (name) env->ReleaseStringUTFChars(args->nice_name, name);
        if (!match) return;
        g_want = 1;

        int dirfd = api->getModuleDir();
        if (dirfd < 0) { LOGI("init: no dir"); g_want = 0; return; }
        LOGI("init: stage 1");

        load_config(dirfd);

        int fd = openat(dirfd, "core.bin", O_RDONLY | O_CLOEXEC);
        if (fd < 0) { LOGI("init: image %d", errno); g_want = 0; return; }
        struct stat st;
        if (fstat(fd, &st) != 0 || st.st_size <= 0) {
            LOGI("init: stat %d", errno); close(fd); g_want = 0; return;
        }
        size_t size = (size_t) st.st_size;
        void *buf = mmap(nullptr, size, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (buf == MAP_FAILED) {
            LOGI("init: alloc %d", errno); close(fd); g_want = 0; return;
        }
        size_t off = 0;
        while (off < size) {
            ssize_t r = read(fd, (char *) buf + off, size - off);
            if (r <= 0) {
                if (r < 0 && errno == EINTR) continue;
                break;
            }
            off += (size_t) r;
        }
        close(fd);
        if (off != size) {
            LOGI("init: short %zu/%zu", off, size);
            munmap(buf, size);
            g_want = 0;
            return;
        }
        g_img = (unsigned char *) buf;
        g_imgsz = size;
        LOGI("init: image %zu", size);
    }

    void postAppSpecialize(const zygisk::AppSpecializeArgs *) override {
        if (!g_want || !g_img) return;

        JavaVM *vm = g_vm;
        if (!vm) { LOGI("init: no vm"); return; }
        // keep this library mapped even if the framework drops its reference:
        // the worker thread below lives in here
        Dl_info di;
        if (dladdr((void *) &loader_worker, &di) && di.dli_fname) {
            dlopen(di.dli_fname, RTLD_NOW | RTLD_NODELETE);
        }
        pthread_t t;
        pthread_attr_t a;
        pthread_attr_init(&a);
        pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
        pthread_create(&t, &a, loader_worker, vm);
        pthread_attr_destroy(&a);
    }

private:
    zygisk::Api *api = nullptr;
    JNIEnv *env = nullptr;
};

REGISTER_ZYGISK_MODULE(R10)
