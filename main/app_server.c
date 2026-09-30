// main/app_server.c —— 远程管理 HTTP 服务实现,见 app_server.h。
#include "app_server.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "app_lang.h"
#include "app_store.h"
#include "app_totp.h"
#include "app_wifi.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_random.h"

static const char *TAG = "app_server";

#define LOGIN_FAIL_LIMIT 5
#define SESSION_TTL_S 300
#define COOKIE_NAME "totp_s="
#define CODE_LEN 8

typedef struct {
    httpd_handle_t server;
    char code[CODE_LEN + 1];
    char token[33];       // 16 字节随机数的 hex + NUL
    int64_t session_until; // epoch 秒;0 = 无会话
    int fails;
    bool exit_requested;
    char note[48];
} mgmt_t;

// httpd 单任务串行处理请求,以上字段只在 httpd 任务与启停调用处读写。
static mgmt_t s_m;

bool app_server_active(void) {
    return s_m.server != NULL;
}

const char *app_server_code(void) {
    return s_m.code;
}

int app_server_fails_left(void) {
    return LOGIN_FAIL_LIMIT - s_m.fails;
}

bool app_server_session_alive(void) {
    return s_m.token[0] != '\0' && (long long)time(NULL) < s_m.session_until;
}

const char *app_server_note(void) {
    return s_m.note;
}

bool app_server_exit_requested(void) {
    return s_m.exit_requested;
}

void app_server_url(char *buf, size_t cap) {
    snprintf(buf, cap, "http://%s/", app_wifi_ip());
}

static void set_note(const char *text) {
    strlcpy(s_m.note, text, sizeof(s_m.note));
}

// ---------------------------------------------------------------------------
// 会话
// ---------------------------------------------------------------------------
static bool random_token(char out[33]) {
    uint8_t raw[16];
    esp_fill_random(raw, sizeof(raw));
    for (int i = 0; i < 16; i++) {
        snprintf(out + 2 * i, 3, "%02x", raw[i]);
    }
    return true;
}

static bool session_valid(httpd_req_t *req) {
    if (s_m.token[0] == '\0') return false;
    char cookies[128];
    if (httpd_req_get_hdr_value_str(req, "Cookie", cookies, sizeof(cookies)) != ESP_OK) {
        return false;
    }
    const char *hit = strstr(cookies, COOKIE_NAME);
    if (!hit || strlen(hit + strlen(COOKIE_NAME)) < 32) return false;
    if (memcmp(hit + strlen(COOKIE_NAME), s_m.token, 32) != 0) return false;
    if ((long long)time(NULL) >= s_m.session_until) return false;
    s_m.session_until = (long long)time(NULL) + SESSION_TTL_S;  // 滑动过期
    return true;
}

static void session_grant(void) {
    (void)random_token(s_m.token);
    s_m.session_until = (long long)time(NULL) + SESSION_TTL_S;
    s_m.fails = 0;
}

static void session_revoke(void) {
    memset(s_m.token, 0, sizeof(s_m.token));
    s_m.session_until = 0;
}

// ---------------------------------------------------------------------------
// JSON 辅助(同 app_provision 的受限解析,见那里的理由)
// ---------------------------------------------------------------------------
static bool json_extract(const char *body, size_t body_len, const char *key,
                         char *out, size_t out_cap) {
    char pat[24];
    int pat_len = snprintf(pat, sizeof(pat), "\"%s\":\"", key);
    if (pat_len <= 0 || (size_t)pat_len >= sizeof(pat)) return false;
    const char *p = body;
    size_t left = body_len;
    while (left > 0) {
        const char *hit = memchr(p, pat[0], left);
        if (!hit || (size_t)(body + body_len - hit) < (size_t)pat_len) return false;
        if (memcmp(hit, pat, (size_t)pat_len) != 0) {
            left -= (size_t)(hit - p) + 1;
            p = hit + 1;
            continue;
        }
        const char *val = hit + pat_len;
        size_t o = 0;
        while (val < body + body_len && *val != '"') {
            char c = *val;
            if (c == '\\' && val + 1 < body + body_len) {
                val++;
                c = *val;
            }
            if (o + 1 >= out_cap) return false;
            out[o++] = c;
            val++;
        }
        if (val >= body + body_len) return false;
        out[o] = '\0';
        return true;
    }
    return false;
}

