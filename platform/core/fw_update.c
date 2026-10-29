/* fw_update.c - see fw_update.h */
#include <math.h>
#include <stddef.h>
#include <string.h>
#include "fw_update.h"
#include "plat_sha256.h"
#include "plat_log.h"
#include "plat_port.h"
#include "plat_cmdq.h"
#include "bsp.h"
#include "param.h"
#include "plat_state.h"
#include "plat_clock.h"

bool (*fw_safe_check)(char *why, size_t cap);
#define LOCAL_WAIT_MS (5u * 60u * 1000u)

#define REC_MAGIC 0x52425746u      /* "FWBR" */
#define WBUF 256u

typedef struct {
    uint32_t magic, gen, state, trial_boots;
    char     from[16], to[16];
    uint32_t t_act;            /* when the restart onto "to" was made (0 = clock not set) */
    uint8_t  src, level, clk, rsvd;
    uint32_t crc;
} boot_rec_t;
_Static_assert(sizeof(boot_rec_t) <= 64u, "boot record slot");

#define HIST_MAGIC 0x48425746u     /* "FWBH" */
#define HIST_OFF   256u            /* from fram_addr */
typedef struct {
    uint32_t  magic, gen;
    uint16_t  n, rsvd;
    fw_hist_t e[FW_HIST_MAX];      /* e[0] newest */
    uint32_t  crc;
} hist_t;
#define HIST_SLOT 232u
_Static_assert(sizeof(hist_t) <= HIST_SLOT, "history slot");

static struct {
    fw_flash_t *fl;
    nvm_dev_t  *fram;
    uint32_t    fram_addr;
    char        running[24];
    boot_rec_t  rec;
    hist_t      hist;
    bool        hist_pending;      /* a rollback to record once the clock is readable */
    uint32_t    boot_ms;
    uint8_t     act_src, act_level;
    /* upload */
    bool        uploading;
    uint32_t    total, received;
    uint8_t     hdr_raw[FW_HDR_SIZE];
    fw_header_t hdr;
    bool        hdr_ok;
    sha256_t    sha;
    uint8_t     wbuf[WBUF];
    uint32_t    wn, woff, next_erase;
    /* staged image in the free bank */
    bool        staged;
    fw_header_t staged_hdr;
    /* trial */
    uint32_t    healthy_since;
    bool        healthy_run;
    uint32_t    trial_left;
    bool        rolled_back;
    volatile bool activate_req;
    volatile bool wait_local;
    uint8_t  wait_src, wait_level;
    uint32_t wait_until;
    bool     wait_started;
    char     not_safe[48];
    fw_err_t    last_err;
} F;

void (*fw_before_swap)(void);
uint32_t fw_trial_ms = FW_TRIAL_MS;       /* the store task flushes the parameter store and the log */


static uint32_t min_version(void) {
    plat_val_t v;
    return param_read(PARAM_ID_FW_MIN_VERSION, &v) ? v.u : 0u;
}

static bool safe(void) {
    F.not_safe[0] = 0;
    char why[48] = "";
    const char *w;
    if (!plat_state_allows(PLAT_ACT_FW_RESTART)) w = plat_state_refusal();     /* CALIBRATION (section 7) */
    else if (!fw_safe_check || fw_safe_check(why, sizeof why)) return true;
    else w = why[0] ? why : "the application is busy";
    size_t k = 0;
    for (; w[k] && k + 1 < sizeof F.not_safe; ++k) F.not_safe[k] = w[k];
    F.not_safe[k] = 0;
    return false;
}

const char *fw_err_name(fw_err_t e) {
    static const char *const n[] = { "ok", "busy", "state", "not a firmware image", "wrong product", "size",
                                     "flash error", "checksum mismatch", "signature not valid", "no image",
                                     "not safe to restart now", "older than the minimum version allowed",
                                     "waiting for confirmation on the touchscreen",
                                     "built for another board revision" };
    return (unsigned)e < sizeof n / sizeof n[0] ? n[e] : "error";
}

