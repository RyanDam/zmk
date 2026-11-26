/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Coban Tab 1a time-source seam.
 * Screens that show wall-clock time ask this API instead of reading uptime
 * themselves. The v1 backend is a simulation: a Kconfig epoch advanced by
 * system uptime, reporting synced = false until something calls
 * zmk_time_sync(). A hardware RTC registers itself with
 * zmk_time_source_set(); the UI never changes. All calls are safe from any
 * thread (plain state, no locking needed at this rate).
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

/** A civil-time reading. */
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

/** Hardware time backend.
 *  @param synced_out set true only if the hardware clock is running.
 *  @return current epoch seconds, or negative if unavailable. */
typedef int64_t (*zmk_time_source_fn)(bool *synced_out);

/** Replace the epoch source; NULL restores the simulation backend. */
void zmk_time_source_set(zmk_time_source_fn fn);

/** Read the current wall time. Returns 0, or -1 if no source can tell. */
int zmk_time_now(struct zmk_time *out);

/** Raw epoch seconds + sync flag (same source as zmk_time_now). */
int64_t zmk_time_now_epoch(bool *synced_out);

/** Confirm the wall clock at @p epoch (host set_time, or a dev bench toggle).
 *  Until something calls this the sim backend reports synced = false. */
void zmk_time_sync(int64_t epoch);

/** Drop back to the unsynced simulation. */
void zmk_time_mark_unsynced(void);
