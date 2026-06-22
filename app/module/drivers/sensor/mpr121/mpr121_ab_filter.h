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

static inline void
ab_filter_update(struct mpr121_ab_filter *f, float meas_x, float meas_y,
                 float alpha, float dt_ms) {
    if (!f->initialised) {
        ab_filter_init(f, meas_x, meas_y);
        return;
    }

    float beta = ab_beta_from_alpha(alpha);

    float x_pred = f->x + f->vx * dt_ms;
    float y_pred = f->y + f->vy * dt_ms;

    float rx = meas_x - x_pred;
    float ry = meas_y - y_pred;

    f->x = x_pred + alpha * rx;
    f->y = y_pred + alpha * ry;
    f->vx = f->vx + (beta / dt_ms) * rx;
    f->vy = f->vy + (beta / dt_ms) * ry;
}
