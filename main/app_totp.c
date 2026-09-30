// main/app_totp.c —— TOTP 纯逻辑实现。算法与编码必须与 RFC 一致,
// 由 tests/test_app_totp.c 用 RFC 4226/4648/6238 官方向量回归。
#include "app_totp.h"

#include <string.h>

// ---------------------------------------------------------------------------
// SHA-1 (RFC 3174)。只服务于本模块的 HMAC-SHA1,不对外暴露。
// ---------------------------------------------------------------------------
typedef struct {
    uint32_t h[5];
    uint64_t total_bits;
    uint8_t buf[64];
    size_t buf_len;
} sha1_ctx_t;

static uint32_t rol32(uint32_t v, unsigned n) {
    return (v << n) | (v >> (32u - n));
}

static void sha1_init(sha1_ctx_t *c) {
    c->h[0] = 0x67452301;
    c->h[1] = 0xEFCDAB89;
    c->h[2] = 0x98BADCFE;
    c->h[3] = 0x10325476;
    c->h[4] = 0xC3D2E1F0;
    c->total_bits = 0;
    c->buf_len = 0;
}

static void sha1_block(sha1_ctx_t *c, const uint8_t *p) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)p[4 * i] << 24) | ((uint32_t)p[4 * i + 1] << 16) |
               ((uint32_t)p[4 * i + 2] << 8) | (uint32_t)p[4 * i + 3];
    }
    for (int i = 16; i < 80; i++) {
        w[i] = rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }
    uint32_t a = c->h[0];
    uint32_t b = c->h[1];
    uint32_t cc = c->h[2];
    uint32_t d = c->h[3];
    uint32_t e = c->h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f;
        uint32_t k;
        if (i < 20) {
            f = (b & cc) | (~b & d);
            k = 0x5A827999;
        } else if (i < 40) {
            f = b ^ cc ^ d;
            k = 0x6ED9EBA1;
        } else if (i < 60) {
            f = (b & cc) | (b & d) | (cc & d);
            k = 0x8F1BBCDC;
        } else {
            f = b ^ cc ^ d;
            k = 0xCA62C1D6;
        }
        uint32_t tmp = rol32(a, 5) + f + e + k + w[i];
        e = d;
        d = cc;
        cc = rol32(b, 30);
        b = a;
        a = tmp;
    }
    c->h[0] += a;
    c->h[1] += b;
    c->h[2] += cc;
    c->h[3] += d;
    c->h[4] += e;
}

static void sha1_update(sha1_ctx_t *c, const uint8_t *data, size_t len) {
    c->total_bits += (uint64_t)len * 8;
    while (len > 0) {
        size_t take = 64 - c->buf_len;
        if (take > len) take = len;
        memcpy(c->buf + c->buf_len, data, take);
        c->buf_len += take;
        data += take;
        len -= take;
        if (c->buf_len == 64) {
            sha1_block(c, c->buf);
            c->buf_len = 0;
        }
    }
}

static void sha1_final(sha1_ctx_t *c, uint8_t out[20]) {
    uint64_t bits = c->total_bits;
    uint8_t pad = 0x80;
    sha1_update(c, &pad, 1);
    uint8_t zero = 0;
    while (c->buf_len != 56) {
        sha1_update(c, &zero, 1);
    }
    uint8_t len_be[8];
    for (int i = 0; i < 8; i++) {
        len_be[i] = (uint8_t)(bits >> (56 - 8 * i));
    }
    // 直接写尾部避免再次累计 total_bits(结果不再使用,但保持语义干净)。
    memcpy(c->buf + 56, len_be, 8);
    sha1_block(c, c->buf);
    c->buf_len = 0;
    for (int i = 0; i < 5; i++) {
        out[4 * i] = (uint8_t)(c->h[i] >> 24);
        out[4 * i + 1] = (uint8_t)(c->h[i] >> 16);
        out[4 * i + 2] = (uint8_t)(c->h[i] >> 8);
        out[4 * i + 3] = (uint8_t)c->h[i];
    }
}

static void sha1(const uint8_t *data, size_t len, uint8_t out[20]) {
    sha1_ctx_t c;
    sha1_init(&c);
    sha1_update(&c, data, len);
    sha1_final(&c, out);
}

// HMAC-SHA1(RFC 2104),消息固定为 8 字节 HOTP 计数器,接口按此收窄。
static void hmac_sha1_counter(const uint8_t *key, size_t key_len, uint64_t counter,
                              uint8_t mac[20]) {
    uint8_t k[64];
    memset(k, 0, sizeof(k));
    if (key_len > 64) {
        sha1(key, key_len, k);  // 长密钥先散列
    } else {
        memcpy(k, key, key_len);
    }

    uint8_t counter_be[8];
    for (int i = 0; i < 8; i++) {
        counter_be[i] = (uint8_t)(counter >> (56 - 8 * i));
    }

    uint8_t ipad[64];
    uint8_t opad[64];
    for (int i = 0; i < 64; i++) {
        ipad[i] = k[i] ^ 0x36;
        opad[i] = k[i] ^ 0x5c;
    }
    uint8_t inner[20];
    sha1_ctx_t c;
    sha1_init(&c);
    sha1_update(&c, ipad, sizeof(ipad));
    sha1_update(&c, counter_be, sizeof(counter_be));
    sha1_final(&c, inner);
    sha1_init(&c);
    sha1_update(&c, opad, sizeof(opad));
    sha1_update(&c, inner, sizeof(inner));
    sha1_final(&c, mac);
}

