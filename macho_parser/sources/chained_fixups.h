#ifndef CHAINED_FIXUPS_H
#define CHAINED_FIXUPS_H

#include <stdio.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

void parse_chained_fixups(uint8_t *base, uint32_t dataoff, uint32_t datasize);

#ifdef __cplusplus
}
#endif

#endif /* CHAINED_FIXUPS_H */
