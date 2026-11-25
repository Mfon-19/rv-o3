// Bit-identical FixedDiv override (linked with --wrap=FixedDiv) using two
// hardware RV32IM 32-bit divides when the divisor fits in 16 bits instead of
// calling a slow 64-bit software divide helper.

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>

#include "m_fixed.h"

fixed_t __wrap_FixedDiv(fixed_t dividend, fixed_t divisor) {
    if ((abs(dividend) >> 14) >= abs(divisor))
        return (dividend ^ divisor) < 0 ? INT_MIN : INT_MAX;

    const uint32_t abs_dividend =
        dividend < 0 ? 0u - (uint32_t)dividend : (uint32_t)dividend;
    const uint32_t abs_divisor =
        divisor < 0 ? 0u - (uint32_t)divisor : (uint32_t)divisor;

    uint32_t quotient = 0;

    if (abs_divisor <= 0xffff) {
        const uint32_t whole = abs_dividend / abs_divisor;
        const uint32_t remainder = abs_dividend - whole * abs_divisor;
        quotient = (whole << 16) + (remainder << 16) / abs_divisor;
    } else {
        quotient = (uint32_t)(((uint64_t)abs_dividend << 16) / abs_divisor);
    }

    return (fixed_t)((dividend ^ divisor) < 0 ? 0u - quotient : quotient);
}
