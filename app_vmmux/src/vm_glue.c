/* vm_glue.c - connections between chunks that the Stage 4 tests made with stubs.
 * Each one says which call it stands for and where that call is specified. */
#include "vm_tags.h"

/* 4E calls SearchInArray (41f) through an out-parameter form; 4A implements the
 * function as specified (returns the index). Same function, same arguments. */
void SearchInArray_u8(const uint8_t *a, int16_t maxv, int16_t *out)
{
    *out = SearchInArray(a, maxv);
}
