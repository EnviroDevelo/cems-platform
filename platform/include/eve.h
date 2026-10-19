/* eve.h - driver for the Bridgetek BT817 (EVE4) display controller on the Riverdi
 * RVT70HSBNWC00-B (7", 1024x600, capacitive touch), in plain C over SPI (design spec
 * "UI software"; decision D16: EVE co-processor commands directly, no LVGL).
 *
 * The BT817 draws the screen itself from a display list, so the H5 holds no frame
 * buffer: each frame is a short list of co-processor commands (text, buttons,
 * keypads, rectangles) built in a RAM buffer here and sent in one burst through
 * REG_CMDB_WRITE. Touch is read back as the tag of the widget under the finger.
 *
 * Only the UI task uses the display. The board supplies the SPI transport. */
#ifndef EVE_H
#define EVE_H
#include <stdint.h>
#include <stdbool.h>

/* ---------------- transport (from the board) ---------------- */
typedef struct eve_bus {
    void (*select)(struct eve_bus *b, bool on);                     /* CS low = on */
    int  (*write)(struct eve_bus *b, const uint8_t *p, uint32_t n); /* 0 = ok */
    int  (*read)(struct eve_bus *b, uint8_t *p, uint32_t n);
    void (*power)(struct eve_bus *b, bool on);                      /* PD_N high = on; may be NULL */
    void (*fast)(struct eve_bus *b, bool on);                       /* SPI clock: <= 11 MHz until the PLL runs; may be NULL */
    void (*delay_ms)(uint32_t ms);
    void *ctx;
} eve_bus_t;

/* ---------------- panel timing ---------------- */
typedef struct {
    uint16_t width, height;
    uint16_t hcycle, hoffset, hsync0, hsync1;
    uint16_t vcycle, voffset, vsync0, vsync1;
    uint16_t pclk_freq;        /* BT817 REG_PCLK_FREQ (pixel clock from the PLL) */
    uint8_t  pclk, pclk_pol, swizzle, cspread, dither, pclk_2x;
    bool     ext_clock;        /* crystal fitted (Riverdi EVE4 modules: yes) */
    uint16_t touch_config;     /* REG_TOUCH_CONFIG, 0 = leave the chip's default */
} eve_panel_t;
extern const eve_panel_t eve_panel_rvt70h;   /* Riverdi RVT70HSBNWC00-B (values from Riverdi's library, IPS_70) */

/* ---------------- memory map and registers (BT81x programming guide) ---------------- */
#define EVE_RAM_G        0x000000u
#define EVE_RAM_DL       0x300000u
#define EVE_RAM_REG      0x302000u
#define EVE_RAM_CMD      0x308000u
#define EVE_RAM_ERR      0x309800u   /* co-processor fault message (BT81x) */
#define REG_ID           0x302000u
#define REG_FRAMES       0x302004u
#define REG_FREQUENCY    0x30200Cu
#define REG_CPURESET     0x302020u
#define REG_HCYCLE       0x30202Cu
#define REG_HOFFSET      0x302030u
#define REG_HSIZE        0x302034u
#define REG_HSYNC0       0x302038u
#define REG_HSYNC1       0x30203Cu
#define REG_VCYCLE       0x302040u
#define REG_VOFFSET      0x302044u
#define REG_VSIZE        0x302048u
#define REG_VSYNC0       0x30204Cu
#define REG_VSYNC1       0x302050u
#define REG_DLSWAP       0x302054u
#define REG_DITHER       0x302060u
#define REG_SWIZZLE      0x302064u
#define REG_CSPREAD      0x302068u
#define REG_PCLK_POL     0x30206Cu
#define REG_PCLK         0x302070u
#define REG_GPIOX_DIR    0x302098u
#define REG_GPIOX        0x30209Cu
#define REG_PWM_HZ       0x3020D0u
#define REG_PWM_DUTY     0x3020D4u
#define REG_CMD_READ     0x3020F8u
#define REG_CMD_WRITE    0x3020FCu
#define REG_CMD_DL       0x302100u
#define REG_TOUCH_MODE   0x302104u
#define REG_TOUCH_SCREEN_XY 0x302124u
#define REG_TOUCH_TAG    0x30212Cu
#define REG_TOUCH_CONFIG 0x302168u
#define REG_TRIM         0x302180u
#define REG_CMDB_SPACE   0x302574u
#define REG_CMDB_WRITE   0x302578u
#define REG_COPRO_PATCH_PTR 0x309162u
#define REG_PCLK_FREQ    0x302614u
#define REG_PCLK_2X      0x302618u
#define EVE_CHIP_ID      0x7Cu

/* host commands */
#define EVE_HC_ACTIVE    0x00u
#define EVE_HC_CLKEXT    0x44u
#define EVE_HC_CLKINT    0x48u
#define EVE_HC_RST_PULSE 0x68u

