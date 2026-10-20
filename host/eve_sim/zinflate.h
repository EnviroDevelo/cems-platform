/* zinflate.h - zlib stream decoder for the simulator (zinflate.c). */
#ifndef ZINFLATE_H
#define ZINFLATE_H
#include <stddef.h>
#include <stdint.h>
/* 0 = done (inused bytes of input, outlen bytes out), 1 = needs more input, -1 = bad data or no room */
int zinflate(const uint8_t *in, size_t inlen, uint8_t *out, size_t outcap, size_t *inused, size_t *outlen);
#endif
