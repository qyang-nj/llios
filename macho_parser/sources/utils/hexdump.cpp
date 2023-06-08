#include <iostream>
#include <iomanip>

// Hex dump a range of memory to stdout
// This method is mostly written by ChatGPT
void hexdump(uint32_t start, const void* data, size_t size)
{
    const unsigned char* bytes = static_cast<const unsigned char*>(data);
    size_t offset = 0;

    for (size_t i = 0; i < size; i += 16)
    {
        // Print the current offset
        std::cout << std::hex << std::setw(8) << std::setfill('0') << start + offset << ": ";

        // Print hex values for this row
        for (size_t j = 0; j < 16; ++j)
        {
            if (i + j < size)
                std::cout << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(bytes[i + j]);
            else
                std::cout << "  ";

            if (j % 2 == 1)
                std::cout << ' ';
        }

        std::cout << ' ';

        // Print ASCII values for this row
        for (size_t j = 0; j < 16; ++j)
        {
            if (i + j < size)
            {
                if (bytes[i + j] >= 32 && bytes[i + j] <= 126)
                    std::cout << static_cast<char>(bytes[i + j]);
                else
                    std::cout << '.';
            }
            else
            {
                std::cout << ' ';
            }
        }

        std::cout << '\n';
        offset += 16;
    }
}
