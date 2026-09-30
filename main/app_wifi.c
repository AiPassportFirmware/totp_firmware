// main/app_wifi.c —— Wi-Fi 管理器实现,见 app_wifi.h。
#include "app_wifi.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "app_lang.h"
#include "app_store.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "app_wifi";

// 时钟可信下限:2025-01-01(TOTP 与 60 分钟惩罚都依赖真实时间)。
#define TIME_VALID_MIN_S 1735689600LL

// 可重试断开的退避策略:前 5 次快重试,之后放慢;认证失败/无 AP 是终态。
#define FAST_RETRY_MAX 5
#define FAST_RETRY_MS 2000
#define SLOW_RETRY_MS 10000
// 连接看门狗:CONNECTING 超过该秒数仍无事件,则重新发起连接兜底。
#define CONNECT_WATCHDOG_S 15
typedef struct {
    esp_netif_t *sta_netif;
    esp_timer_handle_t retry_timer;
    esp_timer_handle_t connect_watchdog;
    bool wifi_started;
    char ssid[APP_STORE_WIFI_SSID_MAX + 1];
    char ip[16];
    volatile app_wifi_state_t state;
    uint32_t connecting_since_s;
    int fast_retries;
    bool sntp_started;
} app_wifi_t;

static app_wifi_t s_wi;
static volatile bool s_scan_done;  // WIFI_EVENT_SCAN_DONE 置位,scan_fetch 消费

static esp_err_t connect_sta(void);

// 内部状态迁移统一走这里,保证与展示字符串一致。
static void set_state(app_wifi_state_t st) {
    s_wi.state = st;
    if (st == APP_WIFI_CONNECTING) {
        s_wi.connecting_since_s = (uint32_t)(esp_timer_get_time() / 1000000LL);
    }
}

const char *app_wifi_state_text(void) {
    // 设备 UI 与远程管理页共用,随设备语言。
    switch (s_wi.state) {
    case APP_WIFI_OFF:
        return tr(TR_WIFI_OFF);
    case APP_WIFI_IDLE:
        return tr(TR_WIFI_IDLE);
    case APP_WIFI_CONNECTING:
        return tr(TR_WIFI_CONNECTING);
    case APP_WIFI_CONNECTED:
        return tr(TR_WIFI_CONNECTED);
    case APP_WIFI_AUTH_FAIL:
        return tr(TR_WIFI_AUTH_FAIL);
    case APP_WIFI_NO_AP:
        return tr(TR_WIFI_NO_AP);
    case APP_WIFI_RETRY_WAIT:
        return tr(TR_WIFI_RETRY_WAIT);
    default:
        return tr(TR_WIFI_ERROR);
    }
}

app_wifi_state_t app_wifi_state(void) {
    return s_wi.state;
}

const char *app_wifi_ssid(void) {
    return s_wi.ssid;
}

const char *app_wifi_ip(void) {
    return s_wi.ip;
}

bool app_wifi_time_synced(void) {
    return (long long)time(NULL) > TIME_VALID_MIN_S;
}

// ---------------------------------------------------------------------------
// 内部:连接重试
// ---------------------------------------------------------------------------
static void retry_timer_cb(void *arg) {
    (void)arg;
    // esp_timer 任务上下文:直接发起连接尝试(非阻塞调用)。
    if (s_wi.state == APP_WIFI_RETRY_WAIT) {
        set_state(APP_WIFI_CONNECTING);
        esp_err_t err = esp_wifi_connect();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_wifi_connect 失败: %s", esp_err_to_name(err));
            set_state(APP_WIFI_ERROR);
        }
    }
}

// 连接看门狗:CONNECTING 卡住(驱动静默、事件丢失)时重新发起连接。
static void connect_watchdog_cb(void *arg) {
    (void)arg;
    if (s_wi.state != APP_WIFI_CONNECTING) return;
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000000LL);
    if (now - s_wi.connecting_since_s < CONNECT_WATCHDOG_S) return;
    ESP_LOGW(TAG, "连接超过 %ds 无事件,重新发起连接", CONNECT_WATCHDOG_S);
    s_wi.connecting_since_s = now;
    esp_wifi_connect();
}

static void schedule_retry(void) {
    uint32_t delay_ms;
    s_wi.fast_retries++;
    if (s_wi.fast_retries <= FAST_RETRY_MAX) {
        delay_ms = FAST_RETRY_MS;
    } else {
        // 放慢节奏,兼顾功耗;一直重试到用户干预或网络恢复。
        delay_ms = SLOW_RETRY_MS;
    }
    set_state(APP_WIFI_RETRY_WAIT);
    esp_timer_stop(s_wi.retry_timer);
    esp_timer_start_once(s_wi.retry_timer, (uint64_t)delay_ms * 1000);
}

