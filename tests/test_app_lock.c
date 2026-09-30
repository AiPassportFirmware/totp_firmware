// tests/test_app_lock.c —— 手势密码状态机宿主测试:
// 首次设密/确认、解锁、连续失败惩罚、惩罚到期、空闲锁定、退格。
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "app_lock.h"

// 输入一个完整的 8 步序列,返回最后一个事件。
static app_lock_event_t enter(app_lock_t *lk, uint8_t moves, uint32_t now_epoch_s) {
    app_lock_event_t ev = APP_LOCK_EV_NONE;
    for (int i = 0; i < APP_LOCK_MOVES; i++) {
        app_lock_move_t move = (moves >> i) & 1 ? APP_LOCK_MOVE_UP : APP_LOCK_MOVE_DOWN;
        ev = app_lock_push_move(lk, move, now_epoch_s);
        if (ev != APP_LOCK_EV_MORE) break;
    }
    return ev;
}

static const uint8_t SALT[APP_LOCK_SALT_LEN] = { 0x12, 0x34, 0x56, 0x78 };

static void test_hash_distinct(void) {
    // 全部 256 组合两两哈希互异(逐位混合的直接后果;O(n^2) 在宿主机瞬间完成)。
    static uint64_t hashes[256];
    for (uint16_t v = 0; v < 256; v++) {
        hashes[v] = app_lock_hash(SALT, (uint8_t)v);
    }
    for (uint16_t a = 0; a < 256; a++) {
        for (uint16_t b = (uint16_t)(a + 1); b < 256; b++) {
            assert(hashes[a] != hashes[b]);
        }
    }
    // salt 参与哈希
    const uint8_t zero_salt[APP_LOCK_SALT_LEN] = { 0, 0, 0, 0 };
    assert(app_lock_hash(SALT, 0) != app_lock_hash(zero_salt, 0));
}

static void test_setup_flow(void) {
    app_lock_t lk;
    app_lock_restore(&lk, false, SALT, 0, 0, 0);
    assert(lk.phase == APP_LOCK_PHASE_SETUP);

    // 第一次输入:下上下上...
    const uint8_t pwd = 0b10101010;
    assert(enter(&lk, pwd, 1000) == APP_LOCK_EV_SETUP_ENTER_CONFIRM);
    assert(lk.phase == APP_LOCK_PHASE_CONFIRM);

    // 不一致 → 回到 SETUP
    assert(enter(&lk, 0b10101011, 1000) == APP_LOCK_EV_SETUP_MISMATCH);
    assert(lk.phase == APP_LOCK_PHASE_SETUP);

    // 再来一次并正确确认
    assert(enter(&lk, pwd, 1000) == APP_LOCK_EV_SETUP_ENTER_CONFIRM);
    assert(enter(&lk, pwd, 1000) == APP_LOCK_EV_SETUP_DONE);
    assert(lk.phase == APP_LOCK_PHASE_OPEN);
    assert(lk.has_pwd);
    assert(lk.pwd_hash == app_lock_hash(SALT, pwd));

    // 上锁后用同一序列解锁
    app_lock_lock(&lk);
    assert(lk.phase == APP_LOCK_PHASE_UNLOCK);
    assert(enter(&lk, pwd, 2000) == APP_LOCK_EV_UNLOCKED);
    assert(lk.phase == APP_LOCK_PHASE_OPEN);
    assert(lk.fail_count == 0);
}

static void test_idle_lock(void) {
    app_lock_t lk;
    app_lock_restore(&lk, false, SALT, 0, 0, 0);
    const uint8_t pwd = 0b00001111;
    assert(enter(&lk, pwd, 0) == APP_LOCK_EV_SETUP_ENTER_CONFIRM);
    assert(enter(&lk, pwd, 0) == APP_LOCK_EV_SETUP_DONE);
    app_lock_on_activity(&lk, 1000);
    assert(!app_lock_idle_expired(&lk, 1000 + APP_LOCK_IDLE_TIMEOUT_S - 1));
    assert(app_lock_idle_expired(&lk, 1000 + APP_LOCK_IDLE_TIMEOUT_S));

    // SETUP 阶段空闲不锁定(本来也未解锁)
    app_lock_t lk2;
    app_lock_restore(&lk2, false, SALT, 0, 0, 0);
    assert(!app_lock_idle_expired(&lk2, 999999));
}

static void test_fail_lockout(void) {
    app_lock_t lk;
    app_lock_restore(&lk, true, SALT, app_lock_hash(SALT, 0), 0, 0);
    assert(lk.phase == APP_LOCK_PHASE_UNLOCK);

    const uint32_t t0 = 100000;
    // 连续错 9 次:每次 EV_WRONG
    for (int i = 0; i < APP_LOCK_FAIL_LIMIT - 1; i++) {
        assert(enter(&lk, 0xff, t0) == APP_LOCK_EV_WRONG);
        assert(lk.phase == APP_LOCK_PHASE_UNLOCK);
    }
    // 第 10 次:惩罚开始,fail_count 清零
    assert(enter(&lk, 0xff, t0) == APP_LOCK_EV_LOCKOUT_START);
    assert(lk.phase == APP_LOCK_PHASE_LOCKOUT);
    assert(lk.fail_count == 0);
    assert(lk.lock_until_s == t0 + APP_LOCK_LOCKOUT_S);
    assert(app_lock_lockout_remaining(&lk, t0 + 1) == APP_LOCK_LOCKOUT_S - 1);

    // 惩罚期输入被忽略
    assert(enter(&lk, 0, t0) == APP_LOCK_EV_NONE);
    app_lock_lock(&lk);  // 不应提前解除
    assert(lk.phase == APP_LOCK_PHASE_LOCKOUT);

    // 未到期 poll:仍处惩罚
    assert(app_lock_poll_lockout(&lk, t0 + APP_LOCK_LOCKOUT_S - 1));
    assert(lk.phase == APP_LOCK_PHASE_LOCKOUT);

    // 到期 poll:解除,回 UNLOCK,lock_until 清零
    assert(!app_lock_poll_lockout(&lk, t0 + APP_LOCK_LOCKOUT_S));
    assert(lk.phase == APP_LOCK_PHASE_UNLOCK);
    assert(lk.lock_until_s == 0);

    // 解除后第一次输入正确即解锁(计数已重置)
    assert(enter(&lk, 0, t0 + APP_LOCK_LOCKOUT_S) == APP_LOCK_EV_UNLOCKED);
}

