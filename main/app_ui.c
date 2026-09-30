// main/app_ui.c —— FoloTOTP 应用 UI 实现,见 app_ui.h。
//
// 中文界面:全部中文文本使用 app_font_sc_16(思源黑体 16px 子集,覆盖常用汉字,
// 见 assets/README.md 的字符清单与生成命令);数字/URL/键盘字符等纯 ASCII 文本
// 使用 Montserrat。覆盖集之外的中文(个别 SSID/条目名)会显示为占位方框,
// 策略见 docs/totp-app.md。
#include "app_ui.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "app_kb.h"
#include "app_lang.h"
#include "app_lock.h"
#include "app_server.h"
#include "app_store.h"
#include "app_totp.h"
#include "app_wifi.h"
#include "bsp_battery.h"
#include "bsp_display.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lvgl.h"

LV_FONT_DECLARE(app_font_sc_16);

static const char *TAG = "app_ui";
#define FONT_SC  &app_font_sc_16      // 中文/正文
#define FONT_NUM &lv_font_montserrat_14  // 纯 ASCII 小字(电量等)
#define FONT_KEY &lv_font_montserrat_20  // 键盘字符/数字码

// ---------------------------------------------------------------------------
// 主题
// ---------------------------------------------------------------------------
#define C_BG      0x10151B
#define C_SURFACE  0x1A222C
#define C_SURFACE2 0x232C38
#define C_ACCENT   0x35C9B0
#define C_TEXT     0xE8EDF2
#define C_SUB      0x8CA0B3
#define C_ERR      0xE05B4E
#define C_QR_DARK  0x10151B

#define STATUS_TICKS 10  // 每 10 秒读一次电量

typedef enum {
    PAGE_SETUP = 0,
    PAGE_UNLOCK,
    PAGE_MAIN,
    PAGE_MENU,
    PAGE_MGMT,
    PAGE_WIFI,
    PAGE_WIFI_SCAN,
    PAGE_KB,
    PAGE_DELETE,
} page_t;

// 菜单/键盘/删除页条目文案按当前语言取值(见 app_lang.c);第 5 项为语言切换。
#define MENU_COUNT 5

#define WIFI_COUNT 3
#define SCAN_ROWS 8  // 扫描列表最多显示 8 个 AP
#define DELETE_COUNT 2

static const char *menu_item_text(int i) {
    switch (i) {
    case 0: return tr(TR_MENU_REMOTE);
    case 1: return tr(TR_MENU_WIFI);
    case 2: return tr(TR_MENU_LOCK);
    case 3: return tr(TR_MENU_DELETE);
    default: return tr(TR_LANG_TOGGLE);
    }
}

static const char *kb_chip_text(int i) {
    switch (i) {
    case 0: return tr(TR_KB_LOWER);
    case 1: return tr(TR_KB_UPPER);
    case 2: return tr(TR_KB_DIGIT);
    case 3: return tr(TR_KB_GROUP_SYM);
    case 4: return tr(TR_KB_SPACE);
    case 5: return tr(TR_KB_BACK);
    default: return tr(TR_KB_DONE);
    }
}


// ---------------------------------------------------------------------------
// 状态
// ---------------------------------------------------------------------------
static app_lock_t s_lock;
static SemaphoreHandle_t s_lock_mtx;

static lv_obj_t *s_scr;             // 当前屏幕
static page_t s_page;

// 状态栏(顶层,跨屏幕)
static lv_obj_t *s_battery_label;
static int s_battery_cache = -2;    // -2 = 尚未读取

static lv_timer_t *s_tick_timer;
static uint32_t s_ticks;

// 设密/解锁共享:8 个槽位标签 + 消息行
static lv_obj_t *s_slot_labels[APP_LOCK_MOVES];
static lv_obj_t *s_slot_msg;
static lv_obj_t *s_slot_title;

// 主页
static lv_obj_t *s_main_index;
static lv_obj_t *s_main_name;
static lv_obj_t *s_main_code;
static lv_obj_t *s_main_bar;
static lv_obj_t *s_main_status;
static size_t s_entry_sel;
static totp_entry_t s_cur_entry;
static bool s_cur_valid;
static uint64_t s_cur_window;
static char s_cur_code[12];
static uint32_t s_last_generation;

// 菜单/列表页
static lv_obj_t *s_menu_items[MENU_COUNT];
static int s_menu_sel;
static lv_obj_t *s_menu_msg;
static lv_obj_t *s_wifi_items[WIFI_COUNT];
static int s_wifi_sel;
static lv_obj_t *s_wifi_status;
static lv_obj_t *s_del_items[DELETE_COUNT];
static int s_del_sel;
static lv_obj_t *s_del_title;

// 扫描连接页
static lv_obj_t *s_scan_rows[SCAN_ROWS + 2];  // 8 个 AP + 重新扫描 + 返回
static int s_scan_sel;
static int s_scan_count;                      // -1 扫描中,-2 失败,否则条数
static app_wifi_scan_item_t s_scan_items[SCAN_ROWS];

// 键盘页
static app_kb_t s_kb;
static char s_kb_ssid[33];
static lv_obj_t *s_kb_dots;
static lv_obj_t *s_kb_count;
static lv_obj_t *s_kb_chips[APP_KB_ITEM_NUM];  // 第一级:分组/直达项
static lv_obj_t *s_kb_prev;                    // 第二级:组内字符滚动条
static lv_obj_t *s_kb_cur;
static lv_obj_t *s_kb_next;
static lv_obj_t *s_kb_title;

// 管理页
static lv_obj_t *s_mgmt_status;
static char s_mgmt_url[48];

// ---------------------------------------------------------------------------
// 小工具
// ---------------------------------------------------------------------------
static uint32_t uptime_s(void) {
    return (uint32_t)(esp_timer_get_time() / 1000000LL);
}

static int64_t now_s(void) {
    return (int64_t)time(NULL);
}

static void lock_mtx_take(void) {
    xSemaphoreTake(s_lock_mtx, portMAX_DELAY);
}

static void lock_mtx_give(void) {
    xSemaphoreGive(s_lock_mtx);
}

static lv_obj_t *make_label(lv_obj_t *parent, const char *text, const lv_font_t *font,
                            lv_color_t color) {
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    return l;
}

static lv_obj_t *make_screen(void) {
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(C_BG), 0);
    return scr;
}

static lv_obj_t *make_title(lv_obj_t *scr, const char *text) {
    lv_obj_t *l = make_label(scr, text, FONT_SC, lv_color_hex(C_TEXT));
    lv_obj_align(l, LV_ALIGN_TOP_MID, 0, 36);
    return l;
}

