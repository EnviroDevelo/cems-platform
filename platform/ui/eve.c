/* eve.c - see eve.h */
#include <string.h>
#include "eve.h"

/* Riverdi RVT70HSBNWC00-B: 1024x600 IPS. Timing from Riverdi's riverdi-eve-lib
 * (modules.h, IPS_70), which drives the same module. */
const eve_panel_t eve_panel_rvt70h = {
    .width = 1024, .height = 600,
    .hcycle = 1344, .hoffset = 160, .hsync0 = 0, .hsync1 = 70,
    .vcycle = 635, .voffset = 23, .vsync0 = 0, .vsync1 = 10,
    .pclk_freq = 0xD12, .pclk = 1, .pclk_pol = 1, .swizzle = 0, .cspread = 0, .dither = 0, .pclk_2x = 0,
    .ext_clock = true, .touch_config = 0,
};

/* ---------------- SPI framing ---------------- */
static void addr_w(uint8_t *h, uint32_t a) { h[0] = (uint8_t)(0x80u | ((a >> 16) & 0x3Fu)); h[1] = (uint8_t)(a >> 8); h[2] = (uint8_t)a; }

void eve_host_cmd(eve_t *e, uint8_t cmd, uint8_t param) {
    uint8_t h[3] = { cmd, param, 0 };
    e->bus->select(e->bus, true);
    e->bus->write(e->bus, h, 3);
    e->bus->select(e->bus, false);
}

void eve_wr_mem(eve_t *e, uint32_t a, const void *p, uint32_t n) {
    uint8_t h[3];
    addr_w(h, a);
    e->bus->select(e->bus, true);
    e->bus->write(e->bus, h, 3);
    if (n) e->bus->write(e->bus, p, n);
    e->bus->select(e->bus, false);
}

static void rd_mem(eve_t *e, uint32_t a, uint8_t *p, uint32_t n) {
    uint8_t h[4] = { (uint8_t)((a >> 16) & 0x3Fu), (uint8_t)(a >> 8), (uint8_t)a, 0 };   /* + dummy byte */
    e->bus->select(e->bus, true);
    e->bus->write(e->bus, h, 4);
    if (e->bus->read(e->bus, p, n) != 0) memset(p, 0, n);
    e->bus->select(e->bus, false);
}

uint8_t  eve_rd8(eve_t *e, uint32_t a)  { uint8_t b[1]; rd_mem(e, a, b, 1); return b[0]; }
uint16_t eve_rd16(eve_t *e, uint32_t a) { uint8_t b[2]; rd_mem(e, a, b, 2); return (uint16_t)(b[0] | (b[1] << 8)); }
uint32_t eve_rd32(eve_t *e, uint32_t a) {
    uint8_t b[4]; rd_mem(e, a, b, 4);
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}
void eve_wr8(eve_t *e, uint32_t a, uint8_t v)   { eve_wr_mem(e, a, &v, 1); }
void eve_wr16(eve_t *e, uint32_t a, uint16_t v) { uint8_t b[2] = { (uint8_t)v, (uint8_t)(v >> 8) }; eve_wr_mem(e, a, b, 2); }
void eve_wr32(eve_t *e, uint32_t a, uint32_t v) {
    uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
    eve_wr_mem(e, a, b, 4);
}

