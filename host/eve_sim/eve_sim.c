/* eve_sim.c - see eve_sim.h */
#include <string.h>
#include "eve_sim.h"
#include "zinflate.h"

#define RAM_G_SIZE (1u << 20)
static uint8_t ram_g_pool[RAM_G_SIZE];
static uint8_t zbuf_pool[256u * 1024u];

static uint32_t crc32_std(const uint8_t *p, uint32_t n) {       /* CRC-32 as CMD_MEMCRC computes it */
    uint32_t c = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < n; ++i) { c ^= p[i]; for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u))); }
    return ~c;
}

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static void wr16r(eve_sim_t *s, uint32_t a, uint16_t v) { s->regs[a - EVE_RAM_REG] = (uint8_t)v; s->regs[a - EVE_RAM_REG + 1] = (uint8_t)(v >> 8); }
static uint16_t rd16r(const eve_sim_t *s, uint32_t a) { return (uint16_t)(s->regs[a - EVE_RAM_REG] | (s->regs[a - EVE_RAM_REG + 1] << 8)); }

static void cmd_bytes(eve_sim_t *s, const uint8_t *p, uint32_t n);

/* commands that work outside a frame; true = the byte was taken here */
static bool out_of_frame(eve_sim_t *s, const uint8_t *p, uint32_t i, uint32_t n, uint32_t *used) {
    *used = 0;
    if (s->op == CMD_INFLATE && s->op_n == 1) {               /* collecting the zlib stream */
        uint32_t take = n - i;
        if (s->zn + take > sizeof zbuf_pool) take = (uint32_t)sizeof zbuf_pool - s->zn;
        memcpy(s->zbuf + s->zn, p + i, take);
        s->zn += take;
        *used = n - i;
        size_t in = 0, out = 0;
        int rc = zinflate(s->zbuf, s->zn, s->ram_g + s->op_args[0], RAM_G_SIZE - s->op_args[0], &in, &out);
        if (rc == 1 && s->zn < sizeof zbuf_pool) return true;  /* more to come */
        if (rc != 0) { s->inflate_errors++; s->op = 0; s->zn = 0; return true; }
        s->inflates++;
        uint32_t padded = ((uint32_t)in + 3u) & ~3u;
        s->op = 0;
        uint32_t extra = s->zn > padded ? s->zn - padded : 0;   /* bytes after the stream: the next commands */
        if (extra) { uint8_t tmp[4096]; uint32_t k = extra < sizeof tmp ? extra : (uint32_t)sizeof tmp;
                     memcpy(tmp, s->zbuf + padded, k); s->zn = 0; cmd_bytes(s, tmp, k); }
        s->zn = 0;
        return true;
    }
    return false;
}

static void cmd_bytes(eve_sim_t *s, const uint8_t *p, uint32_t n) {
    uint16_t w0 = rd16r(s, REG_CMD_WRITE);
    for (uint32_t i = 0; i < n; ++i) s->ram_cmd[(w0 + i) & 0xFFFu] = p[i];
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t used;
        if (out_of_frame(s, p, i, n, &used)) { i += used - 1; continue; }
        if (s->op) {                                           /* the words of an out-of-frame command */
            s->opw[s->opw_n++] = p[i];
            if (s->opw_n == 4) {
                s->op_args[s->op_n++] = rd32(s->opw); s->opw_n = 0;
                if (s->op == CMD_INFLATE && s->op_n == 1) { s->zn = 0; }
                if (s->op == CMD_MEMCRC && s->op_n == 3) {
                    uint32_t a = s->op_args[0], len = s->op_args[1];
                    uint32_t crc = a + len <= RAM_G_SIZE ? crc32_std(s->ram_g + a, len) : 0;
                    uint32_t at = (w0 + i - 3u) & 0xFFFu;      /* the result word just received */
                    for (int k = 0; k < 4; ++k) s->ram_cmd[(at + (uint32_t)k) & 0xFFFu] = (uint8_t)(crc >> (8 * k));
                    s->memcrcs++;
                    s->op = 0;
                }
            }
            continue;
        }
        if (s->cur_n == 0) {                                   /* a command word starts here */
            s->opw[s->opw_n++] = p[i];
            if (s->opw_n < 4) continue;
            uint32_t c = rd32(s->opw);
            s->opw_n = 0;
            if (c == CMD_INFLATE || c == CMD_MEMCRC) { s->op = c; s->op_n = 0; continue; }
            for (int k = 0; k < 4; ++k) s->cur[s->cur_n++] = (uint8_t)(c >> (8 * k));
            if (c == CMD_SWAP) { memcpy(s->frame, s->cur, s->cur_n); s->frame_n = s->cur_n; s->frames++; s->cur_n = 0; }
            continue;
        }
        if (s->cur_n < sizeof s->cur) s->cur[s->cur_n++] = p[i];
        if (s->cur_n % 4u == 0 && s->cur_n >= 4u && rd32(s->cur + s->cur_n - 4) == CMD_SWAP) {
            /* a whole frame (the stream is word-aligned, strings are padded) */
            memcpy(s->frame, s->cur, s->cur_n);
            s->frame_n = s->cur_n;
            s->frames++;
            s->cur_n = 0;
        }
    }
    uint16_t w = (uint16_t)((rd16r(s, REG_CMD_WRITE) + n) & 0xFFFu);
    wr16r(s, REG_CMD_WRITE, w);
    if (s->fault_next) {
        s->fault_next = false;
        wr16r(s, REG_CMD_READ, 0xFFF);
        strcpy((char *)s->err, "ERROR: test fault");
    } else if (rd16r(s, REG_CMD_READ) != 0xFFFu) {
        wr16r(s, REG_CMD_READ, w);                       /* executed at once */
    }
}

