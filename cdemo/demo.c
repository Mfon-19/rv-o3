/*
 * A small, genuinely compiler-generated program for rvsim.
 *
 * This is freestanding C: there is no operating system or C library beneath
 * it. Normal functions, loops, stack variables, arrays, and arithmetic still
 * work because Clang lowers them directly to RV32IM instructions. Output must
 * use the simulator's deliberately small ecall interface instead of printf.
 */

/*
 * Fixing these variables to a0 and a7 follows the simulator's syscall ABI:
 * a7 contains the operation number and a0 contains its argument. The memory
 * clobber stops the compiler from moving memory operations across the ecall.
 */
static __attribute__((noinline)) void print_int(int value) {
    register int a0 __asm__("a0") = value;
    register int a7 __asm__("a7") = 1;
    __asm__ volatile("ecall" : "+r"(a0) : "r"(a7) : "memory");
}

static __attribute__((noinline)) void print_string(const char* text) {
    register const char* a0 __asm__("a0") = text;
    register int a7 __asm__("a7") = 3;
    __asm__ volatile("ecall" : "+r"(a0) : "r"(a7) : "memory");
}

/* noinline makes the demo exercise an ordinary function call and stack ABI. */
static __attribute__((noinline)) void insertion_sort(int* values,
                                                     unsigned count) {
    for (unsigned i = 1; i < count; ++i) {
        int key = values[i];
        unsigned j = i;

        while (j != 0 && values[j - 1] > key) {
            values[j] = values[j - 1];
            --j;
        }
        values[j] = key;
    }
}

/* Variable remainder compiles to an RV32M rem instruction. */
static __attribute__((noinline)) int gcd(int a, int b) {
    while (b != 0) {
        int remainder = a % b;
        a = b;
        b = remainder;
    }
    return a;
}

int main(void) {
    int values[] = {7, -3, 42, 0, 15, -8, 23, 5, 1, 9};
    const unsigned count = sizeof(values) / sizeof(values[0]);

    insertion_sort(values, count);

    /* String literals demonstrate that the linker also places .rodata. */
    print_string("sorted:\n");
    for (unsigned i = 0; i < count; ++i) {
        print_int(values[i]);
    }

    print_string("gcd(84, 30):\n");
    print_int(gcd(84, 30));

    /* start.S passes main's return value to syscall 93 as the exit status. */
    return 0;
}
