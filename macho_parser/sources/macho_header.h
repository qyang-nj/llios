#ifndef MACHO_HEADER_H
#define MACHO_HEADER_H

#include <mach-o/loader.h>

struct load_cmd_info {
    // The offset of the first load command
    int offset;
    // The number of load command
    int count;
};

struct load_cmd_info parse_header(void *base);

#endif /* MACHO_HEADER_H */
