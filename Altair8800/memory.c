#include "memory.h"

// Altair system memory - 64KB
uint8_t memory[64 * 1024] = {0};

// Load disk boot loader ROM into memory at specified address
void loadDiskLoader(uint16_t address)
{
    // Disk boot loader ROM data from 88dskrom.bin
    const uint8_t rom_data[] = {
        #include "88dskrom.h"
    };
    
    // Copy ROM data to memory
    for (uint16_t i = 0; i < sizeof(rom_data); i++) {
        memory[address + i] = rom_data[i];
    }
}

// Load 4K BASIC ROM into memory at specified address
void load4kRom(uint16_t address)
{
    // 4K BASIC ROM data
    const uint8_t rom_data[] = {
        #include "4kbas32.h"
    };
    
    // Copy ROM data to memory
    for (uint16_t i = 0; i < sizeof(rom_data); i++) {
        memory[address + i] = rom_data[i];
    }
}

// Load 8K BASIC ROM into memory at specified address
void load8kRom(uint16_t address)
{
    // 8K BASIC ROM data
    const uint8_t rom_data[] = {
        #include "8krom.h"
    };
    
    // Copy ROM data to memory
    for (uint16_t i = 0; i < sizeof(rom_data); i++) {
        memory[address + i] = rom_data[i];
    }
}

// Memory functions are now inlined in memory.h for better performance