/* ---------------- start-up ---------------- */
int eve_init(eve_t *e, eve_bus_t *bus, const eve_panel_t *p) {
    memset(e, 0, sizeof *e);
    e->bus = bus;
    e->panel = p;
    if (bus->fast) bus->fast(bus, false);
    if (bus->power) { bus->power(bus, false); bus->delay_ms(20); bus->power(bus, true); bus->delay_ms(20); }
    eve_host_cmd(e, p->ext_clock ? EVE_HC_CLKEXT : EVE_HC_CLKINT, 0);
    eve_host_cmd(e, EVE_HC_ACTIVE, 0);
    bus->delay_ms(50);
    int k;
    for (k = 0; k < 100 && eve_rd8(e, REG_ID) != EVE_CHIP_ID; ++k) bus->delay_ms(10);
    if (k == 100) return -1;                                   /* nothing answering */
    for (k = 0; k < 100 && (eve_rd8(e, REG_CPURESET) & 7u) != 0u; ++k) bus->delay_ms(10);
    if (k == 100) return -2;
    eve_wr32(e, REG_FREQUENCY, 72000000u);                    /* as Riverdi's library does for this module */
    eve_wr16(e, REG_PWM_HZ, 4000);
    eve_wr8(e, REG_PWM_DUTY, 0);                               /* backlight off until the first frame */
    eve_wr16(e, REG_PCLK_FREQ, p->pclk_freq);
    eve_wr8(e, REG_PCLK_2X, p->pclk_2x);
    eve_wr16(e, REG_HCYCLE, p->hcycle);
    eve_wr16(e, REG_HOFFSET, p->hoffset);
    eve_wr16(e, REG_HSYNC0, p->hsync0);
    eve_wr16(e, REG_HSYNC1, p->hsync1);
    eve_wr16(e, REG_VCYCLE, p->vcycle);
    eve_wr16(e, REG_VOFFSET, p->voffset);
    eve_wr16(e, REG_VSYNC0, p->vsync0);
    eve_wr16(e, REG_VSYNC1, p->vsync1);
    eve_wr8(e, REG_SWIZZLE, p->swizzle);
    eve_wr8(e, REG_PCLK_POL, p->pclk_pol);
    eve_wr16(e, REG_HSIZE, p->width);
    eve_wr16(e, REG_VSIZE, p->height);
    eve_wr16(e, REG_CSPREAD, p->cspread);
    eve_wr16(e, REG_DITHER, p->dither);
    eve_wr16(e, REG_GPIOX_DIR, 0xFFFF);                        /* DISP enable and the rest as Riverdi sets them */
    eve_wr16(e, REG_GPIOX, 0xFFFF);
    if (p->touch_config) {                                     /* touch engine held in reset while configuring */
        eve_wr8(e, REG_CPURESET, 2);
        eve_wr16(e, REG_TOUCH_CONFIG, p->touch_config);
        eve_wr8(e, REG_CPURESET, 0);
    }
    eve_wr8(e, REG_TOUCH_MODE, 3);                             /* continuous */
    e->ok = true;
    eve_frame_begin(e, 0, 0, 0);
    if (eve_frame_end(e) != 0) { e->ok = false; return -2; }
    eve_wr8(e, REG_PCLK, p->pclk);                             /* the panel starts scanning */
    if (bus->fast) bus->fast(bus, true);
    return 0;
}

void eve_backlight(eve_t *e, uint8_t pct) {
    if (pct > 100) pct = 100;
    eve_wr8(e, REG_PWM_DUTY, (uint8_t)((pct * 128u) / 100u));
}

/* ---------------- frame building ---------------- */
static bool room(eve_t *e, uint32_t bytes) {
    if (e->n + bytes + 8u > EVE_CMDBUF) { e->overflow = true; return false; }   /* keep 8 for DISPLAY+SWAP */
    return true;
}
static void put32(eve_t *e, uint32_t w) {
    e->buf[e->n++] = (uint8_t)w; e->buf[e->n++] = (uint8_t)(w >> 8);
    e->buf[e->n++] = (uint8_t)(w >> 16); e->buf[e->n++] = (uint8_t)(w >> 24);
}
static void put16x2(eve_t *e, int16_t a, int16_t b) { put32(e, (uint32_t)(uint16_t)a | ((uint32_t)(uint16_t)b << 16)); }
static uint32_t strwords(const char *s) { return ((uint32_t)strlen(s) + 4u) & ~3u; }
static void putstr(eve_t *e, const char *s) {
    uint32_t len = (uint32_t)strlen(s), pad = strwords(s);
    memcpy(e->buf + e->n, s, len);
    memset(e->buf + e->n + len, 0, pad - len);
    e->n += pad;
}

