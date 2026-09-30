// main/app_store.h —— NVS 持久化 + TOTP 条目内存缓存。
//
// 职责:
//   - 密码(salt+hash)、连续失败计数、惩罚截止时间的读写(解锁策略防断电绕过);
//   - Wi-Fi 凭证读写(绝不写日志);
//   - TOTP 条目的增删查:内存缓存 + NVS 落盘,互斥锁保护,可从 LVGL/UI 任务、
//     HTTP 服务任务并发调用。
//
// 条目按 NVS blob 持久化(app_totp 的 pack/unpack),删除用 swap-delete 压缩空洞。
// 本模块绝不自动擦除 NVS:分区损坏时报错返回,由上层在 UI 上呈现(密钥无价)。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "app_totp.h"
#include "esp_err.h"

#define APP_STORE_WIFI_SSID_MAX 32
#define APP_STORE_WIFI_PASS_MAX 64

// 初始化:打开 NVS(不自动擦除)、加载条目缓存。重复调用安全。
esp_err_t app_store_init(void);

// ---------------------------------------------------------------------------
// 密码与锁定状态
// ---------------------------------------------------------------------------
bool app_store_has_password(void);
esp_err_t app_store_save_password(const uint8_t salt[4], uint64_t hash);
// 读取密码 salt 与哈希(仅在 has_password() 为真时有意义)。
esp_err_t app_store_load_password(uint8_t salt[4], uint64_t *hash);
// 读取持久化的连续失败计数与惩罚截止 epoch 秒(*lock_until 为 0 表示无)。
esp_err_t app_store_load_lock(uint8_t *fail_count, uint32_t *lock_until);
esp_err_t app_store_save_lock(uint8_t fail_count, uint32_t lock_until);

// ---------------------------------------------------------------------------
// 界面语言(1 字节:0=中文,1=English;见 app_lang.h)
// ---------------------------------------------------------------------------
esp_err_t app_store_save_lang(uint8_t lang);
// 未保存过时 *lang 返回 0(中文)。
esp_err_t app_store_load_lang(uint8_t *lang);

// ---------------------------------------------------------------------------
// Wi-Fi 凭证
// ---------------------------------------------------------------------------
bool app_store_wifi_has(void);
esp_err_t app_store_wifi_save(const char *ssid, const char *password);
// ssid_cap >= 33, pass_cap >= 65。未配置时返回 ESP_ERR_NOT_FOUND。
esp_err_t app_store_wifi_load(char *ssid, size_t ssid_cap, char *pass, size_t pass_cap);
esp_err_t app_store_wifi_clear(void);

// ---------------------------------------------------------------------------
// TOTP 条目(内存缓存 + 落盘)
// ---------------------------------------------------------------------------
size_t app_store_entry_count(void);
// 每次增删自增;UI 用它检测"远程已变更"并刷新,无需推送机制。
uint32_t app_store_generation(void);
// idx 越界返回 ESP_ERR_INVALID_ARG。
esp_err_t app_store_entry_get(size_t idx, totp_entry_t *out);
// 缓存满(TOTP_ENTRY_MAX)返回 ESP_ERR_NO_MEM;密钥重复时仍允许(同一站点不同账号)。
esp_err_t app_store_entry_add(const totp_entry_t *entry);
esp_err_t app_store_entry_delete(size_t idx);
