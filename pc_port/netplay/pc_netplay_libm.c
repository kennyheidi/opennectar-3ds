/* Deterministic software libm for netplay builds.
 *
 * Netplay build only: this TU is added to the link in CMakeLists.txt under
 * `if(PIKMIN_NETPLAY_BUILD)`, so the default build never sees it. The plain
 * C symbols below (sinf, cosf, sincosf, tanf, atanf, atan2f, asinf, acosf,
 * expf, logf, log10f, log2f, exp2f, powf, fmodf, hypotf, sqrtf and the
 * double versions sin, cos, sincos, tan, asin, acos, atan, atan2, exp, log,
 * log10, log2, exp2, pow, fmod, hypot, sqrt) are strong definitions inside
 * the game executable. At static link they satisfy the game's undefined references,
 * so the linker never pulls the mingw-w64 libm archive members that use
 * x87 transcendentals (fsin/fcos/fsincos/fpatan/f2xm1/fyl2x), and never
 * emits the msvcrt.dll `tan` import thunk. Verified by disassembly: the
 * netplay exe's sinf/tanf resolve to the addresses of this file's
 * functions, and `tools/netplay/fp_gate.py` reports zero x87
 * transcendentals and zero msvcrt math imports.
 *
 * Determinism contract: every routine below uses only
 *   - integer bit manipulation on the IEEE-754 representation,
 *   - binary32/binary64 `+ - * /` and `sqrt` (SSE2 `sqrtsd`/`sqrtss`,
 *     exact IEEE-754, bit identical on every x86-64 CPU for the same
 *     inputs; via the pc_sqrt/pc_sqrtf helpers below, never a CRT
 *     `sqrt`/`sqrtf` call),
 *   - branches and table lookups.
 * There is no `long double`, no x87 inline asm, and this file is compiled
 * with per-name `-fno-builtin-sinf ...` (see CMakeLists.txt) so GCC never
 * lowers one of the overridden names to x87 microcode, while `memcpy`
 * stays a builtin and folds to a move. No routine calls back into the
 * CRT libm. fenv state is pinned per tick by pc_netplay_det (MXCSR
 * 0x1F80, x87 word 0x037F), so rounding is always round-to-nearest.
 *
 * Source policy:
 *   - The tree's Metrowerks/MSL sources `src/MSL_C/PPCEABI/bare/H/trigf.c`
 *     (with tables from `common_float_tables.c`) and `inverse_trig.c` were
 *     ported first (functions pc_msl_*, retained under
 *     PC_NETPLAY_LIBM_USE_MSL_FLOAT_TRIG for reference; licence-wise they
 *     are already part of the tree). Measurement against the mingw libm
 *     showed 100-900 ulp divergence on ordinary game domains: the console
 *     algorithm's single-precision argument reduction cancels catastroph-
 *     ically near quadrant boundaries, and 1-x*x cancels near |x|=1. That
 *     is inherent to the 1990s algorithm, not a port error, so the public
 *     float entry points instead round the fdlibm-style double kernels
 *     below once (this deviation is required by the accuracy clause).
 *   - atan/atan2/asin/acos (double) follow the fdlibm files already in the
 *     tree: `s_atan.c` (aT[] coefficients, same Sun licence header family)
 *     and the structure of `e_atan2.c`. asin/acos wrap our atan2.
 *   - pow/powf port `e_pow.c` (already in the tree, Sun licence) with
 *     PC-only changes: fdlibm.h bit macros redone with __builtin_memcpy,
 *     fabs via bit mask, scalbn via exact exponent/mantissa adjustment,
 *     sqrt via the SSE pc_sqrt helper above, `__float_nan` via 0.0/0.0.
 *   - exp/log (double) use range reduction plus the polynomial kernels
 *     whose coefficients come from that same `e_pow.c` (P1..P5 for 2^z,
 *     L1..L6/dp_h/dp_l/bp for log2), i.e. fdlibm-style. float wrappers round the double result once.
 *   - sin/cos/tan (double) use Cody-Waite reduction with a split pi/2
 *     plus fdlibm-style sine/cosine kernels; the float trig entry points
 *     round these once (see above for why the MSL float path is kept off).
 *   - fmod/fmodf are exact for quotients below 2^52 (trunc plus bounded
 *     correction; 0 ulp vs a correct fmod by construction) and
 *     deterministic beyond. hypot/hypotf scale then call sqrt, matching
 *     the reference to < 1 ulp on game domains.
 *
 * Accuracy: on normal game domains (|angle| <= 1e8 for
 * trig, |x| <= 710 for exp, x in [2^-30, 2^30] for log, |x| <= 1e6 for
 * fmod, base/exponent in the ranges the game uses for pow) every float
 * entry point is at most 1 ulp from the mingw-w64 libm - measured 0 ulp
 * on ~1.2M-point grids - by `pc_netplay_libm_test` (dense deterministic
 * grids plus edge cases: 0, +-0, +-pi multiples, large arguments, NaN,
 * inf, denormals). Trig stays ulp-clean far beyond the game domain:
 * direct Cody-Waite reduction runs to |x| = 1e8 and the double-double 2pi
 * pre-reduction above keeps it there to |x| = 2.8e16 (measured 0 ulp to
 * |x| = 1e10). Past that the deterministic fmod core takes over
 * (bit-identical on every CPU, accuracy there is checksum-only, not
 * ulp-asserted). Full per-function maxima are printed by the test; the
 * netplay ctest asserts max <= 1 ulp on the asserted domains and folds
 * every result, including edges and huge args, into the golden checksum.
 */

#include "netplay/pc_netplay_libm.h"

#include <errno.h>
#include <float.h>
#include <math.h>
#include <stdint.h>
#include <string.h>
#if !defined(__aarch64__)
#include <emmintrin.h>
#endif

/* ------------------------------------------------------------------ */
/* Bit helpers (__builtin_memcpy: no aliasing UB, folds to a move even */
/* under -fno-builtin-*; plain memcpy would stay a CRT call there).    */
/* SSE square roots (exact IEEE-754, never an x87 fsqrt): a plain      */
/* sqrt() call lowers to sqrtsd + a CRT fallback for errno, which is   */
/* the mingw x87 fsqrt path this file exists to avoid. */
/* ------------------------------------------------------------------ */

#if defined(__aarch64__)
/* ARM64 (Android): fsqrt is the IEEE-754 correctly rounded square root, so
   it gives the same bits as SSE sqrtsd/sqrtss; written as asm so no libm
   call or errno path can appear. */
static double pc_sqrt(double x)
{
	double r;
	__asm__("fsqrt %d0, %d1" : "=w"(r) : "w"(x));
	return r;
}

static float pc_sqrtf(float x)
{
	float r;
	__asm__("fsqrt %s0, %s1" : "=w"(r) : "w"(x));
	return r;
}
#else
static double pc_sqrt(double x)
{
	__m128d v = _mm_set_sd(x);
	v = _mm_sqrt_sd(_mm_setzero_pd(), v);
	return _mm_cvtsd_f64(v);
}

static float pc_sqrtf(float x)
{
	__m128 v = _mm_set_ss(x);
	v = _mm_sqrt_ss(v);
	return _mm_cvtss_f32(v);
}
#endif

static uint32_t f32_bits(float x)
{
	uint32_t u;
	__builtin_memcpy(&u, &x, 4);
	return u;
}

static float bits_f32(uint32_t u)
{
	float x;
	__builtin_memcpy(&x, &u, 4);
	return x;
}

static uint64_t f64_bits(double x)
{
	uint64_t u;
	__builtin_memcpy(&u, &x, 8);
	return u;
}

static double bits_f64(uint64_t u)
{
	double x;
	__builtin_memcpy(&x, &u, 8);
	return x;
}

static float pc_fabsf(float x) { return bits_f32(f32_bits(x) & 0x7fffffffu); }

static double pc_fabs(double x) { return bits_f64(f64_bits(x) & 0x7fffffffffffffffull); }

static int pc_isnan_d(double x)
{
	uint64_t u = f64_bits(x);
	return ((u >> 52) & 0x7ffu) == 0x7ffu && (u & 0xfffffffffffffull) != 0;
}

static int pc_isinf_d(double x)
{
	uint64_t u = f64_bits(x);
	return ((u >> 52) & 0x7ffu) == 0x7ffu && (u & 0xfffffffffffffull) == 0;
}

static int pc_isnan_f(float x)
{
	uint32_t u = f32_bits(x);
	return ((u >> 23) & 0xffu) == 0xffu && (u & 0x7fffffu) != 0;
}

static int pc_isinf_f(float x)
{
	uint32_t u = f32_bits(x);
	return ((u >> 23) & 0xffu) == 0xffu && (u & 0x7fffffu) == 0;
}

