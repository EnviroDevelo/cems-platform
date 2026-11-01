/* ul_models.c - stand-ins for UniLogic panel objects the old program writes to,
 * where the new platform has no equivalent (yet). They keep the faithful logic
 * running unchanged; what replaces each one is in INTEGRATION.md (disposition).
 *
 *   g_csv / g_csv_lines   the file /UserLogs/Csv/scheduleinlets.csv that the MUX
 *                         sequence editor writes its inlet list into, for the old
 *                         panel's combo boxes (45e). The new UI reads the valve
 *                         names directly, so this stays a RAM copy. */
#include "vm_tags.h"

char g_csv[24][24];
int  g_csv_lines;
