#include "http_io.h"
#include "http_get.h"

#include "pico/stdlib.h" // Must be included before WiFi check to get board definitions

// HTTP file transfer is only available on WiFi-enabled boards
#if defined(CYW43_WL_GPIO_LED_PIN)

#include <stdio.h>
#include <string.h>

#include "pico/util/queue.h"

// Port definitions matching gf.c
#define WG_IDX_RESET 109
#define WG_EP_NAME 110
#define WG_FILENAME 114
#define WG_STATUS 33
#define WG_GET_BYTE 201

// Status values matching gf.c
#define WG_EOF 0
#define WG_WAITING 1
#define WG_DATAREADY 2
#define WG_FAILED 3

// Configuration
#define ENDPOINT_LEN 128
#define FILENAME_LEN 128

// State for port handling (Core 0)
typedef struct
{
    char endpoint[ENDPOINT_LEN];
    char filename[FILENAME_LEN];
    int index;
    uint8_t status;

    // Current chunk being read by Altair
    uint8_t chunk_buffer[HTTP_CHUNK_SIZE];
    size_t chunk_bytes_available;
    size_t chunk_position;
} http_port_state_t;

// Queues for inter-core communication (provided by http_get module)
static queue_t* outbound_queue; // Core 0 -> Core 1
static queue_t* inbound_queue;  // Core 1 -> Core 0

// State variables
static http_port_state_t port_state;

// === CORE 0: Port Handlers ===

void http_io_init(void)
{
    // Initialize HTTP GET module
    http_get_init();

    // Get queue pointers from http_get module
    http_get_queues(&outbound_queue, &inbound_queue);

    // Initialize state
    memset(&port_state, 0, sizeof(port_state));
    port_state.status = WG_EOF;
}

size_t http_output(int port, uint8_t data, char* buffer, size_t buffer_length)
{
    size_t len = 0;

    switch (port)
    {
        case WG_IDX_RESET:
            port_state.index = 0;
            break;

        case WG_EP_NAME: // Set endpoint URL
            if (port_state.index == 0)
            {
                memset(port_state.endpoint, 0, ENDPOINT_LEN);
            }

            if (data != 0 && port_state.index < ENDPOINT_LEN - 1)
            {
                port_state.endpoint[port_state.index++] = (char)data;
            }

            if (data == 0) // NULL termination
            {
                if (port_state.index < ENDPOINT_LEN)
                {
                    port_state.endpoint[port_state.index] = '\0';
                }
                port_state.index = 0;
            }
            break;

        case WG_FILENAME: // Set filename and trigger transfer
            if (port_state.index == 0)
            {
                memset(port_state.filename, 0, FILENAME_LEN);
            }

            if (data != 0 && port_state.index < FILENAME_LEN - 1)
            {
                port_state.filename[port_state.index++] = (char)data;
            }

            if (data == 0) // NULL termination - trigger transfer
            {
                if (port_state.index < FILENAME_LEN)
                {
                    port_state.filename[port_state.index] = '\0';
                }
                port_state.index = 0;

                // Build full URL
                http_request_t request;
                memset(&request, 0, sizeof(request));
                snprintf(request.url, HTTP_URL_MAX_LEN, "%s/%s", port_state.endpoint, port_state.filename);
                request.abort = false;

                // Reset chunk state for new transfer
                port_state.chunk_bytes_available = 0;
                port_state.chunk_position = 0;
                port_state.status = WG_WAITING;

                // Send request to Core 1
                if (!queue_try_add(outbound_queue, &request))
                {
                    port_state.status = WG_FAILED;
                }
            }
            break;
    }

    return len;
}

uint8_t http_input(uint8_t port)
{
    uint8_t retVal = 0;

    switch (port)
    {
        case WG_STATUS:
            // If no data in buffer, try to load a chunk from the queue
            if (port_state.chunk_bytes_available == 0)
            {
                http_response_t response;
                if (queue_try_remove(inbound_queue, &response))
                {
                    // Load chunk
                    memcpy(port_state.chunk_buffer, response.data, response.len);
                    port_state.chunk_bytes_available = response.len;
                    port_state.chunk_position = 0;
                    port_state.status = response.status;
                }
            }
            retVal = port_state.status;
            break;

        case WG_GET_BYTE:
            // Check if we have data in current chunk
            if (port_state.chunk_bytes_available > 0 && port_state.chunk_position < port_state.chunk_bytes_available)
            {
                retVal = port_state.chunk_buffer[port_state.chunk_position++];

                // Check if chunk is depleted
                if (port_state.chunk_position >= port_state.chunk_bytes_available)
                {
                    // Try to get next chunk from queue
                    http_response_t response;
                    if (queue_try_remove(inbound_queue, &response))
                    {
                        // Load new chunk
                        memcpy(port_state.chunk_buffer, response.data, response.len);
                        port_state.chunk_bytes_available = response.len;
                        port_state.chunk_position = 0;
                        port_state.status = response.status;
                    }
                    else
                    {
                        // No more chunks available
                        port_state.chunk_bytes_available = 0;
                        port_state.chunk_position = 0;

                        // Status remains as-is (could be WAITING, EOF, or FAILED)
                        // If status was DATAREADY but no more chunks, set to WAITING
                        if (port_state.status == WG_DATAREADY)
                        {
                            port_state.status = WG_WAITING;
                        }
                    }
                }
                else
                {
                    // More bytes available in current chunk
                    port_state.status = WG_DATAREADY;
                }
            }
            else
            {
                // No data in buffer
                retVal = 0x00;
            }
            break;
    }

    return retVal;
}

// === CORE 1: HTTP Client ===

void http_poll(void)
{
    // Delegate to http_get module
    http_get_poll();
}

#else // !CYW43_WL_GPIO_LED_PIN - Stub implementations for non-WiFi boards

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

void http_io_init(void)
{
    // No-op on non-WiFi boards
}

size_t http_output(int port, uint8_t data, char* buffer, size_t buffer_length)
{
    (void)port;
    (void)data;
    (void)buffer;
    (void)buffer_length;
    return 0;
}

uint8_t http_input(uint8_t port)
{
    (void)port;
    return 0; // Return EOF status
}

void http_poll(void)
{
    // No-op on non-WiFi boards
}

#endif // CYW43_WL_GPIO_LED_PIN