static void mem_write(eve_sim_t *s, uint32_t a, const uint8_t *p, uint32_t n) {
    if (a == REG_CMDB_WRITE) { cmd_bytes(s, p, n); return; }
    if (a < RAM_G_SIZE) { uint32_t k = a + n <= RAM_G_SIZE ? n : RAM_G_SIZE - a; memcpy(s->ram_g + a, p, k); return; }
    for (uint32_t i = 0; i < n; ++i, ++a) {
        if (a >= EVE_RAM_REG && a < EVE_RAM_REG + sizeof s->regs) {
            if (a == REG_CPURESET && (p[i] & 1u)) { s->cur_n = 0; s->op = 0; s->opw_n = 0; s->zn = 0; }
            s->regs[a - EVE_RAM_REG] = p[i];
        } else if (a == REG_COPRO_PATCH_PTR) s->patch_ptr = (uint16_t)((s->patch_ptr & 0xFF00u) | p[i]);
        else if (a == REG_COPRO_PATCH_PTR + 1) s->patch_ptr = (uint16_t)((s->patch_ptr & 0x00FFu) | (p[i] << 8));
    }
}

static void mem_read(eve_sim_t *s, uint32_t a, uint8_t *p, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i, ++a) {
        if (!s->present) { p[i] = 0; continue; }
        if (a >= REG_CMDB_SPACE && a < REG_CMDB_SPACE + 4) {
            uint16_t sp = rd16r(s, REG_CMD_READ) == 0xFFFu ? 0 : 4092;
            p[i] = (uint8_t)(a == REG_CMDB_SPACE ? sp : a == REG_CMDB_SPACE + 1 ? sp >> 8 : 0);
        } else if (a >= EVE_RAM_REG && a < EVE_RAM_REG + sizeof s->regs) p[i] = s->regs[a - EVE_RAM_REG];
        else if (a < RAM_G_SIZE) p[i] = s->ram_g[a];
        else if (a >= EVE_RAM_CMD && a < EVE_RAM_CMD + 4096u) p[i] = s->ram_cmd[a - EVE_RAM_CMD];
        else if (a >= EVE_RAM_ERR && a < EVE_RAM_ERR + sizeof s->err) p[i] = s->err[a - EVE_RAM_ERR];
        else if (a == REG_COPRO_PATCH_PTR) p[i] = (uint8_t)s->patch_ptr;
        else if (a == REG_COPRO_PATCH_PTR + 1) p[i] = (uint8_t)(s->patch_ptr >> 8);
        else p[i] = 0;
    }
}

static void b_select(eve_bus_t *b, bool on) {
    eve_sim_t *s = b->ctx;
    if (on) { s->cs = true; s->hn = 0; s->did_read = false; return; }
    /* host command: 01xxxxxx (CLKEXT, RST_PULSE ...) or ACTIVE (00 00 00) with no data read */
    if (s->cs && s->hn >= 3 && ((s->hdr[0] & 0xC0u) == 0x40u || ((s->hdr[0] & 0xC0u) == 0 && !s->did_read))) {
        s->host_cmds++;
        s->last_host_cmd = s->hdr[0];
    }
    s->cs = false;
}

static int b_write(eve_bus_t *b, const uint8_t *p, uint32_t n) {
    eve_sim_t *s = b->ctx;
    uint32_t i = 0;
    while (i < n && s->hn < 3) s->hdr[s->hn++] = p[i++];
    if (s->hn < 3) return 0;
    if (s->hn == 3) {                                   /* header complete: address and direction */
        s->addr = ((uint32_t)(s->hdr[0] & 0x3Fu) << 16) | ((uint32_t)s->hdr[1] << 8) | s->hdr[2];
        s->is_write = (s->hdr[0] & 0x80u) != 0;
        s->hn = 4;
    }
    if (!s->is_write || i >= n) return 0;               /* (a read's dummy byte is ignored) */
    mem_write(s, s->addr, p + i, n - i);
    if (s->addr != REG_CMDB_WRITE) s->addr += n - i;
    return 0;
}

static int b_read(eve_bus_t *b, uint8_t *p, uint32_t n) {
    eve_sim_t *s = b->ctx;
    s->did_read = true;
    mem_read(s, s->addr, p, n);
    s->addr += n;
    return 0;
}

