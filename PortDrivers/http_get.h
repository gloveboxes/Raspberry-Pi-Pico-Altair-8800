#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pico/util/queue.h"

// Configuration
#define HTTP_CHUNK_SIZE 256
#define HTTP_URL_MAX_LEN 280

// Status values matching gf.c
#define HTTP_WG_EOF 0
#define HTTP_WG_WAITING 1
#define HTTP_WG_DATAREADY 2
#define HTTP_WG_FAILED 3

// HTTP request message (Core 0 -> Core 1)
typedef struct
{
    char url[HTTP_URL_MAX_LEN];
    bool abort;
} http_request_t;

// HTTP response message (Core 1 -> Core 0)
typedef struct
{
    uint8_t data[HTTP_CHUNK_SIZE];
    size_t len;
    uint8_t status;
} http_response_t;

// State for HTTP transfer (Core 1)
typedef struct
{
    bool transfer_active;
    bool transfer_complete;
    http_response_t current_chunk;
    size_t total_bytes_received;

    // Pending final messages (for non-blocking retry)
    bool pending_final_chunk;
    bool pending_final_status;
    http_response_t final_chunk;
    http_response_t final_status;

    // TCP Flow Control: Pending pbuf when queue is full
    struct pbuf* pending_pbuf;
    size_t pending_pbuf_offset;
    void* conn; // Connection handle for async Flow Control ACKs (opaque, cast in http_get.c)
} http_transfer_state_t;

/**
 * Initialize HTTP GET subsystem
 * Creates queues for inter-core communication
 * Must be called before starting Core 1 operations
 */
void http_get_init(void);

/**
 * Poll for HTTP GET requests and process responses
 * Called from Core 1's main loop
 * Handles HTTP client operations and queue management
 */
void http_get_poll(void);

/**
 * Get pointers to the HTTP GET queues
 * Used by http_io.c to access queues for port handling
 *
 * @param outbound Pointer to receive outbound queue pointer (Core 0 -> Core 1)
 * @param inbound Pointer to receive inbound queue pointer (Core 1 -> Core 0)
 */
void http_get_queues(queue_t** outbound, queue_t** inbound);
