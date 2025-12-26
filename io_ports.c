#include "io_ports.h"

#include "PortDrivers/http_io.h"
#include "PortDrivers/openai_io.h"
#include "PortDrivers/time_io.h"
#include "PortDrivers/utility_io.h"
#include <stdio.h>
#include <string.h>

#define REQUEST_BUFFER_SIZE 128

typedef struct
{
    size_t len;
    size_t count;
    char buffer[REQUEST_BUFFER_SIZE];
} request_unit_t;

static request_unit_t request_unit;

void io_port_out(uint8_t port, uint8_t data)
{
    memset(&request_unit, 0, sizeof(request_unit));

    switch (port)
    {
        case 24:
        case 25:
        case 26:
        case 27:
        case 28:
        case 29:
        case 30:
        case 41:
        case 42:
        case 43:
            request_unit.len = time_output(port, data, request_unit.buffer, sizeof(request_unit.buffer));
            break;
        case 45:
        case 70:
            request_unit.len = utility_output(port, data, request_unit.buffer, sizeof(request_unit.buffer));
            break;
        case 109:
        case 110:
        case 114:
            request_unit.len = http_output(port, data, request_unit.buffer, sizeof(request_unit.buffer));
            break;
        case 120: // OpenAI: Reset request buffer
        case 121: // OpenAI: Add byte to request buffer
        case 122: // OpenAI: Reset response buffer
        case 126: // OpenAI: Set content length low byte
        case 127: // OpenAI: Set content length high byte
            request_unit.len = openai_output(port, data, request_unit.buffer, sizeof(request_unit.buffer));
            break;
        default:
            break;
    }
}

uint8_t io_port_in(uint8_t port)
{
    switch (port)
    {
        case 24:
        case 25:
        case 26:
        case 27:
        case 28:
        case 29:
        case 30:
            return time_input(port);
        case 33:
        case 201:
            return http_input(port);
        case 120: // OpenAI: Trigger API call / get status
        case 121: // OpenAI: Get buffer length low byte
        case 122: // OpenAI: Get buffer length high byte
        case 123: // OpenAI: Get status (EOF/WAITING/DATA_READY)
        case 124: // OpenAI: Read response byte
        case 125: // OpenAI: Check if stream complete
            return openai_input(port);
        case 200:
            if (request_unit.count < request_unit.len && request_unit.count < sizeof(request_unit.buffer))
            {
                return (uint8_t)request_unit.buffer[request_unit.count++];
            }
            return 0x00;
        default:
            return 0x00;
    }
}
