// R10 Enhanced v3 — 智能界面识别版本
// 增加屏幕像素检测、界面识别、反检测措施
#include <jni.h>
#include <android/log.h>
#include <android/dlext.h>
#include <android/bitmap.h>
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
#include <ctime>
#include <cmath>

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

// 游戏界面类型
enum GameState {
    STATE_UNKNOWN = 0,
    STATE_IN_GAME,           // 游戏中（自动跑图）
    STATE_SETTLEMENT,        // 结算页面
    STATE_RANK,              // 排行榜
    STATE_POPUP_YIQIJUECHEN, // 一骑绝尘弹窗
    STATE_MATCH_READY        // 匹配准备界面
};

// ---------------------------------------------------------------- config ---
struct Config {
    int   delay_ms      = 2500;
    int   auto_enable   = 1;
    int   wait_libs_ms  = 60000;
    int   rename        = 1;

    // 智能识别配置
    int   recognition_enabled = 1;        // 启用智能识别
    int   check_interval_ms = 3000;       // 界面检测间隔（3秒）
    int   random_delay_range = 500;       // 随机延迟范围（±500ms）
    int   random_offset_pixels = 15;      // 随机位置偏移（±15像素）

    // 各界面的点击坐标（归一化坐标）
    float match_btn_x = 0.86f;            // 【开始匹配】按钮
    float match_btn_y = 0.88f;

    float continue_btn_x = 0.89f;         // 【点击继续】按钮
    float continue_btn_y = 0.88f;

    float cancel_btn_x = 0.31f;           // 【取消】按钮
    float cancel_btn_y = 0.88f;

    float again_btn_x = 0.79f;            // 【再来一局】按钮
    float again_btn_y = 0.88f;

    float ingame_x = 0.50f;               // 游戏中点击位置
    float ingame_y = 0.86f;

    int   hold_time_ms = 60;              // 按压时间
};

static Config g_cfg;
static unsigned char *g_img = nullptr;
static size_t g_imgsz = 0;
static int g_loaded = 0;
static int g_want = 0;
static JavaVM *g_vm = nullptr;
static GameState g_current_state = STATE_UNKNOWN;

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
        else if (!strcmp(k, "rename"))       g_cfg.rename = parse_int(v, g_cfg.rename);
        else if (!strcmp(k, "recognition")) g_cfg.recognition_enabled = parse_int(v, 1);
        else if (!strcmp(k, "check_interval")) g_cfg.check_interval_ms = parse_int(v, 3000);
    }
}

// --------------------------------------------------------------- random ---
// 生成随机数（反检测）
static int random_range(int min, int max) {
    if (min >= max) return min;
    static unsigned int seed = 0;
    if (seed == 0) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        seed = (unsigned int)(ts.tv_sec ^ ts.tv_nsec ^ getpid());
    }
    seed = seed * 1103515245 + 12345;
    return min + (int)((seed / 65536) % (max - min + 1));
}

// 生成随机延迟（反检测）
static void random_delay() {
    int range = g_cfg.random_delay_range;
    if (range > 0) {
        int delay_ms = random_range(-range, range);
        if (delay_ms > 0) {
            usleep(delay_ms * 1000);
        }
    }
}

// ------------------------------------------------------------------ misc ---
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
    { "libnbyydsop_jshook_r13.so",   "libil2cpp_gc.so" },
    { "libnbyydsop_payload_r13.so",  "jit-cache-1.so" },
    { "nbyydsop_r13_payload",        "jit-cache" },
    { "JuanReplayR13",               "jit" },
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

// -------------------------------------------------------- screen capture ---
struct TapCtx { JavaVM *vm; };

static void release(JNIEnv *env, jobject o) { if (o) env->DeleteLocalRef(o); }

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

