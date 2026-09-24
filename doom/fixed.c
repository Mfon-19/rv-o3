/* FixedDiv without libgcc's signed 64-bit divide. Linked with
 * --wrap=FixedDiv, so the vendored engine keeps its own version unused.
 * The result is bit-identical to ((int64_t)a << 16) / b truncated to 32
 * bits. When |b| fits in 16 bits the 48-bit dividend splits exactly into
 * two 32-bit hardware divides:
 *   (|a| << 16) / |b| = (|a| / |b|) << 16  +  ((|a| % |b|) << 16) / |b|
 * because |a| % |b| < |b| <= 0xffff keeps the second dividend in 32 bits. 
 */
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include "m_fixed.h"

fixed_t __wrap_FixedDiv(fixed_t a, fixed_t b) {
    if ((abs(a) >> 14) >= abs(b))
        return (a ^ b) < 0 ? INT_MIN : INT_MAX;
    const uint32_t ua = a < 0 ? 0u - (uint32_t)a : (uint32_t)a;
    const uint32_t ub = b < 0 ? 0u - (uint32_t)b : (uint32_t)b;
    uint32_t q;
    if (ub <= 0xffff) {
        const uint32_t whole = ua / ub, rest = ua - whole * ub;
        q = (whole << 16) + (rest << 16) / ub;
    } else {
        q = (uint32_t)(((uint64_t)ua << 16) / ub);
    }
    return (fixed_t)((a ^ b) < 0 ? 0u - q : q);
}
