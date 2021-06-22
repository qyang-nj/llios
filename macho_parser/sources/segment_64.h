#ifndef SEGMENT_64_H
#define SEGMENT_64_H

#include <stdio.h>
#include <mach-o/loader.h>

void parse_segment(FILE *fptr, struct segment_command_64 *seg_cmd);

#endif /* SEGMENT_64_H */
