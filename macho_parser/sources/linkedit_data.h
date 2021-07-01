#ifndef LINKEDIT_DATA_H
#define LINKEDIT_DATA_H

#include <stdio.h>
#include <mach-o/loader.h>

void parse_linkedit_data(void *base, struct linkedit_data_command *);

#endif /* LINKEDIT_DATA_H */
