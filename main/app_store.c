// main/app_store.c —— NVS 持久化实现,见 app_store.h。
#include "app_store.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "app_store";
#define NS "totp"

// 缓存与 NVS 的一致性策略:互斥锁内"先写 NVS,成功后改缓存"。NVS 写失败时
// 缓存不变,调用方拿到错误码;HTTP/UI 层把失败呈现给用户,而不是悄悄丢数据。
static SemaphoreHandle_t s_mtx;
static totp_entry_t s_entries[TOTP_ENTRY_MAX];
static size_t s_count;
static bool s_has_pwd;
static uint32_t s_generation;

static esp_err_t nvs_u8_get(nvs_handle_t h, const char *key, uint8_t *out, uint8_t def) {
    esp_err_t err = nvs_get_u8(h, key, out);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        *out = def;
        return ESP_OK;
    }
    return err;
}

static esp_err_t nvs_u32_get(nvs_handle_t h, const char *key, uint32_t *out) {
    esp_err_t err = nvs_get_u32(h, key, out);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        *out = 0;
        return ESP_OK;
    }
    return err;
}

static esp_err_t load_all_locked(void) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;  // 首次启动,无命名空间
    if (err != ESP_OK) return err;

    uint8_t cnt = 0;
    err = nvs_u8_get(h, "ent_cnt", &cnt, 0);
    if (err == ESP_OK) {
        for (uint8_t i = 0; i < cnt && i < TOTP_ENTRY_MAX && err == ESP_OK; i++) {
            char key[8];
            snprintf(key, sizeof(key), "ent%u", i);
            uint8_t blob[TOTP_ENTRY_BLOB_MAX];
            size_t blob_len = sizeof(blob);
            err = nvs_get_blob(h, key, blob, &blob_len);
            if (err == ESP_ERR_NVS_NOT_FOUND) continue;  // 容忍个别空洞
            if (err == ESP_OK &&
                totp_entry_unpack(blob, blob_len, &s_entries[s_count])) {
                s_count++;
            }
        }
    }
    uint8_t has_pwd = 0;
    nvs_u8_get(h, "has_pwd", &has_pwd, 0);
    s_has_pwd = has_pwd != 0;
    nvs_close(h);
    return ESP_OK;
}

esp_err_t app_store_init(void) {
    if (s_mtx) return ESP_OK;
    s_mtx = xSemaphoreCreateMutex();
    if (!s_mtx) return ESP_ERR_NO_MEM;

    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK) {
        // NVS 里是用户的 TOTP 密钥:分区异常时绝不自动擦除,交给上层呈现。
        ESP_LOGE(TAG, "NVS 初始化失败(%s);不自动擦除分区", esp_err_to_name(err));
        vSemaphoreDelete(s_mtx);
        s_mtx = NULL;
        return err;
    }
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    err = load_all_locked();
    xSemaphoreGive(s_mtx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "条目缓存加载失败: %s", esp_err_to_name(err));
    }
    return err;
}

// ---------------------------------------------------------------------------
// 密码与锁定状态
// ---------------------------------------------------------------------------
bool app_store_has_password(void) {
    return s_has_pwd;
}

esp_err_t app_store_save_password(const uint8_t salt[4], uint64_t hash) {
    if (!s_mtx || salt == NULL) return ESP_ERR_INVALID_STATE;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(h, "pwd_salt", salt, 4);
    if (err == ESP_OK) err = nvs_set_u64(h, "pwd_hash", hash);
    if (err == ESP_OK) err = nvs_set_u8(h, "has_pwd", 1);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err == ESP_OK) s_has_pwd = true;
    return err;
}

esp_err_t app_store_load_password(uint8_t salt[4], uint64_t *hash) {
    if (!s_mtx || !salt || !hash) return ESP_ERR_INVALID_STATE;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READONLY, &h);
    if (err != ESP_OK) return err == ESP_ERR_NVS_NOT_FOUND ? ESP_ERR_NOT_FOUND : err;
    size_t len = 4;
    err = nvs_get_blob(h, "pwd_salt", salt, &len);
    if (err == ESP_OK) err = nvs_get_u64(h, "pwd_hash", hash);
    nvs_close(h);
    if (err == ESP_OK && len != 4) err = ESP_ERR_INVALID_SIZE;
    return err;
}

esp_err_t app_store_load_lock(uint8_t *fail_count, uint32_t *lock_until) {
    if (!s_mtx || !fail_count || !lock_until) return ESP_ERR_INVALID_STATE;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        *fail_count = 0;
        *lock_until = 0;
        return ESP_OK;
    }
    if (err != ESP_OK) return err;
    err = nvs_u8_get(h, "fail_cnt", fail_count, 0);
    if (err == ESP_OK) err = nvs_u32_get(h, "lock_until", lock_until);
    nvs_close(h);
    return err;
}

esp_err_t app_store_save_lock(uint8_t fail_count, uint32_t lock_until) {
    if (!s_mtx) return ESP_ERR_INVALID_STATE;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(h, "fail_cnt", fail_count);
    if (err == ESP_OK) err = nvs_set_u32(h, "lock_until", lock_until);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

// ---------------------------------------------------------------------------
// Wi-Fi 凭证
// ---------------------------------------------------------------------------
bool app_store_wifi_has(void) {
    if (!s_mtx) return false;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READONLY, &h);
    if (err != ESP_OK) return false;
    size_t len = 1;
    bool has = nvs_get_str(h, "wf_ssid", NULL, &len) == ESP_OK;
    nvs_close(h);
    return has;
}

