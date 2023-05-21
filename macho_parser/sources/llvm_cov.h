#ifndef MACHO_PARSER_COVERAGE_H
#define MACHO_PARSER_COVERAGE_H

#include <mach-o/loader.h>

#ifdef __cplusplus
extern "C" {
#endif

void printCovMapSection(uint8_t *base, struct section_64 *sect);

void printCovFunSection(uint8_t *base, struct section_64 *sect);

#ifdef __cplusplus
}
#endif

#endif /* MACHO_PARSER_COVERAGE_H */
