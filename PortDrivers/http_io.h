#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * Initialize HTTP IO subsystem
 * Creates queues for inter-core communication
 * Must be called before websocket_console_start() on Core 0
 */
void http_io_init(void);

/**
 * HTTP port output handler
 * Called from io_port_out() on Core 0 (Altair emulator)
 *
 * @param port Port number (109, 110, 114)
 * @param data Data byte written to port
 * @param buffer Output buffer for response data
 * @param buffer_length Size of output buffer
 * @return Number of bytes written to buffer
 */
size_t http_output(uint8_t port, uint8_t data, char* buffer, size_t buffer_length);

/**
 * HTTP port input handler
 * Called from io_port_in() on Core 0 (Altair emulator)
 *
 * @param port Port number (33, 201)
 * @return Data byte read from port
 */
uint8_t http_input(uint8_t port);

/**
 * Poll for HTTP requests and process responses
 * Called from Core 1's main loop in websocket_console_core1_entry()
 * Handles HTTP client operations and queue management
 */
void http_poll(void);