void eve_frame_begin(eve_t *e, uint8_t r, uint8_t g, uint8_t b) {
    e->n = 0;
    e->overflow = false;
    put32(e, CMD_DLSTART);
    put32(e, DL_CLEAR_RGB(r, g, b));
    put32(e, DL_CLEAR(1, 1, 1));
    put32(e, DL_VERTEX_FORMAT(0));                             /* whole pixels */
}

void eve_cmd(eve_t *e, uint32_t w) { if (room(e, 4)) put32(e, w); }
void eve_cmd_str(eve_t *e, const char *s) { if (room(e, strwords(s))) putstr(e, s); }

static void cmd_xy_str(eve_t *e, uint32_t cmd, int16_t x, int16_t y, int16_t a, uint16_t b, const char *s) {
    if (!room(e, 12u + strwords(s))) return;
    put32(e, cmd); put16x2(e, x, y); put16x2(e, a, (int16_t)b); putstr(e, s);
}
static void cmd_box_str(eve_t *e, uint32_t cmd, int16_t x, int16_t y, int16_t w, int16_t h, int16_t f, uint16_t o, const char *s) {
    if (!room(e, 16u + strwords(s))) return;
    put32(e, cmd); put16x2(e, x, y); put16x2(e, w, h); put16x2(e, f, (int16_t)o); putstr(e, s);
}
void eve_text(eve_t *e, int16_t x, int16_t y, int16_t font, uint16_t opt, const char *s) { cmd_xy_str(e, CMD_TEXT, x, y, font, opt, s); }
void eve_button(eve_t *e, int16_t x, int16_t y, int16_t w, int16_t h, int16_t font, uint16_t opt, const char *s) { cmd_box_str(e, CMD_BUTTON, x, y, w, h, font, opt, s); }
void eve_keys(eve_t *e, int16_t x, int16_t y, int16_t w, int16_t h, int16_t font, uint16_t opt, const char *s) { cmd_box_str(e, CMD_KEYS, x, y, w, h, font, opt, s); }
void eve_number(eve_t *e, int16_t x, int16_t y, int16_t font, uint16_t opt, int32_t n) {
    if (!room(e, 16)) return;
    put32(e, CMD_NUMBER); put16x2(e, x, y); put16x2(e, font, (int16_t)opt); put32(e, (uint32_t)n);
}
void eve_progress(eve_t *e, int16_t x, int16_t y, int16_t w, int16_t h, uint16_t opt, uint16_t val, uint16_t range) {
    if (!room(e, 20)) return;
    put32(e, CMD_PROGRESS); put16x2(e, x, y); put16x2(e, w, h); put16x2(e, (int16_t)opt, (int16_t)val);
    put32(e, range);                                           /* range + 2 bytes padding */
}
void eve_spinner(eve_t *e, int16_t x, int16_t y, uint16_t style, uint16_t scale) {
    if (!room(e, 12)) return;
    put32(e, CMD_SPINNER); put16x2(e, x, y); put16x2(e, (int16_t)style, (int16_t)scale);
}
void eve_fgcolor(eve_t *e, uint32_t rgb)   { if (room(e, 8)) { put32(e, CMD_FGCOLOR); put32(e, rgb); } }
void eve_bgcolor(eve_t *e, uint32_t rgb)   { if (room(e, 8)) { put32(e, CMD_BGCOLOR); put32(e, rgb); } }
void eve_gradcolor(eve_t *e, uint32_t rgb) { if (room(e, 8)) { put32(e, CMD_GRADCOLOR); put32(e, rgb); } }
void eve_color(eve_t *e, uint32_t rgb) { eve_cmd(e, DL_COLOR_RGB((rgb >> 16) & 255u, (rgb >> 8) & 255u, rgb & 255u)); }
void eve_tag(eve_t *e, uint8_t tag) { eve_cmd(e, DL_TAG(tag)); }

void eve_rect(eve_t *e, int16_t x, int16_t y, int16_t w, int16_t h, uint16_t radius) {
    if (!room(e, 20)) return;
    uint16_t r = radius ? radius : 1;                          /* line width sets the corner radius (1/16 px) */
    put32(e, DL_LINE_WIDTH(r * 16u));
    put32(e, DL_BEGIN(EVE_RECTS));
    put32(e, DL_VERTEX2F(x + (int16_t)r - 1, y + (int16_t)r - 1));
    put32(e, DL_VERTEX2F(x + w - (int16_t)r, y + h - (int16_t)r));
    put32(e, DL_END);
}

