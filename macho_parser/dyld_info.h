#ifndef DYLD_INFO_H
#define DYLD_INFO_H

#include <stdio.h>
#include <mach-o/loader.h>

void parse_dyld_info(FILE *fptr, struct dyld_info_command *dyld_info_cmd);

#endif /* DYLD_INFO_H */
