// main/app_ui.h —— TOTP 应用自有 UI(全新设计,不复用基线演示外壳)。
//
// 页面:首次设密 / 解锁(含惩罚倒计时)/ 验证码主列表(30s 刷新+翻页)/
// 主菜单 / 远程管理(URL+二维码+数字码)/ Wi-Fi 状态与操作 / SoftAP 配网 /
// 删除确认。单极简主题,右上角电量(约定位置),30s 无输入调暗背光。
//
// 并发:app_ui_key 由输入任务调用(自行取 LVGL 锁);1s 周期 lv_timer 在
// LVGL 任务内做刷新/空闲锁定/状态同步。app_lock 访问统一经内部互斥锁。
#pragma once

#include "bsp_button.h"

// 创建状态栏、恢复锁定状态(读 app_store)并显示首屏。LVGL 任务或持锁上下文调用。
void app_ui_init(void);

// 按键事件入口(输入任务上下文)。回调线程约定:入队后串行调用本函数。
void app_ui_key(bsp_btn_t btn, bsp_btn_ev_t ev);
