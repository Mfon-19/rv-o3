// Minimal freestanding memcpy/memset required by Clang-generated RV32IM binaries.

typedef __SIZE_TYPE__ size_t;

void *memcpy(void *destination, const void *source, size_t count) {
    unsigned char *dst = (unsigned char *)destination;
    const unsigned char *src = (const unsigned char *)source;

    for (size_t i = 0; i < count; ++i)
        dst[i] = src[i];

    return destination;
}

void *memset(void *destination, int value, size_t count) {
    unsigned char *dst = (unsigned char *)destination;

    for (size_t i = 0; i < count; ++i)
        dst[i] = (unsigned char)value;

    return destination;
}