// 获取屏幕截图的Bitmap（用于界面识别）
static jobject capture_screen_bitmap(JNIEnv *env) {
    jobject view = root_view(env);
    if (!view) return nullptr;

    jclass vc = env->GetObjectClass(view);
    jmethodID is_drawing_cache_enabled = env->GetMethodID(vc, "isDrawingCacheEnabled", "()Z");
    jmethodID set_drawing_cache_enabled = env->GetMethodID(vc, "setDrawingCacheEnabled", "(Z)V");
    jmethodID build_drawing_cache = env->GetMethodID(vc, "buildDrawingCache", "()V");
    jmethodID get_drawing_cache = env->GetMethodID(vc, "getDrawingCache", "()Landroid/graphics/Bitmap;");

    if (env->ExceptionCheck() || !set_drawing_cache_enabled || !build_drawing_cache || !get_drawing_cache) {
        env->ExceptionClear();
        env->DeleteLocalRef(vc);
        release(env, view);
        return nullptr;
    }

    jboolean was_enabled = JNI_FALSE;
    if (is_drawing_cache_enabled) {
        was_enabled = env->CallBooleanMethod(view, is_drawing_cache_enabled);
    }

    if (!was_enabled) {
        env->CallVoidMethod(view, set_drawing_cache_enabled, JNI_TRUE);
    }
    env->CallVoidMethod(view, build_drawing_cache);

    jobject bitmap = env->CallObjectMethod(view, get_drawing_cache);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        bitmap = nullptr;
    }

    // 创建bitmap的副本，因为原始drawing cache会被回收
    jobject result = nullptr;
    if (bitmap) {
        jclass bitmap_cls = env->FindClass("android/graphics/Bitmap");
        jmethodID create_bitmap = env->GetStaticMethodID(bitmap_cls, "createBitmap",
            "(Landroid/graphics/Bitmap;)Landroid/graphics/Bitmap;");
        if (create_bitmap && !env->ExceptionCheck()) {
            result = env->CallStaticObjectMethod(bitmap_cls, create_bitmap, bitmap);
            if (env->ExceptionCheck()) {
                env->ExceptionClear();
                result = nullptr;
            }
        }
        env->DeleteLocalRef(bitmap_cls);
    }

    if (!was_enabled) {
        env->CallVoidMethod(view, set_drawing_cache_enabled, JNI_FALSE);
    }

    env->DeleteLocalRef(vc);
    release(env, view);
    return result;
}

// 检测特定位置的颜色（用于界面识别）
static uint32_t get_pixel_color(JNIEnv *env, jobject bitmap, int x, int y) {
    if (!bitmap) return 0;

    jclass bitmap_cls = env->GetObjectClass(bitmap);
    jmethodID get_pixel = env->GetMethodID(bitmap_cls, "getPixel", "(II)I");
    jmethodID get_width = env->GetMethodID(bitmap_cls, "getWidth", "()I");
    jmethodID get_height = env->GetMethodID(bitmap_cls, "getHeight", "()I");

    if (!get_pixel || !get_width || !get_height) {
        env->DeleteLocalRef(bitmap_cls);
        return 0;
    }

    jint w = env->CallIntMethod(bitmap, get_width);
    jint h = env->CallIntMethod(bitmap, get_height);

    if (x < 0 || x >= w || y < 0 || y >= h) {
        env->DeleteLocalRef(bitmap_cls);
        return 0;
    }

    jint color = env->CallIntMethod(bitmap, get_pixel, x, y);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        color = 0;
    }

    env->DeleteLocalRef(bitmap_cls);
    return (uint32_t)color;
}

