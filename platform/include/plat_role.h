/* plat_role.h - one firmware, two roles (the shared CORE / I/O node PCB, 30 Oct). The role is
 * decided once at power-up, before any task starts, and never changes while running:
 *
 *   CORE  the supervisor: display, HTTPS, plant Modbus, the application, I/O board master
 *   NODE  an I/O board: answers the CORE's Modbus TCP and discovery (iob_net.h) with the node core
 *         (node/node.c), drives its outputs and reads its inputs; no display, no web, no plant
 *         Modbus, no application (plat_node.h)
 *   NONE  the jumper is missing or broken: the CORE services run so the board can be reached, but
 *         without the application, so every output stays off (as after a reboot loop)
 *
 * Where it comes from:
 *   - PLAT_ROLE_FIXED defined (1 CORE, 2 NODE): that role. For boards without the jumper: the
 *     STM32H573I-DK is CORE, the NUCLEO-H563ZI is NODE (docs/CUBEMX_NUCLEO_NODE.md).
 *   - else the role jumper J7 (CORE_PCB.md 12) through bsp_role_read(): ROLE_A (PE5) and ROLE_B
 *     (PE6), 10k pull-ups on the board, the shunt pulls one to 0 V:
 *       PE5 0, PE6 1 (shunt 1-2) = CORE;  PE5 1, PE6 0 (shunt 2-3) = NODE;  11 or 00 = NONE
 *   - a board without the jumper's labels (bsp_role_read false) is a CORE, as every board was
 *     before the shared PCB. */
#ifndef PLAT_ROLE_H
#define PLAT_ROLE_H
#include <stdbool.h>

typedef enum { PLAT_ROLE_NONE = 0, PLAT_ROLE_CORE = 1, PLAT_ROLE_NODE = 2 } plat_role_t;

/* The board's jumper: false = the board has none. a, b = ROLE_A, ROLE_B read high. */
bool        bsp_role_read(bool *a, bool *b);

plat_role_t plat_role_decode(bool fitted, bool a, bool b);   /* the table above (tests) */
plat_role_t plat_role_init(void);   /* plat_start: decides it, once */
plat_role_t plat_role(void);        /* as decided (CORE before plat_role_init) */
const char *plat_role_name(plat_role_t r);   /* "core", "node", "none" */

#endif