uint32_t fw_parse_version(const char *s) {
    uint32_t part[3] = { 0, 0, 0 };
    for (unsigned k = 0; k < 3u && s && *s; ++k) {
        while (*s >= '0' && *s <= '9') { part[k] = part[k] * 10u + (uint32_t)(*s - '0'); s++; }
        if (*s != '.') break;
        s++;
    }
    return (part[0] & 0xFFu) << 16 | (part[1] & 0xFFu) << 8 | (part[2] & 0xFFu);
}

bool fw_header_ok(const fw_header_t *h) {
    return h->magic == FW_MAGIC && h->hdr_size == FW_HDR_SIZE && (h->hdr_version == 1u || h->hdr_version == 2u) &&
           h->hdr_crc == plat_crc32(0, h, (uint32_t)offsetof(fw_header_t, hdr_crc));
}

static void copy_ver(char *dst, size_t cap, const char *src) {
    size_t k = 0;
    if (src) for (; k + 1 < cap && src[k]; ++k) dst[k] = src[k];
    dst[k] = 0;
    for (size_t j = k; j < cap; ++j) dst[j] = 0;
}

/* ---------------- boot record (FRAM, two copies) ---------------- */
static void rec_save(void) {
    if (!F.fram) return;
    F.rec.magic = REC_MAGIC;
    F.rec.gen++;
    F.rec.crc = plat_crc32(0, &F.rec, (uint32_t)offsetof(boot_rec_t, crc));
    F.fram->write(F.fram, F.fram_addr + (F.rec.gen & 1u) * 64u, &F.rec, sizeof F.rec);
}

static void rec_load(void) {
    memset(&F.rec, 0, sizeof F.rec);
    if (!F.fram) return;
    bool have = false;
    for (unsigned c = 0; c < 2u; ++c) {
        boot_rec_t r;
        if (F.fram->read(F.fram, F.fram_addr + c * 64u, &r, sizeof r) != 0) continue;
        if (r.magic != REC_MAGIC || r.crc != plat_crc32(0, &r, (uint32_t)offsetof(boot_rec_t, crc))) continue;
        if (r.state > FW_ST_ROLLED_BACK) continue;
        if (!have || (int32_t)(r.gen - F.rec.gen) > 0) { F.rec = r; have = true; }
    }
}

/* ---------------- the free bank ---------------- */
static uint32_t trailer_off(void) { return F.fl->bank_size - F.fl->sector; }
static uint32_t img_off(void) { return F.fl->image_off; }               /* after the bootloader */
static uint32_t img_max(void) { return trailer_off() - img_off(); }
static void trial_mark(bool on) { if (F.fl && F.fl->trial_mark) F.fl->trial_mark(F.fl, on); }

static bool read_trailer(fw_header_t *h) {
    return F.fl && F.fl->read(F.fl, false, trailer_off(), h, sizeof *h) == 0 && fw_header_ok(h) &&
           h->image_size <= img_max();
}

/* SHA-256 of the image in the free bank, read back from the flash */
static bool bank_hash_ok(const fw_header_t *h) {
    sha256_t s;
    uint8_t buf[WBUF], dig[32];
    sha256_init(&s);
    for (uint32_t off = 0; off < h->image_size; off += WBUF) {
        uint32_t n = h->image_size - off < WBUF ? h->image_size - off : WBUF;
        if (F.fl->read(F.fl, false, img_off() + off, buf, n) != 0) return false;
        sha256_update(&s, buf, n);
    }
    sha256_final(&s, dig);
    return memcmp(dig, h->sha256, 32) == 0;
}

/* ---------------- the running image (start-up self-test, CIP-11.1) ---------------- */
static struct {
    int         result;            /* fw_running_check's answer once known (0 = not yet) */
    bool        busy;              /* hashing */
    fw_header_t h;
    sha256_t    sha;
    uint32_t    off;
} RC;

