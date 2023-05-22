#include <iomanip>
#include <sstream>

std::string formatSize(uint64_t sizeInByte) {
    std::stringstream sstream;

    if (sizeInByte < 1024) {
        sstream << sizeInByte << "B";
    } else {
        sstream << std::fixed << std::setprecision(2);
        if (sizeInByte / 1024 < 1024) {
            sstream << (double)sizeInByte / 1024 << "KB";
        } else if (sizeInByte / 1024 / 1024 < 1024) {
            sstream << (double)sizeInByte / 1024 / 1024 << "MB";
        } else {
            sstream << (double)sizeInByte / 1024 / 1024 / 1024 << "GB";
        }
    }

    return sstream.str();
}
