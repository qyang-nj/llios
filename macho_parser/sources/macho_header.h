#ifndef MACHO_HEADER_H
#define MACHO_HEADER_H

#include <mach-o/loader.h>

#ifdef __cplusplus
extern "C" {
#endif

struct mach_header_64 *parse_mach_header(uint8_t *base);

#ifdef __cplusplus
}
#endif

#endif /* MACHO_HEADER_H */
