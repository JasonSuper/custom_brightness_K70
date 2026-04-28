/**
 * @file main.c
 * @brief K70亮度增强 - C语言版本
 *
 * 功能特性:
 * - 双光线传感器融合（前置 + 后置）：智能加权融合前后传感器数据
 * - 滑动窗口均值滤波：维护最近N个光线值，过滤瞬时噪声
 * - 线性趋势预测：基于最小二乘法预判下一刻光线，提前平滑调整
 * - 突变检测过滤：单次变化超过阈值时判定为瞬时突变，暂不调整
 * - 非对称滞回阈值：亮度提升时低阈值响应快，变暗时高阈值防抖动
 * - 分段线性亮度转换：根据不同光线环境使用不同的亮度映射曲线
 * - 自动检测背光节点：按优先级检测常见的背光路径
 * - 日志记录系统：带时间戳的详细日志，支持日志文件自动截断
 * - 系统自动亮度检测：当系统开启自动亮度时，暂停自定义逻辑
 * - 系统UI亮度条同步：确保亮度调整与系统UI保持一致
 *
 * @author 魔力王
 * @version v1.7.6
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <signal.h>
#include <stdarg.h>
#include <errno.h>
#include <stdint.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <android/sensor.h>
#include <android/looper.h>
#include <dlfcn.h>

// ==================== 日志系统 ====================

#define LV_ERR  0
#define LV_WARN 1
#define LV_INFO 2
#define LV_DBG  3

#ifndef LOG
#define LOG LV_INFO
#endif

static int    g_log_level   = LOG;
static FILE  *g_log_fp      = NULL;
static char   g_log_buf[4096];
static size_t g_log_buf_pos = 0;
static char   g_log_path[256]        = "/data/local/tmp/brightness_curve.log";
static char   g_log_path_old[256]    = "/data/local/tmp/brightness_curve.log.old";
static size_t g_log_max_size        = 131072;
static size_t g_log_buf_size        = 4096;

#define LOGE(...) do { if (g_log_level >= LV_ERR)  log_msg(__VA_ARGS__); } while (0)
#define LOGW(...) do { if (g_log_level >= LV_WARN) log_msg(__VA_ARGS__); } while (0)
#define LOGI(...) do { if (g_log_level >= LV_INFO) log_msg(__VA_ARGS__); } while (0)
#define LOGD(...) do { if (g_log_level >= LV_DBG)  log_msg(__VA_ARGS__); } while (0)

/**
 * @brief 将缓冲区内容刷入日志文件，并在超出上限时执行日志轮转
 */
static void log_flush(void) {
    if (!g_log_fp || g_log_buf_pos == 0) return;

    fwrite(g_log_buf, 1, g_log_buf_pos, g_log_fp);
    fflush(g_log_fp);
    g_log_buf_pos = 0;

    if (ftell(g_log_fp) > (long)g_log_max_size) {
        fclose(g_log_fp);
        unlink(g_log_path_old);
        rename(g_log_path, g_log_path_old);
        g_log_fp = fopen(g_log_path, "a");
    }
}

/**
 * @brief 写入带时间戳的日志消息
 * @param fmt printf 格式字符串
 */
static void log_msg(const char *fmt, ...) {
    if (!g_log_fp) return;

    char tmp[512];
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    int len = snprintf(tmp, sizeof(tmp), "[%02d:%02d:%02d] ",
                       t->tm_hour, t->tm_min, t->tm_sec);

    va_list ap;
    va_start(ap, fmt);
    len += vsnprintf(tmp + len, sizeof(tmp) - len - 1, fmt, ap);
    va_end(ap);

    tmp[len++] = '\n';

    if (g_log_buf_pos + (size_t)len > g_log_buf_size) {
        log_flush();
    }
    memcpy(g_log_buf + g_log_buf_pos, tmp, len);
    g_log_buf_pos += len;
    log_flush(); // 每条消息立即落盘，保证日志完整性
}

// ==================== 亮度配置 ====================

#define PATH_LEN 256

typedef struct {
    // 本分段对应的 lux 下限（含）
    int lux_min;
    // 本分段对应的 lux 上限（含）
    int lux_max;
    // 分段起点亮度（与 lux_min 对应）
    int brightness_min;
    // 分段终点亮度（与 lux_max 对应）
    int brightness_max;
} BrightnessSegment;