// 识别当前游戏界面类型
static GameState recognize_game_state(JNIEnv *env) {
    jobject bitmap = capture_screen_bitmap(env);
    if (!bitmap) {
        LOGI("recognize: no bitmap");
        return STATE_UNKNOWN;
    }

    jclass bitmap_cls = env->GetObjectClass(bitmap);
    jmethodID get_width = env->GetMethodID(bitmap_cls, "getWidth", "()I");
    jmethodID get_height = env->GetMethodID(bitmap_cls, "getHeight", "()I");

    if (!get_width || !get_height) {
        env->DeleteLocalRef(bitmap_cls);
        release(env, bitmap);
        return STATE_UNKNOWN;
    }

    jint width = env->CallIntMethod(bitmap, get_width);
    jint height = env->CallIntMethod(bitmap, get_height);
    env->DeleteLocalRef(bitmap_cls);

    LOGI("recognize: screen %dx%d", width, height);

    // 检测关键区域的颜色特征
    // 策略：检测按钮区域的亮度和色彩特征

    // 右下角区域（大部分按钮都在这里）
    int check_x = (int)(width * 0.85f);
    int check_y = (int)(height * 0.88f);

    // 左下角区域（一骑绝尘的取消按钮）
    int check_x_left = (int)(width * 0.31f);
    int check_y_left = (int)(height * 0.88f);

    // 屏幕中央区域
    int check_x_center = width / 2;
    int check_y_center = height / 2;

    uint32_t color_right = get_pixel_color(env, bitmap, check_x, check_y);
    uint32_t color_left = get_pixel_color(env, bitmap, check_x_left, check_y_left);
    uint32_t color_center = get_pixel_color(env, bitmap, check_x_center, check_y_center);

    release(env, bitmap);

    // 提取RGB分量
    int r_right = (color_right >> 16) & 0xFF;
    int g_right = (color_right >> 8) & 0xFF;
    int b_right = color_right & 0xFF;

    int r_left = (color_left >> 16) & 0xFF;
    int g_left = (color_left >> 8) & 0xFF;
    int b_left = color_left & 0xFF;

    int r_center = (color_center >> 16) & 0xFF;
    int g_center = (color_center >> 8) & 0xFF;
    int b_center = color_center & 0xFF;

    // 计算亮度
    int brightness_right = (r_right + g_right + b_right) / 3;
    int brightness_left = (r_left + g_left + b_left) / 3;
    int brightness_center = (r_center + g_center + b_center) / 3;

    LOGI("recognize: right_bright=%d left_bright=%d center_bright=%d",
         brightness_right, brightness_left, brightness_center);

    // 简化的识别逻辑：
    // 1. 如果右下角很亮（按钮区域）且左下角较暗 -> 结算页面或排行榜
    // 2. 如果左下角和右下角都亮 -> 一骑绝尘弹窗
    // 3. 如果整体较暗 -> 游戏中
    // 4. 其他情况 -> 匹配准备界面

    GameState state = STATE_UNKNOWN;

    if (brightness_left > 150 && brightness_right > 150) {
        // 左右按钮都亮 -> 一骑绝尘弹窗
        state = STATE_POPUP_YIQIJUECHEN;
        LOGI("recognize: detected POPUP (left and right buttons bright)");
    } else if (brightness_right > 150 && brightness_center < 100) {
        // 右下角亮，中央暗 -> 结算页面
        state = STATE_SETTLEMENT;
        LOGI("recognize: detected SETTLEMENT (right bright, center dark)");
    } else if (brightness_right > 120) {
        // 右下角较亮 -> 排行榜或匹配准备
        // 需要更细致的判断，这里简化处理
        static int settlement_count = 0;
        if (settlement_count > 0) {
            state = STATE_RANK;
            LOGI("recognize: detected RANK (after settlement)");
            settlement_count = 0;
        } else {
            state = STATE_MATCH_READY;
            LOGI("recognize: detected MATCH_READY (right moderately bright)");
        }
        if (g_current_state == STATE_SETTLEMENT) {
            settlement_count = 1;
        }
    } else {
        // 整体较暗 -> 游戏中
        state = STATE_IN_GAME;
        LOGI("recognize: detected IN_GAME (overall dark)");
    }

    g_current_state = state;
    return state;
}

// ----------------------------------------------------------- touch event ---
static void tap_impl(JNIEnv *env, float fx, float fy, int hold_ms) {
    jobject view = root_view(env);
    if (!view) return;

    jclass vc = env->GetObjectClass(view);
    jmethodID gw = env->GetMethodID(vc, "getWidth", "()I");
    jmethodID gh = env->GetMethodID(vc, "getHeight", "()I");
    jmethodID dispatch = env->GetMethodID(vc, "dispatchTouchEvent",
                                         "(Landroid/view/MotionEvent;)Z");
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

    // 添加随机位置偏移（反检测）
    float offset_x = 0.0f, offset_y = 0.0f;
    if (g_cfg.random_offset_pixels > 0) {
        int range = g_cfg.random_offset_pixels;
        offset_x = (float)random_range(-range, range) / (float)w;
        offset_y = (float)random_range(-range, range) / (float)h;
    }

    fx += offset_x;
    fy += offset_y;

    if (fx < 0.f) fx = 0.f; if (fx > 1.f) fx = 1.f;
    if (fy < 0.f) fy = 0.f; if (fy > 1.f) fy = 1.f;
    float x = (float) w * fx;
    float y = (float) h * fy;

    LOGI("tap: x=%.0f y=%.0f (%.3f, %.3f) w=%d h=%d", x, y, fx, fy, w, h);

    jclass mec = env->FindClass("android/view/MotionEvent");
    jmethodID obtain = env->GetStaticMethodID(
        mec, "obtain", "(JJIFFI)Landroid/view/MotionEvent;");
    jmethodID set_source = env->GetMethodID(mec, "setSource", "(I)V");
    jmethodID recycle = env->GetMethodID(mec, "recycle", "()V");
    long long down = 0, up = 0;
    {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        down = (long long) ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;

        // 添加随机按压时间变化（反检测）
        int hold_variation = random_range(-10, 10);
        int actual_hold = hold_ms + hold_variation;
        if (actual_hold < 20) actual_hold = 20;
        if (actual_hold > 150) actual_hold = 150;

        up = down + actual_hold;
    }
    if (env->ExceptionCheck() || !obtain) {
        env->ExceptionClear();
        goto done;
    }

    for (int phase = 0; phase < 2; phase++) {
        jint action = phase == 0 ? 0 /*DOWN*/ : 1 /*UP*/;
        long long when = phase == 0 ? down : up;
        jobject ev = env->CallStaticObjectMethod(mec, obtain,
                                                 (jlong) down, (jlong) when,
                                                 action, x, y, (jint) 0);
        if (env->ExceptionCheck() || !ev) {
            env->ExceptionClear();
            break;
        }
        if (set_source) env->CallVoidMethod(ev, set_source, 4098 /*TOUCHSCREEN*/);
        if (env->ExceptionCheck()) env->ExceptionClear();
        jboolean dispatched = env->CallBooleanMethod(view, dispatch, ev);
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
        }
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
    if (env->PushLocalFrame(64) != JNI_OK) {
        env->ExceptionClear();
        return;
    }
    tap_impl(env, fx, fy, hold_ms);
    if (env->ExceptionCheck()) env->ExceptionClear();
    env->PopLocalFrame(nullptr);
}

