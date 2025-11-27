#include "memory.h"

// Altair system memory - 64KB
uint8_t memory[64 * 1024] = {0};

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