static lv_obj_t *make_hint(lv_obj_t *scr, const char *text) {
    lv_obj_t *l = make_label(scr, text, FONT_SC, lv_color_hex(C_SUB));
    // 固定宽度并允许换行:长提示(英文按键说明)不再超出屏幕被左右裁切。
    lv_obj_set_width(l, 228);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(l, LV_ALIGN_BOTTOM_MID, 0, -10);
    return l;
}

// 列表项(菜单/Wi-Fi/扫描/删除确认共用):圆角条 + 居中文本。
static lv_obj_t *make_item(lv_obj_t *scr, const char *text, int y, int h) {
    lv_obj_t *item = lv_obj_create(scr);
    lv_obj_remove_style_all(item);
    lv_obj_set_size(item, 200, h);
    lv_obj_align(item, LV_ALIGN_TOP_MID, 0, y);
    lv_obj_set_style_radius(item, 8, 0);
    lv_obj_set_style_bg_opa(item, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(item, lv_color_hex(C_SURFACE), 0);
    lv_obj_t *l = make_label(item, text, FONT_SC, lv_color_hex(C_TEXT));
    lv_obj_center(l);
    return item;
}

// 8 个手势槽位(设密/解锁共用),一行排布。
static void make_slots(lv_obj_t *scr, lv_obj_t **slots) {
    lv_obj_t *row = lv_obj_create(scr);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, 216, 34);
    lv_obj_align(row, LV_ALIGN_TOP_MID, 0, 78);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 4, 0);
    for (int i = 0; i < APP_LOCK_MOVES; i++) {
        lv_obj_t *cell = lv_obj_create(row);
        lv_obj_set_size(cell, 23, 30);
        lv_obj_set_style_radius(cell, 6, 0);
        lv_obj_set_style_bg_color(cell, lv_color_hex(C_SURFACE), 0);
        lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(cell, lv_color_hex(C_SURFACE2), 0);
        lv_obj_set_style_border_width(cell, 1, 0);
        slots[i] = make_label(cell, "-", FONT_NUM, lv_color_hex(C_SUB));
        lv_obj_center(slots[i]);
    }
}

// 依锁定状态刷新槽位与标题。
static void slots_refresh(void) {
    const char *title = s_lock.phase == APP_LOCK_PHASE_CONFIRM ? tr(TR_TITLE_CONFIRM)
                        : s_lock.phase == APP_LOCK_PHASE_SETUP ? tr(TR_TITLE_SETUP)
                                                               : tr(TR_TITLE_UNLOCK);
    lv_label_set_text(s_slot_title, title);
    for (int i = 0; i < APP_LOCK_MOVES; i++) {
        lv_obj_t *cell = lv_obj_get_parent(s_slot_labels[i]);
        if (i < s_lock.progress) {
            bool up = (s_lock.draft >> i) & 1u;
            lv_label_set_text(s_slot_labels[i], up ? LV_SYMBOL_UP : LV_SYMBOL_DOWN);
            lv_obj_set_style_text_color(s_slot_labels[i], lv_color_hex(C_ACCENT), 0);
            lv_obj_set_style_border_color(cell, lv_color_hex(C_ACCENT), 0);
        } else {
            lv_label_set_text(s_slot_labels[i], "-");
            lv_obj_set_style_text_color(s_slot_labels[i], lv_color_hex(C_SUB), 0);
            lv_obj_set_style_border_color(cell, lv_color_hex(C_SURFACE2), 0);
        }
    }
}

static void format_code_grouped(uint8_t digits, const char *code, char *out, size_t cap) {
    // 6 位分两组更易读;8 位直接整排(宽度受限)。
    if (digits == 6 && strlen(code) == 6) {
        snprintf(out, cap, "%.3s %.3s", code, code + 3);
    } else {
        snprintf(out, cap, "%s", code);
    }
}

// ---------------------------------------------------------------------------
// 页面:设密 / 解锁
// ---------------------------------------------------------------------------
static void build_password_page(void) {
    s_scr = make_screen();
    s_slot_title = make_title(s_scr, tr(TR_TITLE_SETUP));
    make_slots(s_scr, s_slot_labels);
    s_slot_msg = make_label(s_scr, "", FONT_SC, lv_color_hex(C_ERR));
    lv_obj_align(s_slot_msg, LV_ALIGN_TOP_MID, 0, 126);
    lv_obj_set_width(s_slot_msg, 220);
    lv_label_set_long_mode(s_slot_msg, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s_slot_msg, LV_TEXT_ALIGN_CENTER, 0);
    make_hint(s_scr, tr(TR_HINT_SETUP));
    slots_refresh();
    lv_screen_load(s_scr);
}

// ---------------------------------------------------------------------------
// 页面:验证码主列表
// ---------------------------------------------------------------------------
static void main_load_entry(void) {
    size_t count = app_store_entry_count();
    s_cur_valid = false;
    s_cur_code[0] = '\0';
    if (count == 0) return;
    if (s_entry_sel >= count) s_entry_sel = count - 1;
    if (app_store_entry_get(s_entry_sel, &s_cur_entry) == ESP_OK) {
        s_cur_valid = true;
    }
}

static void main_refresh_code(void) {
    if (!s_cur_valid) return;
    if (!app_wifi_time_synced()) {
        strlcpy(s_cur_code, "- - - - - -", sizeof(s_cur_code));
        s_cur_window = 0;
        return;
    }
    uint64_t window = (uint64_t)now_s() / s_cur_entry.period;
    if (window != s_cur_window || s_cur_code[0] == '\0') {
        s_cur_window = window;
        uint32_t code = totp_at(&s_cur_entry, now_s());
        char raw[10];
        totp_format(code, s_cur_entry.digits, raw);
        format_code_grouped(s_cur_entry.digits, raw, s_cur_code, sizeof(s_cur_code));
    }
}

static void main_refresh(void) {
    size_t count = app_store_entry_count();
    if (count == 0 || !s_cur_valid) {
        lv_label_set_text(s_main_index, "");
        lv_label_set_text(s_main_name, tr(TR_NO_ENTRIES));
        lv_label_set_text(s_main_code, "- - -");
        lv_obj_set_width(s_main_name, 220);
        lv_label_set_text(s_main_status,
                          count == 0 ? tr(TR_ADD_VIA_REMOTE) : tr(TR_WAIT_SYNC));
        lv_bar_set_value(s_main_bar, 0, LV_ANIM_OFF);
        return;
    }
    main_refresh_code();
    char idx[16];
    snprintf(idx, sizeof(idx), "%u/%u", (unsigned)(s_entry_sel + 1), (unsigned)count);
    lv_label_set_text(s_main_index, idx);
    lv_label_set_text(s_main_name, s_cur_entry.label);
    lv_label_set_text(s_main_code, s_cur_code);
    if (app_wifi_time_synced()) {
        uint32_t remain = totp_remaining(&s_cur_entry, now_s());
        lv_bar_set_range(s_main_bar, 0, (int32_t)s_cur_entry.period);
        lv_bar_set_value(s_main_bar, (int32_t)remain, LV_ANIM_OFF);
        char st[32];
        snprintf(st, sizeof(st), tr(TR_FMT_REFRESH), (unsigned)remain);
        lv_label_set_text(s_main_status, st);
    }
}

