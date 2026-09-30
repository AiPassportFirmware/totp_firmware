// main/app_kb.c —— 三键两级键盘模型实现,见 app_kb.h。
#include "app_kb.h"

#include <string.h>

static const char *const k_group_strs[APP_KB_GROUP_NUM] = {
    "abcdefghijklmnopqrstuvwxyz",
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ",
    "0123456789",
    // 可打印 ASCII 中除字母、数字、空格外的全部 32 个符号
    "!\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~",
};

const char *app_kb_group_str(uint8_t group) {
    if (group >= APP_KB_GROUP_NUM) return "";
    return k_group_strs[group];
}

size_t app_kb_group_len(uint8_t group) {
    if (group >= APP_KB_GROUP_NUM) return 0;
    return strlen(k_group_strs[group]);
}

void app_kb_init(app_kb_t *kb) {
    if (kb == NULL) return;
    memset(kb->buf, 0, sizeof(kb->buf));
    kb->len = 0;
    kb->stage = APP_KB_STAGE_GROUP;
    kb->group = 0;         // 小写组最常用,作默认
    kb->group_active = 0;
    kb->char_sel = 1;
}

void app_kb_move(app_kb_t *kb, int delta) {
    if (kb == NULL || delta == 0) return;
    if (kb->stage == APP_KB_STAGE_GROUP) {
        int n = (int)kb->group + delta;
        n %= APP_KB_ITEM_NUM;
        if (n < 0) n += APP_KB_ITEM_NUM;
        kb->group = (uint8_t)n;
    } else {
        // 第二级条目数 = 返回项 + 组内字符数
        int total = (int)app_kb_group_len(kb->group_active) + 1;
        int n = (int)kb->char_sel + delta;
        n %= total;
        if (n < 0) n += total;
        kb->char_sel = (uint16_t)n;
    }
}

app_kb_event_t app_kb_pick(app_kb_t *kb) {
    if (kb == NULL) return APP_KB_EV_NONE;
    if (kb->stage == APP_KB_STAGE_GROUP) {
        switch (kb->group) {
        case APP_KB_ITEM_SPACE:
            if (kb->len < APP_KB_BUF_MAX) {
                kb->buf[kb->len++] = ' ';
                kb->buf[kb->len] = '\0';
                return APP_KB_EV_CHAR;
            }
            return APP_KB_EV_NONE;
        case APP_KB_ITEM_BACK:
            app_kb_backspace(kb);
            return APP_KB_EV_BACKSPACE;
        case APP_KB_ITEM_DONE:
            return APP_KB_EV_DONE;
        default:  // 字符组
            kb->group_active = kb->group;
            kb->char_sel = 1;  // 停在该组第一个字符
            kb->stage = APP_KB_STAGE_CHAR;
            return APP_KB_EV_ENTER_GROUP;
        }
    }
    if (kb->char_sel == 0) {
        kb->stage = APP_KB_STAGE_GROUP;
        return APP_KB_EV_GROUP_BACK;
    }
    if (kb->len < APP_KB_BUF_MAX) {
        // 录入后光标停在原位:连续输入同一/相邻字符无需重新定位
        kb->buf[kb->len++] = app_kb_char_at(kb->group_active, kb->char_sel);
        kb->buf[kb->len] = '\0';
        return APP_KB_EV_CHAR;
    }
    return APP_KB_EV_NONE;
}

bool app_kb_backspace(app_kb_t *kb) {
    if (kb == NULL || kb->len == 0) return false;
    kb->buf[--kb->len] = '\0';
    return true;
}

const char *app_kb_text(const app_kb_t *kb) {
    return kb != NULL ? kb->buf : "";
}

size_t app_kb_len(const app_kb_t *kb) {
    return kb != NULL ? kb->len : 0;
}

uint8_t app_kb_stage(const app_kb_t *kb) {
    return kb != NULL ? kb->stage : APP_KB_STAGE_GROUP;
}

uint8_t app_kb_group(const app_kb_t *kb) {
    return kb != NULL ? kb->group : 0;
}

uint8_t app_kb_group_active(const app_kb_t *kb) {
    return kb != NULL ? kb->group_active : 0;
}

uint16_t app_kb_char_sel(const app_kb_t *kb) {
    return kb != NULL ? kb->char_sel : 0;
}

char app_kb_char_at(uint8_t group, uint16_t idx) {
    if (group >= APP_KB_GROUP_NUM || idx == 0) return 0;
    const char *str = k_group_strs[group];
    if (idx > strlen(str)) return 0;
    return str[idx - 1];
}