static eve_sim_t *sim_now;                              /* the simulator in use (one at a time) */
static void b_delay(uint32_t ms) {                      /* time passes for REG_CLOCK at sysclk_hz */
    eve_sim_t *s = sim_now;
    if (!s || !s->sysclk_hz) return;
    uint32_t a = 0x302008u - EVE_RAM_REG, v = 0;
    for (int k = 0; k < 4; ++k) v |= (uint32_t)s->regs[a + k] << (8 * k);
    v += (uint32_t)((uint64_t)s->sysclk_hz * ms / 1000u);
    for (int k = 0; k < 4; ++k) s->regs[a + k] = (uint8_t)(v >> (8 * k));
}

void eve_sim_init(eve_sim_t *s) {
    memset(s, 0, sizeof *s);
    s->present = true;
    sim_now = s;
    s->ram_g = ram_g_pool;
    s->zbuf = zbuf_pool;
    memset(ram_g_pool, 0, sizeof ram_g_pool);
    s->regs[REG_ID - EVE_RAM_REG] = EVE_CHIP_ID;
    eve_sim_touch_xy(s, -32768, -32768);             /* no finger, as the chip reports it */
    static const uint8_t chip[4] = { 0x08, 0x17, 0x01, 0x00 };   /* BT817 chip ID in RAM_G after boot */
    memcpy(ram_g_pool + 0x0C0000u, chip, sizeof chip);
    s->bus.select = b_select; s->bus.write = b_write; s->bus.read = b_read;
    s->bus.delay_ms = b_delay; s->bus.ctx = s;
}

void eve_sim_touch(eve_sim_t *s, uint8_t tag) { s->regs[REG_TOUCH_TAG - EVE_RAM_REG] = tag; }
void eve_sim_touch_xy(eve_sim_t *s, int16_t x, int16_t y) {
    uint32_t v = (uint32_t)(uint16_t)x << 16 | (uint16_t)y;
    for (int k = 0; k < 4; ++k) s->regs[REG_TOUCH_SCREEN_XY - EVE_RAM_REG + k] = (uint8_t)(v >> (8 * k));
}

/* ---- frame walking ---- */
typedef void (*visit_fn)(void *ctx, uint32_t cmd, const uint8_t *args, const char *str, uint8_t tag);

static uint32_t str_words(const uint8_t *p, uint32_t max) {
    uint32_t k = 0;
    while (k < max && p[k]) k++;
    return (k + 4u) & ~3u;
}

static void walk(const eve_sim_t *s, visit_fn fn, void *ctx) {
    const uint8_t *f = s->frame;
    uint32_t n = s->frame_n, i = 0;
    uint8_t tag = 255;
    while (i + 4 <= n) {
        uint32_t c = rd32(f + i); i += 4;
        uint32_t fixed = 0;
        bool has_str = false;
        switch (c) {
        case CMD_TEXT: fixed = 8; has_str = true; break;
        case CMD_BUTTON: case CMD_KEYS: fixed = 12; has_str = true; break;
        case CMD_NUMBER: fixed = 12; break;
        case CMD_PROGRESS: fixed = 16; break;
        case CMD_SPINNER: fixed = 8; break;
        case CMD_FGCOLOR: case CMD_BGCOLOR: case CMD_GRADCOLOR: fixed = 4; break;
        case CMD_SETFONT2: fixed = 12; break;
        default:
            if ((c >> 24) == 3u) tag = (uint8_t)c;
            fn(ctx, c, NULL, NULL, tag);
            continue;
        }
        const uint8_t *args = f + i;
        i += fixed;
        const char *str = NULL;
        if (has_str && i < n) { str = (const char *)(f + i); i += str_words(f + i, n - i); }
        fn(ctx, c, args, str, tag);
    }
}

struct find { const char *needle; bool exact; bool found; int tag; };
static void v_find(void *ctx, uint32_t c, const uint8_t *a, const char *str, uint8_t tag) {
    struct find *q = ctx; (void)a;
    if (!str || q->found) return;
    if (q->exact ? strcmp(str, q->needle) == 0 : strstr(str, q->needle) != NULL) {
        q->found = true;
        q->tag = (c == CMD_BUTTON || c == CMD_TEXT) ? tag : -1;
    }
}
bool eve_sim_has_text(const eve_sim_t *s, const char *needle) {
    struct find q = { needle, false, false, -1 };
    walk(s, v_find, &q);
    return q.found;
}
int eve_sim_tag_of(const eve_sim_t *s, const char *label) {
    struct find q = { label, true, false, -1 };
    walk(s, v_find, &q);
    return q.found ? q.tag : -1;
}

void eve_sim_dump(const eve_sim_t *s, FILE *f) { fwrite(s->frame, 1, s->frame_n, f); }

static void v_print(void *ctx, uint32_t c, const uint8_t *a, const char *str, uint8_t tag) {
    FILE *f = ctx;
    if (str) {
        int16_t x = (int16_t)(a[0] | (a[1] << 8)), y = (int16_t)(a[2] | (a[3] << 8));
        fprintf(f, "  %s (%d,%d) tag %u \"%s\"\n", c == CMD_TEXT ? "text" : c == CMD_BUTTON ? "button" : "keys", x, y, tag, str);
    }
}
void eve_sim_print(const eve_sim_t *s, FILE *f) { walk(s, v_print, (void *)f); }