static BrightnessSegment *g_segments       = NULL;
static size_t             g_segments_count = 0;
static float              g_hysteresis_up   = 0.1f;
static float              g_hysteresis_down = 0.2f;

// 滑动窗口配置
static int   g_window_size            = 7;
static float g_spurious_threshold     = 0.3f;

// 自动亮度检测配置
static int g_auto_brightness_check_interval_seconds = 5;

// 亮度融合算法配置
static float g_back_lux_threshold_ratio = 50.0f;
static float g_back_lux_fusion_ratio     = 2.0f;
static float g_back_lux_weight           = 0.3f;
static float g_front_lux_weight          = 0.2f;
static float g_max_lux                   = 2400.0f;

// 趋势预测配置
static float g_window_mean_weight       = 0.7f;
static float g_prediction_weight         = 0.3f;
static float g_min_change_threshold     = 5.0f;
static float g_change_threshold_percent  = 0.05f;

// 亮度调整配置
static int   g_min_brightness_delta          = 2;
static int   g_max_lux_for_brightness        = 2400;

// 传感器配置
static int g_sensor_sample_rate_ms   = 200;
static int g_startup_delay_seconds  = 5;
static int g_poll_interval_ms       = 100;

/**
 * @brief 从配置文件读取所有配置参数
 * @param path 配置文件路径（config.conf）
 * @return 1 = 成功，0 = 失败
 */
static int read_config(const char *path) {
    FILE *fp = fopen(path, "r");
    if (!fp) return 0;

    char line[512];

    int seg_count = 0;
    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "segments.", 9) == 0) seg_count++;
    }

    if (seg_count > 0) {
        g_segments = (BrightnessSegment *)malloc(seg_count * sizeof(BrightnessSegment));
        if (!g_segments) { fclose(fp); return 0; }
    }

    fseek(fp, 0, SEEK_SET);
    size_t idx = 0;
    while (fgets(line, sizeof(line), fp)) {
        char *comment = strchr(line, '#');
        if (comment) *comment = '\0';

        if (strncmp(line, "hysteresis_up=", 14) == 0) {
            sscanf(line + 14, "%f", &g_hysteresis_up);
        } else if (strncmp(line, "hysteresis_down=", 16) == 0) {
            sscanf(line + 16, "%f", &g_hysteresis_down);
        } else if (strncmp(line, "window_size=", 12) == 0) {
            sscanf(line + 12, "%d", &g_window_size);
        } else if (strncmp(line, "spurious_threshold=", 19) == 0) {
            sscanf(line + 19, "%f", &g_spurious_threshold);
        } else if (strncmp(line, "auto_brightness_check_interval_seconds=", 39) == 0) {
            sscanf(line + 39, "%d", &g_auto_brightness_check_interval_seconds);
        } else if (strncmp(line, "back_lux_threshold_ratio=", 24) == 0) {
            sscanf(line + 24, "%f", &g_back_lux_threshold_ratio);
        } else if (strncmp(line, "back_lux_fusion_ratio=", 21) == 0) {
            sscanf(line + 21, "%f", &g_back_lux_fusion_ratio);
        } else if (strncmp(line, "back_lux_weight=", 16) == 0) {
            sscanf(line + 16, "%f", &g_back_lux_weight);
        } else if (strncmp(line, "front_lux_weight=", 17) == 0) {
            sscanf(line + 17, "%f", &g_front_lux_weight);
        } else if (strncmp(line, "max_lux=", 8) == 0) {
            sscanf(line + 8, "%f", &g_max_lux);
        } else if (strncmp(line, "window_mean_weight=", 18) == 0) {
            sscanf(line + 18, "%f", &g_window_mean_weight);
        } else if (strncmp(line, "prediction_weight=", 17) == 0) {
            sscanf(line + 17, "%f", &g_prediction_weight);
        } else if (strncmp(line, "min_change_threshold=", 20) == 0) {
            sscanf(line + 20, "%f", &g_min_change_threshold);
        } else if (strncmp(line, "change_threshold_percent=", 24) == 0) {
            sscanf(line + 24, "%f", &g_change_threshold_percent);
        } else if (strncmp(line, "min_brightness_delta=", 21) == 0) {
            sscanf(line + 21, "%d", &g_min_brightness_delta);
        } else if (strncmp(line, "max_lux_for_brightness=", 23) == 0) {
            sscanf(line + 23, "%d", &g_max_lux_for_brightness);
        } else if (strncmp(line, "sensor_sample_rate_ms=", 21) == 0) {
            sscanf(line + 21, "%d", &g_sensor_sample_rate_ms);
        } else if (strncmp(line, "startup_delay_seconds=", 21) == 0) {
            sscanf(line + 21, "%d", &g_startup_delay_seconds);
        } else if (strncmp(line, "poll_interval_ms=", 16) == 0) {
            sscanf(line + 16, "%d", &g_poll_interval_ms);
        } else if (strncmp(line, "log_path=", 9) == 0) {
            sscanf(line + 9, "%255s", g_log_path);
        } else if (strncmp(line, "log_max_size=", 13) == 0) {
            sscanf(line + 13, "%zu", &g_log_max_size);
        } else if (strncmp(line, "segments.", 9) == 0 && idx < (size_t)seg_count) {
            char *eq = strchr(line, '=');
            if (eq) {
                BrightnessSegment *s = &g_segments[idx];
                if (sscanf(eq + 1, "%d,%d,%d,%d",
                           &s->lux_min, &s->lux_max,
                           &s->brightness_min, &s->brightness_max) == 4) {
                    idx++;
                }
            }
        }
    }

    fclose(fp);
    g_segments_count = idx;
    return (idx > 0) ? 1 : 0;
}