int fw_running_check(uint32_t max_bytes) {
    if (RC.result) return RC.result;
    if (!RC.busy) {
        /* the header an update left in this bank's last sector; none (programmed by the
         * debugger) or one for another version (the debugger wrote over an updated bank)
         * leaves nothing to check against */
        if (!F.fl || F.fl->read(F.fl, true, trailer_off(), &RC.h, sizeof RC.h) != 0 || !fw_header_ok(&RC.h) ||
            RC.h.image_size > img_max() || strncmp(RC.h.version_str, F.running, sizeof RC.h.version_str) != 0)
            return RC.result = -2;
        sha256_init(&RC.sha);
        RC.off = 0;
        RC.busy = true;
    }
    uint8_t buf[WBUF];
    for (uint32_t done = 0; RC.off < RC.h.image_size && done < max_bytes; done += WBUF) {
        uint32_t n = RC.h.image_size - RC.off < WBUF ? RC.h.image_size - RC.off : WBUF;
        if (F.fl->read(F.fl, true, img_off() + RC.off, buf, n) != 0) return RC.result = -1;
        sha256_update(&RC.sha, buf, n);
        RC.off += n;
    }
    if (RC.off < RC.h.image_size) return 0;
    uint8_t dig[32];
    sha256_final(&RC.sha, dig);
    RC.busy = false;
    return RC.result = memcmp(dig, RC.h.sha256, 32) == 0 ? 1 : -1;
}

/* ---------------- update history ---------------- */
static void hist_save(void) {
    if (!F.fram) return;
    F.hist.magic = HIST_MAGIC;
    F.hist.gen++;
    F.hist.crc = plat_crc32(0, &F.hist, (uint32_t)offsetof(hist_t, crc));
    F.fram->write(F.fram, F.fram_addr + HIST_OFF + (F.hist.gen & 1u) * HIST_SLOT, &F.hist, sizeof F.hist);
}

static void hist_load(void) {
    memset(&F.hist, 0, sizeof F.hist);
    if (!F.fram) return;
    bool have = false;
    for (unsigned c = 0; c < 2u; ++c) {
        hist_t h;
        if (F.fram->read(F.fram, F.fram_addr + HIST_OFF + c * HIST_SLOT, &h, sizeof h) != 0) continue;
        if (h.magic != HIST_MAGIC || h.n > FW_HIST_MAX || h.crc != plat_crc32(0, &h, (uint32_t)offsetof(hist_t, crc))) continue;
        if (!have || (int32_t)(h.gen - F.hist.gen) > 0) { F.hist = h; have = true; }
    }
}

static void hist_add(uint8_t kind) {
    fw_hist_t e;
    memset(&e, 0, sizeof e);
    e.kind = kind;
    e.t_installed = F.rec.t_act;
    if (F.rec.clk) e.flags |= FWH_INST_CLOCK;
    if (plat_clock_now(&e.t_done)) e.flags |= FWH_DONE_CLOCK;
    else e.t_done = 0;
    e.src = F.rec.src; e.level = F.rec.level;
    copy_ver(e.from, sizeof e.from, F.rec.from);
    copy_ver(e.to, sizeof e.to, F.rec.to);
    memmove(&F.hist.e[1], &F.hist.e[0], (FW_HIST_MAX - 1u) * sizeof e);
    F.hist.e[0] = e;
    if (F.hist.n < FW_HIST_MAX) F.hist.n++;
    hist_save();
}

size_t fw_history(fw_hist_t *out, size_t max) {
    size_t n = F.hist.n < max ? F.hist.n : max;
    PLAT_CRIT_ENTER();
    memcpy(out, F.hist.e, n * sizeof *out);
    PLAT_CRIT_EXIT();
    return n;
}

#ifndef PLAT_BUILD_ID
#define PLAT_BUILD_ID "dev"
#endif
uint16_t plat_board_rev_tag = 0xFFFFu;          /* parameter 319 */
const char *fw_build_id(void) { return PLAT_BUILD_ID; }
int fw_board_rev(void) { return plat_board_rev_tag == 0xFFFFu ? -1 : (int)plat_board_rev_tag; }

