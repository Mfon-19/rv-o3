/*
 * Even with -ffreestanding, a C compiler may lower aggregate initialization
 * and simple memory operations to memcpy or memset. On a hosted system libc
 * supplies those symbols. This demo has no libc, so it provides the two small
 * routines that compiler-generated code is most likely to request.
 *
 * __SIZE_TYPE__ is supplied by Clang and gives the target ABI's exact size_t
 * type without depending on any system headers.
 */
typedef __SIZE_TYPE__ size_t;

void* memcpy(void* destination, const void* source, size_t count) {
    unsigned char* out = (unsigned char*)destination;
    const unsigned char* in = (const unsigned char*)source;

    for (size_t i = 0; i < count; ++i) {
        out[i] = in[i];
    }
    return destination;
}

void* memset(void* destination, int value, size_t count) {
    unsigned char* out = (unsigned char*)destination;

    for (size_t i = 0; i < count; ++i) {
        out[i] = (unsigned char)value;
    }
    return destination;
}
