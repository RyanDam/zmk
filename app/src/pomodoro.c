/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Pomodoro session engine implementation (backlog a10).
 *
 * Timing model: every phase stores an absolute deadline (k_uptime
 * milliseconds). The 1 s tick compares against it and carries the phase
 * boundary across, so timer latency never accumulates — pad10a added
 * 1000 ms per callback to an internal counter instead (pomo_cb), which
 * drifts with every LVGL hitch.
 *
 * Locking: state lives in one struct under a spinlock. The tick work
 * (system wq) and the start/pause/cancel calls (any thread) write; the
 * screen reads snapshots. Notify dispatch happens outside the lock.
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/spinlock.h>
#include <zephyr/sys/util.h>

#include <zmk/notify.h>
#include <zmk/pomodoro.h>

#if IS_ENABLED(CONFIG_ZMK_DISPLAY_SCREEN_MGR)
#include <zmk/display/screens.h>
#endif

LOG_MODULE_REGISTER(zmk_pomodoro, CONFIG_ZMK_LOG_LEVEL);

#define ENGINE_TICK_MS 1000

static struct zmk_pomo_config config = {
    .sets = CONFIG_ZMK_POMODORO_SETS,
    .work_s = CONFIG_ZMK_POMODORO_WORK_S,
    .rest_s = CONFIG_ZMK_POMODORO_REST_S,
    .noti_mode = CONFIG_ZMK_POMODORO_NOTI_MODE,
};

static struct k_spinlock state_lock;
static struct zmk_pomo_status state; /* .state is the authority      */
static bool blank_locked;

static void engine_tick(struct k_work *work);
K_WORK_DEFINE(engine_work, engine_tick);

static void engine_timer_cb(struct k_timer *timer) {
    ARG_UNUSED(timer);
    k_work_submit(&engine_work);
}
K_TIMER_DEFINE(engine_timer, engine_timer_cb, NULL);

static uint16_t phase_len_s(uint8_t phase_state) {
    return phase_state == ZMK_POMO_STATE_WORK ? config.work_s : config.rest_s;
}

static void blank_lock_take(void) {
#if IS_ENABLED(CONFIG_ZMK_DISPLAY_SCREEN_MGR)
    if (!blank_locked) {
        zmk_display_blank_lock();
        blank_locked = true;
    }
#endif
}

static void blank_lock_release(void) {
#if IS_ENABLED(CONFIG_ZMK_DISPLAY_SCREEN_MGR)
    if (blank_locked) {
        zmk_display_blank_unlock();
        blank_locked = false;
    }
#endif
}

/* Caller must NOT hold state_lock (notify dispatches to backends). */
static void announce(enum zmk_notify_kind kind) {
    switch (config.noti_mode) {
    case ZMK_POMO_NOTI_BEEP:
        zmk_notify(ZMK_NOTIFY_CH_BEEP, kind);
        break;
    case ZMK_POMO_NOTI_LIGHT:
        zmk_notify(ZMK_NOTIFY_CH_LIGHT, kind);
        break;
    case ZMK_POMO_NOTI_BOTH:
        zmk_notify(ZMK_NOTIFY_CH_BEEP, kind);
        zmk_notify(ZMK_NOTIFY_CH_LIGHT, kind);
        break;
    case ZMK_POMO_NOTI_NONE:
    default:
        break;
    }
}

static int64_t now_ms(void) { return k_uptime_get(); }

/* Phase rollover; runs in the system workqueue. All state mutation under
 * the spinlock; notify dispatch happens after releasing it. */
