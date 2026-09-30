// main/app_wifi.h —— Wi-Fi 管理器:STA 自动连接/重试、设备端扫描、SNTP 校时。
//
// 设计要点:
//   - 仅 STA 模式;配网完全在设备端完成(屏幕扫描列表 + 三键键盘,见 app_ui),
//     无 SoftAP、无蓝牙。凭证只保存在本应用 NVS(app_store),esp_wifi 用
//     WIFI_STORAGE_RAM;密码绝不写日志。
//   - 认证失败/找不到 AP 是终态(等用户重配);其它断开原因按退避自动重连;
//     CONNECTING 超时由看门狗重新发起连接兜底。
//   - 拿到 IP 后启动 SNTP(UTC)。TOTP 依赖正确的墙上时钟,time_synced() 为
//     唯一可信判断;未同步前 UI 不显示验证码。
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

typedef enum {
    APP_WIFI_OFF = 0,       // 未初始化
    APP_WIFI_IDLE,          // 已初始化,无凭证
    APP_WIFI_CONNECTING,    // STA 连接中
    APP_WIFI_CONNECTED,     // 已拿到 IP
    APP_WIFI_AUTH_FAIL,     // 认证失败(密码错),停止自动重试
    APP_WIFI_NO_AP,         // 找不到 SSID,停止自动重试
    APP_WIFI_RETRY_WAIT,    // 断开后等待定时重试
    APP_WIFI_ERROR,         // 其它不可恢复错误
} app_wifi_state_t;

// 初始化网络栈与 Wi-Fi 驱动(幂等)。不决定连接,由调用方继续调度。
esp_err_t app_wifi_init(void);

// 有凭证 → 连接 STA;无凭证 → 保持 IDLE,等待用户从屏幕发起扫描连接。
// app_main 开机调用。
esp_err_t app_wifi_autostart(void);

// 保存凭证并立即以 STA 连接(键盘页确认后调用)。空 SSID 拒绝;
// 空密码按开放网络处理。
esp_err_t app_wifi_apply_credentials(const char *ssid, const char *password);

// 清除已存凭证并断开 STA(设备菜单"清除凭证")。
esp_err_t app_wifi_clear_credentials(void);

// ---------------------------------------------------------------------------
// 设备端非阻塞扫描(屏幕"扫描连接"页用):启动后由 1s UI tick 轮询取结果。
// ---------------------------------------------------------------------------
#define APP_WIFI_SCAN_MAX 10

typedef struct {
    char ssid[33];
    int rssi;
    bool open;  // 开放网络(无需密码)
} app_wifi_scan_item_t;

// 启动一次非阻塞扫描。连接进行中返回 false。
bool app_wifi_scan_begin(void);

// 取扫描结果(按 RSSI 降序,至多 cap 条)。扫描未完成返回 -1;
// 完成则填充 out 并返回条数(0 = 周围没有可见网络),同时清除完成标志。
int app_wifi_scan_fetch(app_wifi_scan_item_t *out, size_t cap);

// 状态与展示信息(线程安全,内部小锁)。
app_wifi_state_t app_wifi_state(void);
const char *app_wifi_state_text(void);  // 人读状态短语(中文,供 UI/网页)
const char *app_wifi_ssid(void);        // 当前/最近使用的 SSID
const char *app_wifi_ip(void);          // 已连接时的 IPv4 字符串;否则 ""
bool app_wifi_time_synced(void);        // 墙上时钟可信(> 2025-01-01)