/* does the image's header allow this board? (v1 images state no range) */
static bool hw_fits(const fw_header_t *h) {
    int rev = fw_board_rev();
    return h->hdr_version < 2u || rev < 0 || (rev >= h->hw_min && rev <= h->hw_max);
}

void fw_init(fw_flash_t *flash, nvm_dev_t *fram, uint32_t fram_addr, const char *running_version) {
    memset(&F, 0, sizeof F);
    uint8_t rev;
    plat_board_rev_tag = bsp_hw_straps(&rev) ? rev : 0xFFFFu;
    F.fl = flash;
    F.fram = fram && fram->persistent ? fram : NULL;
    F.fram_addr = fram_addr;
    copy_ver(F.running, sizeof F.running, running_version);
    rec_load();
    hist_load();
    F.staged = read_trailer(&F.staged_hdr);
}

void fw_boot(uint8_t cause, uint32_t now_ms) {
    /* The bootloader (boot.h) rolled back on its own: the record still says the new image is
     * pending or on trial, but the previous one is running. Swapping "back" now would start
     * the image that just failed, so take it as a finished rollback. (Not detectable when the
     * new image has the same version string as the old one.) */
    if ((F.rec.state == FW_ST_PENDING || F.rec.state == FW_ST_TRIAL) &&
        strncmp(F.running, F.rec.to, sizeof F.rec.to) != 0 && strncmp(F.running, F.rec.from, sizeof F.rec.from) == 0)
        F.rec.state = FW_ST_ROLLED_BACK;
    switch (F.rec.state) {
    case FW_ST_PENDING:                         /* first start of the new image */
        F.rec.state = FW_ST_TRIAL;
        F.rec.trial_boots = 1;
        rec_save();
        break;
    case FW_ST_TRIAL: {                         /* the trial image restarted before confirming */
        bool power = cause == BSP_RESET_POWER_ON || cause == BSP_RESET_BROWNOUT;
        if (power && F.rec.trial_boots < 2u) {  /* one power cut is not the image's fault */
            F.rec.trial_boots++;
            rec_save();
            break;
        }
        F.rec.state = FW_ST_ROLLED_BACK;
        rec_save();
        trial_mark(false);                      /* (or the bootloader would count the previous image's starts) */
        if (F.fl && F.fl->swap_and_reset) F.fl->swap_and_reset(F.fl);   /* back to the previous image */
        return;                                 /* (simulation only) */
    }
    case FW_ST_ROLLED_BACK:                     /* this is the previous image again */
        F.rolled_back = true;
        F.hist_pending = true;                  /* recorded once the clock has been read (fw_service) */
        plat_log_event(LOG_EV_FW_ROLLBACK, PLAT_SRC_INTERNAL, 0, 0, NAN, NAN, F.rec.to);
        F.rec.state = FW_ST_IDLE;
        rec_save();
        trial_mark(false);
        break;
    default:
        trial_mark(false);                      /* no trial: a stale mark must not roll this image back */
        break;
    }
    F.boot_ms = now_ms;
    F.healthy_since = now_ms;
    F.healthy_run = false;
    F.trial_left = fw_trial_ms;
}

/* ---------------- upload ---------------- */
static fw_err_t fail(fw_err_t e) {
    F.uploading = false;
    F.last_err = e;
    return e;
}

static fw_err_t flush_wbuf(bool final) {
    if (F.wn == 0) return FW_OK;
    uint32_t n = F.wn;
    if (final) { uint32_t u = F.fl->prog_unit; while (n % u) F.wbuf[n++] = 0xFF; }
    else if (n % F.fl->prog_unit) return FW_OK;              /* wait for a whole unit */
    for (uint32_t k = 0; k < n; ) {
        uint32_t off = img_off() + F.woff + k;
        if (off >= F.next_erase) {                           /* erase as the image grows */
            if (F.fl->erase(F.fl, F.next_erase) != 0) return FW_E_FLASH;
            F.next_erase += F.fl->sector;
        }
        uint32_t room = F.next_erase - off, m = n - k < room ? n - k : room;
        if (F.fl->write(F.fl, off, F.wbuf + k, m) != 0) return FW_E_FLASH;
        k += m;
    }
    F.woff += n;
    F.wn = 0;
    return FW_OK;
}