/* Exact 2^n scaling (no libm call). Normal range: exponent adjust (exact).
 * Overflow: signed inf. Subnormal range: mantissa shift with
 * round-to-nearest-even (exact, so exp/pow stay <= 1 ulp into denormals). */
static double pc_scalbn(double x, int n)
{
	uint64_t u;
	int e;
	uint64_t sign, m;
	if (x == 0.0 || pc_isnan_d(x) || pc_isinf_d(x))
		return x;
	u = f64_bits(x);
	sign = u & 0x8000000000000000ull;
	e = (int)((u >> 52) & 0x7ffu) + n;
	if (e >= 0x7ff)
		return sign ? bits_f64(0xfff0000000000000ull) : bits_f64(0x7ff0000000000000ull);
	m = (u & 0xfffffffffffffull) | 0x10000000000000ull;
	if (e >= 1) {
		u = sign | ((uint64_t)(unsigned)e << 52) | (m & 0xfffffffffffffull);
		return bits_f64(u);
	} {
		/* Subnormal: right-shift the 53-bit mantissa by (1 - e) with
		 * round-to-nearest-even; a carry out returns the smallest normal. */
		int shift = 1 - e;
		uint64_t q, dropped, roundbit, sticky;
		if (shift >= 64) {
			/* Every mantissa bit shifts out: the value is below half
			 * the smallest subnormal, so it rounds to signed zero. */
			return sign ? bits_f64(0x8000000000000000ull) : 0.0;
		}
		dropped = m & ((1ull << shift) - 1ull);
		q = m >> shift;
		roundbit = (dropped >> (shift - 1)) & 1u;
		sticky = (shift >= 2) ? (dropped & ((1ull << (shift - 1)) - 1ull)) : 0u;
		if (roundbit && (sticky || (q & 1u)))
			q++;
		if (q >= 0x10000000000000ull)
			return bits_f64(sign | (0x0010000000000000ull));
		return bits_f64(sign | q);
	}
}

/* Set to 1 to route the float trig entry points through the ported MSL
 * algorithm below instead of the double kernels. Kept for archaeology:
 * the MSL port (trigf.c/inverse_trig.c, tables verbatim from the tree)
 * was measured at 100-900 ulp from the mingw libm on game domains
 * (single-precision argument reduction near quadrant boundaries and
 * cancellation in 1-x*x near |x|=1 are inherent to the 1990s console
 * algorithm, not port errors), so the accuracy requirement routes the
 * public entry points through the fdlibm-style double kernels rounded
 * once (<= 1 ulp, verified by pc_netplay_libm_test). */
#define PC_NETPLAY_LIBM_USE_MSL_FLOAT_TRIG 0

/* ------------------------------------------------------------------ */
/* MSL tables (copied from src/MSL_C/PPCEABI/bare/H/common_float_tables.c */
/* and the tmp_float initializer in trigf.c).                          */
/* ------------------------------------------------------------------ */

/* Reference tables for the MSL path (external linkage so they never warn
 * as unused when the MSL route is switched off; 84 bytes total). */
const float pc_sincos_on_quadrant[8] = {
	0.0f, 1.0f, 1.0f, 0.0f, 0.0f, -1.0f, -1.0f, 0.0f,
};

const float pc_sincos_poly[10] = {
	3.52876168108e-6f, 3.08974705376e-7f, -0.000325936503941f,
	-3.65723499272e-5f, 0.0158543232828f, 0.00249039311893f,
	-0.30842512846f, -0.080745510757f, 1.0f,
	0.785398185253f,
};

/* (4/pi - 1) split into 4 floats; sums to 0.273239544735... = 4/pi - 1. */
const float pc_four_over_pi_m1[4] = {
	0.25f, 0.0232393741608f, 1.70555722434e-7f, 1.86736494323e-11f,
};

#define PC_PI_F 3.141592653589793f
#define PC_PI_O2_F 1.57079632679489661923132169163975f
#define PC_TRIG_EPS_F 3.45266983e-4f
/* |x| above this is pre-reduced with the exact fmod below. 61500 rad is
 * far outside any game angle domain and keeps (int)(z +- 0.5) in range. */
#define PC_TRIG_CUTOFF_F 61500.0f
/* Dekker TwoProd (error-free product, no FMA): p+e == a*b exactly, using
 * only SSE + - *. Valid for the magnitudes used below (|a| < 2^53 keeps
 * S*a below overflow; callers keep S*b below overflow too). */
static void pc_two_prod(double a, double b, double *p, double *e)
{
	static const double S = 134217729.0; /* 2^27 + 1 */
	double c, ah, al, bh, bl;
	c = S * a;
	ah = c - (c - a);
	al = a - ah;
	c = S * b;
	bh = c - (c - b);
	bl = b - bh;
	*p = a * b;
	*e = ((ah * bh - *p) + ah * bl + al * bh) + al * bl;
}

/* Exact-when-it-matters fmod core.
 *
 * Fast path (quotient < 2^52, i.e. every game/test input): the product
 * n*ay is computed EXACTLY as p+e via TwoProd, so r = (ax-p)-e is the
 * correctly rounded remainder: 0 ulp vs a correct fmod, hence 0 ulp vs
 * the platform libm wherever it is correctly rounded. (The naive
 * ax-n*ay would round once at ulp(ax), which near zero crossings costs
 * hundreds of ulps for large ax.)
 * Fallback (larger ratios; never hit in game/test domains): halve/double
 * stripping. Every step is one SSE op, so the result is still
 * bit-deterministic on any x86-64 CPU; it is documented as
 * deterministic-but-not-guaranteed-exact out there.
 */
static double pc_fmod_core(double x, double y)
{
	double ax = pc_fabs(x), ay = pc_fabs(y);
	if (pc_isnan_d(x) || pc_isnan_d(y))
		return bits_f64(0x7ff8000000000000ull);
	if (pc_isinf_d(x) || ay == 0.0)
		return bits_f64(0x7ff8000000000000ull);
	if (pc_isinf_d(y) || ax < ay)
		return x;
	if (ax == 0.0)
		return x;
	{
		double q = ax / ay;
		if (q < 4503599627370496.0) { /* 2^52 */
			long long n = (long long)q;
			double dn = (double)n;
			double r;
			if (ay < 9.0e299) {
				double p, e, t1;
				pc_two_prod(dn, ay, &p, &e);
				t1 = ax - p;
				r = t1 - e;
			} else {
				r = ax - dn * ay;
			}
			if (r >= ay)
				r -= ay;
			if (r >= ay)
				r -= ay;
			if (r < 0.0)
				r += ay;
			if (r < 0.0)
				r += ay;
			return (f64_bits(x) >> 63) ? -r : r;
		}
		/* Huge-ratio path (exact for all sane inputs, deterministic for
		 * all inputs): halve ax (exact in the normal range) until the
		 * quotient fits below 2^52, take the exact base remainder, then
		 * double back with conditional subtracts. Doubling a remainder
		 * r < ay gives 2r < 2ay, so each subtract is exact by Sterbenz;
		 * every step is one SSE op, hence bit-deterministic on any
		 * x86-64 CPU. Inputs needing subnormal halving (ratio above
		 * ~2^100, never in game/test domains) stay deterministic but
		 * are not guaranteed bit-exact. */
		{
			int k = 0;
			while ((ax / ay) >= 4503599627370496.0 && k < 10000) {
				ax *= 0.5;
				k++;
			}
			{
				long long n = (ax / ay >= 4503599627370496.0) ? 0 : (long long)(ax / ay);
				double r = (n == 0) ? ax : ax - (double)n * ay;
				if (r >= ay)
					r -= ay;
				if (r >= ay)
					r -= ay;
				if (r < 0.0)
					r += ay;
				if (r < 0.0)
					r += ay;
				ax = r;
			}
			while (k-- > 0) {
				ax *= 2.0;
				if (pc_isinf_d(ax)) {
					ax = 0.0;
					break;
				}
				if (ax >= ay)
					ax -= ay;
			}
			return (f64_bits(x) >> 63) ? -ax : ax;
		}
	}
}

#if PC_NETPLAY_LIBM_USE_MSL_FLOAT_TRIG

/* ------------------------------------------------------------------ */
/* MSL float trig (ported from trigf.c).                               */
/* ------------------------------------------------------------------ */

