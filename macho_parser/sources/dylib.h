#ifndef DYLIB_H
#define DYLIB_H

#include <mach-o/loader.h>

void parse_dylib(void *base, struct dylib_command *);

#endif /* DYLIB_H */
