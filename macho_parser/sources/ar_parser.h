#ifndef AR_PARSER_H
#define AR_PARSER_H

#include <stdio.h>
#include <functional>

namespace Archive {
bool isArchive(uint8_t *fileBase, uint32_t fileSize);
void enumerateObjectFileInArchive(uint8_t *archiveBase, uint32_t fileSize, std::function<void(char*, uint8_t*)> const& handler);
}

#endif /* AR_PARSER_H */
