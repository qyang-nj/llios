#ifndef DYSYMTAB_H
#define DYSYMTAB_H

#include <stdio.h>
#include <mach-o/loader.h>

void parse_dynamic_symbol_table(void *base, struct dysymtab_command *);

#endif /* DYSYMTAB_H */