// 请求体不保证 NUL 结尾,用带长度的子串查找。
static const char *mem_find(const char *hay, size_t len, const char *needle) {
    size_t n = strlen(needle);
    if (n == 0 || len < n) return NULL;
    for (size_t i = 0; i + n <= len; i++) {
        if (memcmp(hay + i, needle, n) == 0) return hay + i;
    }
    return NULL;
}

static bool json_get_int(const char *body, size_t body_len, const char *key, long *out) {
    char pat[24];
    int pat_len = snprintf(pat, sizeof(pat), "\"%s\":", key);
    if (pat_len <= 0 || (size_t)pat_len >= sizeof(pat)) return false;
    const char *hit = mem_find(body, body_len, pat);
    if (!hit) return false;
    const char *p = hit + pat_len;
    const char *end = body + body_len;
    while (p < end && (*p == ' ' || *p == '\t')) p++;
    if (p >= end || *p < '0' || *p > '9') return false;
    long v = 0;
    while (p < end && *p >= '0' && *p <= '9') {
        v = v * 10 + (*p - '0');
        if (v > 1000000) return false;
        p++;
    }
    *out = v;
    return true;
}

// JSON 字符串转义(label 为可打印 ASCII,只需处理 " 与 \)。
static void json_escape(const char *in, char *out, size_t cap) {
    size_t o = 0;
    for (const char *p = in; *p && o + 3 < cap; p++) {
        if (*p == '"' || *p == '\\') out[o++] = '\\';
        out[o++] = *p;
    }
    out[o] = '\0';
}

static esp_err_t send_json(httpd_req_t *req, const char *json, int status) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    if (status != 200) httpd_resp_set_status(req, "403 Forbidden");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

// 错误响应统一走这里:JSON 里的 message 随设备语言(tr())。
static esp_err_t send_err(httpd_req_t *req, tr_id_t id, int status) {
    char buf[128];
    snprintf(buf, sizeof(buf), "{\"ok\":false,\"error\":\"%s\"}", tr(id));
    return send_json(req, buf, status);
}

static esp_err_t send_unauthorized(httpd_req_t *req) {
    return send_err(req, TR_ERR_UNAUTHORIZED, 403);
}

static ssize_t read_body(httpd_req_t *req, char *body, size_t cap) {
    size_t total = 0;
    int received;
    while (total < cap - 1 &&
           (received = httpd_req_recv(req, body + total, cap - 1 - total)) > 0) {
        total += (size_t)received;
    }
    body[total] = '\0';
    return (ssize_t)total;
}

// ---------------------------------------------------------------------------
// 处理器
// ---------------------------------------------------------------------------
static esp_err_t handler_login(httpd_req_t *req) {
    char body[128];
    if (read_body(req, body, sizeof(body)) <= 0) {
        return send_err(req, TR_ERR_BAD_REQUEST, 403);
    }
    char input[16];
    if (!json_extract(body, sizeof(body), "code", input, sizeof(input))) {
        return send_err(req, TR_ERR_BAD_REQUEST, 403);
    }
    if (strcmp(input, s_m.code) == 0) {
        session_grant();
        set_note(tr(TR_NOTE_SESSION));
        char hdr[96];  // 7 + 32 hex + 属性串,64 会截断
        snprintf(hdr, sizeof(hdr), "%s%s; HttpOnly; Path=/; Max-Age=%d; SameSite=Strict",
                 COOKIE_NAME, s_m.token, SESSION_TTL_S);
        httpd_resp_set_hdr(req, "Set-Cookie", hdr);
        ESP_LOGI(TAG, "远程管理登录成功");
        return send_json(req, "{\"ok\":true}", 200);
    }
    s_m.fails++;
    ESP_LOGW(TAG, "远程管理验证码错误(%d/%d)", s_m.fails, LOGIN_FAIL_LIMIT);
    if (s_m.fails >= LOGIN_FAIL_LIMIT) {
        // 锁死本轮:设备端轮询 exit_requested 收尾(不能在 handler 里停服务)。
        session_revoke();
        s_m.exit_requested = true;
        set_note(tr(TR_NOTE_LOCKED));
        return send_err(req, TR_ERR_LOCKED, 403);
    }
    char resp[64];
    snprintf(resp, sizeof(resp), "{\"ok\":false,\"error\":\"%s\",\"left\":%d}",
             tr(TR_ERR_WRONG_CODE), app_server_fails_left());
    return send_json(req, resp, 403);
}

