#ifndef DYLIB_H
#define DYLIB_H

#include <mach-o/loader.h>

void parse_dylib(void *base, struct dylib_command *);

// If basename is true, only the dylib file name is returned.
// Otherwise it's the whole install name.
// Do not free returned value.
char *get_dylib_name(struct dylib_command *cmd, bool basename);

#endif /* DYLIB_H */
