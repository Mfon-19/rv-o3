// Recursive quicksort benchmark stressing branch prediction, RAS, and stack traffic.

#include "bench.h"

#define ELEMENT_COUNT 512u

static u32 values[ELEMENT_COUNT];

static void sort_range(int low, int high) {
    if (low >= high)
        return;

    u32 pivot = values[(u32)(low + (high - low) / 2)];
    int left = low;
    int right = high;

    while (left <= right) {
        while (values[left] < pivot)
            left++;
        while (values[right] > pivot)
            right--;

        if (left <= right) {
            u32 temp = values[left];
            values[left] = values[right];
            values[right] = temp;
            left++;
            right--;
        }
    }

    sort_range(low, right);
    sort_range(left, high);
}

int main(void) {
    for (u32 i = 0; i < ELEMENT_COUNT; i++)
        values[i] = next_random();

    sort_range(0, (int)ELEMENT_COUNT - 1);

    u32 sorted_ok = 1;
    for (u32 i = 1; i < ELEMENT_COUNT; i++) {
        if (values[i - 1] > values[i])
            sorted_ok = 0;
    }

    u32 checksum = 0;
    for (u32 i = 0; i < ELEMENT_COUNT; i++)
        checksum ^= values[i] + i;

    print_int((int)sorted_ok);
    print_int((int)checksum);
    return 0;
}
