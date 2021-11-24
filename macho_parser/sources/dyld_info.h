#ifndef DYLD_INFO_H
#define DYLD_INFO_H

#include <mach-o/loader.h>

void printDyldInfo(uint8_t *base, struct dyld_info_command *dyldInfoCmd);

#endif /* DYLD_INFO_H */