void eve_line(eve_t *e, int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t width) {
    if (!room(e, 20)) return;
    put32(e, DL_LINE_WIDTH(width * 8u));                       /* width in px (line width is a radius, 1/16 px) */
    put32(e, DL_BEGIN(EVE_LINES));
    put32(e, DL_VERTEX2F(x0, y0));
    put32(e, DL_VERTEX2F(x1, y1));
    put32(e, DL_END);
}

/* co-processor fault: read the message, reset the co-processor (BT81x guide 5.7) */
static void recover(eve_t *e) {
    uint8_t msg[32];
    rd_mem(e, EVE_RAM_ERR, msg, sizeof msg);
    memcpy(e->fault_msg, msg, sizeof e->fault_msg - 1);
    e->fault_msg[sizeof e->fault_msg - 1] = 0;
    uint16_t patch = eve_rd16(e, REG_COPRO_PATCH_PTR);
    eve_wr8(e, REG_CPURESET, 1);
    eve_wr16(e, REG_CMD_READ, 0);
    eve_wr16(e, REG_CMD_WRITE, 0);
    eve_wr16(e, REG_CMD_DL, 0);
    eve_wr8(e, REG_CPURESET, 0);
    eve_wr16(e, REG_COPRO_PATCH_PTR, patch);
    e->faults++;
}

/* stream n bytes (a multiple of 4) into the co-processor FIFO; 0 ok, -1 fault, -2 timeout */
static int send_stream(eve_t *e, const uint8_t *p, uint32_t n) {
    uint32_t sent = 0;
    for (int tries = 0; sent < n; ) {
        uint16_t space = eve_rd16(e, REG_CMDB_SPACE) & 0xFFCu;
        if (space == 0) {
            if (eve_rd16(e, REG_CMD_READ) == 0xFFFu) { recover(e); return -1; }
            if (++tries > 200) { e->timeouts++; return -2; }
            e->bus->delay_ms(1);
            continue;
        }
        tries = 0;
        uint32_t chunk = n - sent < space ? n - sent : space;
        eve_wr_mem(e, REG_CMDB_WRITE, p + sent, chunk);
        sent += chunk;
    }
    return 0;
}

static int wait_idle(eve_t *e, int ms) {
    for (int k = 0; k < ms; ++k) {                             /* wait for the co-processor to finish */
        uint16_t rd = eve_rd16(e, REG_CMD_READ);
        if (rd == 0xFFFu) { recover(e); return -1; }
        if (rd == eve_rd16(e, REG_CMD_WRITE)) return 0;
        e->bus->delay_ms(1);
    }
    e->timeouts++;
    return -2;
}

int eve_frame_end(eve_t *e) {
    if (!e->ok) return -2;
    put32(e, DL_DISPLAY);                                      /* (room was kept for these two) */
    put32(e, CMD_SWAP);
    int rc = send_stream(e, e->buf, e->n);
    if (rc == 0) rc = wait_idle(e, 50);
    if (rc == 0) e->frames++;
    return rc;
}

/* ---------------- start-up work: inflate, CRC, fonts ---------------- */
static void le32(uint8_t *b, uint32_t v) { b[0] = (uint8_t)v; b[1] = (uint8_t)(v >> 8); b[2] = (uint8_t)(v >> 16); b[3] = (uint8_t)(v >> 24); }

int eve_inflate(eve_t *e, uint32_t ram_g, const uint8_t *z, uint32_t zlen) {
    if (!e->ok) return -2;
    uint8_t h[8];
    le32(h, CMD_INFLATE); le32(h + 4, ram_g);
    int rc = send_stream(e, h, 8);
    uint32_t whole = zlen & ~3u;
    if (rc == 0 && whole) rc = send_stream(e, z, whole);
    if (rc == 0 && whole < zlen) {                             /* the tail, padded to a word */
        uint8_t t[4] = { 0, 0, 0, 0 };
        memcpy(t, z + whole, zlen - whole);
        rc = send_stream(e, t, 4);
    }
    return rc == 0 ? wait_idle(e, 500) : rc;
}

