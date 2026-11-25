// Word-at-a-time memcpy and memset replacing Picolibc's byte loops so Doom's
// 64 KiB framebuffer copies execute in far fewer simulated instructions.

#include <stddef.h>
#include <stdint.h>

typedef uint32_t __attribute__((may_alias)) word;

void *memcpy(void *restrict destination, const void *restrict source,
             size_t count) {
    unsigned char *dst = destination;
    const unsigned char *src = source;

    // Fast path when source and destination share the same 4-byte alignment.
    if ((((uintptr_t)dst ^ (uintptr_t)src) & 3) == 0) {
        for (; count && ((uintptr_t)dst & 3); --count)
            *dst++ = *src++;

        word *dst_words = (word *)dst;
        const word *src_words = (const word *)src;

        for (; count >= 16; count -= 16, dst_words += 4, src_words += 4) {
            const uint32_t w0 = src_words[0];
            const uint32_t w1 = src_words[1];
            const uint32_t w2 = src_words[2];
            const uint32_t w3 = src_words[3];

            dst_words[0] = w0;
            dst_words[1] = w1;
            dst_words[2] = w2;
            dst_words[3] = w3;
        }

        for (; count >= 4; count -= 4)
            *dst_words++ = *src_words++;

        dst = (unsigned char *)dst_words;
        src = (const unsigned char *)src_words;
    }

    while (count--)
        *dst++ = *src++;

    return destination;
}

void *memset(void *destination, int value, size_t count) {
    unsigned char *dst = destination;
    const unsigned char byte = (unsigned char)value;

    for (; count && ((uintptr_t)dst & 3); --count)
        *dst++ = byte;

    word *dst_words = (word *)dst;
    const uint32_t pattern = byte * 0x01010101u;

    for (; count >= 16; count -= 16, dst_words += 4) {
        dst_words[0] = pattern;
        dst_words[1] = pattern;
        dst_words[2] = pattern;
        dst_words[3] = pattern;
    }

    for (; count >= 4; count -= 4)
        *dst_words++ = pattern;

    dst = (unsigned char *)dst_words;
    while (count--)
        *dst++ = byte;

    return destination;
}
