#ifndef MACHO_HEADER_H
#define MACHO_HEADER_H

#include <mach-o/loader.h>

struct mach_header_64 *parseMachHeader(uint8_t *base);

#endif /* MACHO_HEADER_H */