static float pc_msl_sinf(float x)
{
	int n;
	float y, ysq, z;
	if (pc_isnan_f(x) || pc_isinf_f(x))
		return bits_f32(0x7fc00000u);
	if (pc_fabsf(x) > PC_TRIG_CUTOFF_F)
		x = (float)pc_fmod_core((double)x, 6.283185307179586);
	z = (2.0f / PC_PI_F) * x;
	n = (f32_bits(x) & 0x80000000u) ? (int)(z - 0.5f) : (int)(z + 0.5f);
	y = x - (float)n * 2.0f + pc_four_over_pi_m1[0] * x + pc_four_over_pi_m1[1] * x
	    + pc_four_over_pi_m1[2] * x + pc_four_over_pi_m1[3] * x;
	n &= 3;
	if (pc_fabsf(y) < PC_TRIG_EPS_F) {
		n <<= 1;
		return pc_sincos_on_quadrant[n] + (pc_sincos_on_quadrant[n + 1] * y * pc_sincos_poly[9]);
	}
	ysq = y * y;
	if (n & 1) {
		n <<= 1;
		z = (((pc_sincos_poly[0] * ysq + pc_sincos_poly[2]) * ysq + pc_sincos_poly[4]) * ysq
		        + pc_sincos_poly[6])
		        * ysq
		    + pc_sincos_poly[8];
		return z * pc_sincos_on_quadrant[n];
	} else {
		n <<= 1;
		z = (((((pc_sincos_poly[1] * ysq + pc_sincos_poly[3]) * ysq + pc_sincos_poly[5]) * ysq
		         + pc_sincos_poly[7])
		         * ysq
		     + pc_sincos_poly[9])
		    * y);
		return z * pc_sincos_on_quadrant[n + 1];
	}
}

static float pc_msl_cosf(float x)
{
	int n;
	float y, ysq, z;
	if (pc_isnan_f(x) || pc_isinf_f(x))
		return bits_f32(0x7fc00000u);
	if (pc_fabsf(x) > PC_TRIG_CUTOFF_F)
		x = (float)pc_fmod_core((double)x, 6.283185307179586);
	z = (2.0f / PC_PI_F) * x;
	n = (f32_bits(x) & 0x80000000u) ? (int)(z - 0.5f) : (int)(z + 0.5f);
	y = x - (float)n * 2.0f + pc_four_over_pi_m1[0] * x + pc_four_over_pi_m1[1] * x
	    + pc_four_over_pi_m1[2] * x + pc_four_over_pi_m1[3] * x;
	n &= 3;
	if (pc_fabsf(y) < PC_TRIG_EPS_F) {
		n <<= 1;
		return pc_sincos_on_quadrant[n + 1] - y * pc_sincos_on_quadrant[n];
	}
	ysq = y * y;
	if (n & 1) {
		n <<= 1;
		z = -((((pc_sincos_poly[1] * ysq + pc_sincos_poly[3]) * ysq + pc_sincos_poly[5]) * ysq
		         + pc_sincos_poly[7])
		         * ysq
		     + pc_sincos_poly[9])
		    * y;
		return z * pc_sincos_on_quadrant[n];
	} else {
		n <<= 1;
		z = (((pc_sincos_poly[0] * ysq + pc_sincos_poly[2]) * ysq + pc_sincos_poly[4]) * ysq
		        + pc_sincos_poly[6])
		        * ysq
		    + pc_sincos_poly[8];
		return z * pc_sincos_on_quadrant[n + 1];
	}
}

/* Joint kernel: one reduction, both polynomials (for sincosf). */
static void pc_msl_sincosf_kernel(float x, float *s, float *c)
{
	int n;
	float y, ysq, z, sn, cs;
	if (pc_isnan_f(x) || pc_isinf_f(x)) {
		*s = bits_f32(0x7fc00000u);
		*c = bits_f32(0x7fc00000u);
		return;
	}
	if (pc_fabsf(x) > PC_TRIG_CUTOFF_F)
		x = (float)pc_fmod_core((double)x, 6.283185307179586);
	z = (2.0f / PC_PI_F) * x;
	n = (f32_bits(x) & 0x80000000u) ? (int)(z - 0.5f) : (int)(z + 0.5f);
	y = x - (float)n * 2.0f + pc_four_over_pi_m1[0] * x + pc_four_over_pi_m1[1] * x
	    + pc_four_over_pi_m1[2] * x + pc_four_over_pi_m1[3] * x;
	n &= 3;
	ysq = y * y;
	if (pc_fabsf(y) < PC_TRIG_EPS_F) {
		int m = n << 1;
		*s = pc_sincos_on_quadrant[m] + (pc_sincos_on_quadrant[m + 1] * y * pc_sincos_poly[9]);
		*c = pc_sincos_on_quadrant[m + 1] - y * pc_sincos_on_quadrant[m];
		return;
	}
	if (n & 1) {
		int m = n << 1;
		z = (((pc_sincos_poly[0] * ysq + pc_sincos_poly[2]) * ysq + pc_sincos_poly[4]) * ysq
		        + pc_sincos_poly[6])
		        * ysq
		    + pc_sincos_poly[8];
		sn = z * pc_sincos_on_quadrant[m];
		z = -((((pc_sincos_poly[1] * ysq + pc_sincos_poly[3]) * ysq + pc_sincos_poly[5]) * ysq
		         + pc_sincos_poly[7])
		         * ysq
		     + pc_sincos_poly[9])
		    * y;
		cs = z * pc_sincos_on_quadrant[m];
	} else {
		int m = n << 1;
		z = (((((pc_sincos_poly[1] * ysq + pc_sincos_poly[3]) * ysq + pc_sincos_poly[5]) * ysq
		         + pc_sincos_poly[7])
		         * ysq
		     + pc_sincos_poly[9])
		    * y);
		sn = z * pc_sincos_on_quadrant[m + 1];
		z = (((pc_sincos_poly[0] * ysq + pc_sincos_poly[2]) * ysq + pc_sincos_poly[4]) * ysq
		        + pc_sincos_poly[6])
		        * ysq
		    + pc_sincos_poly[8];
		cs = z * pc_sincos_on_quadrant[m + 1];
	}
	*s = sn;
	*c = cs;
}
/* ------------------------------------------------------------------ */
/* MSL float inverse trig (ported from inverse_trig.c).                */
/* ------------------------------------------------------------------ */

static float pc_msl_inv_sqrtf(float x)
{
	const float half = 0.5f;
	const float three = 3.0f;
	if (x > 0.0f) {
		/* MSL seeds this with __frsqrte (a coarse reciprocal-sqrt
		 * estimate) and refines 3x. On PC the SSE sqrtf gives a
		 * correctly rounded seed, so two refinements overshoot the
		 * original precision while staying pure SSE. */
		float guess = 1.0f / pc_sqrtf(x);
		guess = half * guess * (three - guess * guess * x);
		guess = half * guess * (three - guess * guess * x);
		return guess;
	} else if (x != 0.0f) {
		return bits_f32(0x7fc00000u);
	}
	return bits_f32(0x7f800000u);
}

/* Ported verbatim from inverse_trig.c atanf (poly #4964). */
static float pc_msl_atanf(float x)
{
	float z, z_square;
	int index = -1, inv = 0;
	uint32_t sign;
	static const float atan_coeff[] = { 0.999999999f, -0.3333333213f, 0.19999886356f, -0.14281650536f,
		0.11041179874f, -0.084597554152f, 0.04714243524f };
	static const float onep_one_over_xisqr_hi[]
	    = { 6.82842f, 3.239828f, 2.0f, 1.446462f, 1.17157292f, 1.039566130f };
	static const float onep_one_over_xisqr_lo[]
	    = { 0.000007135f, 0.00000082f, 0.0f, 0.00000063f, 0.0f, 0.0f };
	static const float atan_xi_hi[]
	    = { 0.0f, 0.39269f, 0.5890486f, 0.7853981f, 0.981747f, 1.178097f, 1.374446f };
	static const float atan_xi_lo[] = { 0.0f, 0.000009081698724f, 0.000000023f, 0.000000063f,
		0.000000704f, 0.00000025f, 0.00000079f };
	static const float one_over_xi_hi[]
	    = { 2.414213f, 1.49660575f, 1.00000000f, 0.668178618f, 0.414213568f, 0.198912367f };
	static const float one_over_xi_lo[] = { 0.000000562f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };

	if (pc_isnan_f(x))
		return x + x;
	if (pc_isinf_f(x))
		return (f32_bits(x) >> 31) ? -PC_PI_O2_F : PC_PI_O2_F;
	sign = f32_bits(x) & 0x80000000u;
	x = bits_f32(f32_bits(x) & 0x7fffffffu);
	if (x >= 2.414213565f) {
		z = 1.0f / x;
		inv++;
	} else if (0.4142135624f < x) {
		uint32_t hibits;
		index++;
		hibits = f32_bits(x) & 0x7f800000u;
		if (hibits == 0x3f000000u) {
			if (f32_bits(x) >= 0x3f08d5b9u)
				index++;
			if (f32_bits(x) >= 0x3f521801u)
				index++;
		} else if (hibits == 0x3f800000u) {
			index += 2;
			if (f32_bits(x) >= 0x3f9bf7ecu)
				index++;
			if (f32_bits(x) >= 0x3fef789eu)
				index++;
		} else if (hibits == 0x40000000u) {
			index += 4;
		}
		z = 1.0f / (one_over_xi_hi[index] + (one_over_xi_lo[index] + x));
		z = (one_over_xi_hi[index] - onep_one_over_xisqr_hi[index] * z)
		    + (one_over_xi_lo[index] - onep_one_over_xisqr_lo[index] * z);
	} else {
		z = x;
	}
	z_square = z * z;
	z += z * z_square
	    * (atan_coeff[1]
	        + z_square
	            * (atan_coeff[2]
	                + z_square
	                    * (atan_coeff[3]
	                        + z_square
	                            * (atan_coeff[4] + z_square * (atan_coeff[5] + z_square * atan_coeff[6])))));
	z += atan_xi_lo[index + 1];
	z += atan_xi_hi[index + 1];
	if (inv) {
		z -= PC_PI_O2_F;
		if (sign)
			return z;
		return -z;
	}
	return bits_f32(f32_bits(z) | sign);
}

