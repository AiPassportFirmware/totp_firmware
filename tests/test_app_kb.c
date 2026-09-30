// tests/test_app_kb.c —— 三键两级键盘模型宿主测试。
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "app_kb.h"

static void test_groups_cover_printable(void) {
    // 四组 + 空格必须恰好覆盖全部 95 个可打印 ASCII,且互不重复。
    int seen[128] = { 0 };
    seen[' '] = 1;
    size_t total = 1;
    for (uint8_t g = 0; g < APP_KB_GROUP_NUM; g++) {
        const char *str = app_kb_group_str(g);
        assert(strlen(str) == app_kb_group_len(g));
        for (const char *p = str; *p; p++) {
            unsigned char c = (unsigned char)*p;
            assert(c >= 0x20 && c < 0x7F);
            assert(!seen[c]);  // 无重复
            seen[c] = 1;
            total++;
        }
    }
    assert(total == 95);
}

static void test_stage1_navigation_and_actions(void) {
    app_kb_t kb;
    app_kb_init(&kb);
    assert(app_kb_stage(&kb) == APP_KB_STAGE_GROUP);
    assert(app_kb_group(&kb) == 0);

    // 7 项环绕
    for (int i = 0; i < APP_KB_ITEM_NUM; i++) {
        app_kb_move(&kb, 1);
    }
    assert(app_kb_group(&kb) == 0);
    app_kb_move(&kb, -1);
    assert(app_kb_group(&kb) == APP_KB_ITEM_DONE);

    // 空格直达项:录入空格且留在第一级
    app_kb_move(&kb, APP_KB_ITEM_SPACE - APP_KB_ITEM_DONE);
    assert(app_kb_pick(&kb) == APP_KB_EV_CHAR);
    assert(strcmp(app_kb_text(&kb), " ") == 0);
    assert(app_kb_stage(&kb) == APP_KB_STAGE_GROUP);

    // 退格直达项
    app_kb_move(&kb, APP_KB_ITEM_BACK - APP_KB_ITEM_SPACE);
    assert(app_kb_pick(&kb) == APP_KB_EV_BACKSPACE);
    assert(app_kb_len(&kb) == 0);

    // 完成直达项
    app_kb_move(&kb, APP_KB_ITEM_DONE - APP_KB_ITEM_BACK);
    assert(app_kb_pick(&kb) == APP_KB_EV_DONE);
}

static void test_stage2_type_and_back(void) {
    app_kb_t kb;
    app_kb_init(&kb);

    // 进入数字组
    app_kb_move(&kb, APP_KB_GROUP_DIGIT);
    assert(app_kb_pick(&kb) == APP_KB_EV_ENTER_GROUP);
    assert(app_kb_stage(&kb) == APP_KB_STAGE_CHAR);
    assert(app_kb_group_active(&kb) == APP_KB_GROUP_DIGIT);
    assert(app_kb_char_sel(&kb) == 1);

    // 录入 "01":录入后光标停在原位
    assert(app_kb_pick(&kb) == APP_KB_EV_CHAR);  // sel=1 → '0'
    assert(strcmp(app_kb_text(&kb), "0") == 0);
    assert(app_kb_char_sel(&kb) == 1);
    app_kb_move(&kb, 1);
    assert(app_kb_pick(&kb) == APP_KB_EV_CHAR);  // sel=2 → '1'
    assert(strcmp(app_kb_text(&kb), "01") == 0);

    // 移到"返回"项(0 号位)并返回第一级;组选择保留在数字组
    app_kb_move(&kb, -2);
    assert(app_kb_char_sel(&kb) == 0);
    assert(app_kb_pick(&kb) == APP_KB_EV_GROUP_BACK);
    assert(app_kb_stage(&kb) == APP_KB_STAGE_GROUP);
    assert(app_kb_group(&kb) == APP_KB_GROUP_DIGIT);

    // 组内环绕:10 个字符 + 返回项 = 11 项
    assert(app_kb_pick(&kb) == APP_KB_EV_ENTER_GROUP);
    for (int i = 0; i < 11; i++) {
        app_kb_move(&kb, 1);
    }
    assert(app_kb_char_sel(&kb) == 1);
    // 反向长按跳 5(含环绕):1 → 7 → 2 → 8
    app_kb_move(&kb, -APP_KB_JUMP_STEP);
    assert(app_kb_char_sel(&kb) == 7);
    app_kb_move(&kb, -APP_KB_JUMP_STEP);
    assert(app_kb_char_sel(&kb) == 2);
    app_kb_move(&kb, -APP_KB_JUMP_STEP);
    assert(app_kb_char_sel(&kb) == 8);
    // 正向跳 5 越过 0 号位:8 → 13? 组内共 11 项 → 8+5=13 → 2
    app_kb_move(&kb, APP_KB_JUMP_STEP);
    assert(app_kb_char_sel(&kb) == 2);
}