static esp_err_t handler_entries(httpd_req_t *req) {
    if (!session_valid(req)) return send_unauthorized(req);
    char buf[2048];
    size_t used = (size_t)snprintf(buf, sizeof(buf), "[");
    size_t count = app_store_entry_count();
    for (size_t i = 0; i < count && used + 96 < sizeof(buf); i++) {
        totp_entry_t e;
        if (app_store_entry_get(i, &e) != ESP_OK) break;
        char label_esc[TOTP_LABEL_MAX * 2 + 2];
        json_escape(e.label, label_esc, sizeof(label_esc));
        used += (size_t)snprintf(buf + used, sizeof(buf) - used,
                                 "%s{\"id\":%u,\"label\":\"%s\",\"digits\":%u,\"period\":%u}",
                                 used > 1 ? "," : "", (unsigned)i,
                                 label_esc, e.digits, (unsigned)e.period);
    }
    snprintf(buf + used, sizeof(buf) - used, "]");
    return send_json(req, buf, 200);
}

static esp_err_t handler_add(httpd_req_t *req) {
    if (!session_valid(req)) return send_unauthorized(req);
    char body[768];
    ssize_t total = read_body(req, body, sizeof(body));
    if (total <= 0) {
        return send_err(req, TR_ERR_BAD_REQUEST, 403);
    }
    char label[TOTP_LABEL_MAX + 1];
    char secret_b32[160];
    if (!json_extract(body, (size_t)total, "label", label, sizeof(label)) ||
        !json_extract(body, (size_t)total, "secret", secret_b32, sizeof(secret_b32))) {
        return send_err(req, TR_ERR_MISSING, 403);
    }
    long digits = TOTP_DIGITS_DEFAULT;
    long period = TOTP_PERIOD_DEFAULT;
    json_get_int(body, (size_t)total, "digits", &digits);
    json_get_int(body, (size_t)total, "period", &period);

    totp_entry_t e;
    memset(&e, 0, sizeof(e));
    if (!totp_label_ok(label)) {
        return send_err(req, TR_ERR_LABEL, 403);
    }
    strlcpy(e.label, label, sizeof(e.label));
    if (!totp_base32_decode(secret_b32, e.secret, sizeof(e.secret), &e.secret_len)) {
        return send_err(req, TR_ERR_SECRET, 403);
    }
    if (digits < TOTP_DIGITS_MIN || digits > TOTP_DIGITS_MAX ||
        period < TOTP_PERIOD_MIN || period > 86400) {
        return send_err(req, TR_ERR_RANGE, 403);
    }
    e.digits = (uint8_t)digits;
    e.period = (uint32_t)period;

    esp_err_t err = app_store_entry_add(&e);
    // 密钥不进日志;成功后 UI 由脏标记刷新。
    if (err == ESP_ERR_NO_MEM) {
        return send_err(req, TR_ERR_FULL, 403);
    }
    if (err != ESP_OK) {
        return send_err(req, TR_ERR_SAVE, 403);
    }
    ESP_LOGI(TAG, "远程添加条目(密钥 %u 字节)", (unsigned)e.secret_len);
    return send_json(req, "{\"ok\":true}", 200);
}

static esp_err_t handler_delete(httpd_req_t *req) {
    if (!session_valid(req)) return send_unauthorized(req);
    char body[96];
    ssize_t total = read_body(req, body, sizeof(body));
    if (total <= 0) {
        return send_err(req, TR_ERR_BAD_REQUEST, 403);
    }
    long id;
    if (!json_get_int(body, (size_t)total, "id", &id) || id < 0 ||
        id >= (long)app_store_entry_count()) {
        return send_err(req, TR_ERR_BAD_ID, 403);
    }
    esp_err_t err = app_store_entry_delete((size_t)id);
    if (err != ESP_OK) {
        return send_err(req, TR_ERR_DELETE, 403);
    }
    ESP_LOGI(TAG, "远程删除条目 id=%ld", id);
    return send_json(req, "{\"ok\":true}", 200);
}