/* Ported from inverse_trig.c atan2f (signbit spelled with bit ops). */
static float pc_msl_atan2f(float y, float x)
{
	uint32_t sx = f32_bits(x) >> 31, sy = f32_bits(y) >> 31;
	if (pc_isnan_f(x) || pc_isnan_f(y))
		return x + y;
	if (sx == sy) {
		if (sx != 0) {
			return pc_msl_atanf(y / x) - PC_PI_F;
		} else if (x != 0.0f) {
			return pc_msl_atanf(y / x);
		} else if (!pc_isinf_f(y)) {
			return PC_PI_O2_F;
		} else {
			return y > 0.0f ? PC_PI_O2_F : -PC_PI_O2_F;
		}
	} else if (x < 0.0f) {
		return PC_PI_F + pc_msl_atanf(y / x);
	} else if (x != 0.0f) {
		return pc_msl_atanf(y / x);
	}
	/* x == +0 with opposite signs: copysign(pi/2, y). */
	return bits_f32((sy << 31) | 0x3fc90fdbu);
}

static float pc_msl_acosf(float x)
{
	if (pc_isnan_f(x))
		return x + x;
	if (x > 1.0f || x < -1.0f)
		return bits_f32(0x7fc00000u);
	if (x == 1.0f)
		return 0.0f;
	if (x == -1.0f)
		return PC_PI_F;
	return PC_PI_O2_F - pc_msl_atanf(x * pc_msl_inv_sqrtf(1.0f - x * x));
}

/* No MSL float asin exists; mirror acosf through atanf. */
static float pc_msl_asinf(float x)
{
	if (pc_isnan_f(x))
		return x + x;
	if (x > 1.0f || x < -1.0f)
		return bits_f32(0x7fc00000u);
	if (x == 1.0f)
		return PC_PI_O2_F;
	if (x == -1.0f)
		return -PC_PI_O2_F;
	return pc_msl_atanf(x * pc_msl_inv_sqrtf(1.0f - x * x));
}

#endif /* PC_NETPLAY_LIBM_USE_MSL_FLOAT_TRIG */

/* ------------------------------------------------------------------ */
/* Double trig: Cody-Waite reduction + fdlibm-style kernels.           */
/* Coefficients match the classic fdlibm k_sin.c/k_cos.c minimax sets  */
/* (same Sun licence family as the tree's s_atan.c/e_pow.c).           */
/* ------------------------------------------------------------------ */

static const double PC_PI_H = 1.5707963267948965580;          /* pi/2, high 33 bits */
static const double PC_PI_L = 6.12323399573676603587e-17;     /* pi/2 low part */
static const double PC_INV_PI2 = 0.63661977236758134308;      /* 2/pi */
static const double PC_PI_D = 3.14159265358979323846;
static const double PC_PI_O2_D = 1.57079632679489661923;

static double pc_k_sin(double x, double x2)
{
	const double s1 = -1.66666666666666324348e-01, s2 = 8.33333333332248946124e-03,
	             s3 = -1.98412698298579493134e-04, s4 = 2.75573137070700676768e-06,
	             s5 = -2.50507602534068634195e-08, s6 = 1.58969099521155010221e-10;
	return x + x * x2 * (s1 + x2 * (s2 + x2 * (s3 + x2 * (s4 + x2 * (s5 + x2 * s6)))));
}

static double pc_k_cos(double x2)
{
	const double c1 = 4.16666666666666019037e-02, c2 = -1.38888888888741095749e-03,
	             c3 = 2.48015872894767294178e-05, c4 = -2.75573143513906633035e-07,
	             c5 = 2.08757232129817482790e-09, c6 = -1.13596475577881948265e-11;
	return 1.0 - x2 * 0.5 + x2 * x2 * (c1 + x2 * (c2 + x2 * (c3 + x2 * (c4 + x2 * (c5 + x2 * c6)))));
}

/* 2*pi to ~105 bits (double-double): HI is the double 2*pi, LO the
 * correctly-rounded remainder. Error of HI+LO vs true 2*pi is ~2^-105,
 * so pre-reduction below is accurate to ~|x|*2^-105 + 2^-53. */
static const double PC_2PI_HI = 6.283185307179586;
static const double PC_2PI_LO = 2.4492935982947064e-16;
static const double PC_INV2PI = 0.15915494309189535;

/* Reduce x to quadrant q and remainder r in [-pi/4, pi/4].
 *
 * |x| <= 1e8: direct Cody-Waite with a split pi/2. H1 holds the top 27
 * mantissa bits of pi/2 (low 26 bits masked off at runtime: deterministic
 * bit ops), so n*H1 is EXACT for |n| < 2^26, i.e. the whole direct range.
 * Residual rounding is ~2^-80 absolute.
 * 1e8 < |x| <= 2.8e16 (ratio below 2^52): extended-modulus pre-reduction
 * t = x - k*C with C = 2*pi to ~105 bits, the k*C_HI product computed
 * EXACTLY via TwoProd. Error ~1e-15 absolute (vs ~k*2^-53 for a plain
 * double-modulus fmod, which costs hundreds of ulps near crossings).
 * Beyond (up to FLT_MAX): the deterministic fmod core (accuracy there is
 * checksum-only, not ulp-asserted).
 * A quadrant guard repairs the 2^-53-rare wrongly-rounded n in both
 * stages; either representation is valid to ~1e-16, so the guard cannot
 * change any float result by more than the 2^-38-rare boundary flip.
 * Net: the kernels below match a correctly-rounded libm to ~1 ulp
 * double on every asserted input. */
static void pc_trig_reduce(double x, long *q, double *r)
{
	double base = x, fn, H1, H2, t;
	long n;
	uint64_t h1b;
	if (pc_fabs(x) > 1.0e8) {
		double kf = x * PC_INV2PI;
		if (pc_fabs(kf) < 4503599627370496.0) {
			long long k = (long long)(kf + (kf >= 0.0 ? 0.5 : -0.5));
			double p, e, t1, t2;
			pc_two_prod((double)k, PC_2PI_HI, &p, &e);
			t1 = x - p;
			t2 = t1 - e;
			t = t2 - (double)k * PC_2PI_LO;
			if (t > PC_PI_D) {
				t = (t - PC_2PI_HI) - PC_2PI_LO;
			} else if (t < -PC_PI_D) {
				t = (t + PC_2PI_HI) + PC_2PI_LO;
			}
			base = t;
		} else {
			base = pc_fmod_core(x, 6.2831853071795862319959);
		}
	}
	fn = base * PC_INV_PI2;
	n = (long)(fn + (fn >= 0.0 ? 0.5 : -0.5));
	h1b = f64_bits(PC_PI_H) & ~((1ull << 26) - 1ull);
	H1 = bits_f64(h1b);
	H2 = PC_PI_H - H1; /* exact by Sterbenz */
	t = (base - (double)n * H1) - (double)n * H2;
	t -= (double)n * PC_PI_L;
	if (t > PC_PI_O2_D / 2.0) {
		t = (t - PC_PI_H) - PC_PI_L;
		n++;
	} else if (t < -PC_PI_O2_D / 2.0) {
		t = (t + PC_PI_H) + PC_PI_L;
		n--;
	}
	*q = n;
	*r = t;
}

