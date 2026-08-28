#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ld6002_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LD6002_MAX_PAYLOAD_LEN 1024

typedef enum {
    LD6002_PARSE_NONE = 0,
    LD6002_PARSE_FRAME,
    LD6002_PARSE_CHECKSUM_ERROR,
    LD6002_PARSE_LENGTH_ERROR,
} ld6002_parse_result_t;

typedef struct {
    uint8_t buffer[LD6002_MAX_PAYLOAD_LEN + 9];
    size_t pos;
    size_t expected_len;
    uint32_t checksum_errors;
    uint32_t length_errors;
} ld6002_parser_t;

void ld6002_parser_init(ld6002_parser_t *parser);
void ld6002_parser_reset(ld6002_parser_t *parser);
ld6002_parse_result_t ld6002_parser_feed(ld6002_parser_t *parser, uint8_t byte, ld6002_frame_t *frame);
bool ld6002_apply_frame(const ld6002_frame_t *frame, ld6002_snapshot_t *snapshot, int64_t now_us);

uint8_t ld6002_checksum(const uint8_t *data, size_t len);
uint16_t ld6002_read_be_u16(const uint8_t *data);
uint16_t ld6002_read_le_u16(const uint8_t *data);
uint32_t ld6002_read_le_u32(const uint8_t *data);
float ld6002_read_le_float(const uint8_t *data);

#ifdef __cplusplus
}
#endif

