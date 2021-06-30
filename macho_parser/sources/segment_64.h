#ifndef SEGMENT_64_H
#define SEGMENT_64_H

#include <mach-o/loader.h>

void parse_segment(void *base, struct segment_command_64 *seg_cmd);

#endif /* SEGMENT_64_H */
