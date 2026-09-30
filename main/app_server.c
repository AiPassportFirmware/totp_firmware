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
// 设计基线:暗色单主题,色板与设备端 UI 一致。样式抽成独立资源由 /app.css 提供,
// 不内联进模板 —— 共享变量只维护一份,同时每页渲染缓冲省下约 1.3KB。页面不引用
// 任何外链资源:设备只跑 httpd,页面必须在完全离线的局域网里自洽。
// ---------------------------------------------------------------------------
static const char CSS_BASE[] =
    ":root{--bg:#10151b;--surface:#1a222c;--surface-2:#232c38;--border:#2a3542;"
    "--primary:#35c9b0;--primary-soft:#1e3b39;--on-primary:#08221e;--fg:#e8edf2;"
    "--muted:#8ca0b3;--danger:#e8695c;--danger-soft:#33222a;--radius:14px;--rs:10px;"
    "--shadow:0 8px 24px #080e14}"
    "*,*:before,*:after{box-sizing:border-box}"
    "body{margin:0;min-height:100vh;background:var(--bg);color:var(--fg);font-size:15px;"
    "line-height:1.5;font-family:system-ui,-apple-system,'Segoe UI','PingFang SC',"
    "'Microsoft YaHei',sans-serif;-webkit-font-smoothing:antialiased}"
    "h1{font-size:1.05rem;font-weight:650;margin:0}"
    ".brand{display:flex;align-items:center;gap:11px}"
    ".mark{width:34px;height:34px;flex:none;display:grid;place-items:center;font-size:1.05rem;"
    "border-radius:10px;background:var(--primary-soft);color:var(--primary)}"
    ".sub{color:var(--muted);font-size:.82rem;margin:3px 0 0}"
    "button{font-family:inherit;cursor:pointer;border:none;transition:transform .12s}"
    "button:active{transform:scale(.97)}"
    "button:focus-visible{outline:2px solid var(--primary);outline-offset:2px}"
    "@media (prefers-reduced-motion:no-preference){"
    ".enter{animation:rise .38s cubic-bezier(.2,.8,.3,1) both}"
    "@keyframes rise{from{opacity:0;transform:translateY(14px)}to{opacity:1;transform:none}}"
    ".fade{animation:fadein .22s ease-out both}"
    "@keyframes fadein{from{opacity:0}to{opacity:1}}}"
    "button:active{transform:scale(.97)}}";