// ---------------------------------------------------------------------------
// 事件处理
// ---------------------------------------------------------------------------
static void on_sta_disconnected(const wifi_event_sta_disconnected_t *disc) {
    // 终态:密码错或 SSID 不存在 —— 继续重试只会耗电且掩盖问题;配网页面会显示。
    if (disc->reason == WIFI_REASON_AUTH_FAIL ||
        disc->reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT) {
        set_state(APP_WIFI_AUTH_FAIL);
        ESP_LOGW(TAG, "认证失败(reason=%d),停止自动重试", disc->reason);
        return;
    }
    if (disc->reason == WIFI_REASON_NO_AP_FOUND) {
        set_state(APP_WIFI_NO_AP);
        ESP_LOGW(TAG, "未找到 SSID,停止自动重试");
        return;
    }
    if (s_wi.state == APP_WIFI_CONNECTING || s_wi.state == APP_WIFI_CONNECTED ||
        s_wi.state == APP_WIFI_RETRY_WAIT) {
        ESP_LOGI(TAG, "断开(reason=%d),安排重试", disc->reason);
        schedule_retry();
    }
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg;
    (void)base;
    switch (id) {
    case WIFI_EVENT_STA_START:
        if (s_wi.ssid[0] != '\0') {
            set_state(APP_WIFI_CONNECTING);
            esp_wifi_connect();
        }
        break;
    case WIFI_EVENT_STA_DISCONNECTED:
        on_sta_disconnected((const wifi_event_sta_disconnected_t *)data);
        break;
    case WIFI_EVENT_SCAN_DONE:
        s_scan_done = true;
        break;
    default:
        break;
    }
}

static void on_got_ip(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg;
    (void)base;
    (void)id;
    ip_event_got_ip_t *evt = (ip_event_got_ip_t *)data;
    snprintf(s_wi.ip, sizeof(s_wi.ip), IPSTR, IP2STR(&evt->ip_info.ip));
    set_state(APP_WIFI_CONNECTED);
    s_wi.fast_retries = 0;
    ESP_LOGI(TAG, "已连接 SSID=%s IP=%s", s_wi.ssid, s_wi.ip);

    if (!s_wi.sntp_started) {
        esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
        if (esp_netif_sntp_init(&cfg) == ESP_OK) {
            s_wi.sntp_started = true;
        } else {
            ESP_LOGE(TAG, "SNTP 初始化失败");
        }
    } else {
        esp_netif_sntp_start();
    }
}

// ---------------------------------------------------------------------------
// 初始化与模式切换
// ---------------------------------------------------------------------------
esp_err_t app_wifi_init(void) {
    if (s_wi.wifi_started) return ESP_OK;
    memset(&s_wi, 0, sizeof(s_wi));

    esp_err_t err = esp_netif_init();
    if (err == ESP_OK) err = esp_event_loop_create_default();
    if (err == ESP_OK) {
        esp_netif_config_t cfg = ESP_NETIF_DEFAULT_WIFI_STA();
        s_wi.sta_netif = esp_netif_new(&cfg);
        err = s_wi.sta_netif ? ESP_OK : ESP_ERR_NO_MEM;
    }
    if (err == ESP_OK) err = esp_netif_attach_wifi_station(s_wi.sta_netif);
    if (err == ESP_OK) err = esp_wifi_set_default_wifi_sta_handlers();
    if (err == ESP_OK) {
        wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
        err = esp_wifi_init(&wcfg);
    }
    if (err == ESP_OK) err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        on_wifi_event, NULL);
    if (err == ESP_OK) err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        on_got_ip, NULL);
    if (err == ESP_OK) {
        esp_timer_create_args_t tcfg = {
            .callback = retry_timer_cb,
            .name = "wifi_retry",
        };
        err = esp_timer_create(&tcfg, &s_wi.retry_timer);
    }
    if (err == ESP_OK) {
        esp_timer_create_args_t tcfg = {
            .callback = connect_watchdog_cb,
            .name = "wifi_watchdog",
        };
        err = esp_timer_create(&tcfg, &s_wi.connect_watchdog);
    }
    if (err == ESP_OK) {
        err = esp_timer_start_periodic(s_wi.connect_watchdog, 1000000);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi 初始化失败: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err == ESP_OK) err = esp_wifi_set_mode(WIFI_MODE_NULL);
    if (err == ESP_OK) err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi 启动失败: %s", esp_err_to_name(err));
        return err;
    }
    s_wi.wifi_started = true;
    return ESP_OK;
}

static void load_ssid_from_store(void) {
    char pass[APP_STORE_WIFI_PASS_MAX + 1];
    app_store_wifi_load(s_wi.ssid, sizeof(s_wi.ssid), pass, sizeof(pass));
    memset(pass, 0, sizeof(pass));
}

