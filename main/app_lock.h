// main/app_lock.h —— 手势密码与锁定策略的纯逻辑状态机。
//
// 密码 = 8 次"上/下"按键序列,共 2^8 = 256 种组合。存储侧只保存
// salt + 哈希(FNV-1a + 终结混淆),不保存明文序列。
//
// 锁定策略(需求):
//   - 解锁状态 15 分钟无任何按键 → 自动锁定;
//   - 连续 10 次解锁失败 → 锁定 60 分钟(惩罚截止时间持久化,断电重启不清零;
//     依赖调用方把 fail_count/lock_until_s 写入 NVS,见 app_store)。
//
// 时间由调用方注入:now_epoch_s(墙上时钟,用于 60 分钟惩罚,需 SNTP 同步)、
// now_uptime_s(单调开机秒,用于 15 分钟空闲检测)。本模块不调用任何系统时间
// API,可在宿主机确定性测试(tests/test_app_lock.c)。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define APP_LOCK_MOVES           8
#define APP_LOCK_FAIL_LIMIT      10
#define APP_LOCK_LOCKOUT_S       (60 * 60)
#define APP_LOCK_IDLE_TIMEOUT_S  (15 * 60)
#define APP_LOCK_SALT_LEN        4

typedef enum {
    APP_LOCK_MOVE_DOWN = 0,
    APP_LOCK_MOVE_UP = 1,
} app_lock_move_t;

typedef enum {
    APP_LOCK_PHASE_SETUP = 0,  // 未设密码,首次输入中
    APP_LOCK_PHASE_CONFIRM,    // 首次输入完成,等待重复确认
    APP_LOCK_PHASE_UNLOCK,     // 已设密码且处于锁定态
    APP_LOCK_PHASE_OPEN,       // 解锁状态
    APP_LOCK_PHASE_LOCKOUT,    // 惩罚期,拒绝一切输入
} app_lock_phase_t;

typedef enum {
    APP_LOCK_EV_NONE = 0,
    APP_LOCK_EV_MORE,                 // 接受一步,尚未满 8 步
    APP_LOCK_EV_SETUP_ENTER_CONFIRM,  // 首次输入完成,请用户重复输入
    APP_LOCK_EV_SETUP_DONE,           // 确认一致;salt/pwd_hash 已更新,调用方需持久化
    APP_LOCK_EV_SETUP_MISMATCH,       // 两次不一致,重新回到首次输入
    APP_LOCK_EV_UNLOCKED,             // 解锁成功;调用方需清零持久化的 fail_count
    APP_LOCK_EV_WRONG,                // 解锁失败一次;调用方需持久化 fail_count
    APP_LOCK_EV_LOCKOUT_START,        // 连续错误达上限;调用方需持久化 lock_until_s
} app_lock_event_t;

typedef struct {
    app_lock_phase_t phase;
    uint8_t progress;      // 当前已输入步数 0..8
    uint8_t draft;         // bit i = 第 i 步方向(1=上,0=下)
    uint8_t confirm_draft; // SETUP 阶段保存的首次输入
    uint8_t fail_count;    // 连续失败次数(达 APP_LOCK_FAIL_LIMIT 触发惩罚)
    uint32_t lock_until_s; // 惩罚截止 epoch 秒;0 = 无
    bool has_pwd;
    uint8_t salt[APP_LOCK_SALT_LEN];
    uint64_t pwd_hash;
    uint32_t last_activity_uptime_s; // 最近一次按键的单调开机秒(仅 OPEN 态有意义)
} app_lock_t;

// hash = FNV-1a(salt || 每一位方向) 再做雪崩终结。公开供宿主测试与存储校验。
uint64_t app_lock_hash(const uint8_t salt[APP_LOCK_SALT_LEN], uint8_t moves);

// 开机恢复。has_pwd=false 时进入 SETUP(等价于首次设密入口),salt 必须由调用方
// 预先注入;has_pwd=true 时进入 UNLOCK。惩罚是否生效由随后 poll_lockout 判定。
void app_lock_restore(app_lock_t *lk, bool has_pwd, const uint8_t salt[APP_LOCK_SALT_LEN],
                      uint64_t hash, uint8_t fail_count, uint32_t lock_until_s);

app_lock_event_t app_lock_push_move(app_lock_t *lk, app_lock_move_t move, uint32_t now_epoch_s);

// 删除最后一步(UNLOCK/SETUP/CONFIRM 中有效)。
void app_lock_backspace(app_lock_t *lk);

// 主动锁定(菜单项/空闲超时调用)。LOCKOUT 态不受影响,不会提前解除惩罚。
void app_lock_lock(app_lock_t *lk);

// OPEN 态下超过 5 分钟无按键视为应锁定。
bool app_lock_idle_expired(const app_lock_t *lk, uint32_t now_uptime_s);

// 惩罚判定与解除:在 UNLOCK/LOCKOUT 态调用。若 lock_until 未到 → 转入 LOCKOUT;
// 若已到 → 清零 lock_until(调用方需持久化)并停留在 UNLOCK。返回惩罚是否激活。
bool app_lock_poll_lockout(app_lock_t *lk, uint32_t now_epoch_s);

// 惩罚剩余秒数(LOCKOUT 态);非惩罚态返回 0。
uint32_t app_lock_lockout_remaining(const app_lock_t *lk, uint32_t now_epoch_s);

// 任何按键都应调用,刷新空闲计时。
void app_lock_on_activity(app_lock_t *lk, uint32_t now_uptime_s);
