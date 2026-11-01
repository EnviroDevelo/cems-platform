/* ul_blocks.c - UniLogic blocks shared by several chunks (one definition each).
 *
 * The scan-level element models (ton, tacc, tp, pcont, ncont, pcoil) stay static in
 * each chunk file as Stage 4 wrote them; they were checked identical across chunks
 * on integration (I-5). Only the externally visible ones live here. */
#include "vm_tags.h"

/* TimerReset (11 section 4): Current back to Preset, Out and On off. */
void treset(ul_timer_t *t) { t->Current = t->Preset; t->Out = false; t->On = false; }