static void pc_sincos_kernel(double x, double *s, double *c)
{
	long n;
	double r, r2;
	if (pc_isnan_d(x) || pc_isinf_d(x)) {
		*s = bits_f64(0x7ff8000000000000ull);
		*c = bits_f64(0x7ff8000000000000ull);
		return;
	}
	if (x == 0.0) {
		*s = x;
		*c = 1.0;
		return;
	}
	pc_trig_reduce(x, &n, &r);
	r2 = r * r;
	switch ((unsigned long)(n & 3L)) {
	case 0:
		*s = pc_k_sin(r, r2);
		*c = pc_k_cos(r2);
		break;
	case 1:
		*s = pc_k_cos(r2);
		*c = -pc_k_sin(r, r2);
		break;
	case 2:
		*s = -pc_k_sin(r, r2);
		*c = -pc_k_cos(r2);
		break;
	default:
		*s = -pc_k_cos(r2);
		*c = pc_k_sin(r, r2);
		break;
	}
}
/* ------------------------------------------------------------------ */
/* Double atan/atan2 (ported from the tree's s_atan.c / e_atan2.c,      */
/* Sun fdlibm licence family). PC changes: bit macros via memcpy,      */
/* fabs via bit mask. The aT[]/atanhi[]/atanlo[] tables are copied      */
/* verbatim from s_atan.c.                                              */
/* ------------------------------------------------------------------ */

static double pc_atan_kernel(double x)
{
	static const double atanhi[] = {
		4.63647609000806093515e-01, 7.85398163397448278999e-01,
		9.82793723247329054082e-01, 1.57079632679489655800e+00,
	};
	static const double atanlo[] = {
		2.26987774529616870924e-17, 3.06161699786838301793e-17,
		1.39033110312309984516e-17, 6.12323399573676603587e-17,
	};
	static const double aT[] = {
		3.33333333333329318027e-01, -1.99999999998764832476e-01,
		1.42857142725034663711e-01, -1.11111104054623557880e-01,
		9.09088713343650656196e-02, -7.69187620504482999495e-02,
		6.66107313738753120669e-02, -5.83357013379057348645e-02,
		4.97687799461593236017e-02, -3.65315727442169155270e-02,
		1.62858201153657823623e-02,
	};
	static const double one = 1.0, huge = 1.0e300;
	double w, s1, s2, z;
	int ix, hx, id;
	uint64_t u = f64_bits(x);
	hx = (int)(u >> 32);
	ix = hx & 0x7fffffff;
	if (ix >= 0x44100000) {
		uint32_t lo = (uint32_t)u;
		if (ix > 0x7ff00000 || (ix == 0x7ff00000 && lo != 0))
			return x + x;
		if (hx > 0)
			return atanhi[3] + atanlo[3];
		else
			return -atanhi[3] - atanlo[3];
	}
	if (ix < 0x3fdc0000) {
		if (ix < 0x3e200000) {
			if (huge + x > one)
				return x;
		}
		id = -1;
	} else {
		x = pc_fabs(x);
		if (ix < 0x3ff30000) {
			if (ix < 0x3fe60000) {
				id = 0;
				x = (2.0 * x - one) / (2.0 + x);
			} else {
				id = 1;
				x = (x - one) / (x + one);
			}
		} else {
			if (ix < 0x40038000) {
				id = 2;
				x = (x - 1.5) / (one + 1.5 * x);
			} else {
				id = 3;
				x = -1.0 / x;
			}
		}
	}
	z = x * x;
	w = z * z;
	s1 = z * (aT[0] + w * (aT[2] + w * (aT[4] + w * (aT[6] + w * (aT[8] + w * aT[10])))));
	s2 = w * (aT[1] + w * (aT[3] + w * (aT[5] + w * (aT[7] + w * aT[9]))));
	if (id < 0)
		return x - x * (s1 + s2);
	z = atanhi[id] - ((x * (s1 + s2) - atanlo[id]) - x);
	return (hx < 0) ? -z : z;
}

/* e_atan2.c structure; calls pc_atan_kernel instead of atan. */
static double pc_atan2_kernel(double y, double x)
{
	static const double tiny = 1.0e-300, zero = 0.0, pi_o_4 = 7.8539816339744827900E-01,
	                    pi_o_2 = 1.5707963267948965580E+00, pi = 3.1415926535897931160E+00,
	                    pi_lo = 1.2246467991473531772E-16;
	double z;
	int k, m, hx, hy, ix, iy;
	uint64_t ux = f64_bits(x), uy = f64_bits(y);
	uint32_t lx, ly;
	hx = (int)(ux >> 32);
	ix = hx & 0x7fffffff;
	lx = (uint32_t)ux;
	hy = (int)(uy >> 32);
	iy = hy & 0x7fffffff;
	ly = (uint32_t)uy;
	if (((ix > 0x7ff00000) || ((ix == 0x7ff00000) && (lx != 0)))
	    || ((iy > 0x7ff00000) || ((iy == 0x7ff00000) && (ly != 0))))
		return x + y;
	if (hx == 0x3ff00000 && lx == 0)
		return pc_atan_kernel(y);
	m = ((hy >> 31) & 1) | ((hx >> 30) & 2);
	if (iy == 0 && ly == 0) {
		switch (m) {
		case 0:
		case 1:
			return y;
		case 2:
			return pi + tiny;
		default:
			return -pi - tiny;
		}
	}
	if (ix == 0 && lx == 0)
		return (hy < 0) ? -pi_o_2 - tiny : pi_o_2 + tiny;
	if (ix == 0x7ff00000) {
		if (iy == 0x7ff00000) {
			switch (m) {
			case 0:
				return pi_o_4 + tiny;
			case 1:
				return -pi_o_4 - tiny;
			case 2:
				return 3.0 * pi_o_4 + tiny;
			default:
				return -3.0 * pi_o_4 - tiny;
			}
		} else {
			switch (m) {
			case 0:
				return zero;
			case 1:
				return -zero;
			case 2:
				return pi + tiny;
			default:
				return -pi - tiny;
			}
		}
	}
	if (iy == 0x7ff00000)
		return (hy < 0) ? -pi_o_2 - tiny : pi_o_2 + tiny;
	k = (iy - ix) >> 20;
	if (k > 60)
		z = pi_o_2 + 0.5 * pi_lo;
	else if (hx < 0 && k < -60)
		z = 0.0;
	else
		z = pc_atan_kernel(pc_fabs(y / x));
	switch (m) {
	case 0:
		return z;
	case 1: {
		uint64_t uz = f64_bits(z);
		uz ^= 0x8000000000000000ull;
		return bits_f64(uz);
	}
	case 2:
		return pi - (z - pi_lo);
	default:
		return (z - pi_lo) - pi;
	}
}

static double pc_asin_kernel(double x)
{
	double t;
	if (pc_isnan_d(x))
		return x + x;
	if (x > 1.0 || x < -1.0)
		return bits_f64(0x7ff8000000000000ull);
	if (x == 1.0)
		return PC_PI_O2_D;
	if (x == -1.0)
		return -PC_PI_O2_D;
	if (x == 0.0)
		return x;
	/* t = (1-x)(1+x): exact halves by Sterbenz on the small side, so the
	 * relative error stays ~2^-53 even for |x| -> 1 (plain 1-x*x would
	 * cancel to a relative error of 2^-53/(1-x^2)). */
	t = (1.0 - x) * (1.0 + x);
	if (t < 0.0)
		t = 0.0;
	return pc_atan2_kernel(x, pc_sqrt(t));
}

static double pc_acos_kernel(double x)
{
	double t;
	if (pc_isnan_d(x))
		return x + x;
	if (x > 1.0 || x < -1.0)
		return bits_f64(0x7ff8000000000000ull);
	if (x == 1.0)
		return 0.0;
	if (x == -1.0)
		return PC_PI_D;
	if (x == 0.0)
		return PC_PI_O2_D;
	t = (1.0 - x) * (1.0 + x);
	if (t < 0.0)
		t = 0.0;
	return pc_atan2_kernel(pc_sqrt(t), x);
}
/* ------------------------------------------------------------------ */
/* Double exp/log: range reduction + polynomial kernels whose         */
/* coefficients come from the tree's e_pow.c (Sun fdlibm family).     */
/* ------------------------------------------------------------------ */

static const double PC_P1 = 1.66666666666666019037e-01, PC_P2 = -2.77777777770155933842e-03,
                    PC_P3 = 6.61375632143793436117e-05, PC_P4 = -1.65339022054652515390e-06,
                    PC_P5 = 4.13813679705723846039e-08;
static const double PC_L1 = 5.99999999999994648725e-01, PC_L2 = 4.28571428578550184252e-01,
                    PC_L3 = 3.33333329818377432918e-01, PC_L4 = 2.72728123808534006489e-01,
                    PC_L5 = 2.30660745775561754067e-01, PC_L6 = 2.06975017800338417784e-01;