fw_err_t fw_upload_begin(uint32_t file_len) {
    if (!F.fl) return FW_E_STATE;
    if (F.uploading) return FW_E_BUSY;
    if (file_len <= FW_HDR_SIZE || file_len - FW_HDR_SIZE > img_max()) return F.last_err = FW_E_SIZE;
    /* the free bank no longer holds a valid image from here on */
    if (F.fl->erase(F.fl, trailer_off()) != 0) return F.last_err = FW_E_FLASH;
    F.staged = false;
    F.uploading = true;
    F.total = file_len;
    F.received = 0;
    F.hdr_ok = false;
    F.wn = F.woff = 0;
    F.next_erase = img_off();                                /* the bootloader's sectors are never touched */
    F.last_err = FW_OK;
    sha256_init(&F.sha);
    return FW_OK;
}

fw_err_t fw_upload_data(const uint8_t *p, uint32_t n) {
    if (!F.uploading) return FW_E_STATE;
    if (n > F.total - F.received) return fail(FW_E_SIZE);
    while (n) {
        if (F.received < FW_HDR_SIZE) {                       /* header */
            uint32_t m = FW_HDR_SIZE - F.received < n ? FW_HDR_SIZE - F.received : n;
            memcpy(F.hdr_raw + F.received, p, m);
            F.received += m; p += m; n -= m;
            if (F.received == FW_HDR_SIZE) {
                memcpy(&F.hdr, F.hdr_raw, sizeof F.hdr);
                if (!fw_header_ok(&F.hdr)) return fail(FW_E_HEADER);
                if (F.hdr.product != PLAT_FW_PRODUCT) return fail(FW_E_PRODUCT);
                if (F.hdr.version < min_version()) return fail(FW_E_OLD);
                if (!hw_fits(&F.hdr)) return fail(FW_E_HARDWARE);
                if (F.hdr.image_size != F.total - FW_HDR_SIZE || F.hdr.image_size > img_max()) return fail(FW_E_SIZE);
                F.hdr_ok = true;
            }
            continue;
        }
        uint32_t m = WBUF - F.wn < n ? WBUF - F.wn : n;     /* payload */
        memcpy(F.wbuf + F.wn, p, m);
        sha256_update(&F.sha, p, m);
        F.wn += m; F.received += m; p += m; n -= m;
        if (F.wn == WBUF) { fw_err_t e = flush_wbuf(false); if (e != FW_OK) return fail(e); }
    }
    return FW_OK;
}

fw_err_t fw_upload_end(void) {
    if (!F.uploading) return FW_E_STATE;
    if (F.received != F.total || !F.hdr_ok) return fail(FW_E_SIZE);
    fw_err_t e = flush_wbuf(true);
    if (e != FW_OK) return fail(e);
    uint8_t dig[32];
    sha256_final(&F.sha, dig);
    if (memcmp(dig, F.hdr.sha256, 32) != 0) return fail(FW_E_HASH);
    if (F.hdr.flags & FW_F_SIGNED) {
        sha256_t s; uint8_t h[32];
        sha256_init(&s); sha256_update(&s, &F.hdr, FW_SIGNED_LEN);
        if (F.hdr.hdr_version >= 2u)            /* v2 signs the fields after the signature too */
            sha256_update(&s, (const uint8_t *)&F.hdr + FW_SIGNED2_OFF, (uint32_t)offsetof(fw_header_t, hdr_crc) - FW_SIGNED2_OFF);
        sha256_final(&s, h);
        if (!fw_signature_check(h, F.hdr.sig)) return fail(FW_E_SIGNATURE);
    } else if (fw_signing_required()) {
        return fail(FW_E_SIGNATURE);
    }
    if (!bank_hash_ok(&F.hdr)) return fail(FW_E_FLASH);      /* what the flash actually holds */
    if (F.fl->write(F.fl, trailer_off(), &F.hdr, sizeof F.hdr) != 0) return fail(FW_E_FLASH);
    F.uploading = false;
    F.staged = read_trailer(&F.staged_hdr);
    if (!F.staged) return fail(FW_E_FLASH);
    return FW_OK;
}

