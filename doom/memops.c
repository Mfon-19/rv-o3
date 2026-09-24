/* Word-at-a-time memcpy and memset, linked ahead of picolibc's. Picolibc's
 * RV32 versions are built for size and move one byte per iteration; Doom
 * copies a 64 KB frame each tic, which made memcpy 16% of its instructions.
 * rvsim traps misaligned accesses, so words are used only once the pointers
 * are word-aligned; mutually misaligned copies fall back to bytes. */
#include <stddef.h>
#include <stdint.h>

typedef uint32_t __attribute__((may_alias)) word;

void *memcpy(void *restrict destination, const void *restrict source, size_t count) {
    unsigned char *out = destination;
    const unsigned char *in = source;
    if ((((uintptr_t)out ^ (uintptr_t)in) & 3) == 0) {
        for (; count && ((uintptr_t)out & 3); --count) *out++ = *in++;
        word *wo = (word *)out;
        const word *wi = (const word *)in;
        for (; count >= 16; count -= 16, wo += 4, wi += 4) {
            const uint32_t a = wi[0], b = wi[1], c = wi[2], d = wi[3];
            wo[0] = a;
            wo[1] = b;
            wo[2] = c;
            wo[3] = d;
        }
        for (; count >= 4; count -= 4) *wo++ = *wi++;
        out = (unsigned char *)wo;
        in = (const unsigned char *)wi;
    }
    while (count--) *out++ = *in++;
    return destination;
}

void *memset(void *destination, int value, size_t count) {
    unsigned char *out = destination;
    const unsigned char byte = (unsigned char)value;
    for (; count && ((uintptr_t)out & 3); --count) *out++ = byte;
    word *w = (word *)out;
    const uint32_t pattern = byte * 0x01010101u;
    for (; count >= 16; count -= 16, w += 4) {
        w[0] = pattern;
        w[1] = pattern;
        w[2] = pattern;
        w[3] = pattern;
    }
    for (; count >= 4; count -= 4) *w++ = pattern;
    out = (unsigned char *)w;
    while (count--) *out++ = byte;
    return destination;
}
