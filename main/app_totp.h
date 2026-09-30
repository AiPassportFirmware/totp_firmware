// main/app_totp.h —— TOTP 纯逻辑核心:SHA-1/HMAC/HOTP(RFC 4226)/TOTP(RFC 6238)、
// Base32(RFC 4648)解码与条目序列化。
//
// 本模块不包含任何 ESP-IDF/LVGL 依赖,可在宿主机上直接编译测试
// (tests/test_app_totp.c 用 RFC 官方测试向量校验)。所有函数无副作用、不分配内存。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// 条目容量上限。密钥原始长度最大 64 字节(覆盖 SHA1/SHA256 全部用例),
// 标签为 ASCII(设备端 Montserrat 字体无中文字形,见 docs/development/engineering/coding-conventions.md)。
#define TOTP_SECRET_MAX   64
#define TOTP_LABEL_MAX    24
#define TOTP_ENTRY_MAX    30
#define TOTP_PERIOD_MIN   5
#define TOTP_PERIOD_DEFAULT 30
#define TOTP_DIGITS_MIN   6
#define TOTP_DIGITS_MAX   8
#define TOTP_DIGITS_DEFAULT 6

// 序列化 blob 的最大字节数:magic(1)+version(1)+digits(1)+period(4)+label_len(1)
// +label(<=24)+secret_len(1)+secret(<=64) = 97。
#define TOTP_ENTRY_BLOB_MAX (3 + 4 + 1 + TOTP_LABEL_MAX + 1 + TOTP_SECRET_MAX)

typedef struct {
    char label[TOTP_LABEL_MAX + 1];   // NUL 结尾,可打印 ASCII
    uint8_t secret[TOTP_SECRET_MAX];
    size_t secret_len;                // 1..TOTP_SECRET_MAX
    uint8_t digits;                   // 6..8
    uint32_t period;                  // 秒,>= TOTP_PERIOD_MIN
} totp_entry_t;

// ---------------------------------------------------------------------------
// Base32(RFC 4648)。解码时忽略空白与 '-',大小写不敏感;'=' 只允许出现在末尾。
// 输出不足返回 false;*out_len 返回解码后字节数。
// ---------------------------------------------------------------------------
bool totp_base32_decode(const char *in, uint8_t *out, size_t out_cap, size_t *out_len);

// 输出编码串(带 '=' 填充)。buf 至少 ((len+4)/5)*8 + 1 字节。返回是否写成功。
bool totp_base32_encode(const uint8_t *in, size_t len, char *buf, size_t buf_cap);

// ---------------------------------------------------------------------------
// HMAC-SHA1 / HOTP / TOTP
// ---------------------------------------------------------------------------

// RFC 4226 HOTP:对 64 位大端计数器计算动态截断码。返回原始码值(未取模)。
uint32_t totp_hotp(const uint8_t *key, size_t key_len, uint64_t counter, uint8_t digits);

// RFC 6238 TOTP:以 unix 秒与条目周期计算步数后调用 HOTP。
uint32_t totp_at(const totp_entry_t *entry, int64_t unix_s);

// 当前窗口剩余秒数(1..period)。
uint32_t totp_remaining(const totp_entry_t *entry, int64_t unix_s);

// 码值转定宽十进制字符串(前导补零)。out 至少 9 字节。
void totp_format(uint32_t code, uint8_t digits, char *out);

// ---------------------------------------------------------------------------
// 条目校验与序列化(blob 布局:magic 'T', version=1, digits, period(u32 LE),
// label_len(u8), label, secret_len(u8), secret;严格等长,拒绝尾部多余数据)
// ---------------------------------------------------------------------------
bool totp_entry_valid(const totp_entry_t *entry);
size_t totp_entry_pack(const totp_entry_t *entry, uint8_t *buf, size_t buf_cap);
bool totp_entry_unpack(const uint8_t *buf, size_t len, totp_entry_t *out);

// 标签是否合法:非空、UTF-8 编码字节长度 <= TOTP_LABEL_MAX(≈8 个汉字)、
// 不含控制字符。允许中文(设备端 app_font_sc_16 覆盖常用汉字;超出覆盖集的
// 字符会显示为占位方框,见 docs/totp-app.md 的字符策略)。
bool totp_label_ok(const char *label);
