// main/app_main.c —— FoloTOTP:TOTP 验证器固件入口。
//
// 本文件是派生应用的入口(BSP 驱动演示 demo 的注册壳不再编入固件,其源文件
// 保留作参考与宿主测试)。启动顺序:
//   NVS/存储 → 显示/LVGL → 电量计 → Wi-Fi(有凭证即连接,无凭证自动进配网)
//   → 按键分发任务 → UI(锁定状态恢复 + 首屏)。
#include "app_lang.h"
#include "app_store.h"
#include "app_ui.h"
#include "app_wifi.h"
#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_i2c.h"
#include "bsp_pins.h"  // 错误日志中打印显示引脚
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lvgl.h"

static const char *TAG = "app_main";

LV_FONT_DECLARE(app_font_sc_16);

typedef struct {
    bsp_btn_t btn;
    bsp_btn_ev_t event;
} input_event_t;
#define INPUT_QUEUE_DEPTH 8

static QueueHandle_t s_input_queue;
static volatile bool s_input_ready;

// button 回调运行在共享 esp_timer 任务:只入队,立即返回。
static void on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user) {
    (void)user;
    if (!s_input_ready || !s_input_queue) return;
    const input_event_t input = { .btn = btn, .event = ev };
    (void)xQueueSend(s_input_queue, &input, 0);
}

static void input_task(void *arg) {
    (void)arg;
    input_event_t input;
    for (;;) {
        if (xQueueReceive(s_input_queue, &input, portMAX_DELAY) == pdTRUE) {
            app_ui_key(input.btn, input.event);
        }
    }
}

// 存储不可用:没有它密钥/密码都无法持久化,拒绝进入任何可用状态。
static void fatal_storage_screen(void) {
    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "显示初始化失败,无法呈现存储错误(MOSI=%d SCLK=%d CS=%d DC=%d)",
                 BSP_LCD_MOSI, BSP_LCD_SCLK, BSP_LCD_CS, BSP_LCD_DC);
        return;
    }
    bsp_display_backlight(60);
    if (!bsp_lvgl_lock(1000)) return;
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x10151B), 0);
    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, tr(TR_FATAL_TITLE));
    lv_obj_set_style_text_font(title, &app_font_sc_16, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xE05B4E), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 96);
    lv_obj_t *body = lv_label_create(scr);
    lv_label_set_text(body, tr(TR_FATAL_BODY));
    lv_obj_set_style_text_font(body, &app_font_sc_16, 0);
    lv_obj_set_style_text_color(body, lv_color_hex(0x8CA0B3), 0);
    lv_obj_align(body, LV_ALIGN_TOP_MID, 0, 140);
    lv_screen_load(scr);
    bsp_lvgl_unlock();
}

void app_main(void) {
    ESP_LOGI(TAG, "FoloTOTP 启动");

    bsp_i2c_init();

    if (app_store_init() != ESP_OK) {
        ESP_LOGE(TAG, "NVS 初始化失败,进入存储错误屏(不自动擦除)");
        fatal_storage_screen();
        return;
    }

    uint8_t lang = 0;
    app_store_load_lang(&lang);
    app_lang_set_current((app_lang_t)lang);

    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "显示/LVGL 初始化失败,无法继续(MOSI=%d SCLK=%d CS=%d DC=%d BL=%d)",
                 BSP_LCD_MOSI, BSP_LCD_SCLK, BSP_LCD_CS, BSP_LCD_DC, BSP_LCD_BL);
        return;
    }
    bsp_display_backlight(100);

    // 可选外设:电量计失败不影响主功能(UI 显示为空)。
    if (bsp_battery_init() != ESP_OK) {
        ESP_LOGW(TAG, "电量计初始化失败(CW2017 不应答?)");
    }

    // Wi-Fi:有凭证直接连接;无凭证保持待机,由用户在屏幕上扫描连接。
    if (app_wifi_init() == ESP_OK) {
        app_wifi_autostart();
    } else {
        ESP_LOGE(TAG, "Wi-Fi 初始化失败;主功能(本地验证码)不受影响");
    }

    // 按键分发任务 + UI 首屏。
    s_input_queue = xQueueCreate(INPUT_QUEUE_DEPTH, sizeof(input_event_t));
    if (!s_input_queue || xTaskCreate(input_task, "app_input", 4096, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "输入任务创建失败,设备将无法响应按键");
        return;
    }
    if (bsp_button_init(on_key, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "按键初始化失败");
        return;
    }

    if (bsp_lvgl_lock(1000)) {
        app_ui_init();
        bsp_lvgl_unlock();
        s_input_ready = true;
    }

    ESP_LOGI(TAG, "FoloTOTP 就绪");
}
