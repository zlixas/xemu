/*
 * Vectorized fast path for packed single precision SSE add/sub/mul
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef TARGET_I386_TCG_SSE_FAST_H
#define TARGET_I386_TCG_SSE_FAST_H

#include "fpu/softfloat.h"

enum SseFastOp {
    SSE_FAST_ADD,
    SSE_FAST_SUB,
    SSE_FAST_MUL,
};

#if defined(__aarch64__)
#include <arm_neon.h>

/*
 * Compute 4 lanes of a packed single precision add/sub/mul with one host
 * vector instruction, when every lane would take softfloat's hardfloat path
 * anyway (see float32_gen2): inexact already raised, round to nearest even,
 * and zero-or-normal inputs. Lanes whose result is tiny (and so may need
 * underflow or flush-to-zero handling) are redone with the scalar op, so
 * results and exception flags match per-lane softfloat exactly.
 *
 * Returns false, without touching dst, if the scalar loop must be used.
 * dst may alias a or b.
 */
static inline bool sse_fast_ps(float_status *st, uint32_t *dst,
                               const uint32_t *a, const uint32_t *b,
                               enum SseFastOp op)
{
    if (!(st->float_exception_flags & float_flag_inexact) ||
        st->float_rounding_mode != float_round_nearest_even) {
        return false;
    }

    uint32x4_t va = vld1q_u32(a);
    uint32x4_t vb = vld1q_u32(b);

    /* Zero or normal: exponent neither 0 nor 0xff, or magnitude zero */
    const uint32x4_t exp_mask = vdupq_n_u32(0x7f800000);
    const uint32x4_t abs_mask = vdupq_n_u32(0x7fffffff);
    uint32x4_t ea = vandq_u32(va, exp_mask);
    uint32x4_t eb = vandq_u32(vb, exp_mask);
    uint32x4_t zon_a = vorrq_u32(
        vandq_u32(vmvnq_u32(vceqzq_u32(ea)), vmvnq_u32(vceqq_u32(ea, exp_mask))),
        vceqzq_u32(vandq_u32(va, abs_mask)));
    uint32x4_t zon_b = vorrq_u32(
        vandq_u32(vmvnq_u32(vceqzq_u32(eb)), vmvnq_u32(vceqq_u32(eb, exp_mask))),
        vceqzq_u32(vandq_u32(vb, abs_mask)));
    if (vminvq_u32(vandq_u32(zon_a, zon_b)) == 0) {
        return false;
    }

    float32x4_t fa = vreinterpretq_f32_u32(va);
    float32x4_t fb = vreinterpretq_f32_u32(vb);
    float32x4_t fr;
    switch (op) {
    case SSE_FAST_ADD:
        fr = vaddq_f32(fa, fb);
        break;
    case SSE_FAST_SUB:
        fr = vsubq_f32(fa, fb);
        break;
    default:
        fr = vmulq_f32(fa, fb);
        break;
    }
    uint32x4_t vr = vreinterpretq_u32_f32(fr);
    uint32x4_t abs_r = vandq_u32(vr, abs_mask);

    if (vmaxvq_u32(vceqq_u32(abs_r, exp_mask))) {
        float_raise(float_flag_overflow, st);
    }

    /*
     * Tiny results (|r| <= FLT_MIN) may need underflow or flush-to-zero
     * handling: redo those lanes in scalar. Exactly zero results are exact,
     * as in float32_gen2: for add/sub a zero sum of zero-or-normal inputs is
     * exact cancellation, and a product with a zero input is an exact zero.
     */
    uint32x4_t tiny = vcleq_u32(abs_r, vdupq_n_u32(0x00800000));
    if (op == SSE_FAST_MUL) {
        uint32x4_t zero_in = vorrq_u32(vceqzq_u32(vandq_u32(va, abs_mask)),
                                       vceqzq_u32(vandq_u32(vb, abs_mask)));
        tiny = vbicq_u32(tiny, zero_in);
    } else {
        tiny = vbicq_u32(tiny, vceqzq_u32(abs_r));
    }
    if (vmaxvq_u32(tiny)) {
        uint32_t r[4], t[4], la[4], lb[4];
        vst1q_u32(r, vr);
        vst1q_u32(t, tiny);
        vst1q_u32(la, va);
        vst1q_u32(lb, vb);
        for (int i = 0; i < 4; i++) {
            if (!t[i]) {
                continue;
            }
            switch (op) {
            case SSE_FAST_ADD:
                r[i] = float32_add(la[i], lb[i], st);
                break;
            case SSE_FAST_SUB:
                r[i] = float32_sub(la[i], lb[i], st);
                break;
            default:
                r[i] = float32_mul(la[i], lb[i], st);
                break;
            }
        }
        vr = vld1q_u32(r);
    }

    vst1q_u32(dst, vr);
    return true;
}

#else

static inline bool sse_fast_ps(float_status *st, uint32_t *dst,
                               const uint32_t *a, const uint32_t *b,
                               enum SseFastOp op)
{
    return false;
}

#endif

#endif /* TARGET_I386_TCG_SSE_FAST_H */
