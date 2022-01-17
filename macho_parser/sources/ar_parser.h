#ifndef AR_PARSER_H
#define AR_PARSER_H

#include <stdio.h>
#include <functional>

bool isArchive(uint8_t *fileBase);
void enumerateObjectFileInArchive(uint8_t *archiveBase, size_t fileSize, std::function<void(char*, uint8_t*)> const& handler);

#endif /* AR_PARSER_H */