static void build_main_page(void) {
    s_scr = make_screen();
    s_main_index = make_label(s_scr, "", FONT_NUM, lv_color_hex(C_SUB));
    lv_obj_align(s_main_index, LV_ALIGN_TOP_LEFT, 12, 36);

    s_main_name = make_label(s_scr, "", FONT_SC, lv_color_hex(C_TEXT));
    lv_obj_set_width(s_main_name, 216);
    lv_label_set_long_mode(s_main_name, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(s_main_name, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_main_name, LV_ALIGN_TOP_MID, 0, 66);

    s_main_code = make_label(s_scr, "- - -", &lv_font_montserrat_48, lv_color_hex(C_ACCENT));
    lv_obj_align(s_main_code, LV_ALIGN_TOP_MID, 0, 108);
    lv_obj_set_style_text_letter_space(s_main_code, 2, 0);

    s_main_bar = lv_bar_create(s_scr);
    lv_obj_set_size(s_main_bar, 180, 6);
    lv_obj_align(s_main_bar, LV_ALIGN_TOP_MID, 0, 182);
    lv_obj_set_style_bg_color(s_main_bar, lv_color_hex(C_SURFACE2), 0);
    lv_obj_set_style_bg_color(s_main_bar, lv_color_hex(C_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_main_bar, LV_OPA_COVER, 0);

    s_main_status = make_label(s_scr, "", FONT_SC, lv_color_hex(C_SUB));
    lv_obj_align(s_main_status, LV_ALIGN_TOP_MID, 0, 198);

    make_hint(s_scr, tr(TR_HINT_MAIN));
    s_last_generation = app_store_generation();
    main_load_entry();
    main_refresh();
    lv_screen_load(s_scr);
}

// ---------------------------------------------------------------------------
// 页面:菜单
// ---------------------------------------------------------------------------
static void menu_highlight(void) {
    for (int i = 0; i < MENU_COUNT; i++) {
        bool sel = i == s_menu_sel;
        lv_obj_set_style_bg_color(s_menu_items[i],
                                  lv_color_hex(sel ? C_SURFACE2 : C_SURFACE), 0);
        lv_obj_set_style_text_color(lv_obj_get_child(s_menu_items[i], 0),
                                    lv_color_hex(sel ? C_ACCENT : C_TEXT), 0);
    }
}

static void build_menu_page(void) {
    s_scr = make_screen();
    make_title(s_scr, tr(TR_TITLE_MENU));
    for (int i = 0; i < MENU_COUNT; i++) {
        s_menu_items[i] = make_item(s_scr, menu_item_text(i), 60 + i * 41, 34);
    }
    s_menu_msg = make_label(s_scr, "", FONT_SC, lv_color_hex(C_ERR));
    lv_obj_align(s_menu_msg, LV_ALIGN_BOTTOM_MID, 0, -34);
    make_hint(s_scr, tr(TR_HINT_MENU));
    menu_highlight();
    lv_screen_load(s_scr);
}

// ---------------------------------------------------------------------------
// 页面:远程管理(URL + 二维码 + 数字码)
// ---------------------------------------------------------------------------
static void build_mgmt_page(void) {
    s_scr = make_screen();
    make_title(s_scr, tr(TR_TITLE_MGMT));

    app_server_url(s_mgmt_url, sizeof(s_mgmt_url));
    lv_obj_t *url = make_label(s_scr, s_mgmt_url, FONT_NUM, lv_color_hex(C_TEXT));
    lv_obj_align(url, LV_ALIGN_TOP_MID, 0, 66);

    // 二维码:白底圆角卡片 + 深色码点,便于手机相机识别。
    lv_obj_t *card = lv_obj_create(s_scr);
    lv_obj_set_size(card, 150, 150);
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, 82);
    lv_obj_set_style_bg_color(card, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_pad_all(card, 6, 0);
    lv_obj_t *qr = lv_qrcode_create(card);
    lv_qrcode_set_size(qr, 138);
    lv_qrcode_set_dark_color(qr, lv_color_hex(C_QR_DARK));
    lv_qrcode_set_light_color(qr, lv_color_white());
    if (lv_qrcode_update(qr, s_mgmt_url, (uint32_t)strlen(s_mgmt_url)) != LV_RESULT_OK) {
        ESP_LOGE(TAG, "二维码生成失败");
        lv_obj_delete(qr);
    }

    // 数字码 4+4 分组展示
    const char *code = app_server_code();
    char grouped[16];
    snprintf(grouped, sizeof(grouped), "%.4s %.4s", code, code + 4);
    lv_obj_t *code_label = make_label(s_scr, grouped, FONT_KEY, lv_color_hex(C_ACCENT));
    lv_obj_align(code_label, LV_ALIGN_TOP_MID, 0, 236);

    s_mgmt_status = make_label(s_scr, "", FONT_SC, lv_color_hex(C_SUB));
    lv_obj_align(s_mgmt_status, LV_ALIGN_BOTTOM_MID, 0, -34);
    make_hint(s_scr, tr(TR_HINT_MGMT));
    lv_screen_load(s_scr);
}

// ---------------------------------------------------------------------------
// 页面:无线网络(状态 + 操作项)
// ---------------------------------------------------------------------------
static void wifi_items_refresh(void) {
    for (int i = 0; i < WIFI_COUNT; i++) {
        bool sel = i == s_wifi_sel;
        lv_obj_set_style_bg_color(s_wifi_items[i],
                                  lv_color_hex(sel ? C_SURFACE2 : C_SURFACE), 0);
        lv_obj_set_style_text_color(lv_obj_get_child(s_wifi_items[i], 0),
                                    lv_color_hex(sel ? C_ACCENT : C_TEXT), 0);
    }
}

static void build_wifi_page(void) {
    s_scr = make_screen();
    make_title(s_scr, tr(TR_TITLE_WIFI));
    for (int i = 0; i < WIFI_COUNT; i++) {
        const char *label = i == 0 ? tr(TR_WIFI_SCAN)
                          : i == 1 ? tr(TR_WIFI_CLEAR) : tr(TR_BACK);
        s_wifi_items[i] = make_item(s_scr, label, 150 + i * 40, 32);
    }
    s_wifi_status = make_label(s_scr, "", FONT_SC, lv_color_hex(C_SUB));
    lv_obj_align(s_wifi_status, LV_ALIGN_TOP_LEFT, 16, 64);
    lv_obj_set_width(s_wifi_status, 208);
    lv_label_set_long_mode(s_wifi_status, LV_LABEL_LONG_WRAP);
    make_hint(s_scr, tr(TR_HINT_WIFI));
    wifi_items_refresh();
    lv_screen_load(s_scr);
}

// ---------------------------------------------------------------------------
// 页面:扫描连接(设备端选择 AP)
// ---------------------------------------------------------------------------
static void scan_refresh(void) {
    for (int i = 0; i < SCAN_ROWS + 2; i++) {
        lv_obj_t *row = s_scan_rows[i];
        bool sel = i == s_scan_sel;
        lv_obj_t *label = lv_obj_get_child(row, 0);
        if (i < SCAN_ROWS) {
            if (s_scan_count == -1) {
                lv_label_set_text(label, i == 0 ? tr(TR_SCANNING) : "");
            } else if (s_scan_count == -2) {
                lv_label_set_text(label, i == 0 ? tr(TR_SCAN_FAIL) : "");
            } else if (i < s_scan_count) {
                char line[48];
                snprintf(line, sizeof(line), "%s %d", s_scan_items[i].ssid,
                         s_scan_items[i].rssi);
                lv_label_set_text(label, line);
            } else {
                lv_label_set_text(label, "");
            }
        } else if (i == SCAN_ROWS) {
            lv_label_set_text(label, tr(TR_RESCAN));
        } else {
            lv_label_set_text(label, tr(TR_BACK));
        }
        bool selectable = s_scan_count >= 0 || i >= SCAN_ROWS;
        lv_obj_set_style_bg_color(row, lv_color_hex(sel && selectable ? C_SURFACE2 : C_SURFACE), 0);
        lv_obj_set_style_text_color(label,
                                    lv_color_hex(sel && selectable ? C_ACCENT : C_TEXT), 0);
        lv_obj_set_style_opa(row, s_scan_count == -2 && i < SCAN_ROWS ? LV_OPA_50 : LV_OPA_COVER, 0);
    }
}

static void build_scan_page(void) {
    s_scr = make_screen();
    make_title(s_scr, tr(TR_TITLE_SCAN));
    for (int i = 0; i < SCAN_ROWS + 2; i++) {
        int y = 64 + i * 24;
        s_scan_rows[i] = make_item(s_scr, "", y, 22);
        lv_obj_t *label = lv_obj_get_child(s_scan_rows[i], 0);
        lv_obj_set_width(label, 190);
        // 长 SSID 循环左滚(单行,不再换行叠字);短 SSID 静止显示。
        lv_label_set_long_mode(label, LV_LABEL_LONG_SCROLL_CIRCULAR);
    }
    lv_screen_load(s_scr);
}

// ---------------------------------------------------------------------------
// 页面:键盘(三键两级输入:分组 → 组内字符)
// ---------------------------------------------------------------------------
static void kb_chip_highlight(void) {
    for (int i = 0; i < APP_KB_ITEM_NUM; i++) {
        bool sel = i == (int)app_kb_group(&s_kb);
        lv_obj_set_style_bg_color(s_kb_chips[i],
                                  lv_color_hex(sel ? C_SURFACE2 : C_SURFACE), 0);
        lv_obj_set_style_text_color(lv_obj_get_child(s_kb_chips[i], 0),
                                    lv_color_hex(sel ? C_ACCENT : C_TEXT), 0);
    }
}

// 第二级滚动条:3 个可见位,循环取邻位;0 号位显示"返回"箭头。
static void kb_roller_label(lv_obj_t *label, uint16_t idx) {
    char c = app_kb_char_at(app_kb_group_active(&s_kb), idx);
    if (c != 0) {
        char buf[2] = { c, '\0' };
        lv_label_set_text(label, buf);
    } else {
        lv_label_set_text(label, LV_SYMBOL_LEFT);  // "返回分组"项
    }
}

static void kb_refresh(void) {
    // 已录入:掩码星号(最多显示 24 个)+ 计数
    size_t len = app_kb_len(&s_kb);
    char dots[APP_KB_BUF_MAX / 2 + 2];
    size_t shown = len < 24 ? len : 24;
    memset(dots, '*', shown);
    dots[shown] = '\0';
    lv_label_set_text(s_kb_dots, len ? dots : tr(TR_KB_EMPTY));
    char cnt[24];
    snprintf(cnt, sizeof(cnt), tr(TR_FMT_KB_COUNT), (unsigned)len);
    lv_label_set_text(s_kb_count, cnt);

    if (app_kb_stage(&s_kb) == APP_KB_STAGE_GROUP) {
        kb_chip_highlight();
        lv_obj_add_flag(s_kb_prev, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_kb_cur, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_kb_next, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    for (int i = 0; i < APP_KB_ITEM_NUM; i++) {
        lv_obj_clear_flag(s_kb_chips[i], LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_clear_flag(s_kb_prev, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_kb_cur, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_kb_next, LV_OBJ_FLAG_HIDDEN);

    uint16_t sel = app_kb_char_sel(&s_kb);
    uint16_t total = (uint16_t)(app_kb_group_len(app_kb_group_active(&s_kb)) + 1);
    kb_roller_label(s_kb_cur, sel);
    kb_roller_label(s_kb_prev, (uint16_t)((sel + total - 1) % total));
    kb_roller_label(s_kb_next, (uint16_t)((sel + 1) % total));
    // 当前字符:亮青色高亮块 + 深色字(醒目);"返回"箭头保持弱化样式。
    if (sel == 0) {
        lv_obj_set_style_bg_opa(s_kb_cur, LV_OPA_TRANSP, 0);
        lv_obj_set_style_text_color(s_kb_cur, lv_color_hex(C_SUB), 0);
    } else {
        lv_obj_set_style_bg_opa(s_kb_cur, LV_OPA_COVER, 0);
        lv_obj_set_style_text_color(s_kb_cur, lv_color_hex(0x08221E), 0);
    }
}

// 小圆片按钮(第一级分组条)。x 为相对屏幕中心的偏移。
static lv_obj_t *make_chip(lv_obj_t *scr, const char *text, int x, int y, int w) {
    lv_obj_t *item = lv_obj_create(scr);
    lv_obj_remove_style_all(item);
    lv_obj_set_size(item, w, 34);
    lv_obj_align(item, LV_ALIGN_TOP_MID, x, y);
    lv_obj_set_style_radius(item, 8, 0);
    lv_obj_set_style_bg_opa(item, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(item, lv_color_hex(C_SURFACE), 0);
    lv_obj_t *l = make_label(item, text, FONT_SC, lv_color_hex(C_TEXT));
    lv_obj_center(l);
    return item;
}

static void build_kb_page(void) {
    s_scr = make_screen();
    char title[48];
    snprintf(title, sizeof(title), tr(TR_FMT_KB_TITLE), s_kb_ssid);
    s_kb_title = make_label(s_scr, title, FONT_SC, lv_color_hex(C_TEXT));
    lv_obj_set_width(s_kb_title, 216);
    lv_label_set_long_mode(s_kb_title, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(s_kb_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_kb_title, LV_ALIGN_TOP_MID, 0, 36);

    s_kb_dots = make_label(s_scr, tr(TR_KB_EMPTY), FONT_KEY, lv_color_hex(C_TEXT));
    lv_obj_set_width(s_kb_dots, 216);
    lv_label_set_long_mode(s_kb_dots, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(s_kb_dots, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_kb_dots, LV_ALIGN_TOP_MID, 0, 64);

    s_kb_count = make_label(s_scr, "", FONT_SC, lv_color_hex(C_SUB));
    lv_obj_align(s_kb_count, LV_ALIGN_TOP_MID, 0, 96);

    // 第一级:两组圆片 —— 字符组一行,直达项一行
    static const int k_row1_x[4] = { -78, -26, 26, 78 };
    for (int i = 0; i < 4; i++) {
        s_kb_chips[i] = make_chip(s_scr, kb_chip_text(i), k_row1_x[i], 134, 50);
    }
    static const int k_row2_x[3] = { -68, 0, 68 };
    for (int i = 0; i < 3; i++) {
        s_kb_chips[4 + i] = make_chip(s_scr, kb_chip_text(4 + i), k_row2_x[i], 174, 64);
    }
    // 第二级:组内字符滚动条(默认隐藏)
    s_kb_prev = make_label(s_scr, "", &lv_font_montserrat_16, lv_color_hex(C_SUB));
    lv_obj_align(s_kb_prev, LV_ALIGN_TOP_MID, -56, 144);
    lv_obj_add_flag(s_kb_prev, LV_OBJ_FLAG_HIDDEN);
    s_kb_next = make_label(s_scr, "", &lv_font_montserrat_16, lv_color_hex(C_SUB));
    lv_obj_align(s_kb_next, LV_ALIGN_TOP_MID, 56, 144);
    lv_obj_add_flag(s_kb_next, LV_OBJ_FLAG_HIDDEN);
    // 当前字符渲染为高亮块:青色圆角底 + 深色字,比纯色文字醒目得多。
    s_kb_cur = make_label(s_scr, "", &lv_font_montserrat_48, lv_color_hex(0x08221E));
    lv_obj_set_style_bg_color(s_kb_cur, lv_color_hex(C_ACCENT), 0);
    lv_obj_set_style_bg_opa(s_kb_cur, LV_OPA_TRANSP, 0);
    lv_obj_set_style_radius(s_kb_cur, 12, 0);
    lv_obj_set_style_pad_hor(s_kb_cur, 16, 0);
    lv_obj_set_style_pad_ver(s_kb_cur, 2, 0);
    lv_obj_align(s_kb_cur, LV_ALIGN_TOP_MID, 0, 120);
    lv_obj_add_flag(s_kb_cur, LV_OBJ_FLAG_HIDDEN);

    make_hint(s_scr, tr(TR_HINT_KB));
    kb_refresh();
    lv_screen_load(s_scr);
}

// ---------------------------------------------------------------------------
// 页面:删除确认
// ---------------------------------------------------------------------------
static void delete_highlight(void) {
    for (int i = 0; i < DELETE_COUNT; i++) {
        bool sel = i == s_del_sel;
        lv_obj_set_style_bg_color(s_del_items[i],
                                  lv_color_hex(sel ? C_SURFACE2 : C_SURFACE), 0);
        lv_obj_set_style_text_color(lv_obj_get_child(s_del_items[i], 0),
                                    lv_color_hex(i == 1 && sel ? C_ERR : (sel ? C_ACCENT : C_TEXT)), 0);
    }
}

static void build_delete_page(void) {
    s_scr = make_screen();
    char title[80];
    totp_entry_t e;
    const char *name = tr(TR_ENTRY);
    if (app_store_entry_get(s_entry_sel, &e) == ESP_OK) {
        name = e.label;
    }
    snprintf(title, sizeof(title), tr(TR_FMT_DELETE_TITLE), name);
    s_del_title = make_label(s_scr, title, FONT_SC, lv_color_hex(C_TEXT));
    lv_obj_set_width(s_del_title, 216);
    lv_label_set_long_mode(s_del_title, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(s_del_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_del_title, LV_ALIGN_TOP_MID, 0, 80);

    for (int i = 0; i < DELETE_COUNT; i++) {
        s_del_items[i] = make_item(s_scr, i == 0 ? tr(TR_CANCEL) : tr(TR_DELETE), 140 + i * 54, 44);
    }
    make_hint(s_scr, tr(TR_HINT_DELETE));
    delete_highlight();
    lv_screen_load(s_scr);
}

// ---------------------------------------------------------------------------
// 页面切换(先建新屏并载入,再删旧屏,避免删除活动屏)
// ---------------------------------------------------------------------------
static void show_page(page_t page) {
    // 输入任务与 LVGL tick 都会调用本函数;port 锁为递归锁,两条路径均安全。
    if (!bsp_lvgl_lock(1000)) {
        ESP_LOGW(TAG, "LVGL 锁超时,忽略页面切换");
        return;
    }
    lv_obj_t *old = s_scr;
    s_page = page;
    switch (page) {
    case PAGE_SETUP:
    case PAGE_UNLOCK:
        build_password_page();
        break;
    case PAGE_MAIN:
        build_main_page();
        break;
    case PAGE_MENU:
        build_menu_page();
        break;
    case PAGE_MGMT:
        build_mgmt_page();
        break;
    case PAGE_WIFI:
        build_wifi_page();
        break;
    case PAGE_WIFI_SCAN:
        build_scan_page();
        break;
    case PAGE_KB:
        build_kb_page();
        break;
    case PAGE_DELETE:
        build_delete_page();
        break;
    }
    if (old && old != s_scr) {
        lv_obj_delete(old);
    }
    bsp_lvgl_unlock();
}

// ---------------------------------------------------------------------------
// 锁定动作(菜单/空闲),含管理模式联动
// ---------------------------------------------------------------------------
static void do_lock(void) {
    if (app_server_active()) {
        app_server_stop();  // 设备锁定必须同步结束远程管理
    }
    lock_mtx_take();
    app_lock_lock(&s_lock);
    lock_mtx_give();
}

// ---------------------------------------------------------------------------
// 按键处理(输入任务上下文;LVGL 操作经 show_page/显式加锁)
// ---------------------------------------------------------------------------
static void handle_password_key(bsp_btn_t btn, bsp_btn_ev_t ev) {
    lock_mtx_take();
    if (s_lock.phase == APP_LOCK_PHASE_LOCKOUT) {
        lock_mtx_give();
        return;  // 惩罚期拒绝一切输入
    }
    app_lock_event_t event = APP_LOCK_EV_NONE;
    if (ev == BSP_BTN_CLICK && (btn == BSP_BTN_UP || btn == BSP_BTN_DOWN)) {
        event = app_lock_push_move(&s_lock, btn == BSP_BTN_UP ? APP_LOCK_MOVE_UP
                                                              : APP_LOCK_MOVE_DOWN,
                                   (uint32_t)now_s());
    } else if (ev == BSP_BTN_CLICK && btn == BSP_BTN_OK) {
        app_lock_backspace(&s_lock);
    } else if (ev == BSP_BTN_LONG && btn == BSP_BTN_OK) {
        while (s_lock.progress > 0) {
            app_lock_backspace(&s_lock);
        }
    }

    // 事件副作用:持久化与页面迁移(先做非 LVGL 部分)
    bool show_main = false;
    switch (event) {
    case APP_LOCK_EV_SETUP_DONE:
        app_store_save_password(s_lock.salt, s_lock.pwd_hash);
        show_main = true;
        break;
    case APP_LOCK_EV_UNLOCKED:
        app_store_save_lock(0, s_lock.lock_until_s);
        show_main = true;
        break;
    case APP_LOCK_EV_WRONG:
        app_store_save_lock(s_lock.fail_count, 0);
        break;
    case APP_LOCK_EV_LOCKOUT_START:
        app_store_save_lock(0, s_lock.lock_until_s);
        if (app_server_active()) app_server_stop();
        break;
    default:
        break;
    }
    lock_mtx_give();

    if (show_main) {
        show_page(PAGE_MAIN);
        return;
    }
    if (bsp_lvgl_lock(500)) {
        slots_refresh();
        if (event == APP_LOCK_EV_WRONG) {
            char msg[48];
            snprintf(msg, sizeof(msg), tr(TR_FMT_TRIES),
                     APP_LOCK_FAIL_LIMIT - s_lock.fail_count);
            lv_label_set_text(s_slot_msg, msg);
            lv_obj_set_style_text_color(s_slot_msg, lv_color_hex(C_ERR), 0);
        } else if (event == APP_LOCK_EV_SETUP_ENTER_CONFIRM) {
            lv_label_set_text(s_slot_msg, tr(TR_MSG_REPEAT));
            lv_obj_set_style_text_color(s_slot_msg, lv_color_hex(C_SUB), 0);
        } else if (event == APP_LOCK_EV_SETUP_MISMATCH) {
            lv_label_set_text(s_slot_msg, tr(TR_MSG_MISMATCH));
            lv_obj_set_style_text_color(s_slot_msg, lv_color_hex(C_ERR), 0);
        }
        bsp_lvgl_unlock();
    }
}

static void handle_menu_key(bsp_btn_t btn, bsp_btn_ev_t ev) {
    if (ev == BSP_BTN_LONG && btn == BSP_BTN_OK) {
        show_page(PAGE_MAIN);
        return;
    }
    if (ev != BSP_BTN_CLICK) return;
    if (btn == BSP_BTN_UP) {
        s_menu_sel = (s_menu_sel + MENU_COUNT - 1) % MENU_COUNT;
    } else if (btn == BSP_BTN_DOWN) {
        s_menu_sel = (s_menu_sel + 1) % MENU_COUNT;
    } else if (btn == BSP_BTN_OK) {
        switch (s_menu_sel) {
        case 0:  // 远程管理
            if (app_wifi_state() != APP_WIFI_CONNECTED) {
                if (bsp_lvgl_lock(500)) {
                    lv_label_set_text(s_menu_msg, tr(TR_MSG_NEED_WIFI));
                    bsp_lvgl_unlock();
                }
                return;
            }
            if (app_server_start() != ESP_OK) {
                if (bsp_lvgl_lock(500)) {
                    lv_label_set_text(s_menu_msg, tr(TR_MSG_START_FAIL));
                    bsp_lvgl_unlock();
                }
                return;
            }
            show_page(PAGE_MGMT);
            return;
        case 1:  // 无线网络
            show_page(PAGE_WIFI);
            return;
        case 2:  // 立即锁定
            do_lock();
            show_page(PAGE_UNLOCK);
            return;
        case 3:  // 删除条目
            if (app_store_entry_count() == 0) {
                if (bsp_lvgl_lock(500)) {
                    lv_label_set_text(s_menu_msg, tr(TR_MSG_NOTHING_TO_DELETE));
                    bsp_lvgl_unlock();
                }
                return;
            }
            s_del_sel = 0;
            show_page(PAGE_DELETE);
            return;
        case 4: {  // 语言切换:翻转并持久化,重建菜单页即时生效
            app_store_save_lang(app_lang_toggle());
            show_page(PAGE_MENU);
            return;
        }
        }
        return;
    } else {
        return;
    }
    if (bsp_lvgl_lock(500)) {
        menu_highlight();
        lv_label_set_text(s_menu_msg, "");
        bsp_lvgl_unlock();
    }
}

static void handle_main_key(bsp_btn_t btn, bsp_btn_ev_t ev) {
    if (ev != BSP_BTN_CLICK && !(ev == BSP_BTN_LONG && btn == BSP_BTN_OK)) return;
    if (ev == BSP_BTN_LONG && btn == BSP_BTN_OK) {
        do_lock();
        show_page(PAGE_UNLOCK);
        return;
    }
    size_t count = app_store_entry_count();
    if (count == 0) {
        if (btn == BSP_BTN_OK) {
            s_menu_sel = 0;
            show_page(PAGE_MENU);
        }
        return;
    }
    if (btn == BSP_BTN_UP) {
        s_entry_sel = (s_entry_sel + count - 1) % count;
    } else if (btn == BSP_BTN_DOWN) {
        s_entry_sel = (s_entry_sel + 1) % count;
    } else if (btn == BSP_BTN_OK) {
        s_menu_sel = 0;
        show_page(PAGE_MENU);
        return;
    }
    // 换条目:重取数据并刷新(窗口缓存同时失效)
    s_cur_window = 0;
    s_cur_code[0] = '\0';
    main_load_entry();
    if (bsp_lvgl_lock(500)) {
        main_refresh();
        bsp_lvgl_unlock();
    }
}

static void handle_wifi_key(bsp_btn_t btn, bsp_btn_ev_t ev) {
    if (ev == BSP_BTN_LONG && btn == BSP_BTN_OK) {
        show_page(PAGE_MENU);
        return;
    }
    if (ev != BSP_BTN_CLICK) return;
    if (btn == BSP_BTN_UP) {
        s_wifi_sel = (s_wifi_sel + WIFI_COUNT - 1) % WIFI_COUNT;
    } else if (btn == BSP_BTN_DOWN) {
        s_wifi_sel = (s_wifi_sel + 1) % WIFI_COUNT;
    } else if (btn == BSP_BTN_OK) {
        switch (s_wifi_sel) {
        case 0:  // 扫描连接
            s_scan_count = app_wifi_scan_begin() ? -1 : -2;
            s_scan_sel = 0;
            show_page(PAGE_WIFI_SCAN);
            return;
        case 1:  // 清除凭证
            app_wifi_clear_credentials();
            return;
        case 2:  // 返回
            show_page(PAGE_MENU);
            return;
        }
        return;
    } else {
        return;
    }
    if (bsp_lvgl_lock(500)) {
        wifi_items_refresh();
        bsp_lvgl_unlock();
    }
}

static void handle_scan_key(bsp_btn_t btn, bsp_btn_ev_t ev) {
    // 结果就绪时可选:AP×count + 重新扫描 + 返回;扫描中/失败时仅后两项可选。
    int sel_max = s_scan_count >= 0 ? s_scan_count + 1 : SCAN_ROWS + 1;
    if (ev == BSP_BTN_LONG && btn == BSP_BTN_OK) {
        show_page(PAGE_WIFI);
        return;
    }
    if (ev != BSP_BTN_CLICK) return;
    if (btn == BSP_BTN_UP) {
        s_scan_sel = (s_scan_sel + sel_max) % (sel_max + 1);
    } else if (btn == BSP_BTN_DOWN) {
        s_scan_sel = (s_scan_sel + 1) % (sel_max + 1);
    } else if (btn == BSP_BTN_OK) {
        if (s_scan_count >= 0 && s_scan_sel < s_scan_count) {
            // 进入键盘页输入该 AP 的密码
            strlcpy(s_kb_ssid, s_scan_items[s_scan_sel].ssid, sizeof(s_kb_ssid));
            app_kb_init(&s_kb);
            show_page(PAGE_KB);
        } else if (s_scan_sel == SCAN_ROWS) {
            // 重新扫描
            s_scan_count = app_wifi_scan_begin() ? -1 : -2;
            s_scan_sel = SCAN_ROWS;
            if (bsp_lvgl_lock(500)) {
                scan_refresh();
                bsp_lvgl_unlock();
            }
        } else {
            show_page(PAGE_WIFI);
        }
        return;
    } else {
        return;
    }
    if (bsp_lvgl_lock(500)) {
        scan_refresh();
        bsp_lvgl_unlock();
    }
}

static void handle_kb_key(bsp_btn_t btn, bsp_btn_ev_t ev) {
    bool done = false;
    if (ev == BSP_BTN_LONG && btn == BSP_BTN_OK) {
        done = true;  // 长按 = 完成连接
    } else if (ev == BSP_BTN_DOUBLE && btn == BSP_BTN_OK) {
        app_kb_backspace(&s_kb);
    } else if (ev == BSP_BTN_CLICK && (btn == BSP_BTN_UP || btn == BSP_BTN_DOWN)) {
        app_kb_move(&s_kb, btn == BSP_BTN_UP ? -1 : 1);
    } else if (ev == BSP_BTN_LONG && (btn == BSP_BTN_UP || btn == BSP_BTN_DOWN)) {
        app_kb_move(&s_kb, btn == BSP_BTN_UP ? -APP_KB_JUMP_STEP : APP_KB_JUMP_STEP);
    } else if (ev == BSP_BTN_CLICK && btn == BSP_BTN_OK) {
        if (app_kb_pick(&s_kb) == APP_KB_EV_DONE) {
            done = true;
        }
    } else {
        return;
    }

    if (done) {
        // 用录入内容连接所选 AP;结果由无线页状态行呈现(开放网络可空密码)。
        app_wifi_apply_credentials(s_kb_ssid, app_kb_text(&s_kb));
        show_page(PAGE_WIFI);
        return;
    }
    if (bsp_lvgl_lock(500)) {
        kb_refresh();
        bsp_lvgl_unlock();
    }
}

static void handle_mgmt_key(bsp_btn_t btn, bsp_btn_ev_t ev) {
    if (ev == BSP_BTN_LONG && btn == BSP_BTN_OK) {
        app_server_stop();
        show_page(PAGE_MAIN);
    }
}

static void handle_delete_key(bsp_btn_t btn, bsp_btn_ev_t ev) {
    if (ev == BSP_BTN_LONG && btn == BSP_BTN_OK) {
        show_page(PAGE_MAIN);
        return;
    }
    if (ev != BSP_BTN_CLICK) return;
    if (btn == BSP_BTN_UP) {
        s_del_sel = (s_del_sel + DELETE_COUNT - 1) % DELETE_COUNT;
    } else if (btn == BSP_BTN_DOWN) {
        s_del_sel = (s_del_sel + 1) % DELETE_COUNT;
    } else if (btn == BSP_BTN_OK) {
        if (s_del_sel == 1) {
            app_store_entry_delete(s_entry_sel);
            s_entry_sel = 0;
        }
        show_page(PAGE_MAIN);
        return;
    } else {
        return;
    }
    if (bsp_lvgl_lock(500)) {
        delete_highlight();
        bsp_lvgl_unlock();
    }
}

void app_ui_key(bsp_btn_t btn, bsp_btn_ev_t ev) {
    // 任何按键都算活动:刷新空闲计时与背光。
    lock_mtx_take();
    app_lock_on_activity(&s_lock, uptime_s());
    lock_mtx_give();
    bsp_display_backlight(100);

    switch (s_page) {
    case PAGE_SETUP:
    case PAGE_UNLOCK:
        handle_password_key(btn, ev);
        break;
    case PAGE_MAIN:
        handle_main_key(btn, ev);
        break;
    case PAGE_MENU:
        handle_menu_key(btn, ev);
        break;
    case PAGE_MGMT:
        handle_mgmt_key(btn, ev);
        break;
    case PAGE_WIFI:
        handle_wifi_key(btn, ev);
        break;
    case PAGE_WIFI_SCAN:
        handle_scan_key(btn, ev);
        break;
    case PAGE_KB:
        handle_kb_key(btn, ev);
        break;
    case PAGE_DELETE:
        handle_delete_key(btn, ev);
        break;
    }

    // 事件处理器可能完成解锁/设密等状态迁移:补一次活动刷新,保证解锁后
    // 拿到完整的 15 分钟空闲窗口(时间戳在整个事件期间只增不回退)。
    lock_mtx_take();
    app_lock_on_activity(&s_lock, uptime_s());
    lock_mtx_give();
}

// ---------------------------------------------------------------------------
// 1 秒 tick(LVGL 任务上下文,免锁访问 LVGL)
// ---------------------------------------------------------------------------
static void tick_battery(void) {
    if (s_ticks % STATUS_TICKS != 0) return;
    int soc = bsp_battery_soc();
    if (soc == s_battery_cache) return;
    s_battery_cache = soc;
    if (soc < 0) {
        lv_label_set_text(s_battery_label, "");  // 电量计不可用:留白,不画假数据
    } else {
        lv_label_set_text_fmt(s_battery_label, "%d%%", soc);
    }
}

static void tick_lock(void) {
    lock_mtx_take();
    // 惩罚到期检查(时间同步后 lock_until 才有意义;未同步时保持锁定是安全侧)。
    uint32_t before = s_lock.lock_until_s;
    app_lock_poll_lockout(&s_lock, (uint32_t)now_s());
    if (before != 0 && s_lock.lock_until_s == 0) {
        app_store_save_lock(s_lock.fail_count, 0);  // 惩罚到期,清除持久化状态
    }
    bool idle_expired = app_lock_idle_expired(&s_lock, uptime_s());
    bool locked = s_lock.phase != APP_LOCK_PHASE_OPEN && s_lock.has_pwd;
    lock_mtx_give();

    if (idle_expired) {
        do_lock();
        locked = true;
    }
    if (locked && s_page != PAGE_SETUP && s_page != PAGE_UNLOCK) {
        show_page(PAGE_UNLOCK);
    }
}

static void tick_pages(void) {
    switch (s_page) {
    case PAGE_UNLOCK: {
        lock_mtx_take();
        bool lockout = s_lock.phase == APP_LOCK_PHASE_LOCKOUT;
        uint32_t remain = app_lock_lockout_remaining(&s_lock, (uint32_t)now_s());
        lock_mtx_give();
        if (lockout) {
            // 惩罚倒计时替换槽位区;时间未同步时提示等待。
            char buf[64];
            if (remain > 0) {
                snprintf(buf, sizeof(buf), tr(TR_FMT_LOCKED), (unsigned)(remain / 60),
                         (unsigned)(remain % 60));
                lv_label_set_text(s_slot_title, buf);
                for (int i = 0; i < APP_LOCK_MOVES; i++) {
                    lv_label_set_text(s_slot_labels[i], "");
                }
                lv_label_set_text(s_slot_msg, tr(TR_MSG_TOO_MANY));
                lv_obj_set_style_text_color(s_slot_msg, lv_color_hex(C_ERR), 0);
            } else {
                lv_label_set_text(s_slot_msg, app_wifi_time_synced()
                                                  ? tr(TR_MSG_PENALTY_OVER)
                                                  : tr(TR_WAIT_SYNC));
            }
        } else {
            slots_refresh();  // 惩罚解除或初次进入:恢复槽位显示
        }
        break;
    }
    case PAGE_MAIN:
        if (app_store_generation() != s_last_generation) {
            s_last_generation = app_store_generation();
            s_cur_window = 0;
            s_cur_code[0] = '\0';
            main_load_entry();
        }
        main_refresh();
        break;
    case PAGE_MGMT: {
        if (!app_server_active() || app_server_exit_requested() ||
            app_wifi_state() != APP_WIFI_CONNECTED) {
            app_server_stop();
            show_page(PAGE_MAIN);
            break;
        }
        char st[80];
        if (app_server_session_alive()) {
            snprintf(st, sizeof(st), "%s", app_server_note());
        } else {
            snprintf(st, sizeof(st), tr(TR_FMT_ATTEMPTS), app_server_note(),
                     app_server_fails_left());
        }
        lv_label_set_text(s_mgmt_status, st);
        break;
    }
    case PAGE_WIFI: {
        char st[128];
        app_wifi_state_t state = app_wifi_state();
        snprintf(st, sizeof(st), tr(TR_FMT_STATE), app_wifi_state_text());
        if (app_store_wifi_has()) {
            snprintf(st + strlen(st), sizeof(st) - strlen(st), tr(TR_FMT_SSID),
                     app_wifi_ssid());
        }
        if (state == APP_WIFI_CONNECTED) {
            snprintf(st + strlen(st), sizeof(st) - strlen(st), tr(TR_FMT_IP), app_wifi_ip());
        }
        lv_label_set_text(s_wifi_status, st);
        wifi_items_refresh();
        break;
    }
    case PAGE_WIFI_SCAN: {
        if (s_scan_count == -1) {
            int n = app_wifi_scan_fetch(s_scan_items, SCAN_ROWS);
            if (n >= 0) {
                s_scan_count = n;
                s_scan_sel = 0;
            }
        }
        scan_refresh();
        break;
    }
    default:
        break;
    }
}

static void tick_cb(lv_timer_t *timer) {
    (void)timer;
    s_ticks++;
    if (!s_lock_mtx) return;

    tick_battery();
    tick_lock();
    tick_pages();

    // 背光策略:任何键恢复 100(app_ui_key);这里只负责 30s 无输入调暗。
    lock_mtx_take();
    bool idle_30s = (uint32_t)(uptime_s() - s_lock.last_activity_uptime_s) > 30;
    lock_mtx_give();
    if (idle_30s) {
        bsp_display_backlight(20);
    }
}

// ---------------------------------------------------------------------------
// 初始化
// ---------------------------------------------------------------------------
void app_ui_init(void) {
    s_lock_mtx = xSemaphoreCreateMutex();

    // 恢复锁定状态:密码(salt+hash)与惩罚计数/截止时间。
    uint8_t fail = 0;
    uint32_t lock_until = 0;
    app_store_load_lock(&fail, &lock_until);
    uint8_t salt[APP_LOCK_SALT_LEN];
    uint64_t hash = 0;
    bool has_pwd = app_store_has_password() &&
                   app_store_load_password(salt, &hash) == ESP_OK;
    if (!has_pwd) {
        if (app_store_has_password()) {
            ESP_LOGE(TAG, "密码记录损坏,回退到首次设密流程");
        }
        esp_fill_random(salt, sizeof(salt));  // 设密完成时与哈希一起持久化
    }

    // 状态栏:右上角电量(约定位置;顶层,不挡点击)。
    lv_obj_t *top = lv_layer_top();
    s_battery_label = make_label(top, "", FONT_NUM, lv_color_hex(C_SUB));
    lv_obj_align(s_battery_label, LV_ALIGN_TOP_RIGHT, -12, 8);
    lv_obj_clear_flag(s_battery_label, LV_OBJ_FLAG_CLICKABLE);

    s_tick_timer = lv_timer_create(tick_cb, 1000, NULL);

    lock_mtx_take();
    app_lock_restore(&s_lock, has_pwd, salt, hash, fail, lock_until);
    // 开机即判定惩罚:时间未同步(小 epoch)时保持锁定,等 SNTP 同步后倒计时。
    app_lock_poll_lockout(&s_lock, (uint32_t)now_s());
    app_lock_on_activity(&s_lock, uptime_s());
    lock_mtx_give();

    show_page(has_pwd ? PAGE_UNLOCK : PAGE_SETUP);
}