static void test_type_password_flow(void) {
    app_kb_t kb;
    app_kb_init(&kb);
    // 输入 "ab3 ":小写组两字母 + 数字组 '3' + 空格直达
    assert(app_kb_pick(&kb) == APP_KB_EV_ENTER_GROUP);        // 进小写组,光标 'a'
    assert(app_kb_pick(&kb) == APP_KB_EV_CHAR);               // 'a'
    app_kb_move(&kb, 1);
    assert(app_kb_pick(&kb) == APP_KB_EV_CHAR);               // 'b'
    assert(strcmp(app_kb_text(&kb), "ab") == 0);
    // 返回第一级(上到 0 号位)
    app_kb_move(&kb, -(int)app_kb_char_sel(&kb));
    assert(app_kb_pick(&kb) == APP_KB_EV_GROUP_BACK);
    // 切到数字组
    app_kb_move(&kb, APP_KB_GROUP_DIGIT);
    assert(app_kb_pick(&kb) == APP_KB_EV_ENTER_GROUP);
    app_kb_move(&kb, 3);  // sel:1('0') -> 4('3')
    assert(app_kb_pick(&kb) == APP_KB_EV_CHAR);
    assert(app_kb_char_at(APP_KB_GROUP_DIGIT, 4) == '3');
    assert(strcmp(app_kb_text(&kb), "ab3") == 0);
    // 回第一级按空格
    app_kb_move(&kb, -(int)app_kb_char_sel(&kb));
    assert(app_kb_pick(&kb) == APP_KB_EV_GROUP_BACK);
    app_kb_move(&kb, APP_KB_ITEM_SPACE - APP_KB_GROUP_DIGIT);  // 数字组 → 空格项
    assert(app_kb_pick(&kb) == APP_KB_EV_CHAR);
    assert(strcmp(app_kb_text(&kb), "ab3 ") == 0);
    // 退格与完成
    assert(app_kb_backspace(&kb));
    assert(strcmp(app_kb_text(&kb), "ab3") == 0);
    assert(app_kb_backspace(&kb) && app_kb_backspace(&kb) && app_kb_backspace(&kb));
    assert(!app_kb_backspace(&kb));
}

static void test_buffer_full(void) {
    app_kb_t kb;
    app_kb_init(&kb);
    assert(app_kb_pick(&kb) == APP_KB_EV_ENTER_GROUP);
    while (app_kb_len(&kb) < APP_KB_BUF_MAX) {
        assert(app_kb_pick(&kb) == APP_KB_EV_CHAR);  // 光标停在 'a',重复录入
    }
    assert(app_kb_pick(&kb) == APP_KB_EV_NONE);  // 满:录入为空操作
    assert(app_kb_len(&kb) == APP_KB_BUF_MAX);
    assert(app_kb_backspace(&kb));
    assert(app_kb_len(&kb) == APP_KB_BUF_MAX - 1);
}

int main(void) {
    test_groups_cover_printable();
    test_stage1_navigation_and_actions();
    test_stage2_type_and_back();
    test_type_password_flow();
    test_buffer_full();
    printf("test_app_kb: PASS\n");
    return 0;
}
