#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * Initialize OpenAI IO subsystem
 * Creates queues for inter-core communication
 * Must be called before websocket_console_start() on Core 0
 */
void openai_io_init(void);

/**
 * OpenAI port output handler
 * Called from io_port_out() on Core 0 (Altair emulator)
 *
 * Port mapping:
 *   120: Reset request buffer
 *   121: Add byte to request body chunk (NULL byte signals end of body)
 *   122: Reset response buffer
 *   126: Set content length low byte
 *   127: Set content length high byte (enables request trigger)
 *
 * @param port Port number (120-127)
 * @param data Data byte written to port
 * @param buffer Output buffer (unused, for API compatibility)
 * @param buffer_length Size of output buffer
 * @return Number of bytes written to buffer (always 0)
 */
size_t openai_output(uint8_t port, uint8_t data, char* buffer, size_t buffer_length);

/**
 * OpenAI port input handler
 * Called from io_port_in() on Core 0 (Altair emulator)
 *
 * Port mapping:
 *   120: Trigger API call, return 1=success, 0=fail
 *   121: Get request buffer length low byte
 *   122: Get request buffer length high byte
 *   123: Get status (0=EOF, 1=WAITING, 2=DATA_READY)
 *   124: Read one byte from response buffer
 *   125: Check if stream complete (1=complete, 0=ongoing)
 *
 * @param port Port number (120-127)
 * @return Data byte read from port
 */
uint8_t openai_input(uint8_t port);

/**
 * Poll for OpenAI requests and process responses
 * Called from Core 1's main loop in websocket_console_core1_entry()
 * Handles HTTP client operations and SSE stream parsing
 */
void openai_poll(void);
