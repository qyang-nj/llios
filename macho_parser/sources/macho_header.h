#ifndef MACHO_HEADER_H
#define MACHO_HEADER_H

#include <stdio.h>
#include <mach-o/loader.h>

struct load_cmd_info {
    // The offset of the first load command
    int offset;
    // The number of load command
    int count;
};

struct load_cmd_info parse_header(FILE *fptr);

#endif /* MACHO_HEADER_H */
