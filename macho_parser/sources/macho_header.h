#ifndef MACHO_HEADER_H
#define MACHO_HEADER_H

#include <mach-o/loader.h>
#include <tuple>

namespace FatMacho {
bool isFatMacho(uint8_t *fileBase, size_t fileSize);
std::tuple<uint8_t*, uint32_t> getSliceByArch(uint8_t *fileBase, size_t fileSize, char *arch);
}

struct mach_header_64 *parseMachHeader(uint8_t *base);

#endif /* MACHO_HEADER_H */
