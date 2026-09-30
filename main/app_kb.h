// main/app_kb.h —— 三键两级键盘的纯逻辑模型(设备只有 上/下/确定 三键)。
//
// 交互设计(第一级"分组",第二级"组内字符"):
//   第一级条目:[abc] [ABC] [123] [符号] [空格] [退格] [完成]
//     上/下 移动选中(环绕);长按无效(仅 7 项)
//     确定:字符组 → 进入第二级;空格 → 直接插入;退格 → 删除一位;完成 → 结束
//   第二级(组内字符,含首位"返回"项):
//     上/下 移动(环绕);长按 ±APP_KB_JUMP_STEP
//     确定:普通字符 → 录入并停在原位(便于连续输入);"返回"项 → 回第一级
//   任意时刻:确定双击 = 退格;确定长按 = 完成(由 UI 层调用 backspace/done 语义)
//
// 四个字符组合计覆盖全部 95 个可打印 ASCII(空格单列)。本模块无平台依赖,
// 由 tests/test_app_kb.c 宿主测试覆盖。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define APP_KB_BUF_MAX 64
#define APP_KB_GROUP_NUM     4  // 小写/大写/数字/符号
#define APP_KB_GROUP_LOWER   0
#define APP_KB_GROUP_UPPER   1
#define APP_KB_GROUP_DIGIT   2
#define APP_KB_GROUP_PUNCT   3
// 第一级条目索引:4 个字符组 + 三个直达项
#define APP_KB_ITEM_SPACE    4
#define APP_KB_ITEM_BACK     5
#define APP_KB_ITEM_DONE     6
#define APP_KB_ITEM_NUM      7
// 第二级光标:0 = "返回分组"项;1..len = 组内第 idx 个字符
#define APP_KB_JUMP_STEP     5  // 长按快速移动步长

typedef enum {
    APP_KB_STAGE_GROUP = 0,  // 第一级:分组选择
    APP_KB_STAGE_CHAR = 1,   // 第二级:组内字符
} app_kb_stage_t;

typedef enum {
    APP_KB_EV_NONE = 0,
    APP_KB_EV_CHAR,          // 录入了一个字符(含空格)
    APP_KB_EV_ENTER_GROUP,   // 进入字符组
    APP_KB_EV_GROUP_BACK,    // 从字符组返回分组
    APP_KB_EV_BACKSPACE,
    APP_KB_EV_DONE,
} app_kb_event_t;

typedef struct {
    char buf[APP_KB_BUF_MAX + 1];
    size_t len;
    uint8_t stage;         // app_kb_stage_t
    uint8_t group;         // 第一级选中项 0..APP_KB_ITEM_NUM-1
    uint8_t group_active;  // 第二级当前展开的字符组(0..3)
    uint16_t char_sel;     // 第二级光标(0 = 返回项)
} app_kb_t;

// 各字符组的内容(下标 0..APP_KB_GROUP_NUM-1)。
const char *app_kb_group_str(uint8_t group);
size_t app_kb_group_len(uint8_t group);

// 复位缓冲区;停在第 0 项(小写组)。
void app_kb_init(app_kb_t *kb);

// 当前级内移动选中位(环绕)。长按由调用方传 ±APP_KB_JUMP_STEP。
void app_kb_move(app_kb_t *kb, int delta);

// 确定短按:按当前级解释选择并执行。
app_kb_event_t app_kb_pick(app_kb_t *kb);

// 退格一位;空缓冲时无效果并返回 false。
bool app_kb_backspace(app_kb_t *kb);

const char *app_kb_text(const app_kb_t *kb);
size_t app_kb_len(const app_kb_t *kb);
uint8_t app_kb_stage(const app_kb_t *kb);
uint8_t app_kb_group(const app_kb_t *kb);        // 第一级选中项
uint8_t app_kb_group_active(const app_kb_t *kb); // 第二级展开的组
uint16_t app_kb_char_sel(const app_kb_t *kb);    // 第二级光标(0 = 返回项)

// 第二级某位要显示的字符:idx 0 为返回项(返回 0),1..len 为组内字符。
char app_kb_char_at(uint8_t group, uint16_t idx);
