#include "boot.h"
#include <string.h>

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | ((uint16_t)p[1] << 8u)); }
static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8u) |
           ((uint32_t)p[2] << 16u) | ((uint32_t)p[3] << 24u);
}
static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8u); }

uint16_t bl_crc16(const uint8_t *data, size_t length) {
    uint16_t crc = 0xffffu;
    for (size_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (unsigned j = 0; j < 8; ++j)
            crc = (uint16_t)((crc >> 1u) ^ ((crc & 1u) ? 0xa001u : 0u));
    }
    return crc;
}
uint32_t bl_crc32(const uint8_t *data, size_t length) {
    uint32_t crc = 0xffffffffu;
    for (size_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (unsigned j = 0; j < 8; ++j)
            crc = (crc >> 1u) ^ ((crc & 1u) ? 0xedb88320u : 0u);
    }
    return ~crc;
}

int bl_decode(const uint8_t *f, size_t n, bl_packet_t *p) {
    if (!f || !p || n < 9u || f[0] != 'B' || f[1] != 'L' ||
        le16(f + 5) > BL_DATA_MAX || n != 9u + le16(f + 5) ||
        bl_crc16(f, n) != 0u) return 0;
    p->command = f[2]; p->sequence = le16(f + 3); p->length = le16(f + 5);
    if (p->length) memcpy(p->data, f + 7, p->length);
    return 1;
}
size_t bl_encode(uint8_t command, uint16_t seq, const uint8_t *data,
                 uint16_t length, uint8_t *f, size_t capacity) {
    if (!f || length > BL_DATA_MAX || capacity < 9u + length || (length && !data)) return 0;
    f[0] = 'B'; f[1] = 'L'; f[2] = command;
    put16(f + 3, seq); put16(f + 5, length);
    if (length) memcpy(f + 7, data, length);
    put16(f + 7 + length, bl_crc16(f, 7u + length));
    return 9u + length;
}

static uint8_t byte_at(const bl_flash_t *flash, uint32_t address) {
    return (uint8_t)(flash->read_word(flash->ctx, address & ~3u) >> (8u * (address & 3u)));
}
static uint32_t flash_crc(const bl_flash_t *flash, uint32_t size) {
    uint32_t crc = 0xffffffffu;
    for (uint32_t i = 0; i < size; ++i) {
        crc ^= byte_at(flash, BL_APP_ADDR + i);
        for (unsigned j = 0; j < 8; ++j)
            crc = (crc >> 1u) ^ ((crc & 1u) ? 0xedb88320u : 0u);
    }
    return ~crc;
}
static int vector_valid(const bl_flash_t *flash, uint32_t size) {
    if (size < 8u) return 0;
    uint32_t sp = flash->read_word(flash->ctx, BL_APP_ADDR);
    uint32_t pc = flash->read_word(flash->ctx, BL_APP_ADDR + 4u);
    uint32_t entry = pc & ~1u;
    return sp > BL_RAM_BASE && sp <= BL_RAM_END && (sp & 7u) == 0u &&
           (pc & 1u) && entry >= BL_APP_ADDR && entry < BL_APP_ADDR + size;
}
int bl_app_valid(const bl_flash_t *flash) {
    if (!flash || !flash->read_word) return 0;
    uint32_t magic = flash->read_word(flash->ctx, BL_HEADER_ADDR);
    uint32_t size = flash->read_word(flash->ctx, BL_HEADER_ADDR + 4u);
    uint32_t crc = flash->read_word(flash->ctx, BL_HEADER_ADDR + 8u);
    return magic == BL_MAGIC && size >= 8u && size <= BL_APP_MAX &&
           vector_valid(flash, size) && flash_crc(flash, size) == crc;
}

uint8_t bl_process(bl_state_t *s, const bl_flash_t *f, const bl_packet_t *p) {
    if (!s || !f || !p || !f->erase_app || !f->write_word || !f->read_word) return BL_BAD_STATE;
    if (p->command == BL_BEGIN) {
        if (p->length != 8u || p->sequence != 0u) return BL_BAD_FRAME;
        uint32_t size = le32(p->data), crc = le32(p->data + 4u);
        if (size < 8u || size > BL_APP_MAX) return BL_BAD_SIZE;
        if (s->active && !s->received && s->size == size && s->expected_crc == crc)
            return BL_OK; /* lost BEGIN ACK: do not erase twice */
        s->active = 0u;
        if (!f->erase_app(f->ctx)) return BL_FLASH_ERROR;
        s->size = size; s->expected_crc = crc; s->received = 0u;
        s->next_seq = 0u; s->active = 1u;
        return BL_OK;
    }
    if (!s->active) {
        if (p->command == BL_END && !p->length && p->sequence == s->next_seq &&
            bl_app_valid(f)) return BL_OK; /* lost END ACK */
        return BL_BAD_STATE;
    }
    if (p->command == BL_DATA) {
        if (s->next_seq && p->sequence == (uint16_t)(s->next_seq - 1u)) return BL_OK;
        if (p->sequence != s->next_seq) return BL_BAD_SEQ;
        if (!p->length || p->length > s->size - s->received ||
            (p->length != BL_DATA_MAX && p->length != s->size - s->received)) return BL_BAD_SIZE;
        for (uint16_t i = 0; i < p->length; i += 4u) {
            uint32_t word = 0xffffffffu;
            for (uint16_t j = 0; j < 4u && i + j < p->length; ++j) {
                word &= ~((uint32_t)0xffu << (8u * j));
                word |= (uint32_t)p->data[i + j] << (8u * j);
            }
            if (!f->write_word(f->ctx, BL_APP_ADDR + s->received + i, word)) {
                s->active = 0u;
                return BL_FLASH_ERROR;
            }
        }
        s->received += p->length; ++s->next_seq;
        return BL_OK;
    }
    if (p->command == BL_END) {
        if (p->length || p->sequence != s->next_seq) return BL_BAD_FRAME;
        s->active = 0u;
        if (s->received != s->size || !vector_valid(f, s->size) ||
            flash_crc(f, s->size) != s->expected_crc) return BL_BAD_IMAGE;
        /* Magic last: a partial header cannot make an image bootable. */
        if (!f->write_word(f->ctx, BL_HEADER_ADDR + 4u, s->size) ||
            !f->write_word(f->ctx, BL_HEADER_ADDR + 8u, s->expected_crc) ||
            !f->write_word(f->ctx, BL_HEADER_ADDR, BL_MAGIC)) return BL_FLASH_ERROR;
        return BL_OK;
    }
    return BL_BAD_FRAME;
}
