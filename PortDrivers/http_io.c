#include "http_io.h"

#include "pico/stdlib.h" // Must be included before WiFi check to get board definitions

// HTTP file transfer is only available on WiFi-enabled boards
#if defined(CYW43_WL_GPIO_LED_PIN)

#include <stdio.h>
#include <string.h>

#include "lwip/altcp.h"
#include "lwip/apps/http_client.h"
#include "lwip/dns.h"
#include "lwip/err.h"
#include "lwip/pbuf.h"
#include "pico/cyw43_arch.h"
#include "pico/time.h"
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
#define CHUNK_SIZE 256
#define URL_MAX_LEN 280 // endpoint + "/" + filename + null

// Queue sizes
#define OUTBOUND_QUEUE_SIZE 4
#define INBOUND_QUEUE_SIZE 2 // Small queue creates TCP backpressure

// Timeout and retry configuration
#define HTTP_TRANSFER_TIMEOUT_MS 30000 // 30 second timeout
#define QUEUE_RETRY_LIMIT 10000        // Max retries before giving up (~1 second at 100us)

// HTTP request message (Core 0 -> Core 1)
typedef struct
{
    char url[URL_MAX_LEN];
    bool abort;
} http_request_t;

// HTTP response message (Core 1 -> Core 0)
typedef struct
{
    uint8_t data[CHUNK_SIZE];
    size_t len;
    uint8_t status;
} http_response_t;

// State for HTTP transfer (Core 1)
typedef struct
{
    volatile bool transfer_active;
    volatile bool transfer_complete;
    http_response_t current_chunk;
    size_t total_bytes_received;
    size_t unacked_bytes;       // Track bytes not yet ACKed to TCP
    absolute_time_t start_time; // For timeout detection
    struct altcp_pcb* conn;     // TCP connection for flow control
} http_transfer_state_t;

// State for port handling (Core 0)
typedef struct
{
    char endpoint[ENDPOINT_LEN];
    char filename[FILENAME_LEN];
    int index;
    uint8_t status;

    // Current chunk being read by Altair
    uint8_t chunk_buffer[CHUNK_SIZE];
    size_t chunk_bytes_available;
    size_t chunk_position;
} http_port_state_t;

// Queues for inter-core communication
static queue_t outbound_queue; // Core 0 -> Core 1
static queue_t inbound_queue;  // Core 1 -> Core 0

// State variables
static http_port_state_t port_state;
static http_transfer_state_t transfer_state;

// === CORE 0: Port Handlers ===

void http_io_init(void)
{
    // Initialize queues
    queue_init(&outbound_queue, sizeof(http_request_t), OUTBOUND_QUEUE_SIZE);
    queue_init(&inbound_queue, sizeof(http_response_t), INBOUND_QUEUE_SIZE);

    // Initialize state
    memset(&port_state, 0, sizeof(port_state));
    memset(&transfer_state, 0, sizeof(transfer_state));
    port_state.status = WG_EOF;
}

size_t http_output(uint8_t port, uint8_t data, char* buffer, size_t buffer_length)
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
                snprintf(request.url, URL_MAX_LEN, "%s/%s", port_state.endpoint, port_state.filename);
                request.abort = false;

                // Reset chunk state for new transfer
                port_state.chunk_bytes_available = 0;
                port_state.chunk_position = 0;
                port_state.status = WG_WAITING;

                // Send request to Core 1
                if (!queue_try_add(&outbound_queue, &request))
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
                if (queue_try_remove(&inbound_queue, &response))
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
                    if (queue_try_remove(&inbound_queue, &response))
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

// Helper to drain queues during abort/cleanup
static void drain_queues(void)
{
    http_response_t resp_dummy;
    while (queue_try_remove(&inbound_queue, &resp_dummy))
    {
    }

    http_request_t req_dummy;
    while (queue_try_remove(&outbound_queue, &req_dummy))
    {
    }
}

// Try to add to queue with bounded retries. Returns true on success.
static bool queue_try_add_with_retry(queue_t* queue, const void* data)
{
    for (int i = 0; i < QUEUE_RETRY_LIMIT; i++)
    {
        if (queue_try_add(queue, data))
        {
            return true;
        }
        sleep_us(100);
    }
    return false;
}