// ==================== 运行时配置 ====================

/**
 * @brief 运行时检测到的硬件配置
 */
typedef struct {
    char backlight_path[PATH_LEN];
    int  poll_interval_ms;
} RuntimeConfig;

static RuntimeConfig g_config;

// ==================== 滑动窗口 ====================

// 最近一次读取到的前置传感器 lux
static float        g_front_lux       = 0.0f;
// 最近一次读取到的后置传感器 lux
static float        g_back_lux        = 0.0f;
// 最近一次成功下发到系统的亮度值
static int          g_last_brightness = 1;

// lux 滑动窗口（最大容量 100，实际使用 g_window_size）
static float        g_lux_window[100];
// 环形缓冲当前写入索引
static int          g_window_idx = 0;
// 是否已经填满过至少一轮窗口
static int          g_window_filled = 0;

// 自动亮度状态缓存，避免频繁调用 settings 命令
static int      g_auto_brightness_cached   = 0;
// 上次检测自动亮度状态的单调时钟纳秒时间
static uint64_t g_auto_brightness_check_ns = 0;
// 自动亮度状态日志去重标记：-1=未记录，0=关闭，1=开启
static int      g_auto_brightness_logged   = -1;
static volatile int g_running = 1;

// ==================== 配置文件监控 (inotify) ====================

static int       g_inotify_fd = -1;
static char      g_config_path[256];
static time_t    g_config_mtime = 0;

/**
 * @brief 初始化 inotify 监控
 */
static int init_inotify(const char *config_path) {
    strncpy(g_config_path, config_path, sizeof(g_config_path) - 1);
    g_config_path[sizeof(g_config_path) - 1] = '\0';
    
    g_inotify_fd = inotify_init1(IN_NONBLOCK);
    if (g_inotify_fd < 0) {
        LOGW("⚠️ inotify 初始化失败: %s", strerror(errno));
        return -1;
    }
    
    int wd = inotify_add_watch(g_inotify_fd, config_path, IN_MODIFY | IN_MOVE_SELF);
    if (wd < 0) {
        LOGW("⚠️ inotify_add_watch 失败: %s", strerror(errno));
        close(g_inotify_fd);
        g_inotify_fd = -1;
        return -1;
    }
    
    struct stat st;
    if (stat(config_path, &st) == 0) {
        g_config_mtime = st.st_mtime;
    }
    
    LOGI("🔔 inotify 监控已启动: %s", config_path);
    return g_inotify_fd;
}

/**
 * @brief 检查配置文件是否被修改
 * @return 1 = 已修改需要重新加载, 0 = 未修改
 */
static int check_config_changed(void) {
    if (g_inotify_fd < 0) return 0;
    
    char buf[256];
    int len = read(g_inotify_fd, buf, sizeof(buf));
    
    if (len <= 0) {
        struct stat st;
        if (stat(g_config_path, &st) == 0 && st.st_mtime != g_config_mtime) {
            g_config_mtime = st.st_mtime;
            return 1;
        }
        return 0;
    }
    
    for (int i = 0; i < len;) {
        struct inotify_event *ev = (struct inotify_event *)&buf[i];
        if (ev->mask & (IN_MODIFY | IN_MOVE_SELF)) {
            struct stat st;
            if (stat(g_config_path, &st) == 0 && st.st_mtime != g_config_mtime) {
                g_config_mtime = st.st_mtime;
                LOGI("🔔 配置文件已修改，准备重新加载");
                return 1;
            }
        }
        i += sizeof(struct inotify_event) + ev->len;
    }
    
    return 0;
}

