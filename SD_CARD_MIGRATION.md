# SD Card Disk Controller Migration

## Overview
The Altair 8800 emulator now supports two disk controller modes:
- **Embedded XIP Mode** (default): Disks stored in flash memory (2 drives)
- **SD Card Mode**: Disks stored on SD card (4 drives)

## Implementation Details

### Files Modified

#### 1. `Altair8800/pico_88dcdd_sd_card.h`
- Migrated from Linux filesystem APIs to FatFs APIs
- Changed from `int fp` to `FIL fil` for file handles
- Updated to use Pico SD card driver constants and types
- Added disk file path definitions for 4 drives

#### 2. `Altair8800/pico_88dcdd_sd_card.c`
- Replaced POSIX file operations with FatFs equivalents:
  - `open()` → `f_open()`
  - `close()` → `f_close()`
  - `read()` → `f_read()`
  - `write()` → `f_write()`
  - `lseek()` → `f_lseek()`
- Added `f_sync()` after writes to ensure data is flushed to SD card
- Updated function signatures to match embedded disk controller interface
- Added error handling with printf for debugging

#### 3. `main.c`
- Conditional includes based on `SD_CARD_SUPPORT` macro
- Separate initialization paths for embedded vs SD card modes
- SD card mode loads 4 disk images from SD card:
  - DISK_A: `Disks/cpm63k.dsk`
  - DISK_B: `Disks/bdsc-v1.60.dsk`
  - DISK_C: `Disks/escape-posix.dsk`
  - DISK_D: `Disks/blank.dsk`
- Embedded mode loads 2 disk images from XIP flash:
  - DISK_A: cpm63k.dsk (embedded)
  - DISK_B: blank.dsk (embedded)
- Disk loader ROM still loaded from XIP flash in both modes

#### 4. `CMakeLists.txt`
- Conditional compilation of disk controller source:
  - `SD_CARD_SUPPORT=ON` → compile `pico_88dcdd_sd_card.c`
  - `SD_CARD_SUPPORT=OFF` → compile `pico_disk.c`

### Disk Controller Interface

Both disk controllers implement the same 88-DCDD compatible interface:

```c
void disk_select(uint8_t drive);
uint8_t disk_status(void);
void disk_function(uint8_t control);
uint8_t disk_sector(void);
void disk_write(uint8_t data);
uint8_t disk_read(void);
```

**Embedded Mode**: `pico_disk_*` functions
**SD Card Mode**: `sd_disk_*` functions

### Build Configuration

#### Build with SD Card Support (4 drives from SD card):
```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DPICO_BOARD=pico2_w -DSD_CARD_SUPPORT=ON
cmake --build build -- -j
```

#### Build without SD Card Support (2 embedded drives):
```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DPICO_BOARD=pico2_w -DSD_CARD_SUPPORT=OFF
cmake --build build -- -j
```

### SD Card Setup

When using SD card mode:

1. Format SD card as FAT32
2. Create a `Disks/` folder on the SD card
3. Copy disk image files to the `Disks/` folder:
   - `cpm63k.dsk`
   - `bdsc-v1.60.dsk`
   - `escape-posix.dsk`
   - `blank.dsk`
4. Optionally add a `readme.md` file in the `Disks/` folder for documentation

### Pin Configuration

SD Card uses the following pins:
- CS: GPIO 22
- SCK: GPIO 18
- MOSI: GPIO 19
- MISO: GPIO 16

**Note**: SD card MISO (GPIO 16) conflicts with Display 2.8 DC pin. Cannot use both simultaneously.

### Key Differences

| Feature | Embedded Mode | SD Card Mode |
|---------|--------------|--------------|
| Number of drives | 2 | 4 |
| Disk storage | XIP Flash | SD Card |
| Write support | Copy-on-Write (RAM) | Direct file write |
| Disk size | Fixed at compile time | Dynamic (file size) |
| Persistence | Resets on reboot | Persists to SD card |
| Memory usage | Uses patch table in RAM | Buffers one sector |

### API Mapping

| POSIX API | FatFs API |
|-----------|-----------|
| `open(path, O_RDWR)` | `f_open(&fil, path, FA_READ \| FA_WRITE)` |
| `close(fd)` | `f_close(&fil)` |
| `read(fd, buf, size)` | `f_read(&fil, buf, size, &bytes_read)` |
| `write(fd, buf, size)` | `f_write(&fil, buf, size, &bytes_written)` |
| `lseek(fd, offset, SEEK_SET)` | `f_lseek(&fil, offset)` |
| N/A | `f_sync(&fil)` - flush to disk |

## Testing

After building with SD card support:

1. Insert formatted SD card with disk images
2. Power on device
3. Monitor serial output for SD card initialization
4. Boot CP/M and verify disk operations
5. Test writes persist across reboots

## Future Enhancements

Possible improvements:
- Hot-swap disk images via web interface
- Support for more than 4 drives
- Disk image creation tool
- Performance optimizations (sector caching)
