// tests/test_app_lang.c —— 双语字符串表宿主测试:
// 两表等长且无空项、切换/持久化语义、关键文案抽查。
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "app_lang.h"

static void test_tables_complete(void) {
    for (int lang = 0; lang < 2; lang++) {
        app_lang_set_current((app_lang_t)lang);
        for (int id = 0; id < (int)TR_ID_COUNT; id++) {
            const char *s = tr((tr_id_t)id);
            if (s == NULL || s[0] == '\0') {
                printf("lang=%d id=%d: EMPTY\n", lang, id);
                assert(0);
            }
        }
    }
}

static void test_specific_values(void) {
    app_lang_set_current(APP_LANG_ZH);
    assert(strcmp(tr(TR_MENU_LOCK), "立即锁定") == 0);
    assert(strcmp(tr(TR_BACK), "返回") == 0);
    assert(strstr(tr(TR_FMT_TRIES), "%d") != NULL);   // 格式串保留占位符
    assert(strstr(tr(TR_FMT_KB_COUNT), "%u") != NULL);

    app_lang_set_current(APP_LANG_EN);
    assert(strcmp(tr(TR_MENU_LOCK), "Lock now") == 0);
    assert(strcmp(tr(TR_BACK), "Back") == 0);
    assert(strcmp(tr(TR_WEB_SIGN_IN), "Sign in") == 0);
    assert(strstr(tr(TR_ERR_WRONG_CODE), "Wrong code") != NULL);
}

static void test_toggle_and_set(void) {
    app_lang_set_current(APP_LANG_ZH);
    assert(app_lang_current() == APP_LANG_ZH);
    assert(app_lang_toggle() == APP_LANG_EN);
    assert(app_lang_current() == APP_LANG_EN);
    assert(app_lang_toggle() == APP_LANG_ZH);
    // 非法值被忽略(保持当前)
    app_lang_set_current((app_lang_t)9);
    assert(app_lang_current() == APP_LANG_ZH);
    // 格式串参数顺序两语言一致(设备端按同一调用渲染)
    assert(strcmp(tr(TR_FMT_STATE), "状态：%s") == 0);
    app_lang_set_current(APP_LANG_EN);
    assert(strcmp(tr(TR_FMT_STATE), "State: %s") == 0);
    app_lang_set_current(APP_LANG_ZH);
}

int main(void) {
    test_tables_complete();
    test_specific_values();
    test_toggle_and_set();
    printf("test_app_lang: PASS (%d strings x2)\n", (int)TR_ID_COUNT);
    return 0;
}
