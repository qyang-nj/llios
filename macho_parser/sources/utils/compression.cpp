#include <zlib.h>
#include <iostream>

#include "utils.h"

// This function is mostly written by ChatGPT.
void decompressZlibData(const uint8_t *inputData, size_t inputSize, uint8_t *outputData, size_t outputSize) {
    z_stream strm;
    strm.zalloc = Z_NULL;
    strm.zfree = Z_NULL;
    strm.opaque = Z_NULL;
    strm.avail_in = inputSize;
    strm.next_in = (Bytef*)inputData;
    strm.avail_out = outputSize;
    strm.next_out = (Bytef*)outputData;

    int ret = inflateInit(&strm);
    if (ret != Z_OK) {
        std::cerr << "Error initializing zlib inflate: " << ret << std::endl;
        return;
    }

    ret = inflate(&strm, Z_FINISH);
    if (ret != Z_STREAM_END) {
        std::cerr << "Error decompressing zlib data: " << ret << std::endl;
        inflateEnd(&strm);
        return;
    }

    inflateEnd(&strm);
}