void fw_upload_abort(void) { if (F.uploading) (void)fail(FW_E_STATE); }

/* ---------------- activation and trial ---------------- */
fw_err_t fw_request_activate(void) { return fw_request_activate_from(PLAT_SRC_LOCAL, PLAT_LVL_FACTORY); }

fw_err_t fw_request_activate_from(uint8_t src, uint8_t level) {
    if (F.uploading) return FW_E_BUSY;
    if (F.rec.state == FW_ST_TRIAL) return FW_E_STATE;       /* confirm or roll back first */
    if (!F.staged) return FW_E_NO_IMAGE;
    if (F.staged_hdr.version < min_version()) return FW_E_OLD;
    if (!hw_fits(&F.staged_hdr)) return FW_E_HARDWARE;
    if (!safe()) {
        plat_log_event(LOG_EV_FW_UPDATE, src, level, 0, NAN, NAN, "refused: not safe");
        return FW_E_NOT_SAFE;
    }
    plat_val_t v;
    bool need_local = param_read(PARAM_ID_FW_LOCAL_CONFIRM, &v) && v.b;
    if (need_local && src != PLAT_SRC_LOCAL) {
        F.wait_src = src; F.wait_level = level;
        F.wait_started = false;                             /* fw_service starts the 5 minutes */
        F.wait_local = true;
        plat_log_event(LOG_EV_FW_UPDATE, src, level, 0, NAN, NAN, "activate: ask local");
        return FW_E_WAIT_LOCAL;
    }
    F.wait_local = false;
    plat_log_event(LOG_EV_FW_UPDATE, src, level, 0, NAN, NAN, "activate");
    F.act_src = src; F.act_level = level;
    F.activate_req = true;
    return FW_OK;
}

void fw_local_answer(bool yes, uint8_t level) {
    if (!F.wait_local) return;
    F.wait_local = false;
    if (!yes) { plat_log_event(LOG_EV_FW_UPDATE, PLAT_SRC_LOCAL, level, 0, NAN, NAN, "refused on touchscreen"); return; }
    if (!F.staged || F.uploading || F.rec.state == FW_ST_TRIAL) return;
    if (!safe()) { plat_log_event(LOG_EV_FW_UPDATE, PLAT_SRC_LOCAL, level, 0, NAN, NAN, "refused: not safe"); return; }
    plat_log_event(LOG_EV_FW_UPDATE, PLAT_SRC_LOCAL, level, 0, NAN, NAN, "confirmed on touch");
    F.act_src = F.wait_src; F.act_level = F.wait_level;   /* asked from afar, confirmed here */
    F.activate_req = true;
}