/**
 * @brief 重新加载配置文件
 */
static void reload_config(void) {
    LOGI("🔄 重新加载配置文件...");
    
    if (g_segments) {
        free(g_segments);
        g_segments = NULL;
        g_segments_count = 0;
    }
    
    if (read_config(g_config_path)) {
        LOGI("✅ 配置文件重新加载成功");
    } else {
        LOGE("❌ 配置文件重新加载失败");
    }
}

// ==================== 工具函数 ====================

/**
 * @brief 获取单调时钟纳秒时间戳
 */
static uint64_t monotonic_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

/**
 * @brief 读取 sysfs 数值节点
 * @param path 节点路径
 * @return 读取到的整数值，失败返回 -1
 */
static int sysfs_read(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    char buf[32];
    int n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return -1;
    buf[n] = '\0';
    long v = atol(buf);
    if (v < 0) v = 0;
    if (v > 2147483647L) v = 2147483647L;
    return (int)v;
}

/**
 * @brief 写入整数值到 sysfs 节点
 * @param path 节点路径
 * @param value 待写入的值
 * @return 写入字节数，失败返回 -1
 */
static int sysfs_write(const char *path, int value) {
    int fd = open(path, O_WRONLY);
    if (fd < 0) return -1;
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", value);
    int ret = write(fd, buf, strlen(buf));
    close(fd);
    return ret;
}

/**
 * @brief 通过 settings 命令同步系统亮度条 UI
 * @param brightness 目标亮度值（0-255）
 */
static void sync_brightness_ui(int brightness) {
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "settings put system screen_brightness %d", brightness);
    system(cmd);
}

/**
 * @brief 查询系统自动亮度开关状态（带缓存）
 * @return 1 = 已开启，0 = 已关闭
 */
static int is_auto_brightness_on(void) {
    uint64_t now = monotonic_ns();
    uint64_t check_interval_ns = (uint64_t)g_auto_brightness_check_interval_seconds * 1000000000ULL;
    if (g_auto_brightness_check_ns != 0 &&
        (now - g_auto_brightness_check_ns) < check_interval_ns) {
        return g_auto_brightness_cached;
    }

    FILE *fp = popen("settings get system screen_brightness_mode", "r");
    if (!fp) return g_auto_brightness_cached;

    char buf[16] = {0};
    int mode = 0;
    if (fgets(buf, sizeof(buf), fp)) {
        mode = (atoi(buf) == 1) ? 1 : 0;
    }
    pclose(fp);

    g_auto_brightness_cached   = mode;
    g_auto_brightness_check_ns = now;
    return mode;
}

// ==================== 亮度计算 ====================

/**
 * @brief 融合前后光线传感器数据，得到最终用于亮度计算的 lux
 *
 * 融合策略：
 * 1) 若后置数据远高于前置（超过阈值比），判为后置异常，直接使用前置；
 * 2) 若后置明显高于前置，则采用较高权重参与融合；
 * 3) 否则后置以较低权重参与，避免过度放大背面反光影响。
 *
 * @return 夹紧到 [0, g_max_lux] 的融合 lux
 */
static float calculate_fused_lux(void) {
    float lux;
    if (g_back_lux > (g_front_lux + 1.0f) * g_back_lux_threshold_ratio) {
        lux = g_front_lux;
    } else if (g_back_lux >= g_front_lux * g_back_lux_fusion_ratio) {
        lux = g_front_lux + g_back_lux * g_back_lux_weight;
    } else {
        lux = g_front_lux + g_back_lux * g_front_lux_weight;
    }
    if (lux > g_max_lux) lux = g_max_lux;
    if (lux < 0.0f)    lux = 0.0f;
    return lux;
}

/**
 * @brief 将 lux 映射到屏幕亮度（分段线性）
 * @param lux 输入光照强度
 * @return 亮度值（通常 1-255）
 */