static const char LOGIN_TPL[] =
    "<!doctype html><html lang='@LANG@'><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<meta name='color-scheme' content='dark'><title>FoloTOTP</title>"
    "<link rel='stylesheet' href='/app.css'><style>"
    "body{display:grid;place-items:center;padding:24px}"
    ".card{width:100%;max-width:336px;background:var(--surface);border:1px solid var(--border);"
    "border-radius:18px;box-shadow:var(--shadow);padding:30px 24px 20px;text-align:center}"
    ".brand{justify-content:center}"
    ".code{position:relative;margin:24px 0 20px}"
    // The real input is absolutely positioned over the slots and kept
    // transparent, so a tap anywhere on the code opens the soft keyboard on a
    // phone. It lives outside .slots so box.children stays the 8 slots only.
    ".code input{position:absolute;inset:0;width:100%;height:100%;opacity:0;border:none;"
    "background:none;font-size:16px;letter-spacing:1em}"
    ".slots{display:flex;gap:7px;justify-content:center}"
    ".slot{width:31px;height:44px;display:grid;place-items:center;border-radius:var(--rs);"
    "background:var(--surface-2);border:1px solid var(--border);font-size:1.25rem;font-weight:600;"
    "font-variant-numeric:tabular-nums;transition:border-color .15s,box-shadow .15s}"
    ".slot.on{border-color:var(--primary);box-shadow:0 0 0 3px #1e3b39}"
    "button.primary{width:100%;padding:13px;border-radius:var(--rs);background:var(--primary);"
    "color:var(--on-primary);font-weight:650;font-size:.95rem}"
    "#m{min-height:20px;margin:14px 0 0;font-size:.83rem;color:var(--muted)}"
    "#m.err{color:var(--danger)}"
    "</style></head><body>"
    "<div class='card enter'><div class='brand'><span class='mark' aria-hidden='true'>"
    "&#128273;</span><h1>FoloTOTP</h1></div>"
    "<p class='sub' style='margin-top:14px'>@PROMPT@</p>"
    "<form onsubmit='return go()'>"
    "<div class='code'><div class='slots' id='slots'></div>"
    "<input id='c' type='text' inputmode='numeric' pattern='[0-9]*'"
    " maxlength='8' autocomplete='one-time-code' aria-label='@LBLCODE@'></div>"
    "<button class='primary' type='submit'>@SIGNIN@</button></form>"
    "<p id='m' role='status' aria-live='polite'></p></div>"
    "<script>var inp=document.getElementById('c'),box=document.getElementById('slots');"
    "for(var i=0;i<8;i++){var d=document.createElement('div');d.className='slot';"
    "box.appendChild(d);}"
    "function render(){var v=inp.value,k=box.children;"
    "for(var i=0;i<8;i++){k[i].textContent=v.charAt(i);"
    "k[i].className='slot'+(i===v.length?' on':'');}}"
    "inp.addEventListener('input',render);render();"
    "function go(){if(inp.value.length!==8){var m=document.getElementById('m');"
    "m.className='err';m.textContent='@WEB_NEED8@';return false;}"
    "fetch('/api/login',{method:'POST',headers:{'Content-Type':'application/json'},"
    "body:JSON.stringify({code:inp.value})}).then(function(r){return r.json();}).then(function(j){"
    "if(j.ok){location.reload();return;}var m=document.getElementById('m');m.className='err';"
    "m.textContent=j.error+(j.left?(' · '+j.left):'');"
    "inp.value='';render();}).catch(function(){var m=document.getElementById('m');"
    "m.className='err';m.textContent='@WEB_NETERR@';});return false;}"
    "</script></body></html>";

