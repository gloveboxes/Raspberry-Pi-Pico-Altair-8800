#include "pico_disk.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

// Global disk controller instance
pico_disk_controller_t pico_disk_controller;

// Helper: Set status bit to TRUE (clear bit for active-low)
static inline void set_status(uint8_t bit)
{
    pico_disk_controller.current->status &= ~bit;
}

// Helper: Set status bit to FALSE (set bit for active-low)
static inline void clear_status(uint8_t bit)
{
    pico_disk_controller.current->status |= bit;
}

// Helper: Seek to current track
static void seek_to_track(void)
{
    pico_disk_t *disk = pico_disk_controller.current;
    
    if (!disk->disk_loaded) {
        return;
    }
    
    // Note: Writes are currently discarded (disk is in flash, read-only)
    // Clear dirty flag without writing back
    disk->sector_dirty = false;
    
    uint32_t seek_offset = disk->track * TRACK_SIZE;
    disk->disk_pointer = seek_offset;
    disk->have_sector_data = false;
    disk->sector_pointer = 0;
    disk->sector = 0;
}

// Initialize disk controller
void pico_disk_init(void)
{
    memset(&pico_disk_controller, 0, sizeof(pico_disk_controller_t));
    
    // Initialize all drives
    for (int i = 0; i < MAX_DRIVES; i++) {
        // Start with all status bits inactive (high for active-low bits)
        // Clear SECTOR (bit 3) - sector ready
        // Clear MOVE_HEAD (bit 1) - head not moving (this bit is active-high!)
        pico_disk_controller.disk[i].status = 0xFF & ~STATUS_SECTOR & ~STATUS_MOVE_HEAD;
        pico_disk_controller.disk[i].track = 0;
        pico_disk_controller.disk[i].sector = 0;
        pico_disk_controller.disk[i].disk_loaded = false;
    }
    
    // Select drive 0 by default
    pico_disk_controller.current = &pico_disk_controller.disk[0];
    pico_disk_controller.current_disk = 0;
    
    printf("Pico disk controller initialized\n");
}

// Load disk image into RAM for specified drive
bool pico_disk_load(uint8_t drive, const uint8_t *disk_image, uint32_t size)
{
    if (drive >= MAX_DRIVES) {
        return false;
    }
    
    pico_disk_t *disk = &pico_disk_controller.disk[drive];
    
    // Point directly to flash image (read-only for now)
    // TODO: Add write-back cache for modified sectors if needed
    
    // Initialize disk state (matching Linux initialization)
    disk->disk_image = (uint8_t *)disk_image;  // Cast away const - we'll handle writes separately
    disk->disk_size = size;
    disk->disk_loaded = true;
    disk->disk_pointer = 0;
    disk->sector = 0;
    disk->track = 0;
    disk->sector_pointer = 0;
    disk->sector_dirty = false;
    disk->have_sector_data = false;
    disk->write_status = 0;
    
    // Set initial status (active-low logic for most bits)
    // Clear TRACK_0 (bit 6) - at track 0 (active-low)
    // Clear SECTOR (bit 3) - sector ready (active-low)
    // Clear MOVE_HEAD (bit 1) - head not moving (active-high!)
    disk->status = 0xFF & ~STATUS_TRACK_0 & ~STATUS_SECTOR & ~STATUS_MOVE_HEAD;
    
    printf("Loaded disk image into drive %d (%lu bytes, read-only from flash)\n", drive, size);
    return true;
}

// Select disk drive
void pico_disk_select(uint8_t drive)
{
    uint8_t select = drive & DRIVE_SELECT_MASK;
    
    if (select < MAX_DRIVES) {
        pico_disk_controller.current_disk = select;
        pico_disk_controller.current = &pico_disk_controller.disk[select];
    } else {
        pico_disk_controller.current_disk = 0;
        pico_disk_controller.current = &pico_disk_controller.disk[0];
    }
    
    // When disk is selected (or deselected with 0), mark sector as ready
    set_status(STATUS_SECTOR);  // Clear bit 3 (active-low)
}