esp_err_t app_store_wifi_save(const char *ssid, const char *password) {
    if (!s_mtx || !ssid || !password || !ssid[0] ||
        strlen(ssid) > APP_STORE_WIFI_SSID_MAX ||
        strlen(password) > APP_STORE_WIFI_PASS_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_str(h, "wf_ssid", ssid);
    if (err == ESP_OK) err = nvs_set_str(h, "wf_pass", password);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t app_store_wifi_load(char *ssid, size_t ssid_cap, char *pass, size_t pass_cap) {
    if (!s_mtx || !ssid || !pass || ssid_cap < APP_STORE_WIFI_SSID_MAX + 1 ||
        pass_cap < APP_STORE_WIFI_PASS_MAX + 1) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READONLY, &h);
    if (err != ESP_OK) return err == ESP_ERR_NVS_NOT_FOUND ? ESP_ERR_NOT_FOUND : err;
    size_t len = ssid_cap;
    err = nvs_get_str(h, "wf_ssid", ssid, &len);
    if (err == ESP_OK) {
        len = pass_cap;
        err = nvs_get_str(h, "wf_pass", pass, &len);
    }
    nvs_close(h);
    return err;
}

esp_err_t app_store_wifi_clear(void) {
    if (!s_mtx) return ESP_ERR_INVALID_STATE;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    nvs_erase_key(h, "wf_ssid");
    nvs_erase_key(h, "wf_pass");
    err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t app_store_save_lang(uint8_t lang) {
    if (!s_mtx) return ESP_ERR_INVALID_STATE;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(h, "lang", lang);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t app_store_load_lang(uint8_t *lang) {
    if (!s_mtx || !lang) return ESP_ERR_INVALID_STATE;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        *lang = 0;
        return ESP_OK;
    }
    if (err != ESP_OK) return err;
    err = nvs_get_u8(h, "lang", lang);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        *lang = 0;
        err = ESP_OK;
    }
    nvs_close(h);
    return err;
}

// ---------------------------------------------------------------------------
// TOTP 条目
// ---------------------------------------------------------------------------
size_t app_store_entry_count(void) {
    return s_count;
}

uint32_t app_store_generation(void) {
    return s_generation;
}

esp_err_t app_store_entry_get(size_t idx, totp_entry_t *out) {
    if (!out) return ESP_ERR_INVALID_ARG;
    if (!s_mtx) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    esp_err_t err = ESP_OK;
    if (idx >= s_count) {
        err = ESP_ERR_INVALID_ARG;
    } else {
        *out = s_entries[idx];
    }
    xSemaphoreGive(s_mtx);
    return err;
}

esp_err_t app_store_entry_add(const totp_entry_t *entry) {
    if (!entry || !totp_entry_valid(entry)) return ESP_ERR_INVALID_ARG;
    if (!s_mtx) return ESP_ERR_INVALID_STATE;

    uint8_t blob[TOTP_ENTRY_BLOB_MAX];
    size_t blob_len = totp_entry_pack(entry, blob, sizeof(blob));
    if (blob_len == 0) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(s_mtx, portMAX_DELAY);
    esp_err_t err;
    if (s_count >= TOTP_ENTRY_MAX) {
        err = ESP_ERR_NO_MEM;
        goto out;
    }
    nvs_handle_t h;
    err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) goto out;
    char key[8];
    snprintf(key, sizeof(key), "ent%u", (unsigned)s_count);
    err = nvs_set_blob(h, key, blob, blob_len);
    if (err == ESP_OK) {
        err = nvs_set_u8(h, "ent_cnt", (uint8_t)(s_count + 1));
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err == ESP_OK) {
        s_entries[s_count++] = *entry;
        s_generation++;
    }
out:
    xSemaphoreGive(s_mtx);
    return err;
}

esp_err_t app_store_entry_delete(size_t idx) {
    if (!s_mtx) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    esp_err_t err = ESP_OK;
    if (idx >= s_count) {
        err = ESP_ERR_INVALID_ARG;
        goto out;
    }
    nvs_handle_t h;
    err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) goto out;
    // swap-delete:最后一个条目前移覆盖被删槽位,再删除末尾 key、计数减一。
    // NVS 无列表语义,这是维持紧凑下标(键名稳定)的最简单方案。
    if (idx != s_count - 1) {
        s_entries[idx] = s_entries[s_count - 1];
    }
    s_count--;
    char key[8];
    for (size_t i = idx; i <= s_count; i++) {
        snprintf(key, sizeof(key), "ent%u", (unsigned)i);
        if (i < s_count) {
            uint8_t blob[TOTP_ENTRY_BLOB_MAX];
            size_t blob_len = totp_entry_pack(&s_entries[i], blob, sizeof(blob));
            err = blob_len ? nvs_set_blob(h, key, blob, blob_len) : ESP_ERR_INVALID_ARG;
            if (err != ESP_OK) break;
        } else {
            esp_err_t erase_err = nvs_erase_key(h, key);
            if (erase_err != ESP_OK && erase_err != ESP_ERR_NVS_NOT_FOUND) {
                err = erase_err;
                break;
            }
        }
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(h, "ent_cnt", (uint8_t)s_count);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err == ESP_OK) {
        s_generation++;
    }
out:
    xSemaphoreGive(s_mtx);
    return err;
}