static const char ADMIN_TPL[] =
    "<!doctype html><html lang='@LANG@'><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<meta name='color-scheme' content='dark'><title>FoloTOTP</title>"
    "<link rel='stylesheet' href='/app.css'><style>"
    "body{padding:26px 20px 44px;max-width:452px;margin-inline:auto}"
    "header{display:flex;align-items:center;justify-content:space-between;gap:12px;margin-bottom:20px}"
    ".badge{flex:none;min-width:26px;height:26px;padding:0 9px;display:grid;place-items:center;"
    "border-radius:999px;background:var(--primary-soft);color:var(--primary);font-size:.8rem;"
    "font-weight:650;font-variant-numeric:tabular-nums}"
    ".card{background:var(--surface);border:1px solid var(--border);border-radius:var(--radius);"
    "box-shadow:var(--shadow);margin-bottom:16px}"
    ".card>h2{font-size:.74rem;font-weight:650;text-transform:uppercase;letter-spacing:.07em;"
    "color:var(--muted);margin:0;padding:13px 16px;border-bottom:1px solid var(--border)}"
    ".row{display:flex;align-items:center;gap:12px;padding:12px 12px 12px 16px;"
    "border-bottom:1px solid var(--border)}"
    ".row:last-child{border-bottom:none}"
    ".meta{flex:1;min-width:0}"
    ".name{font-weight:550;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}"
    ".spec{color:var(--muted);font-size:.78rem;margin-top:2px;font-variant-numeric:tabular-nums}"
    ".empty{padding:24px 16px;text-align:center;color:var(--muted);font-size:.86rem}"
    ".icon{flex:none;width:40px;height:40px;padding:0;display:grid;place-items:center;"
    "background:var(--danger-soft);color:var(--danger);font-size:.95rem}"
    ".ghost{width:100%;padding:12px;border-radius:var(--rs);background:transparent;color:var(--muted);"
    "border:1px solid var(--border);font-size:.9rem}"
    "label{display:block;font-size:.78rem;color:var(--muted);margin:0 0 6px}"
    "input,select{width:100%;padding:11px 12px;border-radius:var(--rs);border:1px solid var(--border);"
    "background:var(--surface-2);color:var(--fg);font-size:.92rem;font-family:inherit}"
    "input:focus,select:focus{outline:none;border-color:var(--primary);box-shadow:0 0 0 3px #1e3b39}"
    ".fields{padding:16px}"
    ".pair{display:flex;gap:10px;margin-top:14px}.pair>div{flex:1}"
    ".note{font-size:.75rem;color:var(--muted);margin:14px 0 0}"
    ".submit{display:block;width:calc(100% - 32px);margin:0 16px 16px;padding:12px;border-radius:var(--rs);"
    "background:var(--primary);color:var(--on-primary);font-weight:650;font-size:.92rem}"
    "#m{font-size:.85rem;padding:0 16px 12px;margin:0}"
    "#m:empty{display:none}#m.err{color:var(--danger)}#m.ok{color:var(--primary)}"
    "</style></head><body>"
    "<header class='enter'><div class='brand'><span class='mark' aria-hidden='true'>"
    "&#128273;</span><div><h1>FoloTOTP</h1><p class='sub'>@HEADING@</p></div></div>"
    "<span class='badge' id='cnt'>0</span></header>"
    "<div id='m' role='status' aria-live='polite'></div>"
    "<section class='card enter'><h2>@LISTTITLE@</h2><div id='tb'></div></section>"
    "<form class='card enter' onsubmit='return add()'><h2>@ADDTITLE@</h2>"
    "<div class='fields'><label for='lb'>@LBLNAME@</label>"
    "<input id='lb' maxlength='24' autocomplete='off' required>"
    "<div class='pair'><div><label for='dg'>@LBLDIGITS@</label>"
    "<select id='dg'><option value='6'>6</option><option value='8'>8</option></select></div>"
    "<div><label for='pd'>@LBLPERIOD@</label><select id='pd'><option value='30'>30</option>"
    "<option value='60'>60</option></select></div></div>"
    "<div style='margin-top:14px'><label for='sc'>@LBLSECRET@</label>"
    "<input id='sc' autocomplete='off' autocapitalize='off' autocorrect='off' spellcheck='false'"
    " required></div>"
    "<p class='note'>@NOTE@</p></div>"
    "<button class='submit' type='submit'>@ADDBTN@</button></form>"
    "<button class='ghost' onclick='logout()'>@LOGOUT@</button>"
    "<script>var cnt=document.getElementById('cnt');"
    "function msg(t,cls){var m=document.getElementById('m');m.textContent=t;m.className='fade '+cls;}"
    "function load(){fetch('/api/entries').then(function(r){"
    "if(r.status==403){location.href='/';throw 0;}return r.json();}).then(function(a){"
    "var tb=document.getElementById('tb');cnt.textContent=a.length;tb.innerHTML='';"
    "if(!a.length){var p=document.createElement('div');p.className='empty fade';"
    "p.textContent='@NOENT@';tb.appendChild(p);return;}"
    "a.forEach(function(e,i){var row=document.createElement('div');row.className='row fade';"
    "row.style.animationDelay=(i*45)+'ms';"
    "var meta=document.createElement('div');meta.className='meta';"
    "var nm=document.createElement('div');nm.className='name';nm.textContent=e.label;"
    "var sp=document.createElement('div');sp.className='spec';"
    "sp.textContent=e.digits+' · '+e.period+'s';meta.appendChild(nm);meta.appendChild(sp);"
    "var b=document.createElement('button');b.className='icon';b.type='button';"
    "b.setAttribute('data-id',e.id);b.setAttribute('aria-label','@DELBTN@');"
    "b.textContent='\\u2715';row.appendChild(meta);row.appendChild(b);tb.appendChild(row);});"
    "}).catch(function(){});}"
    "document.getElementById('tb').addEventListener('click',function(ev){"
    "var b=ev.target.closest('button.icon');if(b)del(b.getAttribute('data-id'));});"
    "function del(id){fetch('/api/delete',{method:'POST',"
    "headers:{'Content-Type':'application/json'},body:JSON.stringify({id:+id})})"
    ".then(function(r){return r.json();}).then(function(j){"
    "msg(j.ok?'@DELETED@':j.error,j.ok?'ok':'err');load();}).catch(function(){"
    "msg('@WEB_NETERR@','err');});}"
    "function add(){var lb=document.getElementById('lb').value.trim();"
    "var sc=document.getElementById('sc').value.trim().replace(/[\\s-]/g,'').toUpperCase();"
    "fetch('/api/entries',{method:'POST',headers:{'Content-Type':'application/json'},"
    "body:JSON.stringify({label:lb,secret:sc,digits:+document.getElementById('dg').value,"
    "period:+document.getElementById('pd').value})}).then(function(r){return r.json();})"
    ".then(function(j){if(j.ok){msg('@ADDED@','ok');document.getElementById('lb').value='';"
    "document.getElementById('sc').value='';load();}else{msg(j.error,'err');}})"
    ".catch(function(){msg('@WEB_NETERR@','err');});return false;}"
    "function logout(){fetch('/api/logout',{method:'POST'}).then(function(){location.href='/';});}"
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
    { "@LISTTITLE@", TR_WEB_LIST_TITLE },
    { "@LOGOUT@", TR_WEB_LOGOUT },
    { "@ADDTITLE@", TR_WEB_ADD_TITLE },
    { "@LBLNAME@", TR_WEB_LABEL_NAME },
    { "@LBLSECRET@", TR_WEB_LABEL_SECRET },
    { "@LBLDIGITS@", TR_WEB_LABEL_DIGITS },
    { "@LBLPERIOD@", TR_WEB_LABEL_PERIOD },
    { "@LBLCODE@", TR_WEB_LABEL_CODE },
    { "@NOTE@", TR_WEB_NOTE },
    { "@ADDBTN@", TR_WEB_ADD_BTN },
    { "@NOENT@", TR_WEB_NO_ENTRIES },
    { "@ADDED@", TR_WEB_ADDED },
    { "@DELETED@", TR_WEB_DELETED },
    { "@DELBTN@", TR_DELETE },
    { "@WEB_NEED8@", TR_WEB_NEED8 },
    { "@WEB_NETERR@", TR_WEB_NETERR },
};