// 用已存凭证以 STA 模式连接。调用方不持锁;凭证用后即清。
static esp_err_t connect_sta(void) {
    char pass[APP_STORE_WIFI_PASS_MAX + 1];
    char ssid[APP_STORE_WIFI_SSID_MAX + 1];
    esp_err_t err = app_store_wifi_load(ssid, sizeof(ssid), pass, sizeof(pass));
    if (err != ESP_OK) {
        memset(pass, 0, sizeof(pass));
        return err == ESP_ERR_NOT_FOUND ? ESP_ERR_INVALID_STATE : err;
    }
    wifi_config_t wcfg;
    memset(&wcfg, 0, sizeof(wcfg));
    strlcpy((char *)wcfg.sta.ssid, ssid, sizeof(wcfg.sta.ssid));
    strlcpy((char *)wcfg.sta.password, pass, sizeof(wcfg.sta.password));
    // 开放网络(未存密码)不能用 WPA 门槛,否则直接被拒连。
    wcfg.sta.threshold.authmode = pass[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
    memset(pass, 0, sizeof(pass));

    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err == ESP_OK) err = esp_wifi_set_config(WIFI_IF_STA, &wcfg);
    if (err == ESP_OK) {
        strlcpy(s_wi.ssid, ssid, sizeof(s_wi.ssid));
        set_state(APP_WIFI_CONNECTING);
        err = esp_wifi_connect();
        if (err != ESP_OK) set_state(APP_WIFI_ERROR);
    }
    return err;
}

esp_err_t app_wifi_autostart(void) {
    if (!s_wi.wifi_started) return ESP_ERR_INVALID_STATE;
    if (app_store_wifi_has()) {
        load_ssid_from_store();
        return connect_sta();
    }
    set_state(APP_WIFI_IDLE);
    return ESP_OK;
}

esp_err_t app_wifi_apply_credentials(const char *ssid, const char *password) {
    if (!ssid || !password) return ESP_ERR_INVALID_STATE;
    esp_err_t err = app_store_wifi_save(ssid, password);
    if (err != ESP_OK) return err;
    strlcpy(s_wi.ssid, ssid, sizeof(s_wi.ssid));
    s_wi.fast_retries = 0;
    wifi_config_t wcfg;
    memset(&wcfg, 0, sizeof(wcfg));
    strlcpy((char *)wcfg.sta.ssid, ssid, sizeof(wcfg.sta.ssid));
    strlcpy((char *)wcfg.sta.password, password, sizeof(wcfg.sta.password));
    wcfg.sta.threshold.authmode = password[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
    err = esp_wifi_set_config(WIFI_IF_STA, &wcfg);
    if (err == ESP_OK) {
        set_state(APP_WIFI_CONNECTING);
        err = esp_wifi_connect();
        if (err != ESP_OK) set_state(APP_WIFI_ERROR);
    }
    return err;
}

esp_err_t app_wifi_clear_credentials(void) {
    if (!s_wi.wifi_started) return ESP_ERR_INVALID_STATE;
    esp_err_t err = app_store_wifi_clear();
    if (err == ESP_OK) {
        s_wi.ssid[0] = '\0';
        s_wi.ip[0] = '\0';
        esp_wifi_disconnect();
        set_state(APP_WIFI_IDLE);
    }
    return err;
}


// ---------------------------------------------------------------------------
bool app_wifi_scan_begin(void) {
    if (!s_wi.wifi_started) return false;
    // 连接/重试进行中不扫描,避免互相干扰。
    switch (s_wi.state) {
    case APP_WIFI_IDLE:
    case APP_WIFI_CONNECTED:
    case APP_WIFI_AUTH_FAIL:
    case APP_WIFI_NO_AP:
    case APP_WIFI_ERROR:
        break;
    default:
        return false;
    }
    wifi_mode_t mode;
    if (esp_wifi_get_mode(&mode) != ESP_OK) return false;
    if (mode == WIFI_MODE_NULL && esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK) {
        return false;  // 无凭证开机停在 NULL 模式:先切 STA 才能扫描
    }
    s_scan_done = false;
    return esp_wifi_scan_start(NULL, false) == ESP_OK;
}

int app_wifi_scan_fetch(app_wifi_scan_item_t *out, size_t cap) {
    if (!out || cap == 0) return -1;
    if (!s_scan_done) return -1;
    s_scan_done = false;

    uint16_t count = 0;
    if (esp_wifi_scan_get_ap_num(&count) != ESP_OK) return 0;
    if (count > APP_WIFI_SCAN_MAX) count = APP_WIFI_SCAN_MAX;  // IDF 默认 RSSI 降序
    wifi_ap_record_t records[APP_WIFI_SCAN_MAX];
    if (esp_wifi_scan_get_ap_records(&count, records) != ESP_OK) return 0;

    size_t filled = 0;
    for (uint16_t i = 0; i < count && filled < cap; i++) {
        if (records[i].ssid[0] == '\0') continue;  // 跳过隐藏网络
        strlcpy(out[filled].ssid, (const char *)records[i].ssid,
                sizeof(out[filled].ssid));
        out[filled].rssi = records[i].rssi;
        out[filled].open = records[i].authmode == WIFI_AUTH_OPEN;
        filled++;
    }
    return (int)filled;
}