int eve_memcrc(eve_t *e, uint32_t ram_g, uint32_t n, uint32_t *crc) {
    if (!e->ok) return -2;
    uint16_t w = eve_rd16(e, REG_CMD_WRITE);
    uint8_t c[16];
    le32(c, CMD_MEMCRC); le32(c + 4, ram_g); le32(c + 8, n); le32(c + 12, 0);
    int rc = send_stream(e, c, sizeof c);
    if (rc == 0) rc = wait_idle(e, 200);
    if (rc == 0) *crc = eve_rd32(e, EVE_RAM_CMD + ((w + 12u) & 0xFFFu));   /* the result word, in the FIFO */
    return rc;
}

uint8_t eve_fonts_load(eve_t *e, uint32_t base) {
    e->fonts_ok = 0;
    uint32_t a = base;
    for (unsigned i = 0; i < EVE_FONTS && e->ok; ++i) {
        const eve_font_t *f = &eve_fonts[i];
        uint32_t metric = a, data = (a + 148u + 3u) & ~3u;
        uint8_t m[148];
        memcpy(m, f->metric, 144);
        le32(m + 144, data - (uint32_t)f->first * f->cell);    /* glyph address of code 0 */
        eve_wr_mem(e, metric, m, sizeof m);
        uint32_t crc = 0;
        if (eve_inflate(e, data, f->z, f->zlen) == 0 && eve_memcrc(e, data, f->size, &crc) == 0 && crc == f->crc) {
            e->font_metric[i] = metric;
            e->fonts_ok |= (uint8_t)(1u << i);
        }
        a = (data + f->size + 3u) & ~3u;
    }
    return e->fonts_ok;
}

void eve_fonts_use(eve_t *e) {
    for (unsigned i = 0; i < EVE_FONTS; ++i) {
        if (!(e->fonts_ok & (1u << i)) || !room(e, 16)) continue;
        put32(e, CMD_SETFONT2); put32(e, EVE_FONT_HANDLE0 + i); put32(e, e->font_metric[i]); put32(e, eve_fonts[i].first);
    }
}

char *eve_font_encode(const char *s, char *out, size_t cap, bool own) {
    size_t k = 0;
    const unsigned char *p = (const unsigned char *)s;
    while (*p && k + 1 < cap) {
        uint32_t cp = *p;
        int more = 0;
        if (cp >= 0xF0) { cp &= 0x07; more = 3; } else if (cp >= 0xE0) { cp &= 0x0F; more = 2; }
        else if (cp >= 0xC0) { cp &= 0x1F; more = 1; } else if (cp >= 0x80) { p++; out[k++] = '?'; continue; }
        p++;
        for (; more && (*p & 0xC0) == 0x80; --more) cp = (cp << 6) | (*p++ & 0x3Fu);
        if (more) { out[k++] = '?'; continue; }                 /* cut short */
        if (cp >= 32 && cp < 127) { out[k++] = (char)cp; continue; }
        if (cp < 32) continue;                                  /* control characters: dropped */
        char c = '?';
        if (own) for (unsigned j = 0; j < eve_font_n_extra; ++j) if (eve_font_extra[j].cp == cp) { c = (char)eve_font_extra[j].code; break; }
        out[k++] = c;
    }
    out[k] = 0;
    return out;
}

uint8_t eve_touch_tag(eve_t *e) { return e->ok ? eve_rd8(e, REG_TOUCH_TAG) : 0; }

bool eve_touch_xy(eve_t *e, int16_t *x, int16_t *y) {
    if (!e->ok) return false;
    uint32_t v = eve_rd32(e, REG_TOUCH_SCREEN_XY);
    if (v == 0x80008000u) return false;
    *x = (int16_t)(v >> 16);
    *y = (int16_t)(v & 0xFFFFu);
    return true;
}