static void test_success_resets_fail_count(void) {
    app_lock_t lk;
    app_lock_restore(&lk, true, SALT, app_lock_hash(SALT, 0), 0, 0);

    const uint32_t t = 5000;
    // 错 3 次 → 对 1 次(计数清零)→ 再错 3 次不应触发惩罚
    for (int i = 0; i < 3; i++) {
        assert(enter(&lk, 0xff, t) == APP_LOCK_EV_WRONG);
    }
    assert(enter(&lk, 0, t) == APP_LOCK_EV_UNLOCKED);
    app_lock_lock(&lk);  // 解锁后回到锁定态才能继续尝试
    for (int i = 0; i < 3; i++) {
        assert(enter(&lk, 0xff, t) == APP_LOCK_EV_WRONG);
    }
    assert(lk.phase == APP_LOCK_PHASE_UNLOCK);
    // 累计第 10 次错误才惩罚:此处再错 6 次(累计 9 次)仍应返回 WRONG
    for (int i = 0; i < APP_LOCK_FAIL_LIMIT - 3 - 1; i++) {
        assert(enter(&lk, 0xff, t) == APP_LOCK_EV_WRONG);
    }
    assert(enter(&lk, 0xff, t) == APP_LOCK_EV_LOCKOUT_START);
}

static void test_backspace(void) {
    // 密码 = 全 DOWN(0x00)。误按 UP 后退格重按 DOWN,验证 DOWN 显式清位。
    app_lock_t lk;
    app_lock_restore(&lk, true, SALT, app_lock_hash(SALT, 0), 0, 0);
    assert(lk.phase == APP_LOCK_PHASE_UNLOCK);

    app_lock_push_move(&lk, APP_LOCK_MOVE_UP, 100);
    assert(lk.progress == 1);
    app_lock_backspace(&lk);
    assert(lk.progress == 0);

    app_lock_event_t ev = APP_LOCK_EV_NONE;
    for (int i = 0; i < APP_LOCK_MOVES; i++) {
        ev = app_lock_push_move(&lk, APP_LOCK_MOVE_DOWN, 100);
    }
    assert(ev == APP_LOCK_EV_UNLOCKED);

    // progress=0 时退格无副作用
    app_lock_backspace(&lk);
    assert(lk.progress == 0 && lk.phase == APP_LOCK_PHASE_OPEN);
}

static void test_unlock_resets_idle_window(void) {
    // 回归:开机超过 5 分钟后解锁,不得因 last_activity 被清零而立即重新锁定。
    app_lock_t lk;
    app_lock_restore(&lk, true, SALT, app_lock_hash(SALT, 0), 0, 0);
    const uint32_t t_up = 600;  // 设备已运行 10 分钟
    app_lock_on_activity(&lk, t_up);
    app_lock_event_t ev = APP_LOCK_EV_NONE;
    for (int i = 0; i < APP_LOCK_MOVES; i++) {
        ev = app_lock_push_move(&lk, APP_LOCK_MOVE_DOWN, 1000);
    }
    assert(ev == APP_LOCK_EV_UNLOCKED);
    assert(!app_lock_idle_expired(&lk, t_up));  // 解锁瞬间:完整空闲窗口
    assert(!app_lock_idle_expired(&lk, t_up + APP_LOCK_IDLE_TIMEOUT_S - 1));
    assert(app_lock_idle_expired(&lk, t_up + APP_LOCK_IDLE_TIMEOUT_S));
}

static void test_restore_lockout(void) {
    // 模拟断电重启:惩罚未到期 → 直接进入 LOCKOUT
    app_lock_t lk;
    app_lock_restore(&lk, true, SALT, app_lock_hash(SALT, 0), 3, 200000);
    assert(lk.phase == APP_LOCK_PHASE_UNLOCK);
    assert(app_lock_poll_lockout(&lk, 100000));
    assert(lk.phase == APP_LOCK_PHASE_LOCKOUT);
    // 到期后解除
    assert(!app_lock_poll_lockout(&lk, 200000));
    assert(lk.phase == APP_LOCK_PHASE_UNLOCK);
}

int main(void) {
    test_hash_distinct();
    test_setup_flow();
    test_idle_lock();
    test_fail_lockout();
    test_success_resets_fail_count();
    test_backspace();
    test_unlock_resets_idle_window();
    test_restore_lockout();
    printf("test_app_lock: PASS\n");
    return 0;
}
