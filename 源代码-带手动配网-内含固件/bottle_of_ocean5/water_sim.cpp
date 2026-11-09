#include "water_sim.h"
#include "flip.h"
#include "gravity.h"
#include "panel_config.h"
#include "rgb.h"
#include "time_sync.h"
#include <math.h>
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define W PANEL_WIDTH
#define H PANEL_HEIGHT
#define LED_COUNT PANEL_LED_COUNT

#define SIM_FPS 30

#define LED_VAL_MAX_I PANEL_LED_VALUE_MAX
#define LED_VAL_MAX_F ((float)PANEL_LED_VALUE_MAX)
#define LED_LEVELS (LED_VAL_MAX_I + 1)

#define PAL_N 6
#define PALETTE_COUNT 3

// ------------------ 潮汐控制相关常量（你可以修改这两个值）------------------
// 最高潮时对应的水量倍率（相对于创建 FlipFluid 时的“基础水量”）：
// 1.0 表示“基础水量”，>1.0 表示比现在更多的水（上限受内部 max_particles 限制）
#define TIDE_MAX_FILL_RATIO 1.4f

// 最低潮时对应的水量倍率：
// 0.3f 表示最低潮时只保留约 30% 的基础水量，你可以根据喜好改成 0.1f~0.9f 等。
#define TIDE_MIN_FILL_RATIO 0.5f

typedef struct {
    uint8_t r, g, b;
} rgb8_t;

static rgb8_t s_pal_lut[PALETTE_COUNT][LED_LEVELS];
static uint8_t s_lut_ready[PALETTE_COUNT];
static uint8_t s_palette_idx = 0;//可在此选择不同调色板（0~2）

static TaskHandle_t s_task = NULL;
static volatile bool s_running = false;

static const rgb8_t PALETTES[PALETTE_COUNT][PAL_N] = {
    {
        {0, 0, 0},//实际灯珠颜色顺序：GRB。在此调整B的数值会影响蓝色随时间变化的强度
        {255, 0, 100},
        {255, 0, 100},
        {255, 0, 100},
        {255, 0, 100},
        {255, 0, 100},  
    },
    {
        {0, 0, 0},
        {100, 0, 255},
        {100, 0, 255},
        {100, 0, 255},
        {100, 0, 255},
        {100, 0, 255},  
    },
    {
        {0, 0, 0},
        {20, 255, 30},
        {15, 255, 20},
        {10, 255, 10},
        {5, 255, 5},
        {0, 255, 0},
    },
};

static inline rgb8_t lerp_rgb(rgb8_t a, rgb8_t b, float t) {
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;

    rgb8_t o;
    o.r = (uint8_t)lrintf((float)a.r + ((float)b.r - (float)a.r) * t);
    o.g = (uint8_t)lrintf((float)a.g + ((float)b.g - (float)a.g) * t);
    o.b = (uint8_t)lrintf((float)a.b + ((float)b.b - (float)a.b) * t);
    return o;
}

static void build_palette_lut(uint8_t pal_idx) {
    const rgb8_t* pal = PALETTES[pal_idx];

    for (int lv = 0; lv < LED_LEVELS; lv++) {
        float t = (LED_VAL_MAX_F > 0.0f) ? ((float)lv / LED_VAL_MAX_F) : 0.0f;
        float p = t * (float)(PAL_N - 1);
        int i0 = (int)p;
        int i1 = (i0 + 1 < PAL_N) ? (i0 + 1) : i0;
        float ft = p - (float)i0;

        rgb8_t c = lerp_rgb(pal[i0], pal[i1], ft);
        uint8_t m = c.r;
        if (c.g > m) m = c.g;
        if (c.b > m) m = c.b;

        if (m > 0) {
            float k = (float)lv / (float)m;
            c.r = (uint8_t)lrintf((float)c.r * k);
            c.g = (uint8_t)lrintf((float)c.g * k);
            c.b = (uint8_t)lrintf((float)c.b * k);
        } else {
            c.r = 0;
            c.g = 0;
            c.b = 0;
        }

        s_pal_lut[pal_idx][lv] = c;
    }

    s_lut_ready[pal_idx] = 1;
}

static inline float grid_get(const float* grid, int x, int y) {
    return grid[x * H + y];
}