// lwIP HTTP client callback: receive data
static err_t http_recv_callback(void* arg, struct altcp_pcb* conn, struct pbuf* p, err_t err)
{
    http_transfer_state_t* state = (http_transfer_state_t*)arg;

    if (err != ERR_OK || p == NULL)
    {
        if (p != NULL)
        {
            pbuf_free(p);
        }
        return err;
    }

    // Store connection for flow control
    state->conn = conn;

    // Track how much we've acked so far in this callback
    size_t bytes_acked = 0;

    // Process received data
    struct pbuf* current = p;
    size_t offset = 0;

    while (current != NULL && offset < p->tot_len)
    {
        // Copy data from pbuf to response chunk
        size_t bytes_to_copy = current->len;
        uint8_t* data_ptr = (uint8_t*)current->payload;

        while (bytes_to_copy > 0)
        {
            // Determine how much can fit in current chunk
            size_t chunk_space = CHUNK_SIZE - state->current_chunk.len;
            size_t copy_size = (bytes_to_copy > chunk_space) ? chunk_space : bytes_to_copy;

            // Copy to chunk
            memcpy(&state->current_chunk.data[state->current_chunk.len], data_ptr, copy_size);
            state->current_chunk.len += copy_size;
            data_ptr += copy_size;
            bytes_to_copy -= copy_size;

            // If chunk is full, send to queue
            if (state->current_chunk.len >= CHUNK_SIZE)
            {
                state->current_chunk.status = WG_DATAREADY;

                // FLOW CONTROL: Try to add with bounded retries
                if (queue_try_add_with_retry(&inbound_queue, &state->current_chunk))
                {
                    // ACK only after successfully queuing (flow control)
                    altcp_recved(conn, CHUNK_SIZE);
                    bytes_acked += CHUNK_SIZE;
                }
                else
                {
                    // Queue full after retries - track unacked bytes for later
                    state->unacked_bytes += CHUNK_SIZE;
                }

                // Reset chunk for next data
                memset(&state->current_chunk, 0, sizeof(state->current_chunk));
            }
        }

        offset += current->len;
        current = current->next;
    }

    state->total_bytes_received += p->tot_len;

    // Track remaining bytes that weren't part of a full chunk
    // These will be ACKed when the transfer completes
    size_t remaining = p->tot_len - bytes_acked;
    if (remaining > 0)
    {
        state->unacked_bytes += remaining;
    }

    pbuf_free(p);

    return ERR_OK;
}

// lwIP HTTP client callback: headers received
static err_t http_headers_done_callback(httpc_state_t* connection, void* arg, struct pbuf* hdr, u16_t hdr_len,
                                        u32_t content_len)
{
    (void)connection;
    (void)arg;
    (void)hdr;
    (void)hdr_len;
    (void)content_len;
    return ERR_OK;
}

// lwIP HTTP client callback: transfer complete
static void http_result_callback(void* arg, httpc_result_t httpc_result, u32_t rx_content_len, u32_t srv_res, err_t err)
{
    (void)rx_content_len;
    (void)err;

    http_transfer_state_t* state = (http_transfer_state_t*)arg;

    // ACK any remaining unacked bytes before cleanup
    if (state->unacked_bytes > 0 && state->conn != NULL)
    {
        altcp_recved(state->conn, state->unacked_bytes);
        state->unacked_bytes = 0;
    }

    // Send final chunk if there's data
    if (state->current_chunk.len > 0)
    {
        state->current_chunk.status = (httpc_result == HTTPC_RESULT_OK) ? WG_DATAREADY : WG_FAILED;

        if (!queue_try_add_with_retry(&inbound_queue, &state->current_chunk))
        {
            // Failed to queue final chunk - data loss, but we must complete
        }
    }

    // Send final status message
    http_response_t final;
    memset(&final, 0, sizeof(final));

    if (httpc_result == HTTPC_RESULT_OK && srv_res >= 200 && srv_res < 300)
    {
        final.status = WG_EOF;
    }
    else
    {
        final.status = WG_FAILED;
    }

    final.len = 0;
    if (!queue_try_add_with_retry(&inbound_queue, &final))
    {
        // Failed to queue final status - consumer may hang waiting
    }

    state->transfer_active = false;
    state->transfer_complete = true;
}