static esp_err_t handler_logout(httpd_req_t *req) {
    if (!session_valid(req)) return send_unauthorized(req);
    session_revoke();
    set_note(tr(TR_NOTE_LOGOUT));
    return send_json(req, "{\"ok\":true}", 200);
}

static esp_err_t handler_exit(httpd_req_t *req) {
    if (!session_valid(req)) return send_unauthorized(req);
    session_revoke();
    s_m.exit_requested = true;
    set_note(tr(TR_NOTE_EXIT));
    return send_json(req, "{\"ok\":true}", 200);
}

static esp_err_t handler_ping(httpd_req_t *req) {
    if (!session_valid(req)) return send_unauthorized(req);
    return send_json(req, "{\"ok\":true}", 200);
}

// ---------------------------------------------------------------------------
// 页面
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// 页面:令牌模板 + 按设备语言渲染(单一骨架,可见文案走 tr(),避免双语两份 HTML)
// ---------------------------------------------------------------------------
static const char LOGIN_TPL[] =
    "<!doctype html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>FoloTOTP</title><style>"
    "body{font-family:system-ui,sans-serif;background:#10151b;color:#e8edf2;"
    "display:flex;min-height:90vh;align-items:center;justify-content:center;margin:0}"
    "div{background:#1a222c;padding:32px;border-radius:16px;width:300px;text-align:center}"
    "input{width:100%;box-sizing:border-box;padding:14px;font-size:24px;letter-spacing:8px;"
    "text-align:center;border-radius:10px;border:1px solid #2a3542;background:#10151b;"
    "color:inherit}button{width:100%;padding:12px;margin-top:12px;border:none;"
    "border-radius:10px;background:#35c9b0;color:#08221e;font-weight:600;font-size:16px}"
    "p{font-size:13px;color:#8ca0b3}#m{font-size:13px;min-height:16px}.err{color:#e05b4e}"
    "</style></head><body><div><h2>&#128273; FoloTOTP</h2>"
    "<p>@PROMPT@</p>"
    "<input id='c' inputmode='numeric' maxlength='8' autocomplete='off'>"
    "<button onclick='go()'>@SIGNIN@</button><p id='m'></p></div>"
    "<script>function go(){fetch('/api/login',{method:'POST',"
    "headers:{'Content-Type':'application/json'},body:JSON.stringify({code:document.getElementById('c').value})})"
    ".then(r=>r.json()).then(j=>{if(j.ok){location.reload();}else{var m=document.getElementById('m');"
    "m.className='err';m.textContent=j.error+(j.left?('('+j.left+')'):'');}});}"
    "document.getElementById('c').addEventListener('keydown',e=>{if(e.key=='Enter')go();});"
    "</script></body></html>";