// ---------------------------------------------------- intelligent worker ---
// 智能识别模式工作线程
static void *smart_recognition_worker(void *arg) {
    TapCtx *ctx = (TapCtx *) arg;
    JNIEnv *env = nullptr;
    if (ctx->vm->GetEnv((void **) &env, JNI_VERSION_1_6) != JNI_OK) {
        if (ctx->vm->AttachCurrentThread(&env, nullptr) != JNI_OK) {
            LOGI("attach: failed");
            return nullptr;
        }
    }
    LOGI("attach: smart recognition mode");

    // 初始等待，让游戏完全加载
    LOGI("wait: 10s for game initialization");
    usleep(10 * 1000000);

    int consecutive_unknown = 0;
    GameState last_state = STATE_UNKNOWN;

    while (true) {
        // 识别当前界面
        GameState current = recognize_game_state(env);

        // 防止频繁误判：如果连续多次识别为UNKNOWN，则保持上一个有效状态
        if (current == STATE_UNKNOWN) {
            consecutive_unknown++;
            if (consecutive_unknown < 3) {
                current = last_state;
                LOGI("state: keep last state due to unknown");
            }
        } else {
            consecutive_unknown = 0;
            last_state = current;
        }

        // 根据识别结果执行对应操作
        float tap_x = 0.5f, tap_y = 0.5f;
        int should_tap = 1;
        const char *state_name = "UNKNOWN";

        switch (current) {
            case STATE_IN_GAME:
                // 游戏中：点击自动跑图位置
                tap_x = g_cfg.ingame_x;
                tap_y = g_cfg.ingame_y;
                state_name = "IN_GAME";
                break;

            case STATE_SETTLEMENT:
                // 结算页面：点击【点击继续】
                tap_x = g_cfg.continue_btn_x;
                tap_y = g_cfg.continue_btn_y;
                state_name = "SETTLEMENT";
                break;

            case STATE_RANK:
                // 排行榜：点击【再来一局】
                tap_x = g_cfg.again_btn_x;
                tap_y = g_cfg.again_btn_y;
                state_name = "RANK";
                break;

            case STATE_POPUP_YIQIJUECHEN:
                // 一骑绝尘弹窗：点击【取消】
                tap_x = g_cfg.cancel_btn_x;
                tap_y = g_cfg.cancel_btn_y;
                state_name = "POPUP";
                break;

            case STATE_MATCH_READY:
                // 匹配准备：点击【开始匹配】
                tap_x = g_cfg.match_btn_x;
                tap_y = g_cfg.match_btn_y;
                state_name = "MATCH_READY";
                break;

            default:
                should_tap = 0;
                break;
        }

        if (should_tap) {
            LOGI("action: state=%s tap=(%.2f, %.2f)", state_name, tap_x, tap_y);

            // 添加随机延迟（反检测）
            random_delay();

            // 执行点击
            tap(env, tap_x, tap_y, g_cfg.hold_time_ms);
        } else {
            LOGI("action: state=UNKNOWN, skip tap");
        }

        // 等待下一次检测（添加随机延迟）
        int base_interval = g_cfg.check_interval_ms;
        int random_extra = random_range(0, 1000); // 0-1秒随机延迟
        int total_interval = base_interval + random_extra;

        LOGI("wait: %dms until next check", total_interval);
        usleep(total_interval * 1000);
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

        if (g_cfg.recognition_enabled) {
            LOGI("start: smart recognition mode");
            pthread_create(&t, &a, smart_recognition_worker, &ctx);
        } else {
            LOGI("start: recognition disabled");
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
        LOGI("init: smart recognition v3");

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
        LOGI("init: image %zu bytes", size);
    }

    void postAppSpecialize(const zygisk::AppSpecializeArgs *) override {
        if (!g_want || !g_img) return;

        JavaVM *vm = g_vm;
        if (!vm) { LOGI("init: no vm"); return; }

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