// Parse URL to extract hostname/IP, port, and path
// Supports: http://hostname:port/path or just hostname:port/path
// Returns 0 on success, -1 on error
static int parse_url(const char* url, char* hostname, size_t hostname_len, u16_t* port, char* path, size_t path_len)
{
    const char* start = url;
    const char* end;

    // Skip "http://" prefix if present
    if (strncmp(url, "http://", 7) == 0)
    {
        start = url + 7;
    }
    else if (strncmp(url, "HTTP://", 7) == 0)
    {
        start = url + 7;
    }

    // Find end of hostname (either ':', '/', or end of string)
    end = start;
    while (*end != '\0' && *end != ':' && *end != '/')
    {
        end++;
    }

    // Extract hostname
    size_t host_len = end - start;
    if (host_len >= hostname_len)
    {
        return -1; // Hostname too long
    }
    memcpy(hostname, start, host_len);
    hostname[host_len] = '\0';

    // Default port
    *port = 80;

    // Check for port specification
    if (*end == ':')
    {
        // Parse port number
        end++; // Skip ':'
        const char* port_start = end;
        unsigned long parsed_port = 0;

        while (*end >= '0' && *end <= '9')
        {
            parsed_port = parsed_port * 10 + (*end - '0');
            end++;
        }

        if (end > port_start && parsed_port > 0 && parsed_port <= 65535)
        {
            *port = (u16_t)parsed_port;
        }
        else
        {
            return -1; // Invalid port
        }
    }

    // Extract path (everything after hostname:port)
    if (*end == '/')
    {
        size_t path_length = strlen(end);
        if (path_length >= path_len)
        {
            return -1; // Path too long
        }
        strcpy(path, end);
    }
    else
    {
        // No path specified, use root
        strcpy(path, "/");
    }

    return 0;
}

void http_poll(void)
{
    // Check for transfer timeout
    if (transfer_state.transfer_active)
    {
        int64_t elapsed_us = absolute_time_diff_us(transfer_state.start_time, get_absolute_time());
        if (elapsed_us > (int64_t)HTTP_TRANSFER_TIMEOUT_MS * 1000)
        {
            // Timeout - send failure and cleanup
            http_response_t response;
            memset(&response, 0, sizeof(response));
            response.status = WG_FAILED;
            response.len = 0;
            queue_try_add(&inbound_queue, &response);

            transfer_state.transfer_active = false;
            transfer_state.transfer_complete = true;
            return;
        }
    }

    // Check for new HTTP requests from Core 0
    http_request_t request;

    if (queue_try_remove(&outbound_queue, &request))
    {
        if (request.abort)
        {
            // Proper abort cleanup
            drain_queues();
            memset(&transfer_state, 0, sizeof(transfer_state));
            transfer_state.transfer_active = false;
            return;
        }

        // Parse URL to extract hostname, port, and path
        char hostname[128];
        char path[128];
        u16_t port;

        if (parse_url(request.url, hostname, sizeof(hostname), &port, path, sizeof(path)) != 0)
        {
            // Send failure status
            http_response_t response;
            memset(&response, 0, sizeof(response));
            response.status = WG_FAILED;
            response.len = 0;
            queue_try_add(&inbound_queue, &response);
            return;
        }

        // Reset transfer state
        memset(&transfer_state, 0, sizeof(transfer_state));
        transfer_state.transfer_active = true;
        transfer_state.start_time = get_absolute_time();

        // Configure HTTP client settings
        httpc_connection_t settings;
        memset(&settings, 0, sizeof(settings));
        settings.use_proxy = 0;
        settings.result_fn = http_result_callback;
        settings.headers_done_fn = http_headers_done_callback;

        // Start HTTP GET request with parsed port
        httpc_state_t* connection = NULL;
        err_t err =
            httpc_get_file_dns(hostname, port, path, &settings, http_recv_callback, &transfer_state, &connection);

        if (err != ERR_OK)
        {
            // Send failure status
            http_response_t response;
            memset(&response, 0, sizeof(response));
            response.status = WG_FAILED;
            response.len = 0;
            queue_try_add(&inbound_queue, &response);

            transfer_state.transfer_active = false;
        }
    }
}

#else // !CYW43_WL_GPIO_LED_PIN - Stub implementations for non-WiFi boards

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

void http_io_init(void)
{
    // No-op on non-WiFi boards
}

size_t http_output(uint8_t port, uint8_t data, char* buffer, size_t buffer_length)
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