static void engine_tick(struct k_work *work) {
    enum zmk_notify_kind notify_kind = ZMK_NOTIFY_KIND_COUNT;
    int64_t t = now_ms();

    ARG_UNUSED(work);

    k_spinlock_key_t key = k_spin_lock(&state_lock);

    if (state.state == ZMK_POMO_STATE_IDLE) {
        k_spin_unlock(&state_lock, key);
        k_timer_stop(&engine_timer);
        return;
    }

    if (state.paused || t < state.deadline_ms) {
        k_spin_unlock(&state_lock, key);
        return;
    }

    LOG_INF("phase %s set %u/%u took %lld ms (target %u s)",
            state.state == ZMK_POMO_STATE_WORK ? "work" : "rest", state.set_idx, state.sets,
            t - state.phase_start_ms, state.phase_total_s);

    /* Chain the next phase onto the exact boundary, never onto "now":
     * a late tick shifts nothing, so drift cannot accumulate. */
    int64_t boundary = state.deadline_ms;

    if (state.state == ZMK_POMO_STATE_WORK) {
        state.state = ZMK_POMO_STATE_REST;
        state.phase_total_s = phase_len_s(ZMK_POMO_STATE_REST);
        state.phase_start_ms = boundary;
        state.deadline_ms = boundary + (int64_t)state.phase_total_s * MSEC_PER_SEC;
        notify_kind = ZMK_NOTIFY_POMO_WORK_DONE;
    } else if (state.set_idx >= state.sets) {
        state.state = ZMK_POMO_STATE_IDLE;
        notify_kind = ZMK_NOTIFY_POMO_SESSION_DONE;
    } else {
        state.state = ZMK_POMO_STATE_WORK;
        state.set_idx++;
        state.phase_total_s = phase_len_s(ZMK_POMO_STATE_WORK);
        state.phase_start_ms = boundary;
        state.deadline_ms = boundary + (int64_t)state.phase_total_s * MSEC_PER_SEC;
        notify_kind = ZMK_NOTIFY_POMO_REST_DONE;
    }

    bool ended = state.state == ZMK_POMO_STATE_IDLE;
    k_spin_unlock(&state_lock, key);

    if (ended) {
        blank_lock_release();
        k_timer_stop(&engine_timer);
    }
    if (notify_kind < ZMK_NOTIFY_KIND_COUNT) {
        announce(notify_kind);
    }
}

int zmk_pomo_start(void) {
    int ret = 0;
    k_spinlock_key_t key = k_spin_lock(&state_lock);

    if (state.state != ZMK_POMO_STATE_IDLE) {
        ret = -EBUSY;
    } else {
        int64_t t = now_ms();

        state.state = ZMK_POMO_STATE_WORK;
        state.paused = false;
        state.set_idx = 1;
        state.sets = config.sets;
        state.phase_total_s = config.work_s;
        state.phase_start_ms = t;
        state.deadline_ms = t + (int64_t)config.work_s * MSEC_PER_SEC;
        k_timer_start(&engine_timer, K_MSEC(ENGINE_TICK_MS), K_MSEC(ENGINE_TICK_MS));
        LOG_INF("session started: %u sets, work %u s, rest %u s", config.sets, config.work_s,
                config.rest_s);
    }
    k_spin_unlock(&state_lock, key);

    if (ret == 0) {
        blank_lock_take();
    }
    return ret;
}

int zmk_pomo_pause(void) {
    k_spinlock_key_t key = k_spin_lock(&state_lock);
    int ret = 0;

    if (state.state == ZMK_POMO_STATE_IDLE || state.paused) {
        ret = -EINVAL;
    } else {
        state.paused = true;
        state.pause_at_ms = now_ms();
        LOG_INF("session paused in set %u", state.set_idx);
    }
    k_spin_unlock(&state_lock, key);
    return ret;
}

int zmk_pomo_resume(void) {
    k_spinlock_key_t key = k_spin_lock(&state_lock);
    int ret = 0;

    if (state.state == ZMK_POMO_STATE_IDLE || !state.paused) {
        ret = -EINVAL;
    } else {
        /* Shift the whole phase window by the paused duration, keeping
         * phase boundaries (and remaining-time display) exact. */
        int64_t paused_for = now_ms() - state.pause_at_ms;

        state.deadline_ms += paused_for;
        state.phase_start_ms += paused_for;
        state.pause_at_ms = 0;
        state.paused = false;
        LOG_INF("session resumed in set %u (paused %lld ms)", state.set_idx, paused_for);
    }
    k_spin_unlock(&state_lock, key);
    return ret;
}