void fw_service(uint32_t now_ms, bool healthy) {
    if (F.wait_local) {
        if (!F.wait_started) { F.wait_started = true; F.wait_until = now_ms + LOCAL_WAIT_MS; }
        else if ((int32_t)(now_ms - F.wait_until) >= 0) {
            F.wait_local = false;
            plat_log_event(LOG_EV_FW_UPDATE, PLAT_SRC_INTERNAL, 0, 0, NAN, NAN, "not confirmed: dropped");
        }
    }
    if (F.hist_pending) {                       /* a rollback: wait a moment for the clock (first scans) */
        uint32_t t;
        if (plat_clock_now(&t) || now_ms - F.boot_ms >= 3000u) { F.hist_pending = false; hist_add(FWH_ROLLED_BACK); }
    }
    if (F.activate_req) {
        F.activate_req = false;
        if (!safe()) {                                       /* things changed since the request */
            plat_log_event(LOG_EV_FW_UPDATE, PLAT_SRC_INTERNAL, 0, 0, NAN, NAN, "refused: not safe");
            F.last_err = FW_E_NOT_SAFE;
            return;
        }
        fw_header_t h;
        if (!read_trailer(&h) || !bank_hash_ok(&h)) {        /* never swap to a damaged bank */
            F.staged = false;
            F.last_err = FW_E_HASH;
            return;
        }
        F.rec.state = FW_ST_PENDING;
        F.rec.trial_boots = 0;
        copy_ver(F.rec.from, sizeof F.rec.from, F.running);
        copy_ver(F.rec.to, sizeof F.rec.to, h.version_str);
        F.rec.clk = plat_clock_now(&F.rec.t_act) ? 1u : 0u;
        if (!F.rec.clk) F.rec.t_act = 0;
        F.rec.src = F.act_src; F.rec.level = F.act_level;
        rec_save();
        if (fw_before_swap) fw_before_swap();
        trial_mark(true);                                    /* the bootloader counts its starts from here */
        if (F.fl->swap_and_reset) F.fl->swap_and_reset(F.fl);
        return;
    }
    if (F.rec.state != FW_ST_TRIAL) return;
    if (!healthy) { F.healthy_run = false; F.trial_left = fw_trial_ms; return; }
    if (!F.healthy_run) { F.healthy_run = true; F.healthy_since = now_ms; }
    uint32_t ran = now_ms - F.healthy_since;
    F.trial_left = ran >= fw_trial_ms ? 0 : fw_trial_ms - ran;
    if (F.trial_left == 0) {
        char txt[23];
        size_t k = 0;
        for (const char *p = F.rec.from; *p && k < 10u; ++p) txt[k++] = *p;
        txt[k++] = '>';
        for (const char *p = F.rec.to; *p && k < 22u; ++p) txt[k++] = *p;
        txt[k] = 0;
        plat_log_event(LOG_EV_FW_UPDATE, PLAT_SRC_INTERNAL, 0, fw_parse_version(F.rec.to), NAN, NAN, txt);
        F.rec.state = FW_ST_IDLE;
        rec_save();
        trial_mark(false);                                   /* confirmed: the bootloader stops counting */
        hist_add(FWH_UPDATED);
        F.staged = read_trailer(&F.staged_hdr);              /* the free bank now holds the previous image */
    }
}

void fw_status(fw_status_t *o) {
    memset(o, 0, sizeof *o);
    o->state = (fw_state_t)F.rec.state;
    o->uploading = F.uploading;
    o->upload_bytes = F.received;
    o->upload_total = F.total;
    o->staged = F.staged;
    if (F.staged) copy_ver(o->staged_version, sizeof o->staged_version, F.staged_hdr.version_str);
    o->staged_hw_min = 0; o->staged_hw_max = 255; o->staged_build[0] = 0;
    if (F.staged && F.staged_hdr.hdr_version >= 2u) {
        o->staged_hw_min = F.staged_hdr.hw_min; o->staged_hw_max = F.staged_hdr.hw_max;
        size_t k = 0;
        for (; k + 1 < sizeof o->staged_build && k < sizeof F.staged_hdr.build_id && F.staged_hdr.build_id[k]; ++k)
            o->staged_build[k] = F.staged_hdr.build_id[k];
        o->staged_build[k] = 0;
    }
    copy_ver(o->running_version, sizeof o->running_version, F.running);
    o->trial_left_ms = F.rec.state == FW_ST_TRIAL ? F.trial_left : 0;
    o->last_error = F.last_err;
    o->rolled_back = F.rolled_back;
    o->signing_required = fw_signing_required();
    o->wait_local = F.wait_local;
    o->wait_src = F.wait_src; o->wait_level = F.wait_level;
    o->wait_left_ms = F.wait_local && F.wait_started ? F.wait_until - bsp_ms() : (F.wait_local ? LOCAL_WAIT_MS : 0);
    memcpy(o->not_safe, F.not_safe, sizeof o->not_safe);
}

void fw_reset_for_test(void) { memset(&F, 0, sizeof F); memset(&RC, 0, sizeof RC); fw_before_swap = NULL; fw_safe_check = NULL; }
