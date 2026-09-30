// main/app_server.h —— 远程管理 HTTP 服务(STA 局域网内)。
//
// 流程(对应需求):设备端进入"Remote Mgmt"→ 生成随机 8 位数字码 →
// 屏幕显示 URL(http://<设备IP>/)+ 二维码 + 数字码 → 局域网内浏览器打开
// URL,输入数字码验证 → 获得 5 分钟滑动过期的会话 → 可增删 TOTP 条目。
//
// 安全设计:
//   - 会话令牌随机生成,HttpOnly Cookie,5 分钟无活动过期;
//   - 同一管理模式内累计 5 次登录失败 → 服务自动结束(需设备端重新进入);
//   - 设备锁定(15 分钟无操作/手动锁定)会同步结束管理模式;
//   - 全程明文 HTTP:密钥不会通过本服务读取或返回,泄露面限于"增删条目",
//     但请在可信局域网内使用(无 TLS 是已知限制,见交付文档)。
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

esp_err_t app_server_start(void);
void app_server_stop(void);
bool app_server_active(void);

const char *app_server_code(void);              // 8 位数字码字符串
void app_server_url(char *buf, size_t cap);     // "http://<ip>/"
int app_server_fails_left(void);                // 剩余可尝试次数(含本次)
bool app_server_session_alive(void);
const char *app_server_note(void);              // 最近一次状态提示(UI 展示)

// 网页端主动请求退出(UI 轮询到后调用 app_server_stop 收尾)。
bool app_server_exit_requested(void);