int zmk_pomo_cancel(void) {
    k_spinlock_key_t key = k_spin_lock(&state_lock);
    bool was_running = state.state != ZMK_POMO_STATE_IDLE;

    state.state = ZMK_POMO_STATE_IDLE;
    state.paused = false;
    k_timer_stop(&engine_timer);
    k_spin_unlock(&state_lock, key);

    if (was_running) {
        blank_lock_release();
        announce(ZMK_NOTIFY_POMO_CANCELLED);
        LOG_INF("session cancelled");
    }
    return 0;
}

bool zmk_pomo_running(void) {
    k_spinlock_key_t key = k_spin_lock(&state_lock);
    bool running = state.state != ZMK_POMO_STATE_IDLE;

    k_spin_unlock(&state_lock, key);
    return running;
}

void zmk_pomo_get_status(struct zmk_pomo_status *out) {
    k_spinlock_key_t key = k_spin_lock(&state_lock);

    *out = state;
    k_spin_unlock(&state_lock, key);
}

int64_t zmk_pomo_remaining_ms(void) {
    struct zmk_pomo_status s;

    zmk_pomo_get_status(&s);
    if (s.state == ZMK_POMO_STATE_IDLE) {
        return 0;
    }

    /* Frozen at the pause instant while held. */
    int64_t now = s.paused ? s.pause_at_ms : now_ms();
    int64_t left = s.deadline_ms - now;

    return left > 0 ? left : 0;
}

const struct zmk_pomo_config *zmk_pomo_config_get(void) { return &config; }

/* ------------------------------------------------------------------ */
/* Config persistence (settings subtree "pomodoro")                    */
/* ------------------------------------------------------------------ */

static void config_clamp(struct zmk_pomo_config *c) {
    c->sets = CLAMP(c->sets, 1, 8);
    c->work_s = CLAMP(c->work_s, 30, 3600);
    c->rest_s = CLAMP(c->rest_s, 10, 3600);
    if (c->noti_mode > ZMK_POMO_NOTI_BOTH) {
        c->noti_mode = ZMK_POMO_NOTI_BOTH;
    }
}

void zmk_pomo_config_set(const struct zmk_pomo_config *cfg) {
    if (cfg == NULL) {
        return;
    }

    k_spinlock_key_t key = k_spin_lock(&state_lock);
    config = *cfg;
    config_clamp(&config);
    if (state.state != ZMK_POMO_STATE_IDLE) {
        state.sets = config.sets; /* session picks up new set count live */
    }
    k_spin_unlock(&state_lock, key);

#if IS_ENABLED(CONFIG_SETTINGS)
    int err = settings_save_one("pomodoro/config", &config, sizeof(config));

    if (err) {
        LOG_WRN("Could not persist pomodoro config (err %d) - live apply only", err);
    }
#endif
    LOG_INF("pomo cfg: sets %u work %u s rest %u s noti %u", config.sets, config.work_s,
            config.rest_s, config.noti_mode);
}

#if IS_ENABLED(CONFIG_SETTINGS)
static int pomo_settings_handle_set(const char *key, size_t len, settings_read_cb read_cb,
                                    void *cb_arg) {
    struct zmk_pomo_config loaded;
    int err;

    if (strcmp("config", key) != 0) {
        return 0;
    }
    if (len != sizeof(loaded)) {
        LOG_WRN("bad pomodoro config settings size %zu", len);
        return -EINVAL;
    }

    err = read_cb(cb_arg, &loaded, sizeof(loaded));
    if (err <= 0) {
        LOG_ERR("Failed to read pomodoro config from settings (err %d)", err);
        return err;
    }

    config_clamp(&loaded);
    config = loaded; /* settings load precedes any session */
    LOG_INF("pomodoro config loaded from settings");
    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(pomodoro, "pomodoro", NULL, pomo_settings_handle_set, NULL, NULL);
#endif /* CONFIG_SETTINGS */