static int lux_to_brightness(float lux) {
    int lux_i = (int)lux;
    if (lux_i > g_max_lux_for_brightness) return 255;

    for (size_t i = 0; i < g_segments_count; i++) {
        const BrightnessSegment *s = &g_segments[i];
        if (lux_i >= s->lux_min && lux_i <= s->lux_max) {
            int range_in  = s->lux_max - s->lux_min;
            int range_out = s->brightness_max - s->brightness_min;
            return s->brightness_min + (lux_i - s->lux_min) * range_out / range_in;
        }
    }
    return 1;
}

/**
 * @brief 判断当前是否应触发亮度调整，并输出“有效 lux”
 *
 * 处理流程：
 * - 将当前 lux 写入滑动窗口，计算窗口均值；
 * - 通过“瞬时突变阈值”过滤毛刺；
 * - 对窗口数据做线性回归，预测下一时刻 lux；
 * - 按权重融合“窗口均值 + 预测值”得到 effective_lux；
 * - 与上次调整时的 lux 比较，达到动态阈值才允许调整。
 *
 * @param lux 当前融合 lux
 * @param has_new_sample 本轮是否收到新的传感器采样
 * @param out_effective_lux 输出有效 lux（仅当返回 1 时有效）
 * @return 1 = 应调整，0 = 暂不调整
 */
static int should_adjust_brightness(float lux, int has_new_sample, float *out_effective_lux) {
    static float last_adjusted_lux = 0.0f;
    static uint64_t last_window_push_ns = 0;
    int push_count = 0;

    uint64_t now_ns = monotonic_ns();
    uint64_t sample_interval_ns = (uint64_t)g_sensor_sample_rate_ms * 1000000ULL;
    if (sample_interval_ns == 0) sample_interval_ns = 1;

    if (has_new_sample) {
        // 收到新采样时，立即入窗一个点
        push_count = 1;
        last_window_push_ns = now_ns;
    } else {
        // 静止时传感器可能停采：按采样周期用当前 lux 补齐窗口，避免窗口长期停滞
        if (last_window_push_ns == 0) {
            last_window_push_ns = now_ns;
            return 0;
        }
        uint64_t elapsed = now_ns - last_window_push_ns;
        push_count = (int)(elapsed / sample_interval_ns);
        if (push_count <= 0) {
            return 0;
        }
        if (push_count > g_window_size) {
            push_count = g_window_size;
        }
        last_window_push_ns += (uint64_t)push_count * sample_interval_ns;
    }

    for (int n = 0; n < push_count; n++) {
        g_lux_window[g_window_idx] = lux;
        g_window_idx = (g_window_idx + 1) % g_window_size;
        if (g_window_idx == 0) {
            g_window_filled = 1;
        }
    }

    int count = g_window_filled ? g_window_size : g_window_idx;
    if (!g_window_filled && g_window_idx == 0) {
        if (last_adjusted_lux == 0.0f) {
            last_adjusted_lux = lux;
            *out_effective_lux = lux;
            return 1;
        }
        return 0;
    }

    float sum = 0.0f;
    for (int i = 0; i < count; i++) {
        sum += g_lux_window[i];
    }
    float window_mean = sum / count;

    float threshold = window_mean * g_spurious_threshold;
    if (fabsf(lux - window_mean) > threshold) {
        LOGI("检测到瞬时突变，跳过调整 (当前=%.1f, 均值=%.1f)", lux, window_mean);
        return 0;
    }

    // 最小二乘法回归：y = slope * x + intercept
    float sum_x = 0.0f, sum_y = 0.0f, sum_xy = 0.0f, sum_x2 = 0.0f;
    for (int i = 0; i < count; i++) {
        float x = (float)i;
        float y = g_lux_window[i];
        sum_x += x;
        sum_y += y;
        sum_xy += x * y;
        sum_x2 += x * x;
    }

    float slope = 0.0f, intercept = sum_y / count;
    float denominator = (float)count * sum_x2 - sum_x * sum_x;
    if (fabsf(denominator) > 1e-6f) {
        slope = ((float)count * sum_xy - sum_x * sum_y) / denominator;
        intercept = (sum_y - slope * sum_x) / count;
    }

    // 预测窗口下一个采样点的 lux 值
    float predicted_lux = intercept + slope * (float)count;

    // 结合“稳定性（均值）”和“前瞻性（预测）”得到最终决策 lux
    float effective_lux = window_mean * g_window_mean_weight + predicted_lux * g_prediction_weight;

    float change_threshold = effective_lux * g_change_threshold_percent;
    if (change_threshold < g_min_change_threshold) change_threshold = g_min_change_threshold;

    if (fabsf(effective_lux - last_adjusted_lux) >= change_threshold) {
        last_adjusted_lux = effective_lux;
        *out_effective_lux = effective_lux;
        LOGI("调整亮度: 有效lux=%.1f (均值=%.1f, 预测=%.1f)",
             effective_lux, window_mean, predicted_lux);
        return 1;
    }

    return 0;
}

