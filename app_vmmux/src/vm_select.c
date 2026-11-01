#include "plat_tasks.h"
#include "vm_app.h"
const plat_app_t *app_select(void) { return vmmux_app(); }