// ---------------------------------------------------------------------------
// HOTP / TOTP
// ---------------------------------------------------------------------------
uint32_t totp_hotp(const uint8_t *key, size_t key_len, uint64_t counter, uint8_t digits) {
    if (key == NULL || key_len == 0 || digits < TOTP_DIGITS_MIN || digits > TOTP_DIGITS_MAX) {
        return 0;
    }
    uint8_t mac[20];
    hmac_sha1_counter(key, key_len, counter, mac);
    int offset = mac[19] & 0x0f;
    uint32_t bin = ((uint32_t)(mac[offset] & 0x7f) << 24) |
                   ((uint32_t)mac[offset + 1] << 16) |
                   ((uint32_t)mac[offset + 2] << 8) |
                   (uint32_t)mac[offset + 3];
    uint32_t mod = 1;
    for (uint8_t i = 0; i < digits; i++) {
        mod *= 10;
    }
    return bin % mod;
}

uint32_t totp_at(const totp_entry_t *entry, int64_t unix_s) {
    if (entry == NULL || !totp_entry_valid(entry) || unix_s < 0) {
        return 0;
    }
    uint64_t step = (uint64_t)unix_s / entry->period;
    return totp_hotp(entry->secret, entry->secret_len, step, entry->digits);
}

uint32_t totp_remaining(const totp_entry_t *entry, int64_t unix_s) {
    if (entry == NULL || !totp_entry_valid(entry) || unix_s < 0) {
        return 0;
    }
    uint32_t into = (uint32_t)((uint64_t)unix_s % entry->period);
    return entry->period - into;
}

void totp_format(uint32_t code, uint8_t digits, char *out) {
    if (out == NULL) return;
    if (digits < TOTP_DIGITS_MIN || digits > TOTP_DIGITS_MAX) digits = TOTP_DIGITS_DEFAULT;
    // 从最低位反填,再左移补零。
    out[digits] = '\0';
    for (uint8_t i = digits; i > 0; i--) {
        out[i - 1] = (char)('0' + code % 10);
        code /= 10;
    }
}

// ---------------------------------------------------------------------------
// Base32 (RFC 4648)
// ---------------------------------------------------------------------------
static int base32_value(unsigned char ch) {
    if (ch >= 'A' && ch <= 'Z') return ch - 'A';
    if (ch >= 'a' && ch <= 'z') return ch - 'a';
    if (ch >= '2' && ch <= '7') return ch - '2' + 26;
    return -1;
}

bool totp_base32_decode(const char *in, uint8_t *out, size_t out_cap, size_t *out_len) {
    if (in == NULL || out == NULL) return false;
    uint32_t acc = 0;
    int bits = 0;
    size_t olen = 0;
    bool seen_pad = false;
    for (const char *p = in; *p != '\0'; p++) {
        unsigned char ch = (unsigned char)*p;
        // 容忍空格/连字符/换行:网页表单与人工抄写常见分段习惯。
        if (ch == ' ' || ch == '-' || ch == '\t' || ch == '\r' || ch == '\n') continue;
        if (ch == '=') {
            seen_pad = true;
            continue;
        }
        // 填充符之后不允许再出现数据字符。
        if (seen_pad) return false;
        int v = base32_value(ch);
        if (v < 0) return false;
        acc = (acc << 5) | (uint32_t)v;
        bits += 5;
        if (bits >= 8) {
            bits -= 8;
            if (olen >= out_cap) return false;
            out[olen++] = (uint8_t)((acc >> bits) & 0xff);
        }
    }
    if (bits >= 5) return false;  // 悬空字符(剩余有效位不足 8)
    if (olen == 0) return false;  // 空密钥无意义
    if (out_len != NULL) *out_len = olen;
    return true;
}

bool totp_base32_encode(const uint8_t *in, size_t len, char *buf, size_t buf_cap) {
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
    if (in == NULL || buf == NULL) return false;
    size_t need = ((len + 4) / 5) * 8 + 1;
    if (buf_cap < need) return false;
    uint32_t acc = 0;
    int bits = 0;
    size_t o = 0;
    for (size_t i = 0; i < len; i++) {
        acc = (acc << 8) | in[i];
        bits += 8;
        while (bits >= 5) {
            bits -= 5;
            buf[o++] = alphabet[(acc >> bits) & 31];
        }
    }
    if (bits > 0) {
        buf[o++] = alphabet[(acc << (5 - bits)) & 31];
    }
    while (o % 8 != 0) {
        buf[o++] = '=';
    }
    buf[o] = '\0';
    return true;
}

