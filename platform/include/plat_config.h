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
#define PLAT_PRIO_IOBUS           11     /* I/O bus master (iobus.h): a fixed 20 ms cycle */
#define PLAT_PRIO_STORE           10     /* High tier: parameter store + logger owner */
#define PLAT_PRIO_ETH_RX          9      /* Ethernet receive (ST runs it highest; here below control) */
#define PLAT_PRIO_TCPIP           8      /* Medium tier: lwIP tcpip_thread (TCPIP_THREAD_PRIO) */
#define PLAT_PRIO_MODBUS          7      /* Medium tier: Modbus TCP server */
#define PLAT_PRIO_HTTP            6      /* Medium tier: HTTP server */
#define PLAT_PRIO_NET             4      /* network bring-up, link and address changes */
#define PLAT_PRIO_UI              3      /* Lowest tier: touch UI */

/* ---- stacks, in 32-bit words (FreeRTOS units) ----
 * Generous on purpose (Gary, 25 Oct: stack overflows on earlier DK prototypes): about
 * twice what each task was first given, 85 KB in all. Trim only after the 72-hour soak
 * on hardware shows the margins (`health` on /api/info, System screen); RESOURCE_LOW
 * (alarm 11) warns when any margin falls under PLAT_STACK_LOW. A task that overflows
 * anyway is stopped by the CPU's stack limit (PSPLIM) and named in the fault record. */
#ifndef PLAT_STACK_CTRL
#define PLAT_STACK_CTRL           1024
#endif
#ifndef PLAT_STACK_SCAN
#define PLAT_STACK_SCAN           4096   /* Stage 4 code uses large locals in places */
#endif
#ifndef PLAT_STACK_SUPERVISOR
#define PLAT_STACK_SUPERVISOR     768
#endif
#ifndef PLAT_STACK_STORE
#define PLAT_STACK_STORE          2048
#endif
#ifndef PLAT_STACK_MODBUS
#define PLAT_STACK_MODBUS         2048
#endif
#ifndef PLAT_STACK_HTTP
#define PLAT_STACK_HTTP           3072
#endif
#ifndef PLAT_STACK_UI
#define PLAT_STACK_UI             4096
#endif
#ifndef PLAT_STACK_ETH_RX
#define PLAT_STACK_ETH_RX         1024
#endif
#ifndef PLAT_STACK_NET
#define PLAT_STACK_NET            1536
#endif
#ifndef PLAT_STACK_IOBUS
#define PLAT_STACK_IOBUS          2048   /* Modbus TCP and discovery buffers (iob_tcp.c) */
#endif

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
#ifndef PLAT_HTTP_BUF
#define PLAT_HTTP_BUF             8192u  /* web request buffer: headers and form bodies, log reads in 128-record
                                            chunks. Backup files use the store's staging buffer and firmware
                                            uploads stream, so neither needs it to be large (was 49 KB) */
#endif
#define PLAT_SILENCE_NET_MS       5000u
#define PLAT_SILENCE_IOBUS_MS     1000u

/* ---- I/O bus, RS-485 or Ethernet (iobus.h, D27) ---- */
#ifndef PLAT_LOOP_RESETS
#define PLAT_LOOP_RESETS          3u     /* crashes ... (CIP-08.5) */
#endif
#ifndef PLAT_LOOP_WINDOW_MS
#define PLAT_LOOP_WINDOW_MS       600000u /* ... within this much running time: start in SAFE_MODE */
#endif

/* the 24 V supply (plat_power.h; CORE_PCB.md 12 suggests these) */
#ifndef PLAT_SUPPLY_LOW_MV
#define PLAT_SUPPLY_LOW_MV        19000u /* alarm SUPPLY below this ... */
#endif
#ifndef PLAT_SUPPLY_HIGH_MV
#define PLAT_SUPPLY_HIGH_MV       30000u /* ... or above this */
#endif
#ifndef PLAT_SUPPLY_FAIL_MV
#define PLAT_SUPPLY_FAIL_MV       16000u /* failing: no new FRAM or log-flash writes */
#endif
#ifndef PLAT_SUPPLY_RESTORE_MV
#define PLAT_SUPPLY_RESTORE_MV    18000u /* back above this for 1 s: writes resume */
#endif

/* interface versions (CIP-14.4): bump on any change a client could break on */
#define PLAT_API_VERSION          2u     /* the web API (1 = before 26 Oct 2026) */
#define PLAT_MB_BLOCK_VERSION     3u     /* the layout of the platform's Modbus block (2: clock quality, 26 Oct; 3: forced count, 29 Oct) */
#define PLAT_MB_BLOCK_ADDR        9000u  /* input registers 9000-9199 belong to the platform in every product */
#define PLAT_MB_BLOCK_END         9199u

#ifndef PLAT_MEAS_MAX
#define PLAT_MEAS_MAX             32u    /* measurements an application may declare (plat_state.h) */
#endif

#ifndef PLAT_IOB_PERIOD_MS
#define PLAT_IOB_PERIOD_MS        20u    /* one request per running board per cycle */
#endif
#define PLAT_IOB_REPLY_MS         15u    /* wait this long for a node's reply (about 3 frames' time) */
#define PLAT_IOB_RETRY_MS         1000u  /* a board that is not answering is tried this often */
#define PLAT_IOB_LOST_MS          1000u  /* no valid reply this long: the board is "not responding" */
#define PLAT_IOB_STALE_MS         250u   /* outputs not refreshed by the scan this long: send all-off */
#define PLAT_IOB_BAUD             115200u
#define PLAT_IOB_TEMP_MS          500u   /* map 2: a board's temperatures are read this often (one board per cycle) */
/* Ethernet link (iob_net.h) */
#define PLAT_IOB_TCP_REPLY_MS     15u    /* a board's Modbus TCP reply (same budget as RS-485: 3 boards per cycle) */
#define PLAT_IOB_TCP_CONNECT_MS   50u    /* a TCP connection to a board, tried once per retry */
#define PLAT_IOB_DISC_WAIT_MS     300u   /* answers to a discovery broadcast are collected this long */

#endif
