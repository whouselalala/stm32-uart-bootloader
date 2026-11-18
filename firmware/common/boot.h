#ifndef BOOT_H
#define BOOT_H

#include <stddef.h>
#include <stdint.h>

#define BL_FLASH_BASE 0x08000000u
#define BL_FLASH_END  0x08080000u
#define BL_HEADER_ADDR 0x08008000u
#define BL_APP_ADDR 0x08008200u
#define BL_APP_MAX (BL_FLASH_END - BL_APP_ADDR)
#define BL_RAM_BASE 0x20000000u
#define BL_RAM_END 0x20020000u
#define BL_MAGIC 0x424c5631u
#define BL_DATA_MAX 128u

enum { BL_BEGIN = 1, BL_DATA = 2, BL_END = 3 };
enum { BL_OK = 0, BL_BAD_FRAME = 1, BL_BAD_STATE = 2, BL_BAD_SIZE = 3,
       BL_BAD_SEQ = 4, BL_FLASH_ERROR = 5, BL_BAD_IMAGE = 6 };

typedef struct {
    int (*erase_app)(void *ctx);
    int (*write_word)(void *ctx, uint32_t address, uint32_t value);
    uint32_t (*read_word)(void *ctx, uint32_t address);
    void *ctx;
} bl_flash_t;

typedef struct {
    uint32_t size, expected_crc, received;
    uint16_t next_seq;
    uint8_t active;
} bl_state_t;

typedef struct {
    uint8_t command;
    uint16_t sequence, length;
    uint8_t data[BL_DATA_MAX];
} bl_packet_t;

uint16_t bl_crc16(const uint8_t *data, size_t length);
uint32_t bl_crc32(const uint8_t *data, size_t length);
int bl_decode(const uint8_t *frame, size_t length, bl_packet_t *packet);
size_t bl_encode(uint8_t command, uint16_t sequence, const uint8_t *data,
                 uint16_t length, uint8_t *frame, size_t capacity);
uint8_t bl_process(bl_state_t *state, const bl_flash_t *flash, const bl_packet_t *packet);
int bl_app_valid(const bl_flash_t *flash);

#endif