static const double PC_BP0 = 1.0, PC_BP1 = 1.5;
static const double PC_DP_H0 = 0.0, PC_DP_H1 = 5.84962487220764160156e-01;
static const double PC_DP_L0 = 0.0, PC_DP_L1 = 1.35003920212974897128e-08;
static const double PC_CP = 9.61796693925975554329e-01, PC_CP_H = 9.61796700954437255859e-01,
                    PC_CP_L = -7.02846165095275826516e-09;
static const double PC_IVLN2_H = 1.44269502162933349609e+00,
                    PC_IVLN2_L = 1.92596299112661746887e-08;
static const double PC_LN2 = 6.93147180559945286227e-01;
/* ln2 split for extra-precision remainder (from e_pow.c lg2_h/lg2_l):
 * HI+LO == ln2 to ~2^-70. */
static const double PC_LN2_HI = 6.93147182464599609375e-01,
                    PC_LN2_LO = -1.90465429995776804525e-09;
static const double PC_LOG10_2 = 3.01029995663981195214e-01; /* log10(2) */

/* e^z for |z| <= 0.5 (fdlibm exp kernel shape from e_pow.c: P1..P5).
 * In e_pow.c this kernel evaluates e^(fraction*ln2); here it serves exp
 * (z = remainder after subtracting n*ln2) and exp2 (z = fraction*ln2). */
static double pc_exp_small(double z)
{
	double t = z * z;
	double t1 = z - t * (PC_P1 + t * (PC_P2 + t * (PC_P3 + t * (PC_P4 + t * PC_P5))));
	return 1.0 - ((z * t1) / (t1 - 2.0) - z);
}

static double pc_exp_kernel(double x)
{
	double zhi, zlo, zz, r;
	long n;
	if (pc_isnan_d(x))
		return x + x;
	if (x == bits_f64(0x7ff0000000000000ull))
		return x;
	if (x == bits_f64(0xfff0000000000000ull))
		return 0.0;
	if (x > 709.78271289338400)
		return bits_f64(0x7ff0000000000000ull);
	if (x < -745.13321910194122)
		return 0.0;
	/* n = round(x/ln2) with a split 1/ln2; r = x - n*ln2 in extra
	 * precision (|r| <= ln2/2). exp(x) = 2^n * e^r. */
	zhi = x * PC_IVLN2_H;
	zlo = x * PC_IVLN2_L;
	zz = zhi + zlo;
	n = (long)(zz + (zz >= 0.0 ? 0.5 : -0.5));
	r = (x - (double)n * PC_LN2_HI) - (double)n * PC_LN2_LO;
	return pc_scalbn(pc_exp_small(r), (int)n);
}

/* log2(ax) for finite ax > 0; *lo receives the low part. */
static double pc_log2_split(double ax, double *lo)
{
	double s, s_h, s_l, t_h, t_l, u, v, p_h, p_l, z_h, z_l, t1, t2, t, r, s2;
	int n = 0, k, j;
	int ix;
	uint64_t uax = f64_bits(ax);
	ix = (int)(uax >> 32);
	if (ix < 0x00100000) {
		ax *= 9007199254740992.0;
		n -= 53;
		uax = f64_bits(ax);
		ix = (int)(uax >> 32);
	}
	n += (ix >> 20) - 0x3ff;
	j = ix & 0x000fffff;
	ix = j | 0x3ff00000;
	if (j <= 0x3988e)
		k = 0;
	else if (j < 0xbb67a)
		k = 1;
	else {
		k = 0;
		n += 1;
		ix -= 0x00100000;
	}
	uax = (uax & 0xffffffffull) | ((uint64_t)(unsigned)ix << 32);
	ax = bits_f64(uax);
	u = ax - (k ? PC_BP1 : PC_BP0);
	{
		double bp = k ? PC_BP1 : PC_BP0;
		v = 1.0 / (ax + bp);
	}
	s = u * v;
	s_h = s;
	{
		uint64_t us = f64_bits(s_h);
		us &= 0xffffffff00000000ull;
		s_h = bits_f64(us);
	}
	{
		double bp = k ? PC_BP1 : PC_BP0;
		double t_hh;
		uint64_t ut;
		ut = ((uint64_t)(unsigned)(((ix >> 1) | 0x20000000) + 0x00080000 + (k << 18)) << 32);
		t_hh = bits_f64(ut);
		t_l = ax - (t_hh - bp);
		t_h = t_hh;
	}
	s_l = v * ((u - s_h * t_h) - s_h * t_l);
	s2 = s * s;
	r = s2 * s2 * (PC_L1 + s2 * (PC_L2 + s2 * (PC_L3 + s2 * (PC_L4 + s2 * (PC_L5 + s2 * PC_L6)))));
	r += s_l * (s_h + s);
	s2 = s_h * s_h;
	t_h = 3.0 + s2 + r;
	{
		uint64_t ut = f64_bits(t_h);
		ut &= 0xffffffff00000000ull;
		t_h = bits_f64(ut);
	}
	t_l = r - ((t_h - 3.0) - s2);
	u = s_h * t_h;
	v = s_l * t_h + t_l * s;
	p_h = u + v;
	{
		uint64_t up = f64_bits(p_h);
		up &= 0xffffffff00000000ull;
		p_h = bits_f64(up);
	}
	p_l = v - (p_h - u);
	z_h = PC_CP_H * p_h;
	z_l = PC_CP_L * p_h + p_l * PC_CP + (k ? PC_DP_L1 : PC_DP_L0);
	t = (double)n;
	t1 = (((z_h + z_l) + (k ? PC_DP_H1 : PC_DP_H0)) + t);
	{
		uint64_t ut = f64_bits(t1);
		ut &= 0xffffffff00000000ull;
		t1 = bits_f64(ut);
	}
	t2 = z_l - (((t1 - t) - (k ? PC_DP_H1 : PC_DP_H0)) - z_h);
	*lo = t2;
	return t1;
}

static double pc_log_kernel(double x)
{
	double hi, lo;
	if (pc_isnan_d(x))
		return x + x;
	if (x == bits_f64(0x7ff0000000000000ull))
		return x;
	if (x == 0.0)
		return bits_f64(0xfff0000000000000ull);
	if (x < 0.0)
		return bits_f64(0x7ff8000000000000ull);
	hi = pc_log2_split(x, &lo);
	return hi * PC_LN2 + lo * PC_LN2;
}

static double pc_log2_kernel(double x)
{
	double lo, hi;
	if (pc_isnan_d(x))
		return x + x;
	if (x == bits_f64(0x7ff0000000000000ull))
		return x;
	if (x == 0.0)
		return bits_f64(0xfff0000000000000ull);
	if (x < 0.0)
		return bits_f64(0x7ff8000000000000ull);
	hi = pc_log2_split(x, &lo);
	return hi + lo;
}

static double pc_log10_kernel(double x)
{
	double lo, hi;
	if (pc_isnan_d(x))
		return x + x;
	if (x == bits_f64(0x7ff0000000000000ull))
		return x;
	if (x == 0.0)
		return bits_f64(0xfff0000000000000ull);
	if (x < 0.0)
		return bits_f64(0x7ff8000000000000ull);
	hi = pc_log2_split(x, &lo);
	return (hi + lo) * PC_LOG10_2;
}

static double pc_exp2_kernel(double x)
{
	long n;
	double f, u, y;
	if (pc_isnan_d(x))
		return x + x;
	if (x == bits_f64(0x7ff0000000000000ull))
		return x;
	if (x == bits_f64(0xfff0000000000000ull))
		return 0.0;
	if (x >= 1024.0)
		return bits_f64(0x7ff0000000000000ull);
	if (x < -1075.0)
		return 0.0;
	/* n = round(x), f in [-0.5, 0.5]; 2^x = 2^n * e^(f*ln2). */
	n = (long)(x + (x >= 0.0 ? 0.5 : -0.5));
	f = x - (double)n;
	u = f * PC_LN2;
	y = pc_exp_small(u);
	return pc_scalbn(y, (int)n);
}