// 渲染缓冲:按实测的最大页面(ADMIN_TPL,简体中文 6008 B)取 8 KB,留出余量。
// 静态分配避免每请求占堆;render_page() 超出时截断而非越界。
static char s_page_buf[8192];

// <html lang> 必须与页面文字一致,否则读屏软件会用错的发音规则念中文。
static char *render_page(const char *tpl) {
    size_t o = 0;
    size_t cap = sizeof(s_page_buf);
    const char *lang_tag = app_lang_current() == APP_LANG_ZH ? "zh-CN" : "en";
    for (const char *p = tpl; *p != '\0' && o + 1 < cap; ) {
        if (*p == '@') {
            if (strncmp(p, "@LANG@", 6) == 0) {
                size_t vl = strlen(lang_tag);
                if (o + vl >= cap) vl = cap - o - 1;
                memcpy(s_page_buf + o, lang_tag, vl);
                o += vl;
                p += 6;
                continue;
            }
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

// 样式独立成资源:两页共享一份变量,页面本身不必重复携带,浏览器也能缓存。
// 内容不含任何用户数据,因此可以长缓存;设备重启后内容不变。
static esp_err_t handler_css(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/css; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=86400");
    return httpd_resp_send(req, CSS_BASE, HTTPD_RESP_USE_STRLEN);
}

static const httpd_uri_t URIS[] = {
    { .uri = "/", .method = HTTP_GET, .handler = handler_index },
    { .uri = "/app.css", .method = HTTP_GET, .handler = handler_css },
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
