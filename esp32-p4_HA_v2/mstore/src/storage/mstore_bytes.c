#include "storage/mstore_bytes.h"

uint32_t mstore_crc32(const void *data, size_t len) {
    const uint8_t *p = data;
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc & 1u) ? ((crc >> 1) ^ 0xEDB88320u) : (crc >> 1);
        }
    }
    return ~crc;
}

static uint64_t mstore_fnv1a64(const char *text, uint64_t seed) {
    uint64_t hash = seed;
    for (const unsigned char *p = (const unsigned char *)text; *p != 0; p++) {
        hash ^= *p;
        hash *= 1099511628211ULL;
    }
    return hash;
}

void mstore_persist_id(const char *key, uint8_t out[16]) {
    uint64_t a = mstore_fnv1a64(key, 1469598103934665603ULL);
    uint64_t b = mstore_fnv1a64(key, 1099511628211ULL);
    for (int i = 0; i < 8; i++) {
        out[i] = (uint8_t)(a >> (8 * i));
        out[8 + i] = (uint8_t)(b >> (8 * i));
    }
}
