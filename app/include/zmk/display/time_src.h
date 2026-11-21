/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Coban Tab 1a time-source seam (backlog a09).
 *
 * Screens that show wall-clock time (a09 clock, later a10/a11) ask this API
 * instead of reading uptime themselves. The v1 backend is a simulation: a
 * Kconfig epoch advanced by system uptime, reporting `synced = false` — the
 * clock screen renders a visible "unsynced" affordance until something
 * raises the flag. Later items swap the backend, never the UI:
 *
 *   - h02 (DS1302): a bit-bang RTC driver registers zmk_time_source_set()
 *     and reports synced from the chip's oscillator run bit.
 *   - a14 (Studio RPC): the host `set_time` call is zmk_time_sync() — the
 *     pad10a equivalent was hid.c:48-70 writing the screen clock + RTC.
 *
 * All calls are safe from any thread (plain state, no locking needed at
 * this rate).
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

/** A civil-time reading; fields mirror pad10a's screen_time_sync_datetime. */
struct zmk_time {
    uint16_t year;   /* full year, e.g. 2026                       */
    uint8_t month;   /* 1-12                                       */
    uint8_t day;     /* 1-31                                       */
    uint8_t weekday; /* 0 = Sunday                                 */
    uint8_t hour;    /* 0-23                                       */
    uint8_t minute;  /* 0-59                                       */
    uint8_t second;  /* 0-59                                       */
    bool synced;     /* true once an RTC or the host confirmed it  */
};

/**
 * @brief Hardware time backend (h02).
 *
 * @param synced_out set true only if the hardware clock is running.
 * @return current epoch seconds, or negative if unavailable.
 */
typedef int64_t (*zmk_time_source_fn)(bool *synced_out);

/** Replace the epoch source; NULL restores the simulation backend. */
void zmk_time_source_set(zmk_time_source_fn fn);

/** Read the current wall time. Returns 0, or -1 if no source can tell. */
int zmk_time_now(struct zmk_time *out);

/** Raw epoch seconds + sync flag (same source as zmk_time_now). */
int64_t zmk_time_now_epoch(bool *synced_out);

/**
 * @brief Confirm the wall clock at @p epoch (host set_time, a14; or a dev
 * bench toggle). Until something calls this the sim backend reports
 * synced = false and the clock shows its "unsynced" affordance.
 */
void zmk_time_sync(int64_t epoch);

/** Drop back to the unsynced simulation (bench toggle for a09 acceptance). */
void zmk_time_mark_unsynced(void);
