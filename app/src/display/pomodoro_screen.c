/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Pomodoro screen for the Coban Tab 1a (backlog a10).
 *
 * pad10a's spirit (screen_pomodoro.c): the page is a quiet object — a
 * Vietnamese proverb breathing over an accent disc — and timer furniture
 * only exists while a session runs. The 2026-11-22/23 owner passes:
 *
 *   idle     quote centred on a breathing white disc (10 %, untinted);
 *            no text chrome (the settings dev-strip was removed
 *            2026-11-23: durations come from Kconfig defaults and,
 *            later, a14's RPC);
 *   start /
 *   cancel   tap anywhere on the page -> a small modal dialog: the
 *            question ("Start focus session?" / "Cancel the session?"),
 *            the relevant facts (config summary / phase + time left),
 *            one primary action button, and a "close" ghost for a
 *            change of mind. Tapping outside the card closes too.
 *            (Earlier iterations — press-hold the centre disc, and a
 *            ghost Cancel button — were retired the same day: the
 *            hold mis-fired and the tiny word was too easy to miss.)
 *   work     countdown centred (baijamjuree 144, the standalone-clock
 *            hero), caption FOCUS #n above it, segmented session bar
 *            along the bottom.
 *   rest     fusion layout (like the clock page): left = stacked
 *            MM-over-SS countdown on the breathing disc; right = the
 *            quote; bar stays. (pad10a hid the bar in rest — tab1a
 *            keeps it, owner spec 2026-11-22.)
 *
 * The bottom bar is the whole session, not the running phase: one
 * segment per block (work/rest alternating), widths proportional to
 * their durations — pad10a computed those widths (pomo_work_set_width/
 * pomo_rest_set_width) but never drew them. Blocks are whisper-white
 * 10 % pills; work fills accent, rest a lighter accent, the running
 * block fills live.
 *
 * The engine (zmk/pomodoro.h) is untouched: it owns timing, the blank
 * lock and the notify seam; this screen renders zmk_pomo_get_status()
 * at 1 Hz (skip-if-unchanged, ~61 ms full-frame flush per invalidation)
 * and breathes from a 100 ms page timer, which stops with the page — a
 * hidden page flushes nothing.
 *
 * Interaction coexistence: the whole-page tap uses the shared detector
 * (< 24 px press-release, so swipes still navigate). While the dialog
 * is up, taps landing on the scrim only close it; the tap detector
 * still sees the same press, so a re-open guard (dialog state + a
 * short post-close quiet window) keeps a dismiss from re-opening it.
 */

#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/input/input.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <lvgl.h>

#include <zmk/display.h>
#include <zmk/display/screens.h>
#include <zmk/display/time_src.h>
#include <zmk/pomodoro.h>

#include "styles.h"
#include "ui_helpers.h"

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define PAGE_NAME "pomodoro"

/* Breath disc: pad10a breathed 28..64 px on its 172-px-tall glass
 * (screen_pomodoro.c:328, ~16..37 % of height). Scale-preserving port
 * for 320 tall: 56..120 px (17.5..37.5 %). */
#define BREATH_MIN 56
#define BREATH_MAX 120
#define BREATH_PERIOD_MS 7000 /* 3 in / 0.5 hold / 3 out / 0.5 hold (:327) */
#define BREATH_TICK_MS 100

/* Segmented session bar (bottom edge) */
#define BAR_H 10    /* pill height (radius follows)                       */
#define SEG_GAP 4   /* air between pills                                  */
#define SEG_MAX (2 * 8 - 1) /* sets clamp at 8 -> 15 blocks               */

/* Work layout: gap between the FOCUS caption and the hero digits
 * (owner pass 2026-11-23: 16 px read too tight against the 144 px
 * numerals — 32 px, and it follows the digits' box, not the page). */
#define CAP_ABOVE_COUNTDOWN (ZMK_GRID_UNIT * 4)

/* Rest fusion split */
#define QUOTE_W_IDLE (ZMK_GRID_SCREEN_W - ZMK_GRID_UNIT * 10)
#define QUOTE_W_REST 280 /* right zone reach — clear of the stacked
                          * digits (owner balance pass) */
#define STACK_CX (ZMK_GRID_SCREEN_W / 4 - ZMK_GRID_UNIT * 2) /* left-zone centre:
                          * 16 px left of true quarter — the wide quote
                          * owns more of the page */

/* Re-open guard: a tap that dismissed the dialog must not immediately
 * re-open it (the shared tap detector sees the same press as the
 * scrim/button handlers). */
#define DIALOG_QUIET_MS 600

/* Phase ids for the shown-phase cache (mirrors zmk_pomo_state) */
#define PHASE_DIRTY (-1)

struct pomo_ui {
    lv_obj_t *root;
    lv_obj_t *breath;
    lv_obj_t *quote;
    lv_obj_t *countdown;   /* work: MM:SS one line, centred */
    lv_obj_t *caption;     /* work: FOCUS #n above it       */
    lv_obj_t *stack;       /* rest: MM-over-SS column, left zone */
    lv_obj_t *mm;
    lv_obj_t *ss;
    lv_obj_t *seg_row;
    lv_obj_t *seg_objs[SEG_MAX];
    lv_obj_t *seg_fills[SEG_MAX];
    int seg_widths[SEG_MAX];
    int seg_fill_cache[SEG_MAX];
    int seg_count;
    uint8_t seg_cfg_sets;   /* rebuild key */
    uint16_t seg_cfg_work;
    uint16_t seg_cfg_rest;
    /* confirm dialog (NULL when closed) */
    lv_obj_t *dialog;
    lv_obj_t *dlg_question;
    lv_obj_t *dlg_info;
    lv_obj_t *dlg_primary;
    lv_obj_t *dlg_primary_lbl;
    /* last-drawn state: a tick that changes nothing invalidates nothing */
    char time_cache[8];
    char mm_cache[4];
    char ss_cache[4];
    char caption_cache[16];
    char quote_cache[160];
    char dlg_q_cache[32];
    char dlg_i_cache[48];
    char dlg_p_cache[8];
    int shown_phase;        /* PHASE_DIRTY until the first update */
    int cap_w_cache;        /* caption width-lock vs the countdown */
    int breath_vis_shown;   /* -1 = dirty */
    int breath_style_shown; /* -1 dirty, 0 BREATH, 1 BREATH_HOLD */
    uint32_t breath_size_shown;
};

static struct pomo_ui ui;
static int64_t dialog_closed_ms = -10000;

/* ------------------------------------------------------------------ */
/* Vietnamese quotes (verbatim from pad10a screen_pomodoro.c:414-463,  */
/* including its duplicated entries — the rotation order is the data)  */
/* ------------------------------------------------------------------ */

static const char *const quotes[] = {"Có công mài sắt\ncó ngày nên kim",
                                     "Kiến tha lâu\ncũng đầy tổ",
                                     "Có chí thì nên",
                                     "Công thành\nchẳng quản lâu",
                                     "Học thầy không tày học bạn",
                                     "Đi một ngày đàng\nhọc một sàng khôn",
                                     "Uống nước\nnhớ nguồn",
                                     "Ăn quả\nnhớ kẻ trồng cây",
                                     "Đói cho sạch\nrách cho thơm",
                                     "Tốt gỗ\nhơn tốt nước sơn",
                                     "Thất bại\nlà mẹ thành công",
                                     "Cây ngay\nkhông lo chết đứng",
                                     "Biết người biết ta\ntrăm trận trăm thắng",
                                     "Gần mực thì đen\ngần đèn thì sáng",
                                     "Chân cứng đá mềm",
                                     "Tiền nào của nấy",
                                     "Không thầy\nđố mày làm nên",
                                     "Học ăn, học nói\nhọc gói, học mở",
                                     "Hữu chí\ncánh thành",
                                     "Lửa thử vàng\ngian nan thử sức",
                                     "Thua keo này\nta bày keo khác",
                                     "Được mùa chớ phụ ngô khoai",
                                     "Giấy rách\nphải giữ lấy lề",
                                     "Đèn nhà ai nấy rạng",
                                     "Ở hiền gặp lành",
                                     "Có chí làm quan\ncó gan làm giàu",
                                     "Còn nước, còn tát",
                                     "Trời sinh voi\ntrời sinh cỏ",
                                     "Nước chảy đá mòn",
                                     "Cái khó\nló cái khôn",
                                     "Cần cù\nbù thông minh",
                                     "Ăn vóc học hay",
                                     "Học một biết mười",
                                     "Góp gió thành bão",
                                     "Học đi đôi với hành",
                                     "Tre già măng mọc",
                                     "Khó khăn\nthử thách lòng người",
                                     "Đói cho sạch\nrách cho thơm",
                                     "Không ai giàu ba họ\nkhông ai khó ba đời",
                                     "Trăng mờ còn tỏ\nngười khó còn khôn",
                                     "Có chí thì nên",
                                     "Mưa dầm thấm lâu",
                                     "Học thầy\nkhông tày học bạn",
                                     "Giận quá mất khôn",
                                     "Khéo ăn thì no\nkhéo co thì ấm",
                                     "Điều lành\nđem lại điều hay",
                                     "Ghét của nào\ntrời trao của nấy",
                                     "Không có việc gì khó\nchỉ sợ lòng không bền",
                                     "Không làm sao nên",
                                     "Một điều nhịn\nchín điều lành"};

/* Same rotation as pad10a get_current_quote() (:467-471): a new proverb
 * every 5 minutes, keyed to wall-clock seconds so pages agree. The time
 * seam (a09) always answers — the sim epoch stands in until h02/a14. */
static size_t quote_index_now(void) {
    int64_t epoch = zmk_time_now_epoch(NULL);
    uint64_t quintet;

    if (epoch > 0) {
        quintet = (uint64_t)epoch / 300;
    } else {
        quintet = (uint64_t)k_uptime_get() / 300000;
    }
    return (size_t)(quintet % ARRAY_SIZE(quotes));
}

/* ------------------------------------------------------------------ */
/* Breath waveform (LVGL thread, 100 ms tick)                          */
/* ------------------------------------------------------------------ */

static float smoothstep_f(float x) {
    x = CLAMP(x, 0.0f, 1.0f);
    return x * x * (3.0f - 2.0f * x);
}

/* pad10a breathing_wave() (:285-315) in integer-ms phase math: smooth
 * -1..+1 with quarter-second holds at the extremes. The +/-2 % realism
 * wobble on the holds is dropped — imperceptible, and it skips libm. */
static uint32_t breath_size_at(int64_t now_ms) {
    uint32_t ph = (uint32_t)(now_ms % BREATH_PERIOD_MS);
    float norm;

    if (ph < 3000) {
        norm = -1.0f + 2.0f * smoothstep_f(ph / 3000.0f);
    } else if (ph < 3500) {
        norm = 1.0f;
    } else if (ph < 6500) {
        norm = 1.0f - 2.0f * smoothstep_f((float)(ph - 3500) / 3000.0f);
    } else {
        norm = -1.0f;
    }

    return BREATH_MIN + (uint32_t)(((norm + 1.0f) * 0.5f) * (BREATH_MAX - BREATH_MIN) + 0.5f);
}

static void breath_apply(bool vis, uint32_t size) {
    if (ui.breath_vis_shown != (int)vis) {
        lv_obj_set_flag(ui.breath, LV_OBJ_FLAG_HIDDEN, !vis);
        ui.breath_vis_shown = vis;
    }
    if (vis && ui.breath_size_shown != size) {
        lv_obj_set_size(ui.breath, size, size);
        ui.breath_size_shown = size;
    }
}

static void pomo_anim_tick(void) {
    struct zmk_pomo_status s;

    zmk_pomo_get_status(&s);

    if (s.state == ZMK_POMO_STATE_IDLE || s.state == ZMK_POMO_STATE_REST) {
        breath_apply(true, breath_size_at(k_uptime_get()));
    } else {
        breath_apply(false, BREATH_MIN); /* work: no disc — focus is the only story */
    }
}

/* ------------------------------------------------------------------ */
/* Segmented session bar                                               */
/* ------------------------------------------------------------------ */

/* Rebuild when the config changes (config is locked while running, so
 * this only ever happens while the row is invisible or just started). */
static void segs_rebuild(void) {
    const struct zmk_pomo_config *c = zmk_pomo_config_get();

    if (c->sets == ui.seg_cfg_sets && c->work_s == ui.seg_cfg_work &&
        c->rest_s == ui.seg_cfg_rest && ui.seg_count > 0) {
        return;
    }
    ui.seg_cfg_sets = c->sets;
    ui.seg_cfg_work = c->work_s;
    ui.seg_cfg_rest = c->rest_s;

    lv_obj_clean(ui.seg_row);

    int nseg = 2 * (int)c->sets - 1;
    int usable = ZMK_GRID_SCREEN_W - SEG_GAP * (nseg - 1);
    uint32_t total_s = (uint32_t)c->work_s * c->sets + (uint32_t)c->rest_s * (c->sets - 1);
    int acc = 0; /* px handed out so far; the last block eats the remainder
                  * so pills + gaps span the panel width exactly */

    for (int i = 0; i < nseg && i < SEG_MAX; i++) {
        bool is_work = (i % 2) == 0;
        uint32_t dur_s = is_work ? c->work_s : c->rest_s;
        int wpx = (i == nseg - 1) ? usable - acc
                                  : (int)((int64_t)usable * dur_s / total_s);

        acc += wpx;

        lv_obj_t *seg = lv_obj_create(ui.seg_row);

        lv_obj_remove_style_all(seg);
        lv_obj_set_size(seg, wpx, BAR_H);
        lv_obj_add_style(seg, zmk_display_style(ZMK_STYLE_BAR_MAIN), 0);
        lv_obj_remove_flag(seg, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t *fill = lv_obj_create(seg); /* child of a non-flex parent: (0,0) */

        lv_obj_remove_style_all(fill);
        lv_obj_set_size(fill, 0, BAR_H);
        lv_obj_add_style(fill,
                         zmk_display_style(is_work ? ZMK_STYLE_BAR_IND : ZMK_STYLE_BAR_REST_IND),
                         0);
        lv_obj_remove_flag(fill, LV_OBJ_FLAG_CLICKABLE);

        ui.seg_objs[i] = seg;
        ui.seg_fills[i] = fill;
        ui.seg_widths[i] = wpx;
        ui.seg_fill_cache[i] = -1;
    }
    ui.seg_count = MIN(nseg, SEG_MAX);
}

/* Whole-session fill: blocks left of the running one are full, the
 * running block is partial. Engine segment order: work1, rest1, work2,
 * ... so work set n -> 2n-2, rest after set n -> 2n-1. */
static void segs_render(struct zmk_pomo_status *s, int64_t phase_elapsed_ms) {
    int idx = s->state == ZMK_POMO_STATE_WORK ? 2 * ((int)s->set_idx - 1)
                                              : 2 * (int)s->set_idx - 1;
    int64_t total = (int64_t)s->phase_total_s * 1000;
    int32_t permille = total > 0 ? (int32_t)((phase_elapsed_ms * 1000) / total) : 0;

    permille = CLAMP(permille, 0, 1000);

    for (int i = 0; i < ui.seg_count; i++) {
        int want = i < idx ? ui.seg_widths[i]
                           : (i == idx ? (int)((int64_t)ui.seg_widths[i] * permille / 1000) : 0);

        if (want != ui.seg_fill_cache[i]) {
            lv_obj_set_width(ui.seg_fills[i], want);
            ui.seg_fill_cache[i] = want;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Confirm dialog (start / cancel)                                     */
/* ------------------------------------------------------------------ */

static void dialog_close(void) {
    if (ui.dialog != NULL) {
        lv_obj_del(ui.dialog);
        ui.dialog = NULL;
    }
    dialog_closed_ms = k_uptime_get();
}

static void dialog_primary_cb(lv_event_t *e) {
    ARG_UNUSED(e);
    if (zmk_pomo_running()) {
        zmk_pomo_cancel();
    } else {
        zmk_pomo_start();
    }
    dialog_close();
}

static void dialog_close_cb(lv_event_t *e) {
    ARG_UNUSED(e);
    dialog_close();
}

/* Scrim click: close only when the scrim itself was the click target
 * (button clicks bubble through it too). */
static void dialog_scrim_cb(lv_event_t *e) {
    if (lv_event_get_target(e) == lv_event_get_current_target(e)) {
        dialog_close();
    }
}

/* Dialog sizing (owner pass 2026-11-23): fixed card + button sizes so
 * the card never resizes between the idle and running questions.
 * Longest fact line ("4 sets · 25:00 focus · 5:00 rest") fits the
 * 288 px content width on one line. */
#define DLG_CARD_W (ZMK_GRID_SCREEN_W * 2 / 3) /* 320 px */
#define DLG_BTN_W  128
#define DLG_BTN_GAP (ZMK_GRID_UNIT * 2)

static lv_obj_t *mk_dialog_button(lv_obj_t *parent, const char *text, bool primary,
                                  lv_event_cb_t cb, lv_obj_t **label_out) {
    lv_obj_t *btn = lv_obj_create(parent);

    lv_obj_remove_style_all(btn);
    lv_obj_add_style(btn, zmk_display_style(ZMK_STYLE_TOUCH), 0);
    if (primary) {
        /* added second: registry order = later wins, so the fill goes
         * accent while TOUCH keeps the 44 px target and radius */
        lv_obj_add_style(btn, zmk_display_style(ZMK_STYLE_ACCENT), 0);
    }
    lv_obj_set_size(btn, DLG_BTN_W, ZMK_GRID_TOUCH_MIN);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl = zmk_ui_label(btn, primary ? ZMK_STYLE_TEXT_ON_ACCENT : ZMK_STYLE_TEXT_CAPTION);

    lv_label_set_text(lbl, text);
    lv_obj_center(lbl);
    if (label_out != NULL) {
        *label_out = lbl;
    }
    return btn;
}

/* Question / facts / primary label per engine state. */
static void dialog_fill(struct zmk_pomo_status *s) {
    const struct zmk_pomo_config *c = zmk_pomo_config_get();
    char q[32];
    char info[48];

    if (s->state == ZMK_POMO_STATE_IDLE) {
        snprintk(q, sizeof(q), "Start focus session?");
        snprintk(info, sizeof(info), "%u sets · %u:%02u focus · %u:%02u rest", c->sets,
                 c->work_s / 60, c->work_s % 60, c->rest_s / 60, c->rest_s % 60);
        zmk_ui_set_text(ui.dlg_p_cache, sizeof(ui.dlg_p_cache), ui.dlg_primary_lbl, "Start");
    } else {
        uint16_t secs = (uint16_t)((zmk_pomo_remaining_ms() + 999) / 1000);

        if (s->paused) {
            snprintk(q, sizeof(q), "Cancel the session?");
            snprintk(info, sizeof(info), "paused · set %u/%u", s->set_idx, s->sets);
        } else if (s->state == ZMK_POMO_STATE_WORK) {
            snprintk(q, sizeof(q), "Cancel the session?");
            snprintk(info, sizeof(info), "focus #%u · %u:%02u left", s->set_idx, secs / 60,
                     secs % 60);
        } else {
            snprintk(q, sizeof(q), "Cancel the session?");
            snprintk(info, sizeof(info), "rest · %u:%02u left", secs / 60, secs % 60);
        }
        zmk_ui_set_text(ui.dlg_p_cache, sizeof(ui.dlg_p_cache), ui.dlg_primary_lbl, "Cancel");
    }

    zmk_ui_set_text(ui.dlg_q_cache, sizeof(ui.dlg_q_cache), ui.dlg_question, q);
    zmk_ui_set_text(ui.dlg_i_cache, sizeof(ui.dlg_i_cache), ui.dlg_info, info);
}

static void dialog_show(void) {
    struct zmk_pomo_status s;

    if (ui.dialog != NULL || k_uptime_get() - dialog_closed_ms < DIALOG_QUIET_MS) {
        return; /* already open, or a dismissal's echo */
    }

    zmk_pomo_get_status(&s);

    ui.dialog = lv_obj_create(ui.root);
    lv_obj_remove_style_all(ui.dialog);
    lv_obj_add_style(ui.dialog, zmk_display_style(ZMK_STYLE_SCRIM), 0);
    lv_obj_set_size(ui.dialog, lv_pct(100), lv_pct(100));
    lv_obj_add_flag(ui.dialog, LV_OBJ_FLAG_CLICKABLE); /* swallows page taps */
    lv_obj_add_event_cb(ui.dialog, dialog_scrim_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *card = zmk_ui_flex(ui.dialog, LV_FLEX_FLOW_COLUMN, ZMK_GRID_UNIT);

    lv_obj_add_style(card, zmk_display_style(ZMK_STYLE_CARD), 0);
    lv_obj_set_width(card, DLG_CARD_W); /* fixed: same footprint idle vs running */
    lv_obj_center(card);

    ui.dlg_question = zmk_ui_label(card, ZMK_STYLE_TEXT_BODY);
    ui.dlg_info = zmk_ui_label(card, ZMK_STYLE_TEXT_CAPTION);

    lv_obj_t *row = zmk_ui_flex(card, LV_FLEX_FLOW_ROW, DLG_BTN_GAP);

    /* equal half-widths: 2*DLG_BTN_W + gap = 272 <= card content 288 */
    lv_obj_set_width(row, lv_pct(100));

    ui.dlg_primary = mk_dialog_button(row, "Start", true, dialog_primary_cb, &ui.dlg_primary_lbl);
    mk_dialog_button(row, "close", false, dialog_close_cb, NULL);

    /* fresh caches for this session of the dialog */
    ui.dlg_q_cache[0] = '\0';
    ui.dlg_i_cache[0] = '\0';
    ui.dlg_p_cache[0] = '\0';
    dialog_fill(&s);
}

ZMK_UI_WORK_DEFINE(pomo_tap_work, pomo_tap_work_cb) {
    ARG_UNUSED(work);
    dialog_show();
}

/* ------------------------------------------------------------------ */
/* 1 Hz refresh                                                        */
/* ------------------------------------------------------------------ */

/* Phase swap (idle/work/rest): widget sets + the moves that come with
 * them (quote re-centres, the disc re-homes). Rare enough — and cheap
 * enough — to redo the aligns wholesale. */
static void show_phase(int phase) {
    if (phase == ui.shown_phase) {
        return;
    }
    ui.shown_phase = phase;

    bool idle = phase == ZMK_POMO_STATE_IDLE;
    bool work = phase == ZMK_POMO_STATE_WORK;
    bool rest = phase == ZMK_POMO_STATE_REST;

    lv_obj_set_flag(ui.quote, LV_OBJ_FLAG_HIDDEN, !idle && !rest);
    if (idle || rest) {
        if (idle) {
            lv_obj_set_width(ui.quote, QUOTE_W_IDLE);
            lv_obj_center(ui.quote);
        } else {
            lv_obj_set_width(ui.quote, QUOTE_W_REST);
            lv_obj_align(ui.quote, LV_ALIGN_RIGHT_MID, -ZMK_GRID_UNIT * 2, 0);
        }
    }

    lv_obj_set_flag(ui.countdown, LV_OBJ_FLAG_HIDDEN, !work);
    lv_obj_set_flag(ui.caption, LV_OBJ_FLAG_HIDDEN, !work);
    lv_obj_set_flag(ui.stack, LV_OBJ_FLAG_HIDDEN, !rest);

    /* Disc home follows the layout: centre of the page idle, centre of
     * the left zone in rest — behind the stacked digits. */
    if (rest) {
        lv_obj_align(ui.breath, LV_ALIGN_CENTER, STACK_CX - ZMK_GRID_SCREEN_W / 2, 0);
    } else {
        lv_obj_center(ui.breath);
    }

    lv_obj_set_flag(ui.seg_row, LV_OBJ_FLAG_HIDDEN, idle);

    if (!idle) {
        segs_rebuild();
    }

    /* force label refresh after re-show */
    ui.time_cache[0] = '\0';
    ui.mm_cache[0] = '\0';
    ui.ss_cache[0] = '\0';
    ui.caption_cache[0] = '\0';
    ui.cap_w_cache = -1;
}

static void pomo_update(void) {
    struct zmk_pomo_status s;
    char buf[48];

    zmk_pomo_get_status(&s);

    show_phase(s.state);

    if (s.state != ZMK_POMO_STATE_IDLE) {
        int64_t remaining = zmk_pomo_remaining_ms();
        uint16_t secs = (uint16_t)((remaining + 999) / 1000);
        int64_t elapsed = (s.paused ? s.pause_at_ms : k_uptime_get()) - s.phase_start_ms;

        if (s.state == ZMK_POMO_STATE_WORK) {
            snprintk(buf, sizeof(buf), "%02u:%02u", secs / 60, secs % 60);
            zmk_ui_set_text(ui.time_cache, sizeof(ui.time_cache), ui.countdown, buf);

            if (s.paused) {
                zmk_ui_set_text(ui.caption_cache, sizeof(ui.caption_cache), ui.caption, "paused");
            } else {
                /* set_idx is 1-based per work set (engine rest->work bump) */
                snprintk(buf, sizeof(buf), "FOCUS #%u", s.set_idx);
                zmk_ui_set_text(ui.caption_cache, sizeof(ui.caption_cache), ui.caption, buf);
            }

            /* Caption stays width-locked to the digits: re-align only
             * when the (proportional) MM:SS box width changes. */
            int cw = lv_obj_get_width(ui.countdown);

            if (cw != ui.cap_w_cache) {
                ui.cap_w_cache = cw;
                lv_obj_set_width(ui.caption, cw);
                lv_obj_align_to(ui.caption, ui.countdown, LV_ALIGN_TOP_MID, 0,
                                -CAP_ABOVE_COUNTDOWN);
            }
        } else {
            snprintk(buf, sizeof(buf), "%02u", secs / 60);
            zmk_ui_set_text(ui.mm_cache, sizeof(ui.mm_cache), ui.mm, buf);
            snprintk(buf, sizeof(buf), "%02u", secs % 60);
            zmk_ui_set_text(ui.ss_cache, sizeof(ui.ss_cache), ui.ss, buf);
        }

        segs_render(&s, elapsed);
    }

    /* Quote lives in idle AND rest: keep it rotating (cached, so a
     * resting second changes nothing unless the 5-min index rolled). */
    if (s.state != ZMK_POMO_STATE_WORK) {
        zmk_ui_set_text(ui.quote_cache, sizeof(ui.quote_cache), ui.quote,
                        quotes[quote_index_now()]);
    }

    /* Open dialog keeps its facts live. */
    if (ui.dialog != NULL) {
        dialog_fill(&s);
    }
}

static void pomo_tick(void) {
    static uint32_t samples, cycle_sum;
    uint32_t start = k_cycle_get_32();

    pomo_update();
    cycle_sum += k_cycle_get_32() - start;
    if (++samples >= 600) {
        LOG_INF("pomodoro update avg %u us over %u s",
                (unsigned int)k_cyc_to_us_floor32(cycle_sum / samples), samples);
        samples = cycle_sum = 0;
    }
}

/* ------------------------------------------------------------------ */
/* Touch: tap anywhere -> confirm dialog                               */
/* ------------------------------------------------------------------ */

/* Compile-time bound (see stats_screen.c): the global input callback can
 * fire long before this page is ever created. The shared detector
 * handles the current-page gate and the < 24 px tap test, so swipes
 * still page-navigate untouched. */
ZMK_UI_TAP_STATE(pomo_tap) = { .page_name = PAGE_NAME, .work = &pomo_tap_work };

static void pomo_touch_cb(struct input_event *evt, void *user_data) {
    ARG_UNUSED(user_data);
    zmk_ui_tap_input(&pomo_tap, evt);
}
INPUT_CALLBACK_DEFINE(NULL, pomo_touch_cb, NULL);

/* ------------------------------------------------------------------ */
/* Build                                                               */
/* ------------------------------------------------------------------ */

static lv_obj_t *pomo_build(lv_obj_t *parent) {
    ui = (struct pomo_ui){
        .breath_vis_shown = -1,
        .breath_style_shown = -1,
        .breath_size_shown = BREATH_MIN,
        .shown_phase = PHASE_DIRTY,
        .cap_w_cache = -1,
        .seg_count = 0,
    };

    ui.root = lv_obj_create(parent);
    lv_obj_remove_style_all(ui.root);
    lv_obj_set_size(ui.root, lv_pct(100), lv_pct(100));

    /* Painter's order: disc, then quote/digits on top. The disc is not
     * clickable — the tap detector reads raw input, and a clickable
     * here would make LVGL swallow the press from everything else. */
    ui.breath = lv_obj_create(ui.root);
    lv_obj_remove_style_all(ui.breath);
    lv_obj_set_size(ui.breath, BREATH_MIN, BREATH_MIN);
    lv_obj_add_style(ui.breath, zmk_display_style(ZMK_STYLE_POMO_BREATH), 0);
    lv_obj_remove_flag(ui.breath, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(ui.breath);

    ui.quote = zmk_ui_label(ui.root, ZMK_STYLE_TEXT_TITLE);
    lv_obj_set_width(ui.quote, QUOTE_W_IDLE);
    lv_label_set_long_mode(ui.quote, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(ui.quote, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(ui.quote);

    /* Work layout: the countdown owns the exact page centre (not a
     * group-box centre); the caption floats above it, width-locked to
     * the digits' box so it is truly centred on the number. */
    ui.countdown = zmk_ui_label(ui.root, ZMK_STYLE_TEXT_HERO_XL);

    lv_obj_center(ui.countdown);

    ui.caption = zmk_ui_label(ui.root, ZMK_STYLE_TEXT_CAPTION);
    lv_obj_set_style_text_align(ui.caption, LV_TEXT_ALIGN_CENTER, 0);

    /* Rest layout (fusion-style): MM over SS in the left zone, the disc
     * centred behind it by show_phase(). */
    ui.stack = zmk_ui_flex(ui.root, LV_FLEX_FLOW_COLUMN, ZMK_GRID_UNIT);

    lv_obj_align(ui.stack, LV_ALIGN_CENTER, STACK_CX - ZMK_GRID_SCREEN_W / 2, 0);

    ui.mm = zmk_ui_label(ui.stack, ZMK_STYLE_TEXT_HERO);
    ui.ss = zmk_ui_label(ui.stack, ZMK_STYLE_TEXT_HERO);

    /* Segmented session bar on the very bottom edge: whisper-white 10 %
     * pills, one per block, widths proportional to the block durations
     * (pad10a computed those but never drew them). Fills are child
     * objects so the 1 Hz render only moves the running one. */
    ui.seg_row = zmk_ui_flex(ui.root, LV_FLEX_FLOW_ROW, SEG_GAP);

    lv_obj_set_height(ui.seg_row, BAR_H);
    lv_obj_align(ui.seg_row, LV_ALIGN_BOTTOM_MID, 0, 0);

    pomo_update();
    zmk_ui_page_timer(PAGE_NAME, 1000, pomo_tick);
    zmk_ui_page_timer(PAGE_NAME, BREATH_TICK_MS, pomo_anim_tick);

    return parent;
}

static lv_obj_t *pomo_page_create(lv_obj_t *parent) { return pomo_build(parent); }

ZMK_DISPLAY_DEFINE_SCREEN(pomodoro, PAGE_NAME, pomo_page_create, 20);