// Get disk status
uint8_t pico_disk_status(void)
{
    return pico_disk_controller.current->status;
}

// Disk control function
void pico_disk_function(uint8_t control)
{
    pico_disk_t *disk = pico_disk_controller.current;
    
    if (!disk->disk_loaded) {
        return;
    }
    
    // Step in (increase track)
    if (control & CONTROL_STEP_IN) {
        if (disk->track < MAX_TRACKS - 1) {
            disk->track++;
        }
        if (disk->track != 0) {
            clear_status(STATUS_TRACK_0);
        }
        seek_to_track();
    }
    
    // Step out (decrease track)
    if (control & CONTROL_STEP_OUT) {
        if (disk->track > 0) {
            disk->track--;
        }
        if (disk->track == 0) {
            set_status(STATUS_TRACK_0);
        }
        seek_to_track();
    }
    
    // Head load
    if (control & CONTROL_HEAD_LOAD) {
        set_status(STATUS_HEAD);
        set_status(STATUS_NRDA);
    }
    
    // Head unload
    if (control & CONTROL_HEAD_UNLOAD) {
        clear_status(STATUS_HEAD);
    }
    
    // Write enable
    if (control & CONTROL_WE) {
        set_status(STATUS_ENWD);
        disk->write_status = 0;
    }
}

// Get current sector
uint8_t pico_disk_sector(void)
{
    pico_disk_t *disk = pico_disk_controller.current;
    
    if (!disk->disk_loaded) {
        return 0xC0;  // Invalid sector
    }
    
    // Wrap sector to 0 after reaching end of track
    if (disk->sector == SECTORS_PER_TRACK) {
        disk->sector = 0;
    }
    
    // Clear dirty flag (writes are discarded for read-only flash disk)
    disk->sector_dirty = false;
    
    // Calculate new sector position
    uint32_t seek_offset = disk->track * TRACK_SIZE + disk->sector * SECTOR_SIZE;
    disk->disk_pointer = seek_offset;
    disk->sector_pointer = 0;
    disk->have_sector_data = false;
    
    // Format sector number (88-DCDD specification)
    // D7-D6: Always 1
    // D5-D1: Sector number (0-31)
    // D0: Sector True bit (0 at sector start, 1 otherwise)
    uint8_t ret_val = 0xC0;  // Set D7-D6
    ret_val |= (disk->sector << SECTOR_SHIFT_BITS);  // D5-D1
    ret_val |= (disk->sector_pointer == 0) ? 0 : 1;  // D0
    
    disk->sector++;
    return ret_val;
}

// Write byte to disk
void pico_disk_write(uint8_t data)
{
    pico_disk_t *disk = pico_disk_controller.current;
    
    if (!disk->disk_loaded) {
        return;
    }
    
    // Store in sector buffer (but don't write back to flash)
    disk->sector_data[disk->sector_pointer++] = data;
    disk->sector_dirty = true;
    
    if (disk->write_status == SECTOR_SIZE) {
        // Sector complete - note: writes are discarded for read-only flash disk
        disk->sector_dirty = false;
        disk->write_status = 0;
        clear_status(STATUS_ENWD);
    } else {
        disk->write_status++;
    }
}

// Read byte from disk
uint8_t pico_disk_read(void)
{
    pico_disk_t *disk = pico_disk_controller.current;
    
    if (!disk->disk_loaded) {
        return 0x00;
    }
    
    // Load sector data if not already loaded
    if (!disk->have_sector_data) {
        disk->sector_pointer = 0;
        memset(disk->sector_data, 0x00, SECTOR_SIZE);
        
        uint32_t offset = disk->disk_pointer;
        if (offset + SECTOR_SIZE <= disk->disk_size) {
            memcpy(disk->sector_data, &disk->disk_image[offset], SECTOR_SIZE);
            disk->have_sector_data = true;
        }
    }
    
    // Return current byte and advance pointer within sector
    // Note: Sector positioning is controlled by pico_disk_sector() (port 0x09), not here
    return disk->sector_data[disk->sector_pointer++];
}
