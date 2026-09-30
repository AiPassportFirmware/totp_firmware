// tests/test_app_totp.c —— app_totp 纯逻辑宿主测试。
// 向量来源:RFC 4226 Appendix D(HOTP)、RFC 6238 Appendix B(TOTP/SHA-1)、
// RFC 4648 §10(Base32)。编译方式见 tools/validate.sh。
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "app_totp.h"

// RFC 4226 Appendix D:密钥 ASCII "12345678901234567890"
static const uint8_t RFC_KEY[] = "12345678901234567890";
static const uint32_t RFC_HOTP[] = {
    755224, 287082, 359152, 969429, 338314,
    254676, 287922, 162583, 399871, 520489,
};

static void test_hotp_rfc4226(void) {
    for (uint64_t counter = 0; counter < 10; counter++) {
        uint32_t code = totp_hotp(RFC_KEY, sizeof(RFC_KEY) - 1, counter, 6);
        if (code != RFC_HOTP[counter]) {
            printf("HOTP counter %llu: got %06u want %06u\n",
                   (unsigned long long)counter, code, RFC_HOTP[counter]);
            assert(0);
        }
    }
}

static void test_totp_rfc6238(void) {
    // RFC 6238 Appendix B(SHA-1,8 位):T=59 → 94287082;再取若干官方时间点。
    static const struct { int64_t t; uint32_t code; } vectors[] = {
        { 59, 94287082 },
        { 1111111109, 7081804 },
        { 1111111111, 14050471 },
        { 1234567890, 89005924 },
        { 2000000000, 69279037 },
        { 20000000000, 65353130 },
    };
    for (size_t i = 0; i < sizeof(vectors) / sizeof(vectors[0]); i++) {
        uint64_t step = (uint64_t)vectors[i].t / 30;
        uint32_t code = totp_hotp(RFC_KEY, sizeof(RFC_KEY) - 1, step, 8);
        if (code != vectors[i].code) {
            printf("TOTP t=%lld: got %08u want %08u\n",
                   (long long)vectors[i].t, code, vectors[i].code);
            assert(0);
        }
    }
}

static void test_totp_at_and_remaining(void) {
    totp_entry_t e;
    memset(&e, 0, sizeof(e));
    strcpy(e.label, "rfc");
    memcpy(e.secret, RFC_KEY, sizeof(RFC_KEY) - 1);
    e.secret_len = sizeof(RFC_KEY) - 1;
    e.digits = 6;
    e.period = 30;

    // T=59 落在窗口 [30,60),6 位码 = HOTP(1) = 287082
    assert(totp_at(&e, 59) == 287082);
    assert(totp_remaining(&e, 59) == 1);
    assert(totp_at(&e, 60) == RFC_HOTP[2]);   // 窗口 [60,90) → counter 2
    assert(totp_remaining(&e, 60) == 30);
    assert(totp_at(&e, -1) == 0);             // 负时间(未同步)不给码

    e.digits = 8;
    assert(totp_at(&e, 59) == 94287082 % 100000000);
}

static void test_format(void) {
    char buf[16];
    totp_format(123456, 6, buf);
    assert(strcmp(buf, "123456") == 0);
    totp_format(42, 6, buf);
    assert(strcmp(buf, "000042") == 0);
    totp_format(12345678, 8, buf);
    assert(strcmp(buf, "12345678") == 0);
}