static void sim_task(void* arg) {
    (void)arg;

    FlipFluid* f = NULL;
    // 模拟网格与面板分辨率一致：W × H
    float grid[LED_COUNT];

    rgb_init();

    for (int i = 0; i < PALETTE_COUNT; i++) {
        build_palette_lut((uint8_t)i);
    }

    const TickType_t frame_ticks = pdMS_TO_TICKS(1000 / SIM_FPS);
    const float dt = 1.0f / (float)SIM_FPS;
    TickType_t last_wake = xTaskGetTickCount();

    // 让内部网格就是 W×H（无拉伸、无缩放），并保持方格 spacing
    const float sim_w = 1.0f;
    const float sim_h = sim_w * ((float)(H + 1) / (float)(W + 1));
    f = flip_create(sim_w, sim_h, W, H, 0.6f);
    if (f) {
        flip_set_gravity_scale(f, 9.81f);
        flip_set_solver_quality(f, 1, 10, 0.9f);

        while (s_running) {
            vTaskDelayUntil(&last_wake, frame_ticks);

            gravity_xy_t g = gravity_get();
            float gx = g.valid ? g.gx : 0.0f;
            float gy = g.valid ? g.gy : 0.0f;

            // 根据当前时间计算潮汐因子（0.0 = 最低潮, 1.0 = 最高潮），
            // 并在每一帧更新内部粒子数量，实现真实“水量”随潮汐变化。
            float tide = time_sync_get_tide_level();
            flip_set_tide_level(f, tide, TIDE_MIN_FILL_RATIO, TIDE_MAX_FILL_RATIO);

            flip_step(f, dt, gx, gy);
            flip_get_led_grid(f, grid, W, H);

            // 根据当前本地时间计算蓝色强度（0~255）
            uint8_t blue_by_time = time_sync_get_blue();

            for (int y = 0; y < H; y++) {
                for (int x = 0; x < W; x++) {
                    float v = grid_get(grid, x, y);
                    if (!isfinite(v)) v = 0.0f;
                    if (v < 0.0f) v = 0.0f;
                    if (v > LED_VAL_MAX_F) v = LED_VAL_MAX_F;

                    int lv = (int)(v + 0.5f);
                    if (lv < 0) lv = 0;
                    if (lv >= LED_LEVELS) lv = LED_LEVELS - 1;

                    rgb8_t c = s_pal_lut[s_palette_idx][lv];

                    // 只有“有水”的像素才叠加时间蓝色，并且按时间对原本的蓝色分量做比例缩放，
                    // 这样原来的亮度/渐变（由 lv 决定）仍然保留，只是整体蓝色强度随时间变化。
                    if (lv > 0 && blue_by_time > 0) {
                        c.b = (uint8_t)(((uint16_t)c.b * (uint16_t)blue_by_time) / 255u);
                    }

                    // 将模拟坐标 (x, y) 在输出到面板时旋转 180°，
                    // 这样不改变内部物理模拟，只是把整幅画面倒转显示。
                    int rx = W - 1 - x;
                    int ry = H - 1 - y;

                    int idx = panel_led_index(rx, ry);
                    rgb_set((uint32_t)idx, c.r, c.g, c.b);
                }
            }
            rgb_show();
        }
    }

    if (f) {
        flip_destroy(f);
    }
    rgb_clear();
    rgb_show();

    s_running = false;
    s_task = NULL;
    vTaskDelete(NULL);
}

int water_sim_start(int core_id, uint32_t stack_size, int priority) {
    if (s_task) {
        return -1;
    }

    s_running = true;
    BaseType_t ok = xTaskCreatePinnedToCore(sim_task, "water_sim", stack_size, NULL, priority, &s_task, core_id);
    if (ok != pdPASS) {
        s_running = false;
        s_task = NULL;
        return -1;
    }
    return 0;
}

int water_sim_stop(uint32_t timeout_ms) {
    if (!s_task) {
        return 0;
    }

    s_running = false;

    TickType_t start = xTaskGetTickCount();
    TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);

    while (s_task) {
        if ((xTaskGetTickCount() - start) >= timeout_ticks) {
            TaskHandle_t task = s_task;
            if (task) {
                vTaskDelete(task);
            }
            s_task = NULL;
            return -1;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return 0;
}

bool water_sim_is_running(void) {
    return s_task != NULL && s_running;
}
