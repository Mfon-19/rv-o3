// Run-length encode/decode benchmark exercising byte loads/stores in short loops.

#include "bench.h"

#define INPUT_LEN 4096u

static unsigned char input[INPUT_LEN];
static unsigned char encoded[2u * INPUT_LEN + 8u];
static unsigned char decoded[INPUT_LEN];

int main(void) {
    u32 pos = 0;
    while (pos < INPUT_LEN) {
        u32 run_len = 1u + (next_random() & 15u);
        unsigned char byte_val = (unsigned char)(next_random() & 0xFFu);

        for (u32 k = 0; k < run_len && pos < INPUT_LEN; k++)
            input[pos++] = byte_val;
    }

    u32 encoded_len = 0;
    pos = 0;

    while (pos < INPUT_LEN) {
        u32 run_len = 1;
        while (pos + run_len < INPUT_LEN && input[pos + run_len] == input[pos] &&
               run_len < 255u) {
            run_len++;
        }

        encoded[encoded_len++] = (unsigned char)run_len;
        encoded[encoded_len++] = input[pos];
        pos += run_len;
    }

    u32 decoded_len = 0;
    for (u32 idx = 0; idx < encoded_len; idx += 2) {
        for (u32 rep = 0; rep < encoded[idx]; rep++)
            decoded[decoded_len++] = encoded[idx + 1];
    }

    u32 matches = (decoded_len == INPUT_LEN);
    for (pos = 0; pos < INPUT_LEN; pos++) {
        if (decoded[pos] != input[pos])
            matches = 0;
    }

    u32 hash = 0;
    for (pos = 0; pos < INPUT_LEN; pos++)
        hash = hash * 33u + decoded[pos];

    print_int((int)matches);
    print_int((int)encoded_len);
    print_int((int)hash);
    return 0;
}