// ==================== 硬件检测 ====================

/**
 * @brief 自动检测背光节点路径，并读取最大亮度值
 *
 * 按优先级依次检测以下路径，找到第一个可访问的节点后停止：
 *  1. /sys/class/backlight/panel0-backlight/brightness
 *  2. /sys/class/backlight/panel1-backlight/brightness
 *  3. /sys/class/leds/lcd-backlight/brightness
 *  4. /sys/class/backlight/backlight/brightness
 *
 * 若全部未找到则退出程序。
 */
static void detect_backlight(void) {
    static const char *candidates[] = {
        "/sys/class/backlight/panel0-backlight/brightness",
        "/sys/class/backlight/panel1-backlight/brightness",
        "/sys/class/leds/lcd-backlight/brightness",
        "/sys/class/backlight/backlight/brightness",
        NULL
    };

    for (int i = 0; candidates[i]; i++) {
        if (access(candidates[i], F_OK) == 0) {
            strncpy(g_config.backlight_path, candidates[i], PATH_LEN - 1);
            g_config.backlight_path[PATH_LEN - 1] = '\0';
            LOGI("💡 背光节点: %s", g_config.backlight_path);
            return;
        }
    }

    LOGE("❌ 未找到任何背光节点，程序退出");
    exit(1);
}

// ==================== 传感器初始化 ====================

static ASensorManager    *g_sensor_mgr   = NULL;
static ASensorEventQueue *g_sensor_queue = NULL;
static ALooper           *g_looper       = NULL;

/**
 * @brief 初始化 Android 传感器，注册前置和后置光线传感器
 *
 * 后置光线传感器的私有 type 值为 33171055（厂商自定义）。
 * 若前置传感器不存在则退出，后置传感器缺失时仅打印警告。
 *
 * @return 前置传感器指针（成功），NULL（失败，程序已 exit）
 */
static const ASensor *init_sensors(void) {
    g_sensor_mgr = ASensorManager_getInstance();
    if (!g_sensor_mgr) {
        LOGE("❌ 无法获取 SensorManager，程序退出");
        exit(1);
    }

    g_looper       = ALooper_prepare(ALOOPER_PREPARE_ALLOW_NON_CALLBACKS);
    g_sensor_queue = ASensorManager_createEventQueue(g_sensor_mgr, g_looper, 0, NULL, NULL);

    const ASensor *front_sensor = NULL;
    const ASensor *back_sensor  = NULL;
    ASensorList list;
    int count = ASensorManager_getSensorList(g_sensor_mgr, &list);

    for (int i = 0; i < count; i++) {
        int type = ASensor_getType(list[i]);
        if (type == ASENSOR_TYPE_LIGHT && !front_sensor) {
            front_sensor = list[i];
        } else if (type == 33171055 && !back_sensor) {
            back_sensor = list[i];
        }
        if (front_sensor && back_sensor) break;
    }

    if (!front_sensor) {
        LOGE("❌ 未找到前置光线传感器，程序退出");
        exit(1);
    }
    if (!back_sensor) {
        LOGW("⚠️  未找到后置光线传感器，将仅使用前置传感器");
    }

    ASensorEventQueue_enableSensor(g_sensor_queue, front_sensor);
    ASensorEventQueue_setEventRate(g_sensor_queue, front_sensor, g_sensor_sample_rate_ms * 1000);
    if (back_sensor) {
        ASensorEventQueue_enableSensor(g_sensor_queue, back_sensor);
        ASensorEventQueue_setEventRate(g_sensor_queue, back_sensor, g_sensor_sample_rate_ms * 1000);
    }

    LOGI("✅ 前光传感器: %s", ASensor_getName(front_sensor));
    if (back_sensor) LOGI("✅ 后光传感器: %s", ASensor_getName(back_sensor));

    return front_sensor;
}

// ==================== 清理 ====================

/**
 * @brief 释放传感器、Looper 和日志文件等所有资源
 */
