// Freestanding RV32IM C demo: insertion sort and Euclidean GCD printed via ECALL.

static __attribute__((noinline)) void print_int(int value) {
    register int a0 __asm__("a0") = value;
    register int a7 __asm__("a7") = 1;
    __asm__ volatile("ecall" : "+r"(a0) : "r"(a7) : "memory");
}

static __attribute__((noinline)) void print_string(const char *text) {
    register const char *a0 __asm__("a0") = text;
    register int a7 __asm__("a7") = 3;
    __asm__ volatile("ecall" : "+r"(a0) : "r"(a7) : "memory");
}

static __attribute__((noinline)) void insertion_sort(int *values,
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

static __attribute__((noinline)) int gcd(int left, int right) {
    while (right != 0) {
        int remainder = left % right;
        left = right;
        right = remainder;
    }

    return left;
}

int main(void) {
    int values[] = {7, -3, 42, 0, 15, -8, 23, 5, 1, 9};
    const unsigned count = sizeof(values) / sizeof(values[0]);

    insertion_sort(values, count);

    print_string("sorted:\n");
    for (unsigned i = 0; i < count; ++i)
        print_int(values[i]);

    print_string("gcd(84, 30):\n");
    print_int(gcd(84, 30));

    return 0;
}