static const char ADMIN_TPL[] =
    "<!doctype html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>FoloTOTP</title><style>"
    "body{font-family:system-ui,sans-serif;background:#10151b;color:#e8edf2;"
    "margin:0;padding:20px;max-width:460px;margin-inline:auto}"
    "h2{font-size:18px}table{width:100%;border-collapse:collapse;margin:10px 0}"
    "td{padding:10px;border-bottom:1px solid #232c38;font-size:14px}tr:last-child td{border-bottom:none}"
    "input,select,button{box-sizing:border-box;padding:10px;border-radius:10px;"
    "border:1px solid #2a3542;background:#1a222c;color:inherit;font-size:14px}"
    "button{background:#35c9b0;color:#08221e;font-weight:600;border:none;cursor:pointer}"
    ".del{background:#3a2226;color:#e05b4e;padding:6px 12px}.ghost{background:#232c38;color:#e8edf2}"
    "form{background:#1a222c;padding:16px;border-radius:14px;margin-top:16px}"
    "label{font-size:12px;color:#8ca0b3;display:block;margin:8px 0 4px}"
    ".row{display:flex;gap:8px}.row>*{flex:1}#m{font-size:13px;min-height:16px;"
    "margin:8px 0}.err{color:#e05b4e}.ok{color:#35c9b0}"
    "</style></head><body><h2>&#128273; FoloTOTP @HEADING@</h2>"
    "<div id='m'></div><table id='tb'></table>"
    "<button class='ghost' onclick='logout()'>@LOGOUT@</button>"
    "<form onsubmit='return add()'><b style='font-size:14px'>@ADDTITLE@</b>"
    "<label>@LBLNAME@</label><input id='lb' maxlength='24'>"
    "<label>@LBLSECRET@</label><input id='sc' autocomplete='off'>"
    "<div class='row'><div><label>@LBLDIGITS@</label><select id='dg'>"
    "<option value='6'>6</option><option value='8'>8</option></select></div>"
    "<div><label>@LBLPERIOD@</label><select id='pd'>"
    "<option value='30'>30</option><option value='60'>60</option></select></div></div>"
    "<p style='font-size:12px;color:#8ca0b3;margin-top:10px'>@NOTE@</p>"
    "<button type='submit'>@ADDBTN@</button></form>"
    "<script>"
    "function msg(t,cls){var m=document.getElementById('m');m.textContent=t;m.className=cls||'';}"
    "function esc(s){return s.replace(/&/g,'&amp;').replace(/</g,'&lt;');}"
    "function load(){fetch('/api/entries').then(r=>{if(r.status==403){location.href='/';throw 0;}"
    "return r.json();}).then(a=>{var tb=document.getElementById('tb');tb.innerHTML='';"
    "if(!a.length){tb.innerHTML='<tr><td style=\"color:#8ca0b3\">@NOENT@</td></tr>';}"
    "a.forEach(function(e){var row=document.createElement('tr');"
    "row.innerHTML='<td>'+esc(e.label)+'</td><td style=\"color:#8ca0b3\">'+e.digits+'d/'+e.period+'s</td>"
    "<td style=\"text-align:right\"><button class=\"del\" data-id=\"'+e.id+'\">@DELBTN@</button></td>';"
    "tb.appendChild(row);});}).catch(function(){});}"
    "document.getElementById('tb').addEventListener('click',function(ev){"
    "var b=ev.target.closest('button.del');if(b)del(b.getAttribute('data-id'));});"
    "function del(id){fetch('/api/delete',{method:'POST',"
    "headers:{'Content-Type':'application/json'},body:JSON.stringify({id:+id})})"
    ".then(r=>r.json()).then(j=>{msg(j.ok?'@DELETED@':j.error,j.ok?'ok':'err');load();});}"
    "function add(){var lb=document.getElementById('lb').value.trim();"
    "var sc=document.getElementById('sc').value.trim().replace(/[ -]/g,'').toUpperCase();"
    "fetch('/api/entries',{method:'POST',headers:{'Content-Type':'application/json'},"
    "body:JSON.stringify({label:lb,secret:sc,digits:+document.getElementById('dg').value,"
    "period:+document.getElementById('pd').value})}).then(r=>r.json()).then(j=>{"
    "if(j.ok){msg('@ADDED@','ok');document.getElementById('lb').value='';"
    "document.getElementById('sc').value='';load();}else{msg(j.error,'err');}})"
    ".catch(function(){msg('network error','err');});return false;}"
    "function logout(){fetch('/api/logout',{method:'POST'}).then(()=>location.href='/');}"
    "load();setInterval(load,60000);</script></body></html>";

// 令牌 → 文案映射(渲染时查 tr());未知令牌原样保留,便于发现遗漏。
typedef struct {
    const char *tok;
    tr_id_t id;
} tok_map_t;

static const tok_map_t k_tokens[] = {
    { "@PROMPT@", TR_WEB_LOGIN_PROMPT },
    { "@SIGNIN@", TR_WEB_SIGN_IN },
    { "@HEADING@", TR_WEB_HEADING },
    { "@LOGOUT@", TR_WEB_LOGOUT },
    { "@ADDTITLE@", TR_WEB_ADD_TITLE },
    { "@LBLNAME@", TR_WEB_LABEL_NAME },
    { "@LBLSECRET@", TR_WEB_LABEL_SECRET },
    { "@LBLDIGITS@", TR_WEB_LABEL_DIGITS },
    { "@LBLPERIOD@", TR_WEB_LABEL_PERIOD },
    { "@NOTE@", TR_WEB_NOTE },
    { "@ADDBTN@", TR_WEB_ADD_BTN },
    { "@NOENT@", TR_WEB_NO_ENTRIES },
    { "@ADDED@", TR_WEB_ADDED },
    { "@DELETED@", TR_WEB_DELETED },
    { "@DELBTN@", TR_DELETE },
};