// ---------------------------------------------------------------------------
// 条目校验与序列化
// ---------------------------------------------------------------------------
bool totp_label_ok(const char *label) {
    if (label == NULL) return false;
    size_t len = strlen(label);
    if (len == 0 || len > TOTP_LABEL_MAX) return false;
    // UTF-8 严格校验(RFC 3629):拒绝控制字符、坏前缀与坏续字节。
    // 设备端字体覆盖常用汉字;覆盖集之外的合法 UTF-8 也接受,显示为占位方框。
    size_t i = 0;
    while (i < len) {
        unsigned char c = (unsigned char)label[i];
        if (c < 0x20 || c == 0x7F) return false;
        if (c < 0x80) {
            i++;
            continue;
        }
        size_t cont;
        unsigned char lo = 0x80;
        unsigned char hi = 0xBF;
        if (c >= 0xC2 && c <= 0xDF) {
            cont = 1;
        } else if (c == 0xE0) {
            cont = 2;
            lo = 0xA0;
        } else if ((c >= 0xE1 && c <= 0xEC) || (c >= 0xEE && c <= 0xEF)) {
            cont = 2;
        } else if (c == 0xED) {
            cont = 2;
            hi = 0x9F;  // 排除代理区
        } else if (c == 0xF0) {
            cont = 3;
            lo = 0x90;
        } else if (c >= 0xF1 && c <= 0xF3) {
            cont = 3;
        } else if (c == 0xF4) {
            cont = 3;
            hi = 0x8F;  // 不超过 U+10FFFF
        } else {
            return false;  // 0x80-0xC1、0xF5-0xFF 均非合法首字节
        }
        i++;
        for (size_t k = 0; k < cont; k++) {
            if (i >= len) return false;
            unsigned char cc = (unsigned char)label[i];
            if (cc < lo || cc > hi) return false;
            lo = 0x80;
            hi = 0xBF;
            i++;
        }
    }
    return true;
}

bool totp_entry_valid(const totp_entry_t *entry) {
    if (entry == NULL) return false;
    if (!totp_label_ok(entry->label)) return false;
    if (entry->secret_len == 0 || entry->secret_len > TOTP_SECRET_MAX) return false;
    if (entry->digits < TOTP_DIGITS_MIN || entry->digits > TOTP_DIGITS_MAX) return false;
    if (entry->period < TOTP_PERIOD_MIN || entry->period > 86400) return false;
    return true;
}

size_t totp_entry_pack(const totp_entry_t *entry, uint8_t *buf, size_t buf_cap) {
    if (!totp_entry_valid(entry) || buf == NULL) return 0;
    size_t label_len = strlen(entry->label);
    size_t need = 3 + 4 + 1 + label_len + 1 + entry->secret_len;
    if (buf_cap < need) return 0;

    size_t o = 0;
    buf[o++] = 'T';
    buf[o++] = 1;  // 版本
    buf[o++] = entry->digits;
    buf[o++] = (uint8_t)(entry->period & 0xff);
    buf[o++] = (uint8_t)((entry->period >> 8) & 0xff);
    buf[o++] = (uint8_t)((entry->period >> 16) & 0xff);
    buf[o++] = (uint8_t)((entry->period >> 24) & 0xff);
    buf[o++] = (uint8_t)label_len;
    memcpy(buf + o, entry->label, label_len);
    o += label_len;
    buf[o++] = (uint8_t)entry->secret_len;
    memcpy(buf + o, entry->secret, entry->secret_len);
    o += entry->secret_len;
    return o;
}

bool totp_entry_unpack(const uint8_t *buf, size_t len, totp_entry_t *out) {
    if (buf == NULL || out == NULL || len < 3 + 4 + 1 + 1 + 1) return false;
    if (buf[0] != 'T' || buf[1] != 1) return false;

    totp_entry_t e;
    memset(&e, 0, sizeof(e));
    e.digits = buf[2];
    e.period = (uint32_t)buf[3] | ((uint32_t)buf[4] << 8) |
               ((uint32_t)buf[5] << 16) | ((uint32_t)buf[6] << 24);
    size_t o = 7;
    size_t label_len = buf[o++];
    if (label_len == 0 || label_len > TOTP_LABEL_MAX) return false;
    if (o + label_len + 1 > len) return false;
    memcpy(e.label, buf + o, label_len);
    e.label[label_len] = '\0';
    o += label_len;
    size_t secret_len = buf[o++];
    if (secret_len == 0 || secret_len > TOTP_SECRET_MAX) return false;
    if (o + secret_len != len) return false;  // 严格等长,拒绝尾部数据
    memcpy(e.secret, buf + o, secret_len);
    e.secret_len = secret_len;

    if (!totp_entry_valid(&e)) return false;
    *out = e;
    return true;
}
