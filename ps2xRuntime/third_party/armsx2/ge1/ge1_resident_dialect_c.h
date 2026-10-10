// RZV1 S4b: the C dialect for ge1_rz_core.inc / ge1_resident_gen.inc (the MSL
// one is in ge1_resident_metal.mm). Include inside a namespace, then undef.
typedef uint32_t uint;
static inline uint rzc_f2u(float f) { uint u; std::memcpy(&u, &f, 4); return u; }
static inline float rzc_u2f(uint u) { float f; std::memcpy(&f, &u, 4); return f; }
#define F2U rzc_f2u
#define U2F rzc_u2f
#define F2I(x) (static_cast<int>(x))
#define U2FLT(x) (static_cast<float>(x))
#define U32C(x) (static_cast<uint>(x))
#define I32C(x) (static_cast<int>(x))
#define PREC
#define MSB(x) (31 - __builtin_clz(x))
#define UMUL(a, b, hi, lo) { const uint64_t _p = static_cast<uint64_t>(a) * static_cast<uint64_t>(b); hi = static_cast<uint>(_p >> 32); lo = static_cast<uint>(_p); }
#define fma fmaf
#define VARIANT 0
#define DEVC const
#define THR
