/* plat_config.h - core platform build-time configuration.
 * Values marked DECISION are proposals awaiting sign-off (docs/PLATFORM.md section 6).
 * Any of them can be overridden with -D on the compiler command line. */
#ifndef PLAT_CONFIG_H
#define PLAT_CONFIG_H

/* ---- scan (VM/MUX sequence logic) ---- */
#ifndef PLAT_SCAN_PERIOD_MS
#define PLAT_SCAN_PERIOD_MS       10u    /* DECISION D1: 10 ms, matches g_dt_ms used in Stage 4G/4H/4I tests */
#endif
#ifndef PLAT_SCAN_MAX_CMDS
#define PLAT_SCAN_MAX_CMDS        PLAT_CMDQ_DEPTH /* apply everything queued: a multi-value write never straddles scans */
#endif
#ifndef PLAT_CMDQ_DEPTH
#define PLAT_CMDQ_DEPTH           128u   /* pending scalar writes (UI + web + Modbus); one FC16 can carry 123 */
#endif

/* ---- parameter FRAM ---- */
#ifndef PLAT_FRAM_SIZE
#define PLAT_FRAM_SIZE            131072u /* DECISION D6 (Gary, 20 Oct): 128 KB product part (MB85RC1MT); 32768u for the
                                            32 KB development breakout (MB85RC256V) */
#endif

/* ---- fast control loops ---- */
#ifndef PLAT_CTRL_PERIOD_MS
#define PLAT_CTRL_PERIOD_MS       1u     /* DECISION D2: 1 kHz task tick until the loop list is known */
#endif

/* ---- supervisor / watchdog ---- */
#ifndef PLAT_SUPERVISOR_PERIOD_MS
#define PLAT_SUPERVISOR_PERIOD_MS 100u
#endif
#ifndef PLAT_STARTUP_GRACE_MS
#define PLAT_STARTUP_GRACE_MS     5000u  /* time a task has to make its first check-in */
#endif
/* Maximum silence per task before the supervisor stops kicking the IWDG.
 * The IWDG timeout itself is set in CubeMX (docs/CUBEMX_SETUP.md, about 1 s). */
#define PLAT_SILENCE_CTRL_MS      50u
#define PLAT_SILENCE_SCAN_MS      200u
#define PLAT_SILENCE_STORE_MS     2000u
#define PLAT_SILENCE_MODBUS_MS    10000u /* up to 4 slow clients x 1 s send timeout */
#define PLAT_SILENCE_HTTP_MS      5000u
#define PLAT_SILENCE_UI_MS        5000u

/* ---- FreeRTOS priorities (configMAX_PRIORITIES must be >= 16) ----
 * Design spec Rev 1.00 "Firmware architecture": control highest, then parameter
 * store and logger, then Modbus/HTTP, UI lowest. Nothing below the scan can delay it. */
#define PLAT_PRIO_CTRL            14     /* Highest tier: fast control loops   */
#define PLAT_PRIO_SCAN            13     /* Highest tier: VM/MUX sequence scan */
#define PLAT_PRIO_SUPERVISOR      12     /* watchdog supervisor (short, periodic) */
#define PLAT_PRIO_IOBUS           11     /* RS-485 I/O bus master (iobus.h): a fixed 20 ms cycle */
#define PLAT_PRIO_STORE           10     /* High tier: parameter store + logger owner */
#define PLAT_PRIO_ETH_RX          9      /* Ethernet receive (ST runs it highest; here below control) */
#define PLAT_PRIO_TCPIP           8      /* Medium tier: lwIP tcpip_thread (TCPIP_THREAD_PRIO) */
#define PLAT_PRIO_MODBUS          7      /* Medium tier: Modbus TCP server */
#define PLAT_PRIO_HTTP            6      /* Medium tier: HTTP server */
#define PLAT_PRIO_NET             4      /* network bring-up, link and address changes */
#define PLAT_PRIO_UI              3      /* Lowest tier: touch UI */

/* ---- stacks, in 32-bit words (FreeRTOS units) ---- */
#define PLAT_STACK_CTRL           512
#define PLAT_STACK_SCAN           2048   /* Stage 4 code uses large locals in places */
#define PLAT_STACK_SUPERVISOR     384
#define PLAT_STACK_STORE          1024
#define PLAT_STACK_MODBUS         1024
#define PLAT_STACK_HTTP           1536
#define PLAT_STACK_UI             2048
#define PLAT_STACK_ETH_RX         512
#define PLAT_STACK_NET            768
#define PLAT_STACK_IOBUS          768

/* ---- network services ---- */
#ifndef PLAT_MODBUS_PORT
#define PLAT_MODBUS_PORT          502
#endif
#ifndef PLAT_HTTPS_PORT
#define PLAT_HTTPS_PORT           443    /* DECISION D11: the web interface is HTTPS only */
#endif
#ifndef PLAT_HTTP_PORT
#define PLAT_HTTP_PORT            80     /* only redirects to https:// (parameter 6 turns it off) */
#endif
#define PLAT_SILENCE_NET_MS       5000u
#define PLAT_SILENCE_IOBUS_MS     1000u

/* ---- RS-485 I/O bus (iobus.h, D27) ---- */
#ifndef PLAT_IOB_PERIOD_MS
#define PLAT_IOB_PERIOD_MS        20u    /* one request per running board per cycle */
#endif
#define PLAT_IOB_REPLY_MS         15u    /* wait this long for a node's reply (about 3 frames' time) */
#define PLAT_IOB_RETRY_MS         1000u  /* a board that is not answering is tried this often */
#define PLAT_IOB_LOST_MS          1000u  /* no valid reply this long: the board is "not responding" */
#define PLAT_IOB_STALE_MS         250u   /* outputs not refreshed by the scan this long: send all-off */
#define PLAT_IOB_BAUD             115200u

#endif
