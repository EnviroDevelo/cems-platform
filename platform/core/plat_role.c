/* plat_role.c - see plat_role.h */
#include "plat_role.h"

static plat_role_t role = PLAT_ROLE_CORE;

plat_role_t plat_role_decode(bool fitted, bool a, bool b) {
    if (!fitted) return PLAT_ROLE_CORE;
    if (!a && b) return PLAT_ROLE_CORE;
    if (a && !b) return PLAT_ROLE_NODE;
    return PLAT_ROLE_NONE;
}

plat_role_t plat_role_init(void) {
#if defined(PLAT_ROLE_FIXED)
    role = (plat_role_t)(PLAT_ROLE_FIXED);
#else
    bool a = true, b = true;
    bool fitted = bsp_role_read(&a, &b);
    role = plat_role_decode(fitted, a, b);
#endif
    return role;
}

plat_role_t plat_role(void) { return role; }

const char *plat_role_name(plat_role_t r) {
    return r == PLAT_ROLE_CORE ? "core" : r == PLAT_ROLE_NODE ? "node" : "none";
}