static double pc_hypot_kernel(double x, double y)
{
	double ax = pc_fabs(x), ay = pc_fabs(y), m, r;
	if (pc_isinf_d(x) || pc_isinf_d(y))
		return bits_f64(0x7ff0000000000000ull);
	if (pc_isnan_d(x) || pc_isnan_d(y))
		return bits_f64(0x7ff8000000000000ull);
	m = (ax > ay) ? ax : ay;
	if (m == 0.0)
		return 0.0;
	r = ((ax > ay) ? ay : ax) / m;
	return m * pc_sqrt(1.0 + r * r);
}
/* ------------------------------------------------------------------ */
/* Double pow (ported from the tree's e_pow.c, Sun fdlibm licence).    */
/* PC changes: bit macros via __builtin_memcpy, fabs via bit mask,    */
/* exponent adjustment, sqrt via the SSE pc_sqrt helper above,         */
/* __float_nan via 0.0/0.0.                                            */
/* Method (from the original comment): x = 2^n*(1+f); log2(x) = w1+w2  */
/* in two pieces; y*log2(x) = n+y' in simulated multi-precision;       */
/* x**y = 2**n * exp(y'*log2). Nearly rounded, including the rule that */
/* pow(integer,integer) returns the correct integer when representable.*/
/* ------------------------------------------------------------------ */

static double pc_pow_kernel(double x, double y)
{
	static const double bp[] = { 1.0, 1.5 };
	static const double dp_h[] = { 0.0, 5.84962487220764160156e-01 };
	static const double dp_l[] = { 0.0, 1.35003920212974897128e-08 };
	static const double zero = 0.0, one = 1.0, two = 2.0, two53 = 9007199254740992.0,
	                    huge = 1.0e300, tiny = 1.0e-300;
	static const double L1 = 5.99999999999994648725e-01, L2 = 4.28571428578550184252e-01,
	                    L3 = 3.33333329818377432918e-01, L4 = 2.72728123808534006489e-01,
	                    L5 = 2.30660745775561754067e-01, L6 = 2.06975017800338417784e-01;
	static const double P1 = 1.66666666666666019037e-01, P2 = -2.77777777770155933842e-03,
	                    P3 = 6.61375632143793436117e-05, P4 = -1.65339022054652515390e-06,
	                    P5 = 4.13813679705723846039e-08;
	static const double lg2 = 6.93147180559945286227e-01, lg2_h = 6.93147182464599609375e-01,
	                    lg2_l = -1.90465429995776804525e-09;
	static const double ovt = 8.0085662595372944372e-0017, cp = 9.61796693925975554329e-01,
	                    cp_h = 9.61796700954437255859e-01, cp_l = -7.02846165095275826516e-09,
	                    ivln2 = 1.44269504088896338700e+00, ivln2_h = 1.44269502162933349609e+00,
	                    ivln2_l = 1.92596299112661746887e-08;
	static const double qnan = 0.0 / 0.0;
	double z, ax, z_h, z_l, p_h, p_l;
	double y1, t1, t2, r, s, t, u, v, w;
	int i, j, k, yisint, n;
	int hx, hy, ix, iy;
	uint32_t lx, ly;
	uint64_t ux = f64_bits(x), uy = f64_bits(y);

	hx = (int)(ux >> 32);
	lx = (uint32_t)ux;
	hy = (int)(uy >> 32);
	ly = (uint32_t)uy;
	ix = hx & 0x7fffffff;
	iy = hy & 0x7fffffff;

	if ((iy == 0) && (ly == 0))
		return one;

	/* C99 F.9.4.4: pow(1, y) is 1 for any y, even NaN (fdlibm's core
	 * below would return x+y = NaN). */
	if (x == one)
		return one;
	if (ix > 0x7ff00000 || ((ix == 0x7ff00000) && (lx != 0))
	    || iy > 0x7ff00000 || ((iy == 0x7ff00000) && (ly != 0)))
		return x + y;

	yisint = 0;
	if (hx < 0) {
		if (iy >= 0x43400000)
			yisint = 2;
		else if (iy >= 0x3ff00000) {
			k = (iy >> 20) - 0x3ff;
			if (k > 20) {
				uint32_t ju = ly >> (52 - k);
				if ((ju << (52 - k)) == ly)
					yisint = 2 - ((int)ju & 1);
			} else if (ly == 0) {
				uint32_t ju = (uint32_t)iy >> (20 - k);
				if ((ju << (20 - k)) == (uint32_t)iy)
					yisint = 2 - ((int)ju & 1);
			}
		}
	}

	if (ly == 0) {
		if (iy == 0x7ff00000) {
			if (((ix - 0x3ff00000) | (int)lx) == 0)
				return y - y;
			else if (ix >= 0x3ff00000)
				return (hy >= 0) ? y : zero;
			else
				return (hy < 0) ? -y : zero;
		}
		if (iy == 0x3ff00000) {
			if (hy < 0)
				return one / x;
			else
				return x;
		}
		if (hy == 0x40000000)
			return x * x;
		if (hy == 0x3fe00000) {
			if (hx >= 0)
				return pc_sqrt(x);
		}
	}

	ax = pc_fabs(x);
	if (lx == 0) {
		if (ix == 0x7ff00000 || ix == 0 || ix == 0x3ff00000) {
			z = ax;
			if (hy < 0)
				z = one / z;
			if (hx < 0) {
				if (((ix - 0x3ff00000) | yisint) == 0) {
					z = (z - z) / (z - z);
				} else if (yisint == 1)
					z = -z;
			}
			return z;
		}
	}

	if ((((hx >> 31) + 1) | yisint) == 0) {
		errno = EDOM;
		return qnan;
	}

	if (iy > 0x41e00000) {
		if (iy > 0x43f00000) {
			if (ix <= 0x3fefffff)
				return (hy < 0) ? huge * huge : tiny * tiny;
			if (ix >= 0x3ff00000)
				return (hy > 0) ? huge * huge : tiny * tiny;
		}
		if (ix < 0x3fefffff)
			return (hy < 0) ? huge * huge : tiny * tiny;
		if (ix > 0x3ff00000)
			return (hy > 0) ? huge * huge : tiny * tiny;
		t = x - 1;
		w = (t * t) * (0.5 - t * (0.3333333333333333333333 - t * 0.25));
		u = ivln2_h * t;
		v = t * ivln2_l - w * ivln2;
		t1 = u + v;
		{
			uint64_t ut = f64_bits(t1);
			ut &= 0xffffffff00000000ull;
			t1 = bits_f64(ut);
		}
		t2 = v - (t1 - u);
	} else {
		double s2, s_h, s_l, t_h, t_l;
		n = 0;
		if (ix < 0x00100000) {
			ax *= two53;
			n -= 53;
			ix = (int)(f64_bits(ax) >> 32);
		}
		n += ((ix) >> 20) - 0x3ff;
		j = ix & 0x000fffff;
		ix = j | 0x3ff00000;
		if (j <= 0x3988e)
			k = 0;
		else if (j < 0xbb67a)
			k = 1;
		else {
			k = 0;
			n += 1;
			ix -= 0x00100000;
		}
		{
			uint64_t ua = f64_bits(ax);
			ua = (ua & 0xffffffffull) | ((uint64_t)(unsigned)ix << 32);
			ax = bits_f64(ua);
		}
		u = ax - bp[k];
		v = one / (ax + bp[k]);
		s = u * v;
		s_h = s;
		{
			uint64_t us = f64_bits(s_h);
			us &= 0xffffffff00000000ull;
			s_h = bits_f64(us);
		}
		{
			uint64_t ut = f64_bits(zero);
			ut = ((uint64_t)(unsigned)(((ix >> 1) | 0x20000000) + 0x00080000 + (k << 18)) << 32);
			t_h = bits_f64(ut);
		}
		t_l = ax - (t_h - bp[k]);
		s_l = v * ((u - s_h * t_h) - s_h * t_l);
		s2 = s * s;
		r = s2 * s2 * (L1 + s2 * (L2 + s2 * (L3 + s2 * (L4 + s2 * (L5 + s2 * L6)))));
		r += s_l * (s_h + s);
		s2 = s_h * s_h;
		t_h = 3.0 + s2 + r;
		{
			uint64_t ut = f64_bits(t_h);
			ut &= 0xffffffff00000000ull;
			t_h = bits_f64(ut);
		}
		t_l = r - ((t_h - 3.0) - s2);
		u = s_h * t_h;
		v = s_l * t_h + t_l * s;
		p_h = u + v;
		{
			uint64_t up = f64_bits(p_h);
			up &= 0xffffffff00000000ull;
			p_h = bits_f64(up);
		}
		p_l = v - (p_h - u);
		z_h = cp_h * p_h;
		z_l = cp_l * p_h + p_l * cp + dp_l[k];
		t = (double)n;
		t1 = (((z_h + z_l) + dp_h[k]) + t);
		{
			uint64_t ut = f64_bits(t1);
			ut &= 0xffffffff00000000ull;
			t1 = bits_f64(ut);
		}
		t2 = z_l - (((t1 - t) - dp_h[k]) - z_h);
	}

	s = one;
	if ((((hx >> 31) + 1) | (yisint - 1)) == 0)
		s = -one;

	y1 = y;
	{
		uint64_t uy1 = f64_bits(y1);
		uy1 &= 0xffffffff00000000ull;
		y1 = bits_f64(uy1);
	}
	p_l = (y - y1) * t1 + y * t2;
	p_h = y1 * t1;
	z = p_l + p_h;
	{
		uint64_t uz = f64_bits(z);
		i = (int)uz;
		j = (int)(uz >> 32);
	}
	if (j >= 0x40900000) {
		if (((j - 0x40900000) | i) != 0)
			return s * huge * huge;
		else {
			if (p_l + ovt > z - p_h)
				return s * huge * huge;
		}
	} else if ((j & 0x7fffffff) >= 0x4090cc00) {
		if (((j - (int)0xc090cc00) | i) != 0)
			return s * tiny * tiny;
		else {
			if (p_l <= z - p_h)
				return s * tiny * tiny;
		}
	}
	i = j & 0x7fffffff;
	k = (i >> 20) - 0x3ff;
	n = 0;
	if (i > 0x3fe00000) {
		n = (int)((uint32_t)j + (0x00100000u >> (k + 1)));
		k = (int)(((uint32_t)n & 0x7fffffffu) >> 20) - 0x3ff;
		t = zero;
		{
			uint64_t ut = f64_bits(t);
			ut = (ut & 0xffffffffull) | ((uint64_t)(unsigned)(n & ~(0x000fffff >> k)) << 32);
			t = bits_f64(ut);
		}
		n = (int)(((uint32_t)n & 0x000fffffu) | 0x00100000u) >> (20 - k);
		if (j < 0)
			n = -n;
		p_h -= t;
	}
	t = p_l + p_h;
	{
		uint64_t ut = f64_bits(t);
		ut &= 0xffffffff00000000ull;
		t = bits_f64(ut);
	}
	u = t * lg2_h;
	v = (p_l - (t - p_h)) * lg2 + t * lg2_l;
	z = u + v;
	w = v - (z - u);
	t = z * z;
	t1 = z - t * (P1 + t * (P2 + t * (P3 + t * (P4 + t * P5))));
	r = (z * t1) / (t1 - two) - (w + z * w);
	z = one - (r - z);
	j = (int)(f64_bits(z) >> 32);
	/* n << 20 overflows into the sign bit for large |n|; this is the
	 * fdlibm idiom and relies on GCC's documented wrapping of signed
	 * left shift (-fwrapv semantics GCC guarantees for shifts). */
	j += (n << 20);
	if ((j >> 20) <= 0)
		z = pc_scalbn(z, n);
	else {
		uint64_t uz = f64_bits(z);
		uz = (uz & 0xffffffffull) | ((uint64_t)(unsigned)j << 32);
		z = bits_f64(uz);
	}
	return s * z;
}
/* ------------------------------------------------------------------ */
/* Public entry points. Strong C definitions: at static link these    */
/* satisfy the game's undefined references, so the mingw libm archive */
/* members (x87) are never pulled and no msvcrt math import is        */
/* emitted for them.                                                  */
/*                                                                    */
/* Float policy: every float entry point rounds the double kernel     */
/* once. The double kernels measure within ~1 ulp of the platform     */
/* double libm, so the float result matches the mingw float libm to   */
/* at most 1 ulp on game domains (verified by pc_netplay_libm_test).  */
/* The ported MSL float routines above are retained under             */
/* PC_NETPLAY_LIBM_USE_MSL_FLOAT_TRIG for reference; they are not     */
/* routed here because their single-precision reduction cannot meet   */
/* the 1-ulp requirement (see note at the switch).                    */
/* ------------------------------------------------------------------ */

