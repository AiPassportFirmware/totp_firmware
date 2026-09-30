// main/app_lang.h —— 设备端 UI 中英双语支持。
//
// 所有面向用户的界面文案(菜单/提示/状态/错误)统一收拢到本模块的字符串表,
// 通过 tr() 按当前语言取值。网页(配网/管理页面)是独立的静态 HTML,保持双语
// 混排,不随设备语言切换。
//
// 选择持久化:app_store 保存 1 字节(app_lang_t);app_main 启动时加载并调用
// app_lang_set_current。切换由 UI 菜单触发:app_lang_toggle() + 持久化。
// 字体:中英共用 app_font_sc_16(含全部 ASCII + 常用汉字),无需切换字体。
#pragma once

#include <stdint.h>

typedef enum {
    APP_LANG_ZH = 0,  // 简体中文(默认)
    APP_LANG_EN = 1,  // English
} app_lang_t;

// 字符串 ID(两语言表必须等长,宿主测试 tests/test_app_lang.c 校验)。
typedef enum {
    TR_MENU_REMOTE = 0,
    TR_MENU_WIFI,
    TR_MENU_LOCK,
    TR_MENU_DELETE,
    TR_LANG_TOGGLE,       // 菜单里的语言切换项(zh 表显示"切换 English")
    TR_WIFI_SCAN,
    TR_WIFI_CLEAR,
    TR_BACK,
    TR_RESCAN,
    TR_KB_LOWER,
    TR_KB_UPPER,
    TR_KB_DIGIT,
    TR_KB_GROUP_SYM,
    TR_KB_SPACE,
    TR_KB_BACK,
    TR_KB_DONE,
    TR_CANCEL,
    TR_DELETE,
    TR_TITLE_CONFIRM,
    TR_TITLE_SETUP,
    TR_TITLE_UNLOCK,
    TR_HINT_SETUP,
    TR_HINT_MAIN,
    TR_HINT_MENU,
    TR_HINT_WIFI,
    TR_HINT_KB,
    TR_HINT_DELETE,
    TR_HINT_MGMT,
    TR_TITLE_MENU,
    TR_TITLE_MGMT,
    TR_TITLE_WIFI,
    TR_TITLE_SCAN,
    TR_SCANNING,
    TR_SCAN_FAIL,
    TR_NO_ENTRIES,
    TR_ADD_VIA_REMOTE,
    TR_WAIT_SYNC,
    TR_ENTRY,
    TR_MSG_REPEAT,
    TR_MSG_MISMATCH,
    TR_MSG_NEED_WIFI,
    TR_MSG_START_FAIL,
    TR_MSG_NOTHING_TO_DELETE,
    TR_MSG_TOO_MANY,
    TR_MSG_PENALTY_OVER,
    TR_WIFI_OFF,
    TR_WIFI_IDLE,
    TR_WIFI_CONNECTING,
    TR_WIFI_CONNECTED,
    TR_WIFI_AUTH_FAIL,
    TR_WIFI_NO_AP,
    TR_WIFI_RETRY_WAIT,
    TR_WIFI_ERROR,
    TR_NOTE_WAITING,
    TR_NOTE_SESSION,
    TR_NOTE_LOGOUT,
    TR_NOTE_EXIT,
    TR_NOTE_LOCKED,
    TR_FATAL_TITLE,
    TR_FATAL_BODY,
    // 带格式占位符的字符串(两语言参数顺序一致,可安全做格式串)
    TR_FMT_REFRESH,       // "%us后刷新"      / "Refresh in %us"
    TR_FMT_DELETE_TITLE,  // "删除 \"%s\"？"   / "Delete \"%s\"?"
    TR_FMT_TRIES,         // "…还可尝试 %d 次" / "Wrong, %d attempts left"
    TR_FMT_LOCKED,        // "锁定 %u:%02u"    / "Locked %u:%02u"
    TR_FMT_ATTEMPTS,      // "%s · 剩余 %d 次" / "%s · %d left"
    TR_FMT_STATE,         // "状态：%s"        / "State: %s"
    TR_FMT_SSID,          // "\n名称：%s"      / "\nSSID: %s"
    TR_FMT_IP,            // "\nIP：%s"        / "\nIP: %s"
    TR_KB_EMPTY,
    TR_FMT_KB_COUNT,      // "已输入 %u/64"    / "Input %u/64"
    TR_FMT_KB_TITLE,      // "无线密码：%s"    / "Wi-Fi password: %s"
    // 远程管理网页可见文案(网页按语言选变体,见 app_server.c)
    TR_WEB_LOGIN_PROMPT,
    TR_WEB_SIGN_IN,
    TR_WEB_HEADING,
    TR_WEB_LOGOUT,
    TR_WEB_ADD_TITLE,
    TR_WEB_LABEL_NAME,
    TR_WEB_LABEL_SECRET,
    TR_WEB_LABEL_DIGITS,
    TR_WEB_LABEL_PERIOD,
    TR_WEB_NOTE,
    TR_WEB_ADD_BTN,
    TR_WEB_NO_ENTRIES,
    TR_WEB_ADDED,
    TR_WEB_DELETED,
    // 服务端 JSON 错误信息(随设备语言)
    TR_ERR_BAD_REQUEST,
    TR_ERR_UNAUTHORIZED,
    TR_ERR_WRONG_CODE,
    TR_ERR_LOCKED,
    TR_ERR_LABEL,
    TR_ERR_SECRET,
    TR_ERR_RANGE,
    TR_ERR_FULL,
    TR_ERR_SAVE,
    TR_ERR_BAD_ID,
    TR_ERR_DELETE,
    TR_ERR_MISSING,
    TR_ID_COUNT,
} tr_id_t;

// 取当前语言的文案(总是返回非 NULL 非空串)。
const char *tr(tr_id_t id);

void app_lang_set_current(app_lang_t lang);
app_lang_t app_lang_current(void);
app_lang_t app_lang_toggle(void);
