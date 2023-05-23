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

std::string formatBufferToHex(const uint8_t *buffer, size_t bufferSize) {
    std::stringstream ss;
    ss << std::hex << std::setfill('0');

    for (size_t i = 0; i < bufferSize; ++i) {
        ss << std::setw(2) << static_cast<unsigned int>(buffer[i]);
    }

    return ss.str();
}

// If the string contains '\n', replace with literal "\n".
std::string formatStringLiteral(const char *str) {
    std::stringstream ss;
    for (int i = 0; str[i] != '\0'; ++i) {
        switch(str[i]) {
            case '\n':
                ss << "\\n";
                break;
            case '\r':
                ss << "\\r";
                break;
            case '\\':
                ss << "\\\\";
                break;
            default:
                ss << str[i];
                break;
        }
    }

    return ss.str();
}