static void cleanup(void) {
    if (g_sensor_queue && g_sensor_mgr) {
        ASensorManager_destroyEventQueue(g_sensor_mgr, g_sensor_queue);
        g_sensor_queue = NULL;
    }
    if (g_looper) {
        ALooper_release(g_looper);
        g_looper = NULL;
    }
    if (g_log_fp) {
        log_flush();
        fclose(g_log_fp);
        g_log_fp = NULL;
    }
}

// ==================== 主循环 ====================

/**
 * @brief 亮度调节主循环
 *
 * 持续轮询传感器事件，计算融合 lux，当 lux 稳定且变化幅度超过动态阈值时
 * 更新系统亮度。
 */
static void run(void) {
    LOGI("===== K70亮度增强 C版 v1.7.6 启动 =====");
    LOGI("✅ 背光节点=%s", g_config.backlight_path);

    sleep(g_startup_delay_seconds);

    ASensorEvent event;
    while (g_running) {
        ALooper_pollOnce(g_poll_interval_ms, NULL, NULL, NULL);

        int has_new_sample = 0;
        // 消费所有可用的传感器事件
        while (ASensorEventQueue_getEvents(g_sensor_queue, &event, 1) > 0) {
            has_new_sample = 1;
            if (event.type == ASENSOR_TYPE_LIGHT) {
                g_front_lux = (float)(int)event.light;
                LOGI("前传感器 lux=%.1f", g_front_lux);
            } else if (event.type == 33171055) {
                g_back_lux = (float)(int)event.light;
                LOGI("后传感器 lux=%.1f", g_back_lux);
            }
        }

        // 检查配置文件是否被修改
        if (check_config_changed()) {
            reload_config();
        }

        // 系统自动亮度开启时，让出控制权
        int auto_on = is_auto_brightness_on();
        if (auto_on) {
            if (g_auto_brightness_logged != 1) {
                LOGI("🤖 系统自动亮度已开启，暂停自定义调节");
                g_auto_brightness_logged = 1;
            }
            sleep(2);
            continue;
        }
        if (g_auto_brightness_logged != 0) {
            LOGI("🤖 系统自动亮度已关闭，启用自定义调节");
            g_auto_brightness_logged = 0;
        }

        // 计算融合 lux
        float fused_lux = calculate_fused_lux();
        float effective_lux = 0.0f;

        // 使用滑动窗口模式判断是否需要调整亮度
        if (!should_adjust_brightness(fused_lux, has_new_sample, &effective_lux)) {
            continue;
        }

        // 计算目标亮度
        int new_brightness = lux_to_brightness(effective_lux);
        if (new_brightness < 1) new_brightness = 1;

        // 非对称滞回阈值：提升时低阈值，变暗时高阈值
        int diff = abs(new_brightness - g_last_brightness);
        int min_delta = g_min_brightness_delta;
        float factor = (new_brightness > g_last_brightness) ? g_hysteresis_up : g_hysteresis_down;
        int dyn_delta = (int)(g_last_brightness * factor);
        int threshold = (dyn_delta > min_delta) ? dyn_delta : min_delta;

        if (diff >= threshold) {
            sync_brightness_ui(new_brightness);
            g_last_brightness = new_brightness;
            LOGI("✅ 更新亮度=%d (有效lux=%.1f, 原始lux=%.1f)", 
                 new_brightness, effective_lux, fused_lux);
        }
    }

    cleanup();
}

// ==================== 程序入口 ====================

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "用法: %s <config.conf>\n", argv[0]);
        return 1;
    }

    g_log_fp = fopen(g_log_path, "a");
    if (!g_log_fp) {
        fprintf(stderr, "无法打开日志文件: %s\n", g_log_path);
        return 1;
    }
    LOGI("💡 服务启动");

    // 读取配置文件
    if (!read_config(argv[1])) {
        LOGE("❌ 无法从 %s 读取配置，退出程序", argv[1]);
        log_flush();
        fclose(g_log_fp);
        g_log_fp = NULL;
        return 1;
    }
    LOGI("✅ 配置已加载");

    // 初始化 inotify 监控配置文件变化
    init_inotify(argv[1]);

    // 检测硬件
    detect_backlight();

    // 初始化传感器
    init_sensors();

    // 进入主循环
    run();

    // 释放配置内存
    if (g_segments) {
        free(g_segments);
        g_segments = NULL;
    }

    LOGI("✅ 服务正常退出");
    return 0;
}