// 渲染缓冲:模板约 4.3KB,令牌替换后长度相近;静态分配避免每请求占堆。
static char s_page_buf[6144];

static char *render_page(const char *tpl) {
    size_t o = 0;
    size_t cap = sizeof(s_page_buf);
    for (const char *p = tpl; *p != '\0' && o + 1 < cap; ) {
        if (*p == '@') {
            const char *end = strchr(p + 1, '@');
            if (end != NULL) {
                size_t tlen = (size_t)(end - p + 1);
                const char *val = NULL;
                for (size_t i = 0; i < sizeof(k_tokens) / sizeof(k_tokens[0]); i++) {
                    if (strlen(k_tokens[i].tok) == tlen &&
                        memcmp(k_tokens[i].tok, p, tlen) == 0) {
                        val = tr(k_tokens[i].id);
                        break;
                    }
                }
                if (val != NULL) {
                    size_t vl = strlen(val);
                    if (o + vl >= cap) vl = cap - o - 1;
                    memcpy(s_page_buf + o, val, vl);
                    o += vl;
                    p = end + 1;
                    continue;
                }
            }
        }
        s_page_buf[o++] = *p++;
    }
    s_page_buf[o] = '\0';
    return s_page_buf;
}

static esp_err_t handler_index(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    // 页面语言跟随设备当前语言(菜单"切换 English/中文"即时生效)。
    const char *page = session_valid(req) ? render_page(ADMIN_TPL)
                                          : render_page(LOGIN_TPL);
    return httpd_resp_send(req, page, HTTPD_RESP_USE_STRLEN);
}

static const httpd_uri_t URIS[] = {
    { .uri = "/", .method = HTTP_GET, .handler = handler_index },
    { .uri = "/api/login", .method = HTTP_POST, .handler = handler_login },
    { .uri = "/api/entries", .method = HTTP_GET, .handler = handler_entries },
    { .uri = "/api/entries", .method = HTTP_POST, .handler = handler_add },
    { .uri = "/api/delete", .method = HTTP_POST, .handler = handler_delete },
    { .uri = "/api/logout", .method = HTTP_POST, .handler = handler_logout },
    { .uri = "/api/exit", .method = HTTP_POST, .handler = handler_exit },
    { .uri = "/api/ping", .method = HTTP_GET, .handler = handler_ping },
};

esp_err_t app_server_start(void) {
    if (s_m.server) return ESP_OK;
    if (app_wifi_state() != APP_WIFI_CONNECTED) return ESP_ERR_INVALID_STATE;

    // 随机 8 位数字码:拒绝采样避免模偏差,前导零保留。
    uint32_t value;
    do {
        esp_fill_random(&value, sizeof(value));
        value &= 0x7fffffff;
    } while (value >= 100000000u);
    snprintf(s_m.code, sizeof(s_m.code), "%08u", (unsigned)value);

    session_revoke();
    s_m.fails = 0;
    s_m.exit_requested = false;
    set_note(tr(TR_NOTE_WAITING));

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.lru_purge_enable = true;
    esp_err_t err = httpd_start(&s_m.server, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "管理服务启动失败: %s", esp_err_to_name(err));
        return err;
    }
    for (size_t i = 0; i < sizeof(URIS) / sizeof(URIS[0]); i++) {
        err = httpd_register_uri_handler(s_m.server, &URIS[i]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "注册 %s 失败: %s", URIS[i].uri, esp_err_to_name(err));
            httpd_stop(s_m.server);
            s_m.server = NULL;
            return err;
        }
    }
    ESP_LOGI(TAG, "远程管理服务已启动");
    return ESP_OK;
}

void app_server_stop(void) {
    if (!s_m.server) return;
    httpd_stop(s_m.server);
    s_m.server = NULL;
    session_revoke();
    memset(s_m.code, 0, sizeof(s_m.code));
    s_m.fails = 0;
    s_m.exit_requested = false;
    s_m.note[0] = '\0';
    ESP_LOGI(TAG, "远程管理服务已停止");
}
