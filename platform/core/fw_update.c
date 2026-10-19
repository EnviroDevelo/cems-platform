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

#define REC_MAGIC 0x52425746u      /* "FWBR" */
#define WBUF 256u

typedef struct {
    uint32_t magic, gen, state, trial_boots;
    char     from[16], to[16];
    uint32_t crc;
} boot_rec_t;

static struct {
    fw_flash_t *fl;
    nvm_dev_t  *fram;
    uint32_t    fram_addr;
    char        running[24];
    boot_rec_t  rec;
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
    fw_err_t    last_err;
} F;

void (*fw_before_swap)(void);
uint32_t fw_trial_ms = FW_TRIAL_MS;       /* the store task flushes the parameter store and the log */

__attribute__((weak)) bool fw_signature_check(const uint8_t hash[32], const uint8_t sig[64]) {
    (void)hash; (void)sig;
    return false;
}

const char *fw_err_name(fw_err_t e) {
    static const char *const n[] = { "ok", "busy", "state", "not a firmware image", "wrong product", "size",
                                     "flash error", "checksum mismatch", "signature", "no image" };
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
    return h->magic == FW_MAGIC && h->hdr_size == FW_HDR_SIZE && h->hdr_version == 1u &&
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

static bool read_trailer(fw_header_t *h) {
    return F.fl && F.fl->read(F.fl, false, trailer_off(), h, sizeof *h) == 0 && fw_header_ok(h) &&
           h->image_size <= trailer_off();
}

/* SHA-256 of the image in the free bank, read back from the flash */
static bool bank_hash_ok(const fw_header_t *h) {
    sha256_t s;
    uint8_t buf[WBUF], dig[32];
    sha256_init(&s);
    for (uint32_t off = 0; off < h->image_size; off += WBUF) {
        uint32_t n = h->image_size - off < WBUF ? h->image_size - off : WBUF;
        if (F.fl->read(F.fl, false, off, buf, n) != 0) return false;
        sha256_update(&s, buf, n);
    }
    sha256_final(&s, dig);
    return memcmp(dig, h->sha256, 32) == 0;
}

void fw_init(fw_flash_t *flash, nvm_dev_t *fram, uint32_t fram_addr, const char *running_version) {
    memset(&F, 0, sizeof F);
    F.fl = flash;
    F.fram = fram && fram->persistent ? fram : NULL;
    F.fram_addr = fram_addr;
    copy_ver(F.running, sizeof F.running, running_version);
    rec_load();
    F.staged = read_trailer(&F.staged_hdr);
}

void fw_boot(uint8_t cause, uint32_t now_ms) {
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
        if (F.fl && F.fl->swap_and_reset) F.fl->swap_and_reset(F.fl);   /* back to the previous image */
        return;                                 /* (simulation only) */
    }
    case FW_ST_ROLLED_BACK:                     /* this is the previous image again */
        F.rolled_back = true;
        plat_log_event(LOG_EV_FW_ROLLBACK, PLAT_SRC_INTERNAL, 0, 0, NAN, NAN, F.rec.to);
        F.rec.state = FW_ST_IDLE;
        rec_save();
        break;
    default:
        break;
    }
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
        uint32_t off = F.woff + k;
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
    if (file_len <= FW_HDR_SIZE || file_len - FW_HDR_SIZE > trailer_off()) return F.last_err = FW_E_SIZE;
    /* the free bank no longer holds a valid image from here on */
    if (F.fl->erase(F.fl, trailer_off()) != 0) return F.last_err = FW_E_FLASH;
    F.staged = false;
    F.uploading = true;
    F.total = file_len;
    F.received = 0;
    F.hdr_ok = false;
    F.wn = F.woff = F.next_erase = 0;
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
                if (F.hdr.image_size != F.total - FW_HDR_SIZE || F.hdr.image_size > trailer_off()) return fail(FW_E_SIZE);
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
        sha256_init(&s); sha256_update(&s, &F.hdr, FW_SIGNED_LEN); sha256_final(&s, h);
        if (!fw_signature_check(h, F.hdr.sig)) return fail(FW_E_SIGNATURE);
    } else if (PLAT_FW_REQUIRE_SIGNED) {
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
fw_err_t fw_request_activate(void) {
    if (F.uploading) return FW_E_BUSY;
    if (F.rec.state == FW_ST_TRIAL) return FW_E_STATE;       /* confirm or roll back first */
    if (!F.staged) return FW_E_NO_IMAGE;
    F.activate_req = true;
    return FW_OK;
}

void fw_service(uint32_t now_ms, bool healthy) {
    if (F.activate_req) {
        F.activate_req = false;
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
        rec_save();
        if (fw_before_swap) fw_before_swap();
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
    copy_ver(o->running_version, sizeof o->running_version, F.running);
    o->trial_left_ms = F.rec.state == FW_ST_TRIAL ? F.trial_left : 0;
    o->last_error = F.last_err;
    o->rolled_back = F.rolled_back;
}

void fw_reset_for_test(void) { memset(&F, 0, sizeof F); fw_before_swap = NULL; }
