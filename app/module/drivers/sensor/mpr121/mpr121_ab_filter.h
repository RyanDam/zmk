/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <math.h>

struct mpr121_ab_filter {
    float x;
    float y;
    float vx;
    float vy;
    bool initialised;
};

static inline float ab_beta_from_alpha(float alpha) {
    return 2.0f * (2.0f - alpha) - 2.0f * sqrtf(1.0f - alpha);
}

static inline void
ab_filter_init(struct mpr121_ab_filter *f, float seed_x, float seed_y) {
    f->x = seed_x;
    f->y = seed_y;
    f->vx = 0.0f;
    f->vy = 0.0f;
    f->initialised = true;
}

static inline void
ab_filter_reset(struct mpr121_ab_filter *f) {
    f->initialised = false;
}

static inline float ab_alpha_from_tau(float tau_ms, float dt_ms) {
    if (tau_ms <= 0.0f) {
        return 1.0f;
    }
    return 1.0f - expf(-dt_ms / tau_ms);
}

static inline void
ab_filter_update(struct mpr121_ab_filter *f, float meas_x, float meas_y,
                 float tau_ms, float dt_ms) {
    if (!f->initialised) {
        ab_filter_init(f, meas_x, meas_y);
        return;
    }

    float alpha = ab_alpha_from_tau(tau_ms, dt_ms);
    float beta = ab_beta_from_alpha(alpha);

    float x_pred = f->x + f->vx * dt_ms;
    float y_pred = f->y + f->vy * dt_ms;

    float rx = meas_x - x_pred;
    float ry = meas_y - y_pred;

    f->x = x_pred + alpha * rx;
    f->y = y_pred + alpha * ry;

    float delta_vx = (beta / dt_ms) * rx;
    float delta_vy = (beta / dt_ms) * ry;

    float max_v = 2.0f;
    f->vx = fmaxf(-max_v, fminf(max_v, f->vx + delta_vx));
    f->vy = fmaxf(-max_v, fminf(max_v, f->vy + delta_vy));
}
