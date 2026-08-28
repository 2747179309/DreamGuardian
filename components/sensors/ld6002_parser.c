#include "ld6002_parser.h"

#include <string.h>

#define LD6002_SOF 0x01
#define LD6002_HEADER_LEN 8
#define LD6002_NO_DATA_FRAME_LEN 8

void ld6002_parser_reset(ld6002_parser_t *parser)
{
    parser->pos = 0;
    parser->expected_len = 0;
}

void ld6002_parser_init(ld6002_parser_t *parser)
{
    memset(parser, 0, sizeof(*parser));
}

uint8_t ld6002_checksum(const uint8_t *data, size_t len)
{
    uint8_t value = 0;
    for (size_t i = 0; i < len; ++i) {
        value ^= data[i];
    }
    return (uint8_t)~value;
}

uint16_t ld6002_read_be_u16(const uint8_t *data)
{
    return ((uint16_t)data[0] << 8) | data[1];
}

uint16_t ld6002_read_le_u16(const uint8_t *data)
{
    return ((uint16_t)data[1] << 8) | data[0];
}

uint32_t ld6002_read_le_u32(const uint8_t *data)
{
    return ((uint32_t)data[3] << 24) | ((uint32_t)data[2] << 16) | ((uint32_t)data[1] << 8) | data[0];
}

float ld6002_read_le_float(const uint8_t *data)
{
    uint32_t raw = ld6002_read_le_u32(data);
    float value;
    memcpy(&value, &raw, sizeof(value));
    return value;
}

ld6002_parse_result_t ld6002_parser_feed(ld6002_parser_t *parser, uint8_t byte, ld6002_frame_t *frame)
{
    if (parser->pos == 0 && byte != LD6002_SOF) {
        return LD6002_PARSE_NONE;
    }

    parser->buffer[parser->pos++] = byte;

    if (parser->pos == 5) {
        uint16_t payload_len = ld6002_read_be_u16(&parser->buffer[3]);
        if (payload_len > LD6002_MAX_PAYLOAD_LEN) {
            parser->length_errors++;
            ld6002_parser_reset(parser);
            return LD6002_PARSE_LENGTH_ERROR;
        }
        parser->expected_len = LD6002_HEADER_LEN + payload_len + ((payload_len > 0) ? 1 : 0);
        if (parser->expected_len < LD6002_NO_DATA_FRAME_LEN) {
            parser->length_errors++;
            ld6002_parser_reset(parser);
            return LD6002_PARSE_LENGTH_ERROR;
        }
    }

    if (parser->pos == LD6002_HEADER_LEN) {
        uint8_t expected = ld6002_checksum(parser->buffer, LD6002_HEADER_LEN - 1);
        if (expected != parser->buffer[LD6002_HEADER_LEN - 1]) {
            parser->checksum_errors++;
            ld6002_parser_reset(parser);
            return LD6002_PARSE_CHECKSUM_ERROR;
        }
        if (parser->expected_len == LD6002_NO_DATA_FRAME_LEN) {
            frame->id = ld6002_read_be_u16(&parser->buffer[1]);
            frame->len = 0;
            frame->type = ld6002_read_be_u16(&parser->buffer[5]);
            frame->data = NULL;
            ld6002_parser_reset(parser);
            return LD6002_PARSE_FRAME;
        }
    }

    if (parser->expected_len > 0 && parser->pos == parser->expected_len) {
        uint16_t payload_len = ld6002_read_be_u16(&parser->buffer[3]);
        uint8_t expected = ld6002_checksum(&parser->buffer[LD6002_HEADER_LEN], payload_len);
        if (expected != parser->buffer[LD6002_HEADER_LEN + payload_len]) {
            parser->checksum_errors++;
            ld6002_parser_reset(parser);
            return LD6002_PARSE_CHECKSUM_ERROR;
        }

        frame->id = ld6002_read_be_u16(&parser->buffer[1]);
        frame->len = payload_len;
        frame->type = ld6002_read_be_u16(&parser->buffer[5]);
        frame->data = &parser->buffer[LD6002_HEADER_LEN];
        return LD6002_PARSE_FRAME;
    }

    if (parser->pos >= sizeof(parser->buffer)) {
        parser->length_errors++;
        ld6002_parser_reset(parser);
        return LD6002_PARSE_LENGTH_ERROR;
    }

    return LD6002_PARSE_NONE;
}

bool ld6002_apply_frame(const ld6002_frame_t *frame, ld6002_snapshot_t *snapshot, int64_t now_us)
{
    bool updated = true;

    switch (frame->type) {
    case LD6002_TYPE_HUMAN_PRESENT:
        if (frame->len >= 2) {
            snapshot->human_present = (ld6002_read_le_u16(frame->data) != 0);
            snapshot->presence_update_us = now_us;
        } else {
            updated = false;
        }
        break;
    case LD6002_TYPE_PHASE:
        if (frame->len >= 12) {
            snapshot->total_phase = ld6002_read_le_float(&frame->data[0]);
            snapshot->breath_phase = ld6002_read_le_float(&frame->data[4]);
            snapshot->heart_phase = ld6002_read_le_float(&frame->data[8]);
            snapshot->phase_update_us = now_us;
        } else {
            updated = false;
        }
        break;
    case LD6002_TYPE_BREATH_RATE:
        if (frame->len >= 4) {
            snapshot->breath_rate_bpm = ld6002_read_le_float(frame->data);
            snapshot->breath_update_us = now_us;
        } else {
            updated = false;
        }
        break;
    case LD6002_TYPE_HEART_RATE:
        if (frame->len >= 4) {
            snapshot->heart_rate_bpm = ld6002_read_le_float(frame->data);
            snapshot->heart_update_us = now_us;
        } else {
            updated = false;
        }
        break;
    case LD6002_TYPE_RANGE:
        if (frame->len >= 8) {
            snapshot->range_valid = (ld6002_read_le_u32(&frame->data[0]) != 0);
            snapshot->range_cm = ld6002_read_le_float(&frame->data[4]);
            snapshot->range_update_us = now_us;
        } else {
            updated = false;
        }
        break;
    case LD6002_TYPE_TRACK_POS:
        if (frame->len >= 12) {
            snapshot->x_m = ld6002_read_le_float(&frame->data[0]);
            snapshot->y_m = ld6002_read_le_float(&frame->data[4]);
            snapshot->z_m = ld6002_read_le_float(&frame->data[8]);
            snapshot->track_update_us = now_us;
        } else if (frame->len >= 8) {
            snapshot->x_m = ld6002_read_le_float(&frame->data[0]);
            snapshot->y_m = ld6002_read_le_float(&frame->data[4]);
            snapshot->z_m = 0.0f;
            snapshot->track_update_us = now_us;
        } else {
            updated = false;
        }
        break;
    default:
        updated = false;
        break;
    }

    if (updated) {
        snapshot->frames++;
        snapshot->last_update_us = now_us;
    }
    return updated;
}

