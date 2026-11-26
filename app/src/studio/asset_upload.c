/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Studio bulk-upload backend: the flash side of the transfer only (proto
 * encoding lives in assets_subsystem.c).
 *
 * Erase/write on the ESP runs through the HAL's cache-off windows for tens of
 * ms, so it never runs on the RPC or display thread: the handler copies bytes
 * into a staging ring and this preemptive-low queue writes, CRC-checks and
 * commits.
 *
 * Commit-last integrity: begin allocates a gap, data drains the ring into it,
 * end flushes, re-reads the blob, verifies the CRC and only then commits a new
 * manifest generation — so any earlier interruption leaves the live manifest
 * and every byte it references untouched. GC (blob relocation) runs here too.
 *
 * One chunk is in flight, so `seq` only spots gaps and repeats of the last
 * acked chunk (re-ack is idempotent).
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/ring_buffer.h>
#include <zephyr/sys/crc.h>

#include <zmk/assets.h>
#include <zmk/studio/rpc.h>

#include "asset_upload.h"

LOG_MODULE_DECLARE(zmk_studio, CONFIG_ZMK_STUDIO_LOG_LEVEL);

#define UPLOAD_CHUNK_MAX 1024 /* proto max_size — larger never arrives */
#define STAGING_SIZE 16384    /* wire speed vs flash yield windows      */
#define COMMIT_WAIT_MS 15000  /* RPC-thread wait for flush+commit       */
#define ABORT_WAIT_MS 3000
#define QUIESCE_WAIT_MS 3000 /* anim tick is 200 ms: 15x margin        */
#define PROGRESS_STEP (32 * 1024)
#define ENTRY_LIST_MAX 32

#define DATA_SECTOR 4096
#define FLUSH_BUF 1024

/* Session state */

struct upload_session {
    bool active;
    bool got_end;
    bool aborted;
    char name[ZMK_ASSET_NAME_MAX];
    uint32_t size;
    uint32_t want_crc;
    uint16_t kind;
    uint16_t fps;
    uint32_t off;      /* blob region (sector-aligned, from alloc)   */
    uint32_t seq_next; /* next accepted chunk index (RPC thread)     */
    uint32_t accepted; /* bytes into staging (RPC thread)            */
    uint32_t wrote;    /* bytes written to flash (uploader only)     */
    uint32_t erase_next; /* next sector erased (uploader only)       */
};

static struct upload_session session;
static struct k_mutex session_lock;
static struct ring_buf staging;
static uint8_t staging_mem[STAGING_SIZE];
static struct k_sem commit_done; /* given by the work after end/abort  */
static struct k_sem drain_space; /* producer waits when staging full   */
static struct k_sem gc_done;     /* given at the end of every gc run   */

/* Last commit result; only the uploader queue writes it between takes. */
static bool commit_ok;
static char commit_err[48];

static struct k_work flush_work;
static struct k_work gc_work;
static struct k_work progress_work;

static void gc_run_inline(void); /* defined below, used by the commit retry */

/* Dedicated workqueue */

K_KERNEL_STACK_DEFINE(upload_wq_stack, 4096);
static struct k_work_q upload_wq;
static atomic_t gc_busy;

static int upload_wq_start(void) {
    static const struct k_work_queue_config queue_config = {.name = "asset_upload"};

    k_work_queue_start(&upload_wq, upload_wq_stack, K_KERNEL_STACK_SIZEOF(upload_wq_stack),
                       K_PRIO_PREEMPT(7), &queue_config);
    return 0;
}