static void test_base32_rfc4648(void) {
    static const struct { const char *plain; const char *encoded; } vectors[] = {
        { "f", "MY======" },
        { "fo", "MZXQ====" },
        { "foo", "MZXW6===" },
        { "foob", "MZXW6YQ=" },
        { "fooba", "MZXW6YTB" },
        // RFC 4648 §10 印刷时省略了尾部填充;按 §3.2 规范必须补齐到 8 的倍数,
        // 本实现输出规范填充形式,解码端两种形式都接受。
        { "foobar", "MZXW6YTBOI======" },
    };
    char enc[64];
    uint8_t dec[64];
    size_t dec_len;
    for (size_t i = 0; i < sizeof(vectors) / sizeof(vectors[0]); i++) {
        assert(totp_base32_encode((const uint8_t *)vectors[i].plain,
                                  strlen(vectors[i].plain), enc, sizeof(enc)));
        assert(strcmp(enc, vectors[i].encoded) == 0);
        assert(totp_base32_decode(vectors[i].encoded, dec, sizeof(dec), &dec_len));
        assert(dec_len == strlen(vectors[i].plain));
        assert(memcmp(dec, vectors[i].plain, dec_len) == 0);
    }

    // RFC 密钥往返 + 容错:小写、空格、连字符等价
    assert(totp_base32_decode("GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ", dec, sizeof(dec), &dec_len));
    assert(dec_len == 20 && memcmp(dec, RFC_KEY, 20) == 0);
    assert(totp_base32_decode("gezd gnbv gy3t qojq gezd gnbv gy3t qojq", dec, sizeof(dec), &dec_len));
    assert(dec_len == 20 && memcmp(dec, RFC_KEY, 20) == 0);

    // 非法输入
    assert(!totp_base32_decode("MZ1W6===", dec, sizeof(dec), &dec_len));  // 非 Base32 字符
    assert(!totp_base32_decode("MZXW6=YA", dec, sizeof(dec), &dec_len));  // 填充后有数据
    assert(!totp_base32_decode("", dec, sizeof(dec), &dec_len));          // 空密钥
    assert(!totp_base32_decode("A", dec, sizeof(dec), &dec_len));         // 悬空字符

    // 输出容量不足
    assert(!totp_base32_decode("GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ", dec, 8, &dec_len));
}

static void test_pack_unpack_roundtrip(void) {
    totp_entry_t e;
    memset(&e, 0, sizeof(e));
    strcpy(e.label, "GitHub:user");
    memcpy(e.secret, RFC_KEY, sizeof(RFC_KEY) - 1);
    e.secret_len = sizeof(RFC_KEY) - 1;
    e.digits = 6;
    e.period = 30;
    assert(totp_entry_valid(&e));

    uint8_t blob[TOTP_ENTRY_BLOB_MAX];
    size_t packed = totp_entry_pack(&e, blob, sizeof(blob));
    assert(packed > 0);

    totp_entry_t out;
    assert(totp_entry_unpack(blob, packed, &out));
    assert(strcmp(out.label, e.label) == 0);
    assert(out.secret_len == e.secret_len);
    assert(memcmp(out.secret, e.secret, e.secret_len) == 0);
    assert(out.digits == e.digits && out.period == e.period);

    // 篡改检测:截断、尾部长度不符、坏 magic、坏版本、非法 digits
    assert(!totp_entry_unpack(blob, packed - 1, &out));
    uint8_t bad[TOTP_ENTRY_BLOB_MAX];
    memcpy(bad, blob, packed);
    bad[0] = 'X';
    assert(!totp_entry_unpack(bad, packed, &out));
    memcpy(bad, blob, packed);
    bad[1] = 2;
    assert(!totp_entry_unpack(bad, packed, &out));
    memcpy(bad, blob, packed);
    bad[2] = 5;  // digits < 6
    assert(!totp_entry_unpack(bad, packed, &out));
}

static void test_label_policy(void) {
    assert(totp_label_ok("GitHub"));
    assert(totp_label_ok("a b-c_D.e"));
    assert(totp_label_ok("银行 / 工行卡"));  // UTF-8 中文(设备字体覆盖常用汉字)
    assert(totp_label_ok("中"));             // 1 个汉字 = 3 字节
    assert(!totp_label_ok(""));
    assert(!totp_label_ok("01234567890123456789012345"));  // 25 > 24 字节
    assert(totp_label_ok("八个汉字八个汉字"));  // 8 字 × 3 字节 = 24 字节,恰好上界
    assert(!totp_label_ok("八个汉字八个汉字们"));  // 27 字节,超限
    assert(!totp_label_ok("tab\there"));
    assert(!totp_label_ok("bad\xc3\x28"));  // 坏 UTF-8 续字节
    assert(!totp_label_ok("\xed\xa0\x80")); // 代理区
    assert(!totp_label_ok(NULL));
}

int main(void) {
    test_hotp_rfc4226();
    test_totp_rfc6238();
    test_totp_at_and_remaining();
    test_format();
    test_base32_rfc4648();
    test_pack_unpack_roundtrip();
    test_label_policy();
    printf("test_app_totp: PASS\n");
    return 0;
}
