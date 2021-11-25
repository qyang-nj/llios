struct MachoBinary {
    uint8_t *base;
    std::vector<struct load_command *> all_load_commands;
    std::vector<struct segment_command_64 *> segment_commands;
};

extern struct MachoBinary machoBinary;