/* Only queue-start statics; the session is used after every SYS_INIT. */
SYS_INIT(upload_wq_start, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

/* Progress notifications */

static void notify_progress(const char *name, uint32_t accepted, uint32_t total, bool gc_running) {
    zmk_assets_UploadProgress p = {0};

    snprintf(p.name, sizeof(p.name), "%s", name);
    p.accepted_bytes = accepted;
    p.total_bytes = total;
    p.gc_running = gc_running;

    zmk_studio_Notification n = {
        .which_subsystem = zmk_studio_Notification_assets_tag,
        .subsystem =
            {
                .assets =
                    {
                        .which_notification_type = zmk_assets_Notification_upload_progress_tag,
                        .notification_type = {.upload_progress = p},
                    },
            },
    };

    raise_zmk_studio_rpc_notification(
        (struct zmk_studio_rpc_notification){.notification = n});
}

static void progress_work_fn(struct k_work *work) {
    char name[sizeof(session.name)];
    uint32_t accepted, total;

    ARG_UNUSED(work);

    k_mutex_lock(&session_lock, K_FOREVER);
    memcpy(name, session.name, sizeof(name));
    accepted = session.wrote;
    total = session.size;
    k_mutex_unlock(&session_lock);

    notify_progress(name, accepted, total, false);
}

/* Flash staging (uploader thread) */

/* Eager sector erase is safe: the session owns its region and nothing else is
 * there yet (append-only). */
static int erase_to_cover(struct upload_session *s, uint32_t pos, uint32_t len) {
    uint32_t up_to = ROUND_UP(pos + len, DATA_SECTOR);

    while (s->erase_next < up_to) {
        int err = zmk_assets_flash_erase(s->erase_next, DATA_SECTOR);

        if (err != 0) {
            LOG_ERR("upload erase @%u failed (err %d)", s->erase_next, err);
            return err;
        }
        s->erase_next += DATA_SECTOR;
    }
    return 0;
}

/* Drain the ring; verify + commit at end of stream. Returning mid-stream is
 * fine — the next data chunk re-submits this work. */
static void flush_work_fn(struct k_work *work) {
    struct upload_session *s = &session;
    uint8_t buf[FLUSH_BUF];
    bool aborted = false;

    ARG_UNUSED(work);

    for (;;) {
        bool ended;

        k_mutex_lock(&session_lock, K_FOREVER);
        ended = s->got_end;
        aborted = s->aborted;
        k_mutex_unlock(&session_lock);

        if (aborted) {
            ring_buf_reset(&staging); /* drop the rest: nothing committed */
            k_sem_give(&drain_space);
            break;
        }

        /* Never drain a sub-4-byte tail: zmk_assets_flash_write needs 4-byte
         * alignment, so the tail stays staged until the end marker pads it. */
        uint32_t want = FLUSH_BUF;

        if (!ended) {
            uint32_t avail = ring_buf_size_get(&staging);

            want = ROUND_DOWN(MIN(avail, (uint32_t)FLUSH_BUF), 4);
        }

        uint32_t got = ring_buf_get(&staging, buf, want);

        if (got == 0) {
            if (ended) {
                break; /* stream complete: verify + commit below */
            }
            return; /* mid-stream: ring empty, or only a <4-byte tail
                     * held for the end marker — the next chunk or
                     * upload_end re-submits this work */
        }

        uint32_t wlen = got;

        if (ended && ring_buf_size_get(&staging) == 0 && (wlen & 3) != 0) {
            memset(buf + wlen, 0, 4 - (wlen & 3)); /* sector is erased */
            wlen = ROUND_UP(wlen, 4);
        }

        if (erase_to_cover(s, s->off + s->wrote, wlen) != 0 ||
            zmk_assets_flash_write(s->off + s->wrote, buf, wlen) != 0) {
            s->aborted = true;
            ring_buf_reset(&staging);
            k_sem_give(&drain_space);
            aborted = true;
            break;
        }

        s->wrote += got;
        k_sem_give(&drain_space);

        if (s->wrote / PROGRESS_STEP != (s->wrote - got) / PROGRESS_STEP) {
            k_work_submit_to_queue(&upload_wq, &progress_work);
        }
    }

    /* The live manifest is the previous generation until commit_entries' last
     * write, so every early exit here is safe. */
    commit_ok = false;
    commit_err[0] = '\0';

    if (aborted) {
        strncpy(commit_err, "aborted", sizeof(commit_err) - 1);
        goto done;
    }
    if (s->wrote != s->size) {
        LOG_WRN("upload '%s': short stream (%u of %u)", s->name, s->wrote, s->size);
        strncpy(commit_err, "short stream", sizeof(commit_err) - 1);
        goto done;
    }

    /* CRC from flash catches wire corruption and our own write bugs before
     * anything references the blob. */
    {
        struct zmk_asset_info staged = {
            .offset = s->off, .len = s->size, .kind = s->kind, .fps = s->fps};
        uint32_t crc = 0, left = s->size;
        bool rd_err = false;

        while (left > 0) {
            uint32_t chunk = left > FLUSH_BUF ? FLUSH_BUF : left;

            if (zmk_asset_read(&staged, s->size - left, buf, chunk) != 0) {
                rd_err = true;
                break;
            }
            crc = crc32_ieee_update(crc, buf, chunk);
            left -= chunk;
        }

        if (rd_err) {
            strncpy(commit_err, "readback failed", sizeof(commit_err) - 1);
            goto done;
        }
        if (crc != s->want_crc) {
            LOG_WRN("upload '%s': CRC %08x != expected %08x - live image untouched", s->name,
                    crc, s->want_crc);
            strncpy(commit_err, "crc mismatch", sizeof(commit_err) - 1);
            goto done;
        }
    }

    /* Commit: the current manifest entries with this name replaced/added. */
    {
        struct zmk_asset_info list[ENTRY_LIST_MAX];
        struct zmk_asset_info *hit = NULL;
        int n = zmk_assets_live_snapshot(list, ENTRY_LIST_MAX);
        int err;

        for (int i = 0; i < n; i++) {
            if (strcmp(list[i].name, s->name) == 0) {
                hit = &list[i];
                break;
            }
        }
        if (hit != NULL) {
            hit->offset = s->off;
            hit->len = s->size;
            hit->kind = s->kind;
            hit->fps = s->fps;
            hit->crc = s->want_crc;
        } else {
            if (n >= ENTRY_LIST_MAX) {
                strncpy(commit_err, "manifest full", sizeof(commit_err) - 1);
                goto done;
            }
            list[n] = (struct zmk_asset_info){.offset = s->off,
                                              .len = s->size,
                                              .kind = s->kind,
                                              .fps = s->fps,
                                              .crc = s->want_crc};
            snprintf(list[n].name, sizeof(list[n].name), "%.*s", (int)sizeof(list[n].name) - 1,
                     s->name);
            n++;
        }

        err = zmk_assets_commit_entries(list, n);
        if (err == -EAGAIN) {
            /* v1 blobs below the data floor: compact, retry (same queue). */
            LOG_INF("upload '%s': legacy v1 layout - GC compacting first", s->name);
            atomic_set(&gc_busy, 1); /* gc_run_inline clears it */
            gc_run_inline();
            err = zmk_assets_commit_entries(list, n);
        }
        if (err != 0) {
            LOG_ERR("upload '%s' commit failed (err %d)", s->name, err);
            strncpy(commit_err, "commit failed", sizeof(commit_err) - 1);
            goto done;
        }
        commit_ok = true;
        LOG_INF("upload '%s' committed (%u bytes @+%u)", s->name, s->size, s->off);
    }

done:
    k_mutex_lock(&session_lock, K_FOREVER);
    s->active = false;
    s->got_end = false;
    k_mutex_unlock(&session_lock);

    k_work_submit_to_queue(&upload_wq, &progress_work);
    k_sem_give(&commit_done);
}

/* GC (compacting rewrite; uploader thread only) */

/* Rewrites every partition blob into a compacted layout, relocating down only
 * where a full sector of head-room separates destination from source, which is
 * what keeps the forward chunk copy safe (writes stay a sector behind the read
 * position, erases never reach unread bytes). Blobs that cannot move safely
 * stay put. The manifest commits LAST, so a failed copy abandons the pass and
 * the live generation still points at untouched blobs. */
static void gc_run_inline(void) {
    struct zmk_asset_info list[ENTRY_LIST_MAX];
    uint32_t dest[ENTRY_LIST_MAX];
    uint8_t buf[FLUSH_BUF];
    int n, moved = 0, i;

    if (zmk_assets_request_quiesce(QUIESCE_WAIT_MS) != 0) {
        LOG_WRN("gc: playback quiesce timed out - skipping compaction");
        goto out;
    }

    /* Snapshot only after the park: a pre-park snapshot could resurrect a
     * delete or drop a just-committed blob in pass 3. */
    n = zmk_assets_live_snapshot(list, ENTRY_LIST_MAX);
    if (n == 0) {
        /* Nothing to move: an empty commit still performs the v1 -> v2 upgrade. */
        (void)zmk_assets_commit_entries(list, 0);
        zmk_assets_release_quiesce();
        goto out;
    }

    /* Sort ascending by offset (snapshot order is table order). */
    for (i = 0; i < n; i++) {
        for (int j = i + 1; j < n; j++) {
            if (list[j].offset < list[i].offset) {
                struct zmk_asset_info t = list[i];

                list[i] = list[j];
                list[j] = t;
            }
        }
    }

    /* Pass 1: destinations, sector-aligned and disjoint by construction. */
    uint32_t cursor = DATA_SECTOR;

    for (i = 0; i < n; i++) {
        if (cursor + DATA_SECTOR <= list[i].offset) {
            dest[i] = cursor; /* safe relocation */
        } else {
            dest[i] = list[i].offset; /* keep in place */
        }
        cursor = ROUND_UP(dest[i] + list[i].len, DATA_SECTOR);
        if (dest[i] != list[i].offset) {
            moved++;
        }
    }

    LOG_INF("gc: %d entries, %d to relocate", n, moved);

    /* A blob below the data floor (v1 image only) cannot move downward, and the
     * commit gate would reject the whole list after a full copy pass. */
    for (i = 0; i < n; i++) {
        if (dest[i] < DATA_SECTOR) {
            LOG_ERR("gc: '%s' @%u sits below the data floor and cannot be "
                    "relocated - repack + reflash assets.bin (v2)",
                    list[i].name, list[i].offset);
            zmk_assets_release_quiesce();
            goto out;
        }
    }

    if (moved > 0) {
        notify_progress("", 0, 0, true);
    }

    /* Pass 2: relocate forward chunk by chunk, ascending. */
    for (i = 0; i < n && moved > 0; i++) {
        uint32_t left = list[i].len;
        uint32_t woff = dest[i];
        uint32_t erased_to = 0; /* next un-erased sector end */
        uint32_t crc = 0;

        if (dest[i] == list[i].offset) {
            continue;
        }

        while (left > 0) {
            uint32_t chunk = left > FLUSH_BUF ? FLUSH_BUF : left;
            uint32_t wlen = ROUND_UP(chunk, 4);
            uint32_t sec = ROUND_DOWN(woff, DATA_SECTOR);

            if (zmk_asset_read(&list[i], list[i].len - left, buf, chunk) != 0) {
                goto copy_abort;
            }
            if (chunk != wlen) {
                memset(buf + chunk, 0, wlen - chunk);
            }
            crc = crc32_ieee_update(crc, buf, chunk);

            if (woff >= erased_to) { /* each dest sector erased once */
                if (zmk_assets_flash_erase(sec, DATA_SECTOR) != 0) {
                    goto copy_abort;
                }
                erased_to = sec + DATA_SECTOR;
            }
            if (zmk_assets_flash_write(woff, buf, wlen) != 0) {
                goto copy_abort;
            }

            woff += chunk;
            left -= chunk;
        }

        if (list[i].crc != 0 && crc != list[i].crc) {
            LOG_ERR("gc: '%s' CRC moved wrong - abandoning before commit", list[i].name);
            goto copy_abort;
        }
        list[i].offset = dest[i];
    }

    /* Pass 3: commit the compacted manifest (every copy verified by here). */
    if (moved > 0 && zmk_assets_commit_entries(list, n) != 0) {
        LOG_ERR("gc: commit failed - live image untouched");
    }
    zmk_assets_release_quiesce();
    goto out;

copy_abort:
    LOG_ERR("gc: copy failed - abandoning, live manifest stays");
    zmk_assets_release_quiesce();

out:
    atomic_set(&gc_busy, 0);
    k_sem_give(&gc_done);
}

static void gc_work_fn(struct k_work *work) {
    ARG_UNUSED(work);
    gc_run_inline();
}

/* Public API (called from the RPC thread) */

int zmk_asset_upload_begin(const char *name, uint32_t size, uint32_t crc, uint16_t kind,
                           uint16_t fps, uint32_t *free_out, const char **err) {
    struct upload_session *s = &session;
    size_t name_len = strnlen(name, ZMK_ASSET_NAME_MAX);
    uint32_t off;
    int r;

    *err = NULL;
    k_mutex_lock(&session_lock, K_FOREVER);
    if (s->active || atomic_get(&gc_busy) != 0 || zmk_assets_quiesced()) {
        /* Serialize store mutations: GC destinations can overlap an
         * allocation taken from the pre-GC table. */
        k_mutex_unlock(&session_lock);
        *err = s->active ? "session open: abort or finish first"
                         : "gc running - retry when it finishes";
        return -EBUSY;
    }
    if (name_len == 0 || name_len >= ZMK_ASSET_NAME_MAX) {
        k_mutex_unlock(&session_lock);
        *err = "name must be 1..15 chars";
        return -EINVAL;
    }
    if (size == 0 || size > 2u * 1024 * 1024 || kind > ZMK_ASSET_LOTTIE) {
        k_mutex_unlock(&session_lock);
        *err = "bad size/kind";
        return -EINVAL;
    }
    if (zmk_assets_legacy_image()) {
        struct zmk_asset_info part;

        if (zmk_assets_live_snapshot(&part, 1) > 0) {
            /* v1 blobs below DATA_FLOOR can never pass the commit floor gate,
             * so refuse before burning the transfer: repack and reflash a v2
             * assets.bin. An empty v1 manifest is fine (GC upgrades it). */
            k_mutex_unlock(&session_lock);
            *err = "v1 image: reflash assets.bin (v2) before uploads";
            return -EPERM;
        }
    }

    r = zmk_assets_alloc(size, &off);
    if (r != 0) {
        uint32_t gap = 0;

        (void)zmk_assets_free_bytes(&gap);
        k_mutex_unlock(&session_lock);
        *err = "no contiguous gap - run upload_gc";
        if (free_out != NULL) {
            *free_out = gap;
        }
        return -ENOSPC;
    }

    *s = (struct upload_session){.active = true};
    ring_buf_reset(&staging); /* drop any debris from an aborted session */
    k_sem_reset(&drain_space);
    k_sem_reset(&commit_done);
    snprintf(s->name, sizeof(s->name), "%s", name);
    s->size = size;
    s->want_crc = crc;
    s->kind = kind;
    s->fps = fps;
    s->off = off;
    s->erase_next = off; /* alloc returns sector-aligned offsets */
    k_mutex_unlock(&session_lock);

    if (free_out != NULL) {
        uint32_t gap;

        (void)zmk_assets_free_bytes(&gap);
        *free_out = gap;
    }
    LOG_INF("upload begin '%s' %u bytes @+%u", name, size, off);
    return 0;
}

int zmk_asset_upload_data(uint32_t seq, const uint8_t *data, size_t len, uint32_t *accepted,
                          const char **err) {
    struct upload_session *s = &session;
    size_t put = 0;
    int wait_ms = 0;

    *err = NULL;
    *accepted = 0;

    if (len == 0 || len > UPLOAD_CHUNK_MAX) {
        *err = "chunk size out of range";
        return -EINVAL;
    }

    k_mutex_lock(&session_lock, K_FOREVER);
    if (!s->active || s->got_end || s->aborted) {
        k_mutex_unlock(&session_lock);
        *err = "no open session";
        return -ENOTCONN;
    }
    if (seq + 1 == s->seq_next) { /* idempotent re-ack: a lost ack reply */
        *accepted = s->accepted;
        k_mutex_unlock(&session_lock);
        return 0;
    }
    if (seq != s->seq_next) {
        k_mutex_unlock(&session_lock);
        *err = "seq gap - abort and restart";
        return -EPROTO;
    }
    s->seq_next++;
    k_mutex_unlock(&session_lock);

    /* All-or-nothing staging: wait for room, then put once, so a client
     * retransmit can never duplicate partial data. */
    while (put < len) {
        if (ring_buf_size_get(&staging) <= STAGING_SIZE - (len - put)) {
            put += ring_buf_put(&staging, data + put, len - put);
            break;
        }
        k_work_submit_to_queue(&upload_wq, &flush_work);
        if (k_sem_take(&drain_space, K_MSEC(50)) != 0) {
            wait_ms += 50;
        }
        if (wait_ms > 5000) {
            *err = "staging stalled (device busy?)";
            return -EBUSY;
        }
        k_mutex_lock(&session_lock, K_FOREVER);
        bool dead = s->aborted || !s->active;
        k_mutex_unlock(&session_lock);
        if (dead) {
            *err = "session died mid-chunk";
            return -ECONNABORTED;
        }
    }

    s->accepted += put; /* RPC thread is the only writer */
    *accepted = s->accepted;
    k_work_submit_to_queue(&upload_wq, &flush_work);
    return 0;
}

int zmk_asset_upload_end(uint32_t *free_out, bool *committed, const char **err) {
    struct upload_session *s = &session;

    *err = NULL;
    *committed = false;

    k_mutex_lock(&session_lock, K_FOREVER);
    if (!s->active || s->aborted) {
        k_mutex_unlock(&session_lock);
        *err = "no open session";
        return -ENOTCONN;
    }
    s->got_end = true;
    k_mutex_unlock(&session_lock);

    k_work_submit_to_queue(&upload_wq, &flush_work);
    if (k_sem_take(&commit_done, K_MSEC(COMMIT_WAIT_MS)) != 0) {
        *err = "commit timeout";
        return -ETIMEDOUT;
    }

    *committed = commit_ok;
    if (!commit_ok) {
        *err = commit_err[0] != '\0' ? commit_err : "commit failed";
    }
    if (free_out != NULL) {
        uint32_t gap;

        (void)zmk_assets_free_bytes(&gap);
        *free_out = gap;
    }
    return 0;
}

int zmk_asset_upload_abort(void) {
    struct upload_session *s = &session;
    char name[sizeof(s->name)];

    k_mutex_lock(&session_lock, K_FOREVER);
    if (!s->active) {
        k_mutex_unlock(&session_lock);
        return -ENOTCONN;
    }
    s->aborted = true;
    memcpy(name, s->name, sizeof(name));
    k_mutex_unlock(&session_lock);

    k_work_submit_to_queue(&upload_wq, &flush_work);
    (void)k_sem_take(&commit_done, K_MSEC(ABORT_WAIT_MS));
    LOG_INF("upload '%s' aborted (nothing committed)", name);
    return 0;
}

int zmk_asset_upload_status(bool *active, char *name, size_t name_sz, uint32_t *accepted) {
    k_mutex_lock(&session_lock, K_FOREVER);
    *active = session.active && !session.got_end;
    snprintf(name, name_sz, "%s", session.name);
    *accepted = session.accepted;
    k_mutex_unlock(&session_lock);
    return 0;
}

int zmk_asset_upload_gc(uint32_t *free_out) {
    k_mutex_lock(&session_lock, K_FOREVER);
    bool busy = session.active || atomic_get(&gc_busy) != 0 || zmk_assets_quiesced();
    k_mutex_unlock(&session_lock);
    if (busy) {
        return -EBUSY;
    }

    atomic_set(&gc_busy, 1);
    k_work_submit_to_queue(&upload_wq, &gc_work);
    if (free_out != NULL) {
        uint32_t gap;

        (void)zmk_assets_free_bytes(&gap); /* pre-GC view; notifications carry the rest */
        *free_out = gap;
    }
    return 0;
}

int zmk_asset_store_delete(const char *name, bool *deleted) {
    struct zmk_asset_info list[ENTRY_LIST_MAX];
    int n, out, i, err;

    *deleted = false;

    k_mutex_lock(&session_lock, K_FOREVER);
    if (zmk_assets_quiesced() || atomic_get(&gc_busy) != 0 || session.active) {
        /* A queued GC would commit a stale snapshot and resurrect the entry;
         * serialize store mutations. */
        k_mutex_unlock(&session_lock);
        return -EBUSY;
    }
    k_mutex_unlock(&session_lock);

    n = zmk_assets_live_snapshot(list, ENTRY_LIST_MAX);
    out = 0;
    for (i = 0; i < n; i++) {
        if (strcmp(list[i].name, name) == 0) {
            continue;
        }
        list[out++] = list[i];
    }
    if (out == n) {
        return 0; /* builtin-only or unknown: nothing in the manifest to drop */
    }

    err = zmk_assets_commit_entries(list, out);
    if (err == -EAGAIN) {
        /* v1 layout: compact on the uploader queue, then redo there. */
        if (atomic_get(&gc_busy) != 0) {
            return -EBUSY;
        }
        atomic_set(&gc_busy, 1);
        k_work_submit_to_queue(&upload_wq, &gc_work);
        if (k_sem_take(&gc_done, K_MSEC(QUIESCE_WAIT_MS + 10000)) != 0) {
            return -ETIMEDOUT;
        }

        n = zmk_assets_live_snapshot(list, ENTRY_LIST_MAX);
        out = 0;
        for (i = 0; i < n; i++) {
            if (strcmp(list[i].name, name) != 0) {
                list[out++] = list[i];
            }
        }
        if (out == n) {
            return 0;
        }
        err = zmk_assets_commit_entries(list, out);
    }
    if (err != 0) {
        return err;
    }
    *deleted = true;
    return 0;
}

/* Module init */

static int asset_upload_init(void) {
    k_mutex_init(&session_lock);
    k_sem_init(&commit_done, 0, 1);
    k_sem_init(&drain_space, 0, 1);
    k_sem_init(&gc_done, 0, 1);
    ring_buf_init(&staging, sizeof(staging_mem), staging_mem);
    k_work_init(&flush_work, flush_work_fn);
    k_work_init(&gc_work, gc_work_fn);
    k_work_init(&progress_work, progress_work_fn);
    return 0;
}

SYS_INIT(asset_upload_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
