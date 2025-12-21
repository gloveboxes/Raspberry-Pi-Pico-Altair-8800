#pragma once

#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/spi.h"
#include "pico/stdlib.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

// Display dimensions (landscape)
#define ST7789_ASYNC_WIDTH 320
#define ST7789_ASYNC_HEIGHT 240

    // RGB565 color format (16-bit color)
    typedef uint16_t color_t;

    // Create RGB565 color from RGB values with byte swap for SPI
    static inline color_t rgb565(uint8_t r, uint8_t g, uint8_t b)
    {
        // RGB565 with byte swap for little-endian SPI
        uint16_t color = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
        return (color >> 8) | (color << 8);
    }

// Compatibility alias
#define rgb332 rgb565

    // Initialize the async ST7789 driver
    // Returns true on success
    bool st7789_async_init(void);

    // Set a pixel at x,y with given color
    void st7789_async_set_pixel(int x, int y, color_t color);

    // Fill a rectangle (for LED drawing)
    void st7789_async_fill_rect(int x, int y, int w, int h, color_t color);

    // Draw text (capital letters and numbers only)
    void st7789_async_text(const char* str, int x, int y, color_t color);

    // Clear the entire framebuffer to a color
    void st7789_async_clear(color_t color);

    // Start non-blocking DMA transfer to display
    // Returns true if transfer started, false if DMA is busy
    bool st7789_async_update(void);

    // Check if DMA transfer is complete
    bool st7789_async_is_ready(void);

    // Wait for any pending DMA transfer to complete
    void st7789_async_wait(void);

    // Get async statistics
    void st7789_async_get_stats(uint64_t* updates, uint64_t* skipped);

#ifdef __cplusplus
}
#endif
