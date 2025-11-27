#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include <stdio.h>
#include "Altair8800/intel8080.h"
#include "Altair8800/memory.h"
#include "Altair8800/pico_disk.h"

#define ASCII_MASK_7BIT 0x7F

// Include the CPM disk image
#include "Altair8800/cpm63k_disk.h"

// Global CPU instance
static intel8080_t cpu;

// Terminal read function - non-blocking
static char terminal_read(void)
{
    int c = getchar_timeout_us(0); // Non-blocking read
    if (c == PICO_ERROR_TIMEOUT)
    {
        return 0x00; // Return null if no character available
    }
    return (char)(c & ASCII_MASK_7BIT);
}

// Terminal write function
static void terminal_write(uint8_t c)
{
    c &= ASCII_MASK_7BIT; // Take first 7 bits only
    putchar(c);
}

// Sense switches stub
static inline uint8_t sense(void)
{
    return 0x00; // No sense switches on Pico
}

// Port I/O for disk controller
static void io_port_out(uint8_t port, uint8_t data)
{
    switch (port) {
        case 0x08:  // Disk select
            pico_disk_select(data);
            break;
        case 0x09:  // Disk control
            pico_disk_function(data);
            break;
        case 0x0A:  // Disk write
            pico_disk_write(data);
            break;
        default:
            // Other ports - do nothing
            break;
    }
}

static uint8_t io_port_in(uint8_t port)
{
    switch (port) {
        case 0x08:  // Disk status
            return pico_disk_status();
        case 0x09:  // Sector position
            return pico_disk_sector();
        case 0x0A:  // Disk read
            return pico_disk_read();
        default:
            return 0x00;
    }
}

int main(void)
{
    // Initialize stdio first
    stdio_init_all();

    // Give more time for USB serial to enumerate
    sleep_ms(3000);

    // Initialise Wi-Fi/BT chip (needed for LED!)
    bool led_available = false;
    if (cyw43_arch_init() == 0)
    {
        led_available = true;
        // Blink LED to show we're alive
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 1);
        sleep_ms(500);
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 0);
        sleep_ms(500);
    }

    // Wait for user to press Enter (non-blocking check)
    bool enter_pressed = false;
    while (!enter_pressed)
    {
        int c = getchar_timeout_us(100000); // Check every 100ms
        if (c == '\n' || c == '\r')
        {
            enter_pressed = true;
        }
        // Blink LED while waiting
        if (led_available)
        {
            static uint32_t blink_count = 0;
            if (++blink_count % 5 == 0)
            { // Every 500ms
                static bool led_on = false;
                led_on = !led_on;
                cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, led_on);
            }
        }
    }

    // Send test output
    printf("\n\n*** USB Serial Active ***\n");
    printf("========================================\n");
    printf("  Altair 8800 Emulator - Pico 2 W\n");
    printf("========================================\n");
    printf("\n");

    // Initialize disk controller
    printf("Initializing disk controller...\n");
    pico_disk_init();
    
    // Load CPM disk image into drive 0 (DISK_A)
    printf("Opening DISK_A: cpm63k.dsk\n");
    if (pico_disk_load(0, cpm63k_dsk, cpm63k_dsk_len)) {
        printf("DISK_A opened successfully (%u bytes)\n", cpm63k_dsk_len);
    } else {
        printf("DISK_A initialization failed!\n");
        return -1;
    }

    // Load disk boot loader ROM at 0xFF00 (ROM_LOADER_ADDRESS)
    printf("Loading disk boot loader ROM at 0xFF00...\n");
    loadDiskLoader(0xFF00);

    // Set up disk controller structure for CPU
    disk_controller_t disk_controller = {
        .disk_select = (port_out)pico_disk_select,
        .disk_status = (port_in)pico_disk_status,
        .disk_function = (port_out)pico_disk_function,
        .sector = (port_in)pico_disk_sector,
        .write = (port_out)pico_disk_write,
        .read = (port_in)pico_disk_read
    };

    // Reset and initialize the CPU
    printf("Initializing Intel 8080 CPU...\n");
    i8080_reset(&cpu,
                (port_in)terminal_read,
                (port_out)terminal_write,
                sense,
                &disk_controller,
                (azure_sphere_port_in)io_port_in,
                (azure_sphere_port_out)io_port_out);

    // Set CPU to start at ROM_LOADER_ADDRESS (0xFF00) to boot from disk
    printf("Setting CPU to ROM_LOADER_ADDRESS (0xFF00) to boot from disk\n");
    i8080_examine(&cpu, 0xFF00);

    // Report memory usage
    extern char __StackLimit, __bss_end__;
    extern char __flash_binary_end;

    uint32_t heap_free = &__StackLimit - &__bss_end__;
    uint32_t total_ram = 512 * 1024; // Pico 2 W has 512KB SRAM
    uint32_t used_ram = total_ram - heap_free;
    uint32_t flash_used = (uint32_t)&__flash_binary_end;

    printf("\n");
    printf("Memory Report:\n");
    printf("  Flash used:     %lu bytes (%.1f KB)\n", flash_used, flash_used / 1024.0f);
    printf("  RAM used:       %lu bytes (%.1f KB)\n", used_ram, used_ram / 1024.0f);
    printf("  RAM free (heap):%lu bytes (%.1f KB)\n", heap_free, heap_free / 1024.0f);
    printf("  Total SRAM:     %lu bytes (512 KB)\n", total_ram);
    printf("  Altair memory:  65536 bytes (64 KB)\n");
    printf("\n");

    printf("Starting Altair 8800 emulation...\n");
    printf("\n");

    // Blink LED to show we're running
    uint32_t cycle_count = 0;

    // Main emulation loop
    while (true)
    {
        i8080_cycle(&cpu);

        // Blink LED every ~100000 cycles to show activity
        if (led_available)
        {
            cycle_count++;
            if (cycle_count >= 100000)
            {
                static bool led_state = false;
                led_state = !led_state;
                cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, led_state);
                cycle_count = 0;
            }
        }
    }
}