#include "boot.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint8_t storage[BL_FLASH_END - BL_FLASH_BASE];
static int fail_write;

static uint32_t read_word(void *ctx, uint32_t addr) {
    (void)ctx;
    uint32_t v;
    assert(addr >= BL_FLASH_BASE && addr <= BL_FLASH_END - 4u);
    memcpy(&v, storage + addr - BL_FLASH_BASE, 4u);
    return v;
}
static int write_word(void *ctx, uint32_t addr, uint32_t value) {
    (void)ctx;
    if (fail_write || (addr & 3u) || addr < BL_HEADER_ADDR || addr > BL_FLASH_END - 4u) return 0;
    uint32_t old = read_word(ctx, addr);
    if ((old & value) != value) return 0;
    memcpy(storage + addr - BL_FLASH_BASE, &value, 4u);
    return 1;
}
static int erase_app(void *ctx) {
    (void)ctx;
    memset(storage + BL_HEADER_ADDR - BL_FLASH_BASE, 0xff, BL_FLASH_END - BL_HEADER_ADDR);
    return 1;
}
static void put32(uint8_t *p, uint32_t v) {
    for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(v >> (8u * i));
}
int main(void) {
    assert(bl_crc32((const uint8_t *)"123456789", 9u) == 0xcbf43926u);
    memset(storage, 0xff, sizeof storage);
    bl_flash_t flash = {erase_app, write_word, read_word, NULL};
    bl_state_t state = {0};
    uint8_t image[130] = {0};
    put32(image, BL_RAM_END);
    put32(image + 4, BL_APP_ADDR + 33u);
    for (unsigned i = 8; i < sizeof image; ++i) image[i] = (uint8_t)i;
    uint8_t wire[140]; bl_packet_t packet = {0};
    size_t n = bl_encode(BL_DATA, 2u, image, 3u, wire, sizeof wire);
    assert(n == 12u && bl_crc16(wire, n) == 0u);
    assert(bl_decode(wire, n, &packet) && packet.sequence == 2u);
    wire[n - 1u] ^= 1u;
    assert(!bl_decode(wire, n, &packet));
    uint32_t crc = bl_crc32(image, sizeof image);
    assert(crc != 0u);
    packet.command = BL_BEGIN; packet.sequence = 0u; packet.length = 8u;
    put32(packet.data, sizeof image); put32(packet.data + 4, crc);
    assert(bl_process(&state, &flash, &packet) == BL_OK);
    assert(bl_process(&state, &flash, &packet) == BL_OK);
    assert(!bl_app_valid(&flash));
    packet.command = BL_DATA; packet.sequence = 1u; packet.length = 128u;
    memcpy(packet.data, image, 128u);
    assert(bl_process(&state, &flash, &packet) == BL_BAD_SEQ);
    packet.sequence = 0u;
    assert(bl_process(&state, &flash, &packet) == BL_OK);
    assert(bl_process(&state, &flash, &packet) == BL_OK); /* duplicate */
    packet.sequence = 1u; packet.length = 2u;
    memcpy(packet.data, image + 128u, 2u);
    assert(bl_process(&state, &flash, &packet) == BL_OK);
    packet.command = BL_END; packet.sequence = 2u; packet.length = 0u;
    assert(bl_process(&state, &flash, &packet) == BL_OK);
    assert(bl_app_valid(&flash));
    assert(bl_process(&state, &flash, &packet) == BL_OK); /* lost END ACK */
    storage[BL_APP_ADDR - BL_FLASH_BASE + 20u] ^= 1u;
    assert(!bl_app_valid(&flash));
    /* interrupted upgrade erases valid header, leaving recovery mode */
    packet.command = BL_BEGIN; packet.sequence = 0u; packet.length = 8u;
    put32(packet.data, sizeof image); put32(packet.data + 4, crc);
    assert(bl_process(&state, &flash, &packet) == BL_OK);
    assert(!bl_app_valid(&flash));
    packet.command = BL_DATA; packet.length = 128u;
    memcpy(packet.data, image, 128u);
    fail_write = 1;
    assert(bl_process(&state, &flash, &packet) == BL_FLASH_ERROR);
    assert(!bl_app_valid(&flash));
    puts("bootloader host tests passed");
    return 0;
}
