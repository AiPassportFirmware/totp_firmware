// main/app_lock.c —— 手势密码状态机实现,见 app_lock.h 的约定。
#include "app_lock.h"

#include <string.h>

#define FNV64_OFFSET_BASIS 0xcbf29ce484222325ULL
#define FNV64_PRIME        0x100000001b3ULL

uint64_t app_lock_hash(const uint8_t salt[APP_LOCK_SALT_LEN], uint8_t moves) {
    uint64_t h = FNV64_OFFSET_BASIS;
    for (int i = 0; i < APP_LOCK_SALT_LEN; i++) {
        h ^= salt[i];
        h *= FNV64_PRIME;
    }
    // 逐位混合而不是整字节,保证"相邻序列只差一步"时哈希仍然完全不同。
    for (int i = 0; i < APP_LOCK_MOVES; i++) {
        h ^= (uint64_t)((moves >> i) & 1u);
        h *= FNV64_PRIME;
    }
    // 雪崩终结(类似 murmur3 fmix64),避免低位哈希与输入呈可预测关系。
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33;
    h *= 0xc4ceb9fe1a85ec53ULL;
    h ^= h >> 33;
    return h;
}

void app_lock_restore(app_lock_t *lk, bool has_pwd, const uint8_t salt[APP_LOCK_SALT_LEN],
                      uint64_t hash, uint8_t fail_count, uint32_t lock_until_s) {
    if (lk == NULL || salt == NULL) return;
    memset(lk, 0, sizeof(*lk));
    memcpy(lk->salt, salt, APP_LOCK_SALT_LEN);
    lk->has_pwd = has_pwd;
    lk->fail_count = fail_count;
    lk->lock_until_s = lock_until_s;
    if (has_pwd) {
        lk->pwd_hash = hash;
        lk->phase = APP_LOCK_PHASE_UNLOCK;
    } else {
        lk->phase = APP_LOCK_PHASE_SETUP;
    }
}

static void draft_reset(app_lock_t *lk) {
    lk->draft = 0;
    lk->progress = 0;
}

static app_lock_event_t finish_entry(app_lock_t *lk, uint32_t now_epoch_s) {
    switch (lk->phase) {
    case APP_LOCK_PHASE_SETUP:
        // 首次输入完成:保存草稿,进入确认阶段。
        lk->confirm_draft = lk->draft;
        draft_reset(lk);
        lk->phase = APP_LOCK_PHASE_CONFIRM;
        return APP_LOCK_EV_SETUP_ENTER_CONFIRM;

    case APP_LOCK_PHASE_CONFIRM:
        if (lk->draft == lk->confirm_draft) {
            lk->pwd_hash = app_lock_hash(lk->salt, lk->draft);
            lk->has_pwd = true;
            lk->fail_count = 0;
            draft_reset(lk);
            lk->phase = APP_LOCK_PHASE_OPEN;
            return APP_LOCK_EV_SETUP_DONE;
        }
        draft_reset(lk);
        lk->phase = APP_LOCK_PHASE_SETUP;
        return APP_LOCK_EV_SETUP_MISMATCH;

    case APP_LOCK_PHASE_UNLOCK:
        if (app_lock_hash(lk->salt, lk->draft) == lk->pwd_hash) {
            lk->fail_count = 0;
            draft_reset(lk);
            lk->phase = APP_LOCK_PHASE_OPEN;
            return APP_LOCK_EV_UNLOCKED;
        }
        lk->fail_count++;
        draft_reset(lk);
        if (lk->fail_count >= APP_LOCK_FAIL_LIMIT) {
            lk->fail_count = 0;
            lk->lock_until_s = now_epoch_s + APP_LOCK_LOCKOUT_S;
            lk->phase = APP_LOCK_PHASE_LOCKOUT;
            return APP_LOCK_EV_LOCKOUT_START;
        }
        return APP_LOCK_EV_WRONG;

    default:
        return APP_LOCK_EV_NONE;
    }
}

// 解锁/设密成功时不清 last_activity_uptime_s:调用方在本事件开始时已刷新,
// 保持该值即可保证解锁后拥有完整的 5 分钟空闲窗口(见 app_ui_key 的双端刷新)。
app_lock_event_t app_lock_push_move(app_lock_t *lk, app_lock_move_t move, uint32_t now_epoch_s) {
    if (lk == NULL) return APP_LOCK_EV_NONE;
    switch (lk->phase) {
    case APP_LOCK_PHASE_SETUP:
    case APP_LOCK_PHASE_CONFIRM:
    case APP_LOCK_PHASE_UNLOCK:
        break;
    default:
        // OPEN(无需密码)与 LOCKOUT(惩罚中)不接受输入。
        return APP_LOCK_EV_NONE;
    }
    if (lk->progress >= APP_LOCK_MOVES) return APP_LOCK_EV_NONE;

    // DOWN 显式清位,保证退格后重输方向正确。
    if (move == APP_LOCK_MOVE_UP) {
        lk->draft |= (uint8_t)(1u << lk->progress);
    } else {
        lk->draft &= (uint8_t)~(1u << lk->progress);
    }
    lk->progress++;
    if (lk->progress < APP_LOCK_MOVES) return APP_LOCK_EV_MORE;
    return finish_entry(lk, now_epoch_s);
}

void app_lock_backspace(app_lock_t *lk) {
    if (lk == NULL) return;
    if (lk->phase != APP_LOCK_PHASE_SETUP && lk->phase != APP_LOCK_PHASE_CONFIRM &&
        lk->phase != APP_LOCK_PHASE_UNLOCK) {
        return;
    }
    if (lk->progress == 0) return;
    lk->progress--;
    lk->draft &= (uint8_t)~(1u << lk->progress);
}

void app_lock_lock(app_lock_t *lk) {
    if (lk == NULL || lk->phase != APP_LOCK_PHASE_OPEN) return;
    draft_reset(lk);
    lk->phase = APP_LOCK_PHASE_UNLOCK;
}

bool app_lock_idle_expired(const app_lock_t *lk, uint32_t now_uptime_s) {
    if (lk == NULL || lk->phase != APP_LOCK_PHASE_OPEN) return false;
    return (uint32_t)(now_uptime_s - lk->last_activity_uptime_s) >= APP_LOCK_IDLE_TIMEOUT_S;
}

bool app_lock_poll_lockout(app_lock_t *lk, uint32_t now_epoch_s) {
    if (lk == NULL || lk->lock_until_s == 0) return false;
    if (lk->phase != APP_LOCK_PHASE_UNLOCK && lk->phase != APP_LOCK_PHASE_LOCKOUT) {
        return false;
    }
    if (now_epoch_s < lk->lock_until_s) {
        lk->phase = APP_LOCK_PHASE_LOCKOUT;
        return true;
    }
    // 惩罚到期:清零并停留在 UNLOCK,调用方持久化 lock_until_s=0。
    lk->lock_until_s = 0;
    lk->phase = APP_LOCK_PHASE_UNLOCK;
    return false;
}

uint32_t app_lock_lockout_remaining(const app_lock_t *lk, uint32_t now_epoch_s) {
    if (lk == NULL || lk->lock_until_s == 0 || now_epoch_s >= lk->lock_until_s) return 0;
    return lk->lock_until_s - now_epoch_s;
}

void app_lock_on_activity(app_lock_t *lk, uint32_t now_uptime_s) {
    if (lk == NULL) return;
    lk->last_activity_uptime_s = now_uptime_s;
}