/* ---------------- display-list words ---------------- */
#define DL_VERTEX2F(x, y)     ((1u << 30) | (((uint32_t)(x) & 32767u) << 15) | ((uint32_t)(y) & 32767u))
#define DL_CLEAR_RGB(r, g, b) ((2u << 24) | ((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b))
#define DL_TAG(s)             ((3u << 24) | ((uint32_t)(s) & 255u))
#define DL_COLOR_RGB(r, g, b) ((4u << 24) | ((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b))
#define DL_LINE_WIDTH(w)      ((14u << 24) | ((uint32_t)(w) & 4095u))
#define DL_COLOR_A(a)         ((16u << 24) | ((uint32_t)(a) & 255u))
#define DL_TAG_MASK(m)        ((20u << 24) | ((uint32_t)(m) & 1u))
#define DL_SCISSOR_XY(x, y)   ((27u << 24) | (((uint32_t)(x) & 2047u) << 11) | ((uint32_t)(y) & 2047u))
#define DL_SCISSOR_SIZE(w, h) ((28u << 24) | (((uint32_t)(w) & 4095u) << 12) | ((uint32_t)(h) & 4095u))
#define DL_BEGIN(p)           ((31u << 24) | ((uint32_t)(p) & 15u))
#define DL_END                (33u << 24)
#define DL_SAVE_CONTEXT       (34u << 24)
#define DL_RESTORE_CONTEXT    (35u << 24)
#define DL_CLEAR(c, s, t)     ((38u << 24) | ((uint32_t)(c) << 2) | ((uint32_t)(s) << 1) | (uint32_t)(t))
#define DL_VERTEX_FORMAT(f)   ((39u << 24) | ((uint32_t)(f) & 7u))
#define DL_DISPLAY            0u
enum { EVE_POINTS = 2, EVE_LINES = 3, EVE_LINE_STRIP = 4, EVE_RECTS = 9 };

/* co-processor commands */
#define CMD_DLSTART   0xFFFFFF00u
#define CMD_SWAP      0xFFFFFF01u
#define CMD_BGCOLOR   0xFFFFFF09u
#define CMD_FGCOLOR   0xFFFFFF0Au
#define CMD_TEXT      0xFFFFFF0Cu
#define CMD_BUTTON    0xFFFFFF0Du
#define CMD_KEYS      0xFFFFFF0Eu
#define CMD_PROGRESS  0xFFFFFF0Fu
#define CMD_TOGGLE    0xFFFFFF12u
#define CMD_SPINNER   0xFFFFFF16u
#define CMD_STOP      0xFFFFFF17u
#define CMD_NUMBER    0xFFFFFF2Eu
#define CMD_COLDSTART 0xFFFFFF32u
#define CMD_GRADCOLOR 0xFFFFFF34u
#define CMD_ROMFONT   0xFFFFFF3Fu
#define CMD_FILLWIDTH 0xFFFFFF58u
/* options */
#define OPT_FLAT      256u
#define OPT_CENTERX   512u
#define OPT_CENTERY   1024u
#define OPT_CENTER    1536u
#define OPT_RIGHTX    2048u
#define OPT_FILL      8192u

/* ---------------- driver ---------------- */
#ifndef EVE_CMDBUF
#define EVE_CMDBUF 8192u          /* bytes of commands per frame */
#endif

typedef struct {
    eve_bus_t         *bus;
    const eve_panel_t *panel;
    bool      ok;                 /* initialised and answering */
    uint32_t  n;                  /* bytes in buf */
    bool      overflow;
    uint32_t  frames, faults, timeouts;
    char      fault_msg[32];
    uint8_t   buf[EVE_CMDBUF];
} eve_t;

int  eve_init(eve_t *e, eve_bus_t *bus, const eve_panel_t *panel);   /* 0 ok; -1 no chip; -2 engines not ready */
void eve_backlight(eve_t *e, uint8_t percent);

uint8_t  eve_rd8(eve_t *e, uint32_t a);
uint16_t eve_rd16(eve_t *e, uint32_t a);
uint32_t eve_rd32(eve_t *e, uint32_t a);
void eve_wr8(eve_t *e, uint32_t a, uint8_t v);
void eve_wr16(eve_t *e, uint32_t a, uint16_t v);
void eve_wr32(eve_t *e, uint32_t a, uint32_t v);
void eve_wr_mem(eve_t *e, uint32_t a, const void *p, uint32_t n);
void eve_host_cmd(eve_t *e, uint8_t cmd, uint8_t param);

/* frame building: begin, commands, end (sends and waits up to ~50 ms) */
void eve_frame_begin(eve_t *e, uint8_t r, uint8_t g, uint8_t b);
void eve_cmd(eve_t *e, uint32_t word);
void eve_cmd_str(eve_t *e, const char *s);            /* NUL-terminated, padded to 4 bytes */
void eve_text(eve_t *e, int16_t x, int16_t y, int16_t font, uint16_t opt, const char *s);
void eve_button(eve_t *e, int16_t x, int16_t y, int16_t w, int16_t h, int16_t font, uint16_t opt, const char *s);
void eve_keys(eve_t *e, int16_t x, int16_t y, int16_t w, int16_t h, int16_t font, uint16_t opt, const char *s);
void eve_number(eve_t *e, int16_t x, int16_t y, int16_t font, uint16_t opt, int32_t n);
void eve_progress(eve_t *e, int16_t x, int16_t y, int16_t w, int16_t h, uint16_t opt, uint16_t val, uint16_t range);
void eve_spinner(eve_t *e, int16_t x, int16_t y, uint16_t style, uint16_t scale);
void eve_fgcolor(eve_t *e, uint32_t rgb);
void eve_bgcolor(eve_t *e, uint32_t rgb);
void eve_gradcolor(eve_t *e, uint32_t rgb);
void eve_color(eve_t *e, uint32_t rgb);
void eve_rect(eve_t *e, int16_t x, int16_t y, int16_t w, int16_t h, uint16_t radius);   /* filled, current colour */
void eve_line(eve_t *e, int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t width);
void eve_tag(eve_t *e, uint8_t tag);
int  eve_frame_end(eve_t *e);                          /* 0 ok, -1 co-processor fault (recovered), -2 timeout */

/* touch */
uint8_t eve_touch_tag(eve_t *e);                       /* tag under the finger, 0 = none */
bool    eve_touch_xy(eve_t *e, int16_t *x, int16_t *y);

#endif
