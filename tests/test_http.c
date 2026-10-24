/* test_http.c - web side: SHA-256, formatting, text, request parsing, auth and
 * sessions, and the API handlers end to end through a capture buffer. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "tst.h"
#include "plat_sha256.h"
#include "http_util.h"
#include "http_api.h"
#include "web_auth.h"
#include "param.h"
#include "param_store.h"
#include "bsp_host.h"
#include "iobus.h"
#include "iob_node_sim.h"


/* ---------------- primitives ---------------- */
static void hexs(const uint8_t *h, char *out) { hex_encode(h, 32, out); }

static void t_sha256(void) {
    uint8_t h[32]; char x[65]; sha256_t s;
    sha256_init(&s); sha256_update(&s, "abc", 3); sha256_final(&s, h); hexs(h, x);
    CHECK(strcmp(x, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0);
    sha256_init(&s); sha256_final(&s, h); hexs(h, x);
    CHECK(strcmp(x, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") == 0);
    const char *m = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    sha256_init(&s); for (const char *p = m; *p; ++p) sha256_update(&s, p, 1); sha256_final(&s, h); hexs(h, x);
    CHECK(strcmp(x, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1") == 0);
}

static void t_fmt_float(void) {
    char b[32];
    struct { double v; const char *s; } c[] = {
        { 0, "0" }, { 1, "1" }, { -1.5, "-1.5" }, { 12.5, "12.5" }, { 0.001, "0.001" }, { 0.1, "0.1" },
        { 123456.7, "123456.7" }, { 1234567.0, "1234567" }, { 99999999.0, "100000000" }, { 1e9, "1e+09" },
        { 1.2345678e12, "1.234568e+12" }, { 0.0001, "0.0001" }, { 0.00001, "1e-05" }, { 3.14159265, "3.141593" },
        { 1000.0, "1000" }, { 999.99999, "1000" }, { 0.999999949, "0.9999999" }, { 2.5e-7, "2.5e-07" } };
    for (size_t k = 0; k < sizeof c / sizeof c[0]; ++k) {
        fmt_float(b, sizeof b, c[k].v);
        if (strcmp(b, c[k].s) != 0) printf("    fmt_float(%g) = %s, want %s\n", c[k].v, b, c[k].s);
        CHECK(strcmp(b, c[k].s) == 0);
    }
    fmt_float(b, sizeof b, 1.0 / 0.0); CHECK(strcmp(b, "null") == 0);
}

static void t_text_utils(void) {
    uint8_t u[64]; char s[64];
    CHECK_EQ(utf8_to_utf16le("A\xC2\xB0" "C\xE2\x82\xAC", u, sizeof u), 8);       /* A ° C € */
    CHECK_EQ(u[2], 0xB0); CHECK_EQ(u[6], 0xAC); CHECK_EQ(u[7], 0x20);
    CHECK_EQ(utf16le_to_utf8(u, 4, s, sizeof s), 7);              /* 1 + 2 + 1 + 3 bytes */
    CHECK(strcmp(s, "A\xC2\xB0" "C\xE2\x82\xAC") == 0);
    CHECK_EQ(utf8_to_utf16le("\xF0\x9F\x98\x80", u, sizeof u), -1);              /* outside the BMP */
    CHECK_EQ(utf8_to_utf16le("\xC0\x80", u, sizeof u), -1);                      /* overlong */
    CHECK_EQ(utf8_to_utf16le("\xE2\x82", u, sizeof u), -1);                      /* truncated */
    CHECK_EQ(utf8_to_utf16le("abcd", u, 6), -1);                                 /* too long */
    uint8_t h[4]; CHECK_EQ(hex_decode("0aFf", h, 4), 2); CHECK_EQ(h[1], 0xFF);
    CHECK_EQ(hex_decode("abc", h, 4), -1); CHECK_EQ(hex_decode("zz", h, 4), -1);
    char k[16], v[32]; form_iter_t it; const char *f = "id=1001&v=a%20b+c&o=&x";
    form_begin(&it, f, strlen(f));
    CHECK(form_next(&it, k, sizeof k, v, sizeof v)); CHECK(!strcmp(k, "id")); CHECK(!strcmp(v, "1001"));
    CHECK(form_next(&it, k, sizeof k, v, sizeof v)); CHECK(!strcmp(v, "a b c"));
    CHECK(form_next(&it, k, sizeof k, v, sizeof v)); CHECK(!strcmp(k, "o")); CHECK(!strcmp(v, ""));
    CHECK(form_next(&it, k, sizeof k, v, sizeof v)); CHECK(!strcmp(k, "x"));
    CHECK(!form_next(&it, k, sizeof k, v, sizeof v));
    CHECK(form_get("a=1&b=%zz", 9, "b", v, sizeof v) == false);                  /* bad escape */
    CHECK(form_get("a=1&b=%00", 9, "b", v, sizeof v) == false);                  /* NUL refused */
}

static void t_parse_head(void) {
    http_req_t r;
    const char *g = "GET /api/values?ids=1,2 HTTP/1.1\r\nHost: x\r\nCookie: a=b; sid=0123456789abcdef0123456789abcdef\r\n\r\n";
    CHECK_EQ(http_parse_head(g, (uint32_t)strlen(g), &r), (int)strlen(g));
    CHECK(!strcmp(r.method, "GET")); CHECK(!strcmp(r.path, "/api/values")); CHECK(!strcmp(r.query, "ids=1,2"));
    CHECK(!strcmp(r.sid, "0123456789abcdef0123456789abcdef"));
    const char *p = "POST /api/write HTTP/1.1\r\ncontent-length: 12\r\nX-CEMS: 1\r\n\r\nid=1&v=2";
    CHECK(http_parse_head(p, (uint32_t)strlen(p), &r) > 0); CHECK(r.csrf); CHECK_EQ(r.content_length, 12);
    CHECK_EQ(http_parse_head("GET / HTTP/1.1\r\nHost:", 21, &r), 0);              /* incomplete */
    const char *bad[] = { "GET\r\n\r\n", "GET nopath HTTP/1.1\r\n\r\n", "GET / FTP/1.0\r\n\r\n",
                          "POST / HTTP/1.1\r\nContent-Length: 1x\r\n\r\n", "POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n" };
    for (size_t k = 0; k < 5; ++k) CHECK_EQ(http_parse_head(bad[k], (uint32_t)strlen(bad[k]), &r), -1);
    /* keep-alive: HTTP/1.1 by default, unless "Connection: close"; 1.0 only if asked */
    CHECK(http_parse_head(g, (uint32_t)strlen(g), &r) > 0 && r.keep_alive);
    const char *kc = "GET / HTTP/1.1\r\nConnection: Close\r\n\r\n";
    CHECK(http_parse_head(kc, (uint32_t)strlen(kc), &r) > 0 && !r.keep_alive);
    const char *k10 = "GET / HTTP/1.0\r\n\r\n", *k10k = "GET / HTTP/1.0\r\nConnection: keep-alive\r\n\r\n";
    CHECK(http_parse_head(k10, (uint32_t)strlen(k10), &r) > 0 && !r.keep_alive);
    CHECK(http_parse_head(k10k, (uint32_t)strlen(k10k), &r) > 0 && r.keep_alive);
    const char *shortsid = "GET / HTTP/1.1\r\nCookie: sid=abc\r\n\r\n";
    CHECK(http_parse_head(shortsid, (uint32_t)strlen(shortsid), &r) > 0); CHECK_EQ(r.sid[0], 0);
}

/* ---------------- auth ---------------- */
static uint8_t fram[128 * 1024]; static nvm_ram_t ram; static nvm_dev_t dev;
static int16_t h_set; static char h_name[11]; static float h_f;
static int16_t h_arr[3];
static const param_def_t hdefs[] = {
    PARAM_INT (1001, "setpoint", h_set, CT_I16, 0, 100, 10, PLAT_LVL_OPERATOR, PF_PERSIST, "%"),
    PARAM_TEXT(1002, "name", h_name, NULL, PLAT_LVL_OPERATOR, PF_PERSIST),
    PARAM_REAL(1003, "gain", h_f, 0.0f, 10.0f, 1.25f, NULL, PLAT_LVL_ADMIN, PF_PERSIST | PF_ANY_MODE),
    PARAM_ARRAY_INT(1004, "arr", h_arr, CT_I16, -5, 5, PLAT_LVL_OPERATOR, 0),
};
static plat_cmdq_t q; static plat_seqlock_t lk;

static void scan_once(void) {
    plat_cmd_t c; while (plat_cmdq_pop(&q, &c)) (void)param_apply(&c);
    param_blob_drain(); param_store_scan_service(); param_publish();
}
static uint32_t tnow = 100000;
static void store_tick(void) { tnow += 10; param_store_poll(tnow); scan_once(); param_store_poll(tnow); }

static void boot(void) {
    nvm_ram_init(&dev, &ram, fram, sizeof fram, true);
    CHECK_EQ(param_init(hdefs, sizeof hdefs / sizeof hdefs[0]), PERR_OK);
    CHECK(param_store_init(&dev, NULL, NULL, 0, 1, NULL));
    param_store_boot();
    plat_cmdq_init(&q); lk.seq = 0;
    param_bind(&q, &lk);
    auth_init();
}

static void t_auth(void) {
    memset(fram, 0, sizeof fram);
    boot();
    CHECK(auth_any_default());
    CHECK(auth_check(PLAT_LVL_OPERATOR, "1111")); CHECK(!auth_check(PLAT_LVL_OPERATOR, "2222"));
    CHECK(auth_check(PLAT_LVL_ADMIN, "2222")); CHECK(!auth_check(4, "2222")); CHECK(!auth_check(0, "x"));
    CHECK_EQ(auth_set_password(PLAT_LVL_ADMIN, "has space"), AUTH_INVALID);
    CHECK_EQ(auth_set_password(PLAT_LVL_ADMIN, "abc"), AUTH_INVALID);
    CHECK_EQ(auth_set_password(PLAT_LVL_ADMIN, "Str0ng!pw"), AUTH_OK);
    CHECK(!auth_check(PLAT_LVL_ADMIN, "2222")); CHECK(auth_check(PLAT_LVL_ADMIN, "Str0ng!pw"));
    CHECK(!auth_is_default(PLAT_LVL_ADMIN)); CHECK(auth_is_default(PLAT_LVL_OPERATOR));
    /* survives a reboot (salt and hash are in FRAM) */
    for (int k = 0; k < 3; ++k) store_tick();
    tnow += 2000; store_tick(); store_tick();
    boot();
    CHECK(auth_check(PLAT_LVL_ADMIN, "Str0ng!pw")); CHECK(!auth_check(PLAT_LVL_ADMIN, "2222"));
    /* the hashes are not in the readable table */
    for (size_t i = 0; i < param_count(); ++i) CHECK(param_at(i)->id < 900 || param_at(i)->id >= 1000);
    /* sessions */
    char t1[AUTH_TOKEN_LEN + 1], t2[AUTH_TOKEN_LEN + 1];
    uint32_t now = 1000, left;
    CHECK_EQ(auth_login(PLAT_LVL_ADMIN, "Str0ng!pw", now, t1), AUTH_OK);
    CHECK_EQ(strlen(t1), AUTH_TOKEN_LEN);
    CHECK_EQ(auth_session(t1, now + 1000, &left), PLAT_LVL_ADMIN); CHECK_EQ(left, AUTH_IDLE_MS - 1000);
    auth_touch(t1, now + AUTH_IDLE_MS - 10);
    CHECK_EQ(auth_session(t1, now + AUTH_IDLE_MS + 100, NULL), PLAT_LVL_ADMIN);   /* the write extended it */
    CHECK_EQ(auth_session(t1, now + 2 * AUTH_IDLE_MS, NULL), 0);                  /* idle too long */
    CHECK_EQ(auth_session("0123456789abcdef0123456789abcdef", now, NULL), 0);
    CHECK_EQ(auth_session(NULL, now, NULL), 0);
    /* lockout after five wrong passwords, open again after 30 s */
    for (int k = 0; k < 5; ++k) CHECK_EQ(auth_login(PLAT_LVL_OPERATOR, "0000", now, t2), AUTH_BAD);
    CHECK_EQ(auth_login(PLAT_LVL_OPERATOR, "1111", now + 1000, t2), AUTH_LOCKED);
    CHECK(auth_locked_ms(now + 1000) > 28000);
    CHECK_EQ(auth_login(PLAT_LVL_OPERATOR, "1111", now + 31000, t2), AUTH_OK);
    /* sessions are capped: the least recently used one is replaced */
    char t[AUTH_SESSIONS + 1][AUTH_TOKEN_LEN + 1];
    for (int k = 0; k <= AUTH_SESSIONS; ++k) CHECK_EQ(auth_login(PLAT_LVL_OPERATOR, "1111", now + 40000 + (uint32_t)k, t[k]), AUTH_OK);
    CHECK_EQ(auth_session(t[0], now + 40010, NULL), 0);
    CHECK_EQ(auth_session(t[AUTH_SESSIONS], now + 40010, NULL), PLAT_LVL_OPERATOR);
    auth_logout(t[AUTH_SESSIONS]);
    CHECK_EQ(auth_session(t[AUTH_SESSIONS], now + 40010, NULL), 0);
}

/* ---------------- API end to end ---------------- */
static char cap[64 * 1024]; static size_t capn;
static int cap_send(void *ctx, const void *p, size_t n) {
    (void)ctx; if (capn + n >= sizeof cap) return -1;
    memcpy(cap + capn, p, n); capn += n; cap[capn] = 0; return 0;
}
static uint8_t scratch[20 * 1024];
static char sid[AUTH_TOKEN_LEN + 1];

static int call(const char *method, const char *path, const char *body, bool csrf) {
    static char raw[16 * 1024];
    size_t bl = body ? strlen(body) : 0;
    snprintf(raw, sizeof raw, "%s %s HTTP/1.1\r\nHost: unit\r\n%s%s%s%sContent-Length: %zu\r\n\r\n",
             method, path, csrf ? "X-CEMS: 1\r\n" : "", sid[0] ? "Cookie: sid=" : "", sid, sid[0] ? "\r\n" : "", bl);
    size_t hl = strlen(raw);
    if (body) memcpy(raw + hl, body, bl);
    http_req_t r;
    int h = http_parse_head(raw, (uint32_t)(hl + bl), &r);
    if (h <= 0) return -1;
    r.body = (const uint8_t *)raw + h; r.body_len = r.content_length;
    http_out_t o; out_init(&o, cap_send, NULL); capn = 0;
    http_handle(&r, &o, tnow);
    int st = 0; sscanf(cap, "HTTP/1.1 %d", &st);
    const char *ck = strstr(cap, "Set-Cookie: sid=");
    if (ck) { ck += 16; if (*ck == ';') sid[0] = 0; else { memcpy(sid, ck, AUTH_TOKEN_LEN); sid[AUTH_TOKEN_LEN] = 0; } }
    return st;
}
static bool has(const char *s) { return strstr(cap, s) != NULL; }
static void mode(uint8_t m) { plat_cmd_t c = { .id = PARAM_ID_CTRL_MODE, .src = PLAT_SRC_LOCAL, .level = 1, .kind = PLAT_VAL_U32, .v.u = m }; (void)param_apply(&c); }

static void t_api(void) {
    memset(fram, 0, sizeof fram);
    boot();
    http_cfg_t c = { .product = "CEMS Supervisor", .fw_version = "0.1.0-test", .scratch = scratch,
                     .scratch_size = sizeof scratch, .wait = store_tick };
    http_api_init(&c);
    sid[0] = 0;
    param_publish();

    CHECK_EQ(call("GET", "/", NULL, false), 200); CHECK(has("Content-Encoding: gzip"));
    CHECK_EQ(call("GET", "/api/info", NULL, false), 200);
    CHECK(has("\"product\":\"CEMS Supervisor\"")); CHECK(has("\"mode\":\"local\"")); CHECK(has("\"default_passwords\":true"));
    CHECK(has("\"cal_valid\":false")); CHECK(has("\"level\":\"view\""));
    {   /* a kept connection: chunked body that decodes to the same JSON; fixed-length bodies stay as they are */
        static char plain[16 * 1024];
        size_t pn = capn; memcpy(plain, cap, capn + 1);
        const char *raw = "GET /api/info HTTP/1.1\r\nHost: unit\r\n\r\n";
        http_req_t r; int h = http_parse_head(raw, (uint32_t)strlen(raw), &r);
        CHECK(h > 0 && r.keep_alive);
        http_out_t o; out_init(&o, cap_send, NULL); capn = 0; o.keep = true;
        http_handle(&r, &o, tnow);
        CHECK(has("Connection: keep-alive\r\nTransfer-Encoding: chunked\r\n"));
        const char *b = strstr(cap, "\r\n\r\n") + 4, *pb = strstr(plain, "\r\n\r\n") + 4;
        static char body[16 * 1024]; size_t bn = 0; bool ok = true;
        for (;;) {                                       /* de-chunk */
            unsigned long len = strtoul(b, NULL, 16);
            const char *d = strstr(b, "\r\n"); if (!d) { ok = false; break; }
            d += 2;
            if (!len) { ok = strcmp(d, "\r\n") == 0; break; }
            memcpy(body + bn, d, len); bn += len; b = d + len;
            if (strncmp(b, "\r\n", 2)) { ok = false; break; }
            b += 2;
        }
        body[bn] = 0;
        CHECK(ok);
        /* same content apart from the uptime-dependent fields: compare the start */
        CHECK(bn > 100 && strncmp(body, pb, 40) == 0);
        (void)pn;
        const char *rg = "GET / HTTP/1.1\r\n\r\n";
        h = http_parse_head(rg, (uint32_t)strlen(rg), &r);
        out_init(&o, cap_send, NULL); capn = 0; o.keep = true;
        http_handle(&r, &o, tnow);
        CHECK(has("Connection: keep-alive\r\n")); CHECK(!has("chunked")); CHECK(has("Content-Length: "));
    }
    CHECK_EQ(call("GET", "/api/params", NULL, false), 200);
    CHECK(has("{\"id\":1001,\"name\":\"setpoint\",\"type\":\"int\",\"unit\":\"%\",\"min\":0,\"max\":100,\"def\":10,\"level\":1,\"flags\":[\"persist\"]}"));
    CHECK(has("\"name\":\"gain\",\"type\":\"float\",\"min\":0,\"max\":10,\"def\":1.25"));
    CHECK(has("\"elem\":\"i16\""));
    CHECK_EQ(call("GET", "/api/values?ids=1001,1003,9999", NULL, false), 200);
    CHECK(has("\"v\":{\"1001\":10,\"1003\":1.25}}"));
    CHECK_EQ(call("GET", "/api/nothing", NULL, false), 404);
    CHECK_EQ(call("PUT", "/api/values", NULL, false), 405);

    /* writes need the header, a session, the right mode and level */
    CHECK_EQ(call("POST", "/api/write", "id=1001&v=50", false), 403); CHECK(has("X-CEMS"));
    CHECK_EQ(call("POST", "/api/write", "id=1001&v=50", true), 401);
    CHECK_EQ(call("POST", "/api/login", "level=operator&password=nope", true), 401);
    CHECK_EQ(call("POST", "/api/login", "level=operator&password=1111", true), 200);
    CHECK(has("HttpOnly; SameSite=Strict")); CHECK_EQ(strlen(sid), AUTH_TOKEN_LEN);
    CHECK_EQ(call("POST", "/api/write", "id=1001&v=50", true), 409); CHECK(has("\"error\":\"mode\""));
    mode(PARAM_MODE_REMOTE);
    CHECK_EQ(call("POST", "/api/write", "id=1001&v=500", true), 400); CHECK(has("\"error\":\"range\""));
    CHECK_EQ(call("POST", "/api/write", "id=1001&v=abc", true), 400); CHECK(has("\"error\":\"value\""));
    CHECK_EQ(call("POST", "/api/write", "id=1003&v=2", true), 403);   /* admin entry */
    CHECK_EQ(call("POST", "/api/write", "id=1001&v=50&id=1002&v=Stack%20A&id=1004&o=1&v=-5,5", true), 200);
    CHECK(has("\"queued\":3"));
    scan_once();
    CHECK_EQ(h_set, 50); CHECK(!strcmp(h_name, "Stack A")); CHECK_EQ(h_arr[1], -5); CHECK_EQ(h_arr[2], 5);
    CHECK_EQ(call("GET", "/api/values?ids=1001,1002,1004", NULL, false), 200);
    CHECK(has("\"1001\":50,\"1002\":\"Stack A\",\"1004\":[0,-5,5]"));
    /* all or nothing: the second item is out of range, so the first is not applied either */
    CHECK_EQ(call("POST", "/api/write", "id=1001&v=60&id=1004&v=9", true), 400);
    scan_once(); CHECK_EQ(h_set, 50);
    CHECK_EQ(call("POST", "/api/write", "id=1001", true), 400);
    CHECK_EQ(call("POST", "/api/write", "v=1", true), 400);
    CHECK_EQ(call("POST", "/api/write", "id=4242&v=1", true), 404);
    /* text escaping in JSON */
    CHECK_EQ(call("POST", "/api/write", "id=1002&v=a%22b%5Cc", true), 200); scan_once();
    CHECK_EQ(call("GET", "/api/values?ids=1002", NULL, false), 200); CHECK(has("\"1002\":\"a\\\"b\\\\c\""));

    /* admin: password change, backup, restore */
    CHECK_EQ(call("GET", "/api/backup", NULL, false), 403);
    CHECK_EQ(call("POST", "/api/password", "level=admin&password=newpass1", true), 403);   /* operator session */
    CHECK_EQ(call("POST", "/api/iob/find", "addr=246", true), 403);              /* operator session */
    CHECK_EQ(call("POST", "/api/iob/net/find", "", true), 403);
    CHECK_EQ(call("POST", "/api/login", "level=admin&password=2222", true), 200);
    CHECK_EQ(call("POST", "/api/password", "level=admin&password=newpass1", true), 200);
    /* I/O board commissioning: requests are queued for the bus task, results read back */
    iob_reset();
    CHECK_EQ(call("POST", "/api/iob/find", "addr=247", true), 400);
    CHECK_EQ(call("POST", "/api/iob/assign", "from=246&to=246", true), 400);
    CHECK_EQ(call("POST", "/api/iob/assign", "from=246&to=x", true), 400);
    CHECK_EQ(call("POST", "/api/iob/find", "addr=246", true), 202);
    CHECK_EQ(call("POST", "/api/iob/assign", "from=246&to=3", true), 409); CHECK(has("busy"));
    CHECK_EQ(call("GET", "/api/iob/commission", NULL, false), 200);
    CHECK(has("\"busy\":true")); CHECK(has("\"op\":\"find\"")); CHECK(has("\"board\":null"));
    iob_reset();
    /* on Ethernet: list the boards on the network, give one a board slot's IP and address */
    {
        static iob_simnet_t net; static iob_node_t nd;
        iob_simnet_init(&net, 0xC0A80132u);
        iob_node_init(&nd, 0);                                   /* new: no IP, no address */
        memcpy(nd.serial, "CIO-26-00007", 12);
        iob_simnet_add(&net, &nd);
        iob_init_eth(&net.port, &net.disc);
        CHECK_EQ(call("POST", "/api/iob/net/set", "uid=00&board=1", true), 400);
        CHECK_EQ(call("POST", "/api/iob/net/set", "uid=000000000000000000000000&board=4", true), 400);
        CHECK_EQ(call("POST", "/api/iob/net/set", "uid=000000000000000000000000&board=1&ip=10.0.0.5", true), 400);   /* not the supervisor's subnet */
        CHECK_EQ(call("POST", "/api/iob/net/find", "", true), 202);
        CHECK_EQ(call("POST", "/api/iob/net/find", "", true), 409);
        CHECK_EQ(call("GET", "/api/iob/net", NULL, false), 200);
        CHECK(has("\"busy\":true")); CHECK(has("\"link\":\"ethernet\"")); CHECK(has("\"ip\":\"192.168.1.50\""));
        CHECK(has("{\"n\":2,\"enabled\":false,\"address\":2,\"ip\":\"192.168.1.62\"}"));
        iob_cycle(0);                                            /* the bus task does it */
        CHECK_EQ(call("GET", "/api/iob/net", NULL, false), 200);
        CHECK(has("\"result\":\"OK\"") || has("\"ok\":true"));
        CHECK(has("\"uid\":\"000500040003000200010000\"")); CHECK(has("\"serial\":\"CIO-26-00007\""));
        CHECK(has("\"new\":true")); CHECK(has("\"slot\":0"));
        CHECK_EQ(call("POST", "/api/iob/net/set", "uid=000500040003000200010000&board=2", true), 202);
        iob_cycle(0);
        CHECK_EQ(nd.ip, 0xC0A8013Eu); CHECK_EQ(nd.address, 2u); CHECK_EQ(nd.master_ip, 0xC0A80132u);
        CHECK_EQ(nd.mask, 0xFFFFFF00u); CHECK_EQ(nd.gw, 0xC0A80101u);
        CHECK_EQ(call("GET", "/api/iob/net", NULL, false), 200);
        CHECK(has("\"op\":\"set\"")); CHECK(has("\"new\":false")); CHECK(has("\"slot\":2"));
        iob_init_eth(NULL, NULL);
        iob_reset();
    }
    CHECK(auth_check(PLAT_LVL_ADMIN, "newpass1"));
    tnow += 2000; store_tick(); store_tick();                    /* let the store save */
    CHECK_EQ(call("GET", "/api/backup", NULL, false), 200);
    CHECK(has("application/octet-stream")); CHECK(has("filename=\"cems-0.bkp\""));
    const char *body = strstr(cap, "\r\n\r\n") + 4;
    size_t blen = capn - (size_t)(body - cap);
    static uint8_t backup[16 * 1024]; memcpy(backup, body, blen);
    CHECK(blen > 40); CHECK(!memcmp(backup, "CEMX", 4));
    h_set = 77; scan_once();
    /* restore: raw body through the parser */
    static char req[20 * 1024];
    int hn = snprintf(req, sizeof req, "POST /api/restore HTTP/1.1\r\nX-CEMS: 1\r\nCookie: sid=%s\r\nContent-Length: %zu\r\n\r\n", sid, blen);
    memcpy(req + hn, backup, blen);
    http_req_t r; int h = http_parse_head(req, (uint32_t)(hn + (int)blen), &r);
    r.body = (const uint8_t *)req + h; r.body_len = r.content_length;
    http_out_t o; out_init(&o, cap_send, NULL); capn = 0;
    http_handle(&r, &o, tnow);
    CHECK(has("200 OK")); CHECK_EQ(h_set, 50);
    backup[blen - 1] ^= 1;                                       /* damaged file */
    memcpy(req + hn, backup, blen); h = http_parse_head(req, (uint32_t)(hn + (int)blen), &r);
    r.body = (const uint8_t *)req + h; r.body_len = r.content_length;
    out_init(&o, cap_send, NULL); capn = 0; http_handle(&r, &o, tnow);
    CHECK(has("400 Bad Request")); CHECK(has("invalid file"));
    CHECK_EQ(call("POST", "/api/logout", "", true), 200); CHECK_EQ(sid[0], 0);
    CHECK_EQ(call("POST", "/api/write", "id=1001&v=1", true), 401);
}

void test_http_all(void) {
    RUN(t_sha256);
    RUN(t_fmt_float);
    RUN(t_text_utils);
    RUN(t_parse_head);
    RUN(t_auth);
    RUN(t_api);
}