#if PC_NETPLAY_LIBM_USE_MSL_FLOAT_TRIG
float sinf(float x) { return pc_msl_sinf(x); }

float cosf(float x) { return pc_msl_cosf(x); }

void sincosf(float x, float *s, float *c) { pc_msl_sincosf_kernel(x, s, c); }

float tanf(float x)
{
	float s, c;
	if (pc_isnan_f(x))
		return x + x;
	pc_msl_sincosf_kernel(x, &s, &c);
	return s / c;
}

float atanf(float x) { return pc_msl_atanf(x); }

float atan2f(float y, float x) { return pc_msl_atan2f(y, x); }

float asinf(float x) { return pc_msl_asinf(x); }

float acosf(float x) { return pc_msl_acosf(x); }
#else
float sinf(float x)
{
	double s, c;
	if (pc_isnan_f(x) || pc_isinf_f(x))
		return bits_f32(0x7fc00000u);
	pc_sincos_kernel((double)x, &s, &c);
	return (float)s;
}

float cosf(float x)
{
	double s, c;
	if (pc_isnan_f(x) || pc_isinf_f(x))
		return bits_f32(0x7fc00000u);
	pc_sincos_kernel((double)x, &s, &c);
	return (float)c;
}

void sincosf(float x, float *s, float *c)
{
	double sd, cd;
	if (pc_isnan_f(x) || pc_isinf_f(x)) {
		*s = bits_f32(0x7fc00000u);
		*c = bits_f32(0x7fc00000u);
		return;
	}
	pc_sincos_kernel((double)x, &sd, &cd);
	*s = (float)sd;
	*c = (float)cd;
}

float tanf(float x)
{
	double s, c;
	if (pc_isnan_f(x) || pc_isinf_f(x))
		return bits_f32(0x7fc00000u);
	pc_sincos_kernel((double)x, &s, &c);
	return (float)(s / c);
}

float atanf(float x) { return (float)pc_atan_kernel((double)x); }

float atan2f(float y, float x) { return (float)pc_atan2_kernel((double)y, (double)x); }

float asinf(float x) { return (float)pc_asin_kernel((double)x); }

float acosf(float x) { return (float)pc_acos_kernel((double)x); }
#endif

float expf(float x) { return (float)pc_exp_kernel((double)x); }

float logf(float x) { return (float)pc_log_kernel((double)x); }

float log10f(float x) { return (float)pc_log10_kernel((double)x); }

float log2f(float x) { return (float)pc_log2_kernel((double)x); }

float exp2f(float x) { return (float)pc_exp2_kernel((double)x); }

float powf(float x, float y) { return (float)pc_pow_kernel((double)x, (double)y); }

float fmodf(float x, float y) { return (float)pc_fmod_core((double)x, (double)y); }

float hypotf(float x, float y) { return (float)pc_hypot_kernel((double)x, (double)y); }

/* Exact IEEE square roots, SSE only (the mingw-w64
 * sqrt/sqrtf use x87 fldl/fsqrt/fstpl, whose double rounding depends on
 * the x87 precision-control field. These strong definitions keep the
 * linker from ever pulling those archive members. sqrtsd/sqrtss are
 * correctly rounded by hardware, so no accuracy clause applies; the
 * NaN/Inf/zero/sign cases below just spell out the IEEE behavior the
 * instructions already implement). */
double sqrt(double x) { return pc_sqrt(x); }

float sqrtf(float x) { return pc_sqrtf(x); }

double sin(double x)
{
	double s, c;
	pc_sincos_kernel(x, &s, &c);
	return s;
}

double cos(double x)
{
	double s, c;
	pc_sincos_kernel(x, &s, &c);
	return c;
}

void sincos(double x, double *s, double *c) { pc_sincos_kernel(x, s, c); }

double tan(double x)
{
	double s, c;
	if (pc_isnan_d(x))
		return x + x;
	pc_sincos_kernel(x, &s, &c);
	return s / c;
}

double asin(double x) { return pc_asin_kernel(x); }

double acos(double x) { return pc_acos_kernel(x); }

double atan(double x) { return pc_atan_kernel(x); }

double atan2(double y, double x) { return pc_atan2_kernel(y, x); }

double exp(double x) { return pc_exp_kernel(x); }

double log(double x) { return pc_log_kernel(x); }

double log10(double x) { return pc_log10_kernel(x); }

double log2(double x) { return pc_log2_kernel(x); }

double exp2(double x) { return pc_exp2_kernel(x); }

double pow(double x, double y) { return pc_pow_kernel(x, y); }

double fmod(double x, double y) { return pc_fmod_core(x, y); }

double hypot(double x, double y) { return pc_hypot_kernel(x, y); }

/* Test hooks (see pc_netplay_libm.h). */

int pc_netplay_libm_present(void) { return 1; }

/* ABI version of this deterministic libm. Bumped whenever any routine's
 * bit-exact behavior changes; the libm test logs it next to the golden
 * checksum so a checksum mismatch can be told apart from a stale test. */
unsigned long long pc_netplay_libm_abi_version(void) { return 0x6D32642D66700003ull; }
