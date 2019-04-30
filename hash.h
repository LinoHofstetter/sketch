#ifndef DBSKETCH_HASH_H__
#define DBSKETCH_HASH_H__
#include <cstdint>
#include <array>
#include <type_traits>
#include <vector>
#include <climits>
#include <memory>
#include "vec/vec.h"

namespace sketch {
namespace hash {
using std::uint64_t;
using std::uint32_t;
using std::size_t;

using Type  = typename vec::SIMDTypes<uint64_t>::Type;
using VType = typename vec::SIMDTypes<uint64_t>::VType;
using Space = vec::SIMDTypes<uint64_t>;
// Thomas Wang hash
// Original site down, available at https://naml.us/blog/tag/thomas-wang
// This is our core 64-bit hash.
// It a bijection within [0,1<<64)
// and can be inverted with irving_inv_hash.
struct WangHash {
    INLINE auto operator()(uint64_t key) const {
          key = (~key) + (key << 21); // key = (key << 21) - key - 1;
          key = key ^ (key >> 24);
          key = (key + (key << 3)) + (key << 8); // key * 265
          key = key ^ (key >> 14);
          key = (key + (key << 2)) + (key << 4); // key * 21
          key = key ^ (key >> 28);
          key = key + (key << 31);
          return key;
    }
    INLINE auto operator()(int64_t key) const {return operator()(uint64_t(key));}
    INLINE uint32_t operator()(uint32_t key) const {
        key += ~(key << 15);
        key ^=  (key >> 10);
        key +=  (key << 3);
        key ^=  (key >> 6);
        key += ~(key << 11);
        key ^=  (key >> 16);
        return key;
    }
    INLINE auto operator()(int32_t key) const {return operator()(uint32_t(key));}
    INLINE Type operator()(Type element) const {
        VType key = Space::add(Space::slli(element, 21), ~element); // key = (~key) + (key << 21);
        key = Space::srli(key.simd_, 24) ^ key.simd_; //key ^ (key >> 24)
        key = Space::add(Space::add(Space::slli(key.simd_, 3), Space::slli(key.simd_, 8)), key.simd_); // (key + (key << 3)) + (key << 8);
        key = key.simd_ ^ Space::srli(key.simd_, 14);  // key ^ (key >> 14);
        key = Space::add(Space::add(Space::slli(key.simd_, 2), Space::slli(key.simd_, 4)), key.simd_); // (key + (key << 2)) + (key << 4); // key * 21
        key = key.simd_ ^ Space::srli(key.simd_, 28); // key ^ (key >> 28);
        key = Space::add(Space::slli(key.simd_, 31), key.simd_);    // key + (key << 31);
        return key.simd_;
    }
#if VECTOR_WIDTH > 16
    INLINE auto operator()(__m128i key) const {
        key = _mm_add_epi64(~key, _mm_slli_epi64(key, 21)); // key = (key << 21) - key - 1;
        key ^= _mm_srli_epi64(key, 24);
        key = _mm_add_epi64(key, _mm_add_epi64(_mm_slli_epi64(key, 3), _mm_slli_epi64(key, 8)));
        key ^= _mm_srli_epi64(key, 14);
        key = _mm_add_epi64(_mm_add_epi64(key, _mm_slli_epi64(key, 2)), _mm_slli_epi64(key, 4));
        key ^= _mm_srli_epi64(key, 28);
        key = _mm_add_epi64(_mm_slli_epi64(key, 31), key);
        return key;
    }
#endif
};

// pcg32
// The purpose of this is fast random number generation for {Super,Bag}MinHash
struct pcg32_random_t {   // Internals are *Private*.
    uint64_t state;                    // RNG state.  All values are possible.
    uint64_t inc;                      // Controls which RNG sequence (stream) is
                                       // selected. Must *always* be odd.
};

static INLINE uint32_t pcg32_random_r(pcg32_random_t *rng) {
    uint64_t oldstate = rng->state;
    rng->state = oldstate * 6364136223846793005ULL + rng->inc;
    uint32_t xorshifted = ((oldstate >> 18u) ^ oldstate) >> 27u;
    uint32_t rot = oldstate >> 59u;
    return (xorshifted >> rot) | (xorshifted << ((-rot) & 31));
}

struct PCGen: pcg32_random_t {
    PCGen(uint64_t seed, uint64_t inc=3930499866110305181uLL){
        this->state = seed; this->inc = inc|1; // Ensures that increment is odd
    }
    PCGen &seed(uint64_t newseed) {this->state = newseed; return *this;}
    uint32_t operator()() {return pcg32_random_r(this);}
    uint64_t make_u64() {return (uint64_t(this->operator()()) << 32) | this->operator()();}
    static constexpr uint32_t max() {return std::numeric_limits<uint32_t>::max();}
    static constexpr uint32_t min() {return std::numeric_limits<uint32_t>::min();}
};
using int96_t = std::array<uint32_t, 3>;

#define NO_USE_SIAM 1
template<size_t N>
static auto make_coefficients(uint64_t seedseed) {
    std::mt19937_64 mt(seedseed);
#ifndef NO_USE_SIAM
    std::array<int96_t, N> ret;
    for(auto &e: ret) {
        e[0] = mt() % ((1u << 25) - 1);
        e[1] = mt();
        e[2] = mt() % ((1u << 25) - 1);
	}
#else
	std::array<uint64_t, N> ret;
    for(auto &e: ret) e = mt() % ((1ull << 61) - 1);
#endif
    return ret;
}
namespace siam {

// From Tabulation Based 5-Universal Hashing and Linear Probing
//
static constexpr uint64_t Prime89_0  = (((uint64_t)1)<<32)-1;
static constexpr uint64_t Prime89_1  = (((uint64_t)1)<<32)-1;
static constexpr uint64_t Prime89_2  = (((uint64_t)1)<<25)-1;
static constexpr uint64_t Prime89_21 = (((uint64_t)1)<<57)-1;

inline uint64_t Mod64Prime89(const int96_t r) {
    uint64_t r0, r1, r2; //r2r1r0 = r&Prime89 + r>>89
    r2 = r[2];
    r1 = r[1];
    r0 = r[0] + (r2>>25);
    r2 &= Prime89_2;
    return (r2 == Prime89_2 && r1 == Prime89_1 && r0 >= Prime89_0) ?(r0 - Prime89_0) : (r0 + (r1<<32));
}/*Computes a 96-bit r such thatr mod Prime89 == (ax+b) mod Prime89exploiting the structure of Prime89.*/

static constexpr uint64_t HIGH(uint64_t x) {return x >> 32;}
static constexpr uint64_t LOW(uint64_t x) {return x & 0x00000000FFFFFFFFull;}

inline void MultAddPrime89(int96_t & r, uint64_t x, const int96_t &a, const int96_t &b)
{
    uint64_t x1, x0, c21, c20, c11, c10, c01, c00;
    uint64_t d0, d1, d2, d3;
	uint64_t s0, s1, carry;
    x1 = HIGH(x);
    x0 = LOW(x);
    c21 = a[2]*x1;
    c20 = a[2]*x0;
    c11 = a[1]*x1;
    c10 = a[1]*x0;
    c01 = a[0]*x1;
    c00 = a[0]*x0;
    d0 = (c20>>25)+(c11>>25)+(c10>>57)+(c01>>57);
    d1 = (c21<<7);
    d2 = (c10&Prime89_21) + (c01&Prime89_21);
    d3 = (c20&Prime89_2) +(c11&Prime89_2) + (c21>>57);
    s0 = b[0] + LOW(c00) + LOW(d0) + LOW(d1);
    r[0] = LOW(s0);
    carry = HIGH(s0);
    s1 = b[1] + HIGH(c00) + HIGH(d0) +HIGH(d1) + LOW(d2) + carry;
    r[1] = LOW(s1);
    carry = HIGH(s1);
    r[2] = b[2] + HIGH(d2) + d3 + carry;
}
// CWtrick64 for 64-bit key x (Prime = 2ˆ89-1)
template<size_t k>
inline uint64_t CWtrick64(uint64_t x, const std::array<int96_t, k> &keys) {
	static_assert(k > 2, "If you only need 2, don't use this function.");
    int96_t r;
    MultAddPrime89(r,x,keys[0],keys[1]);
	for(size_t i = 2; i < k; ++i)
    	MultAddPrime89(r,x,r,keys[i]);
    //MultAddPrime89(r,x,r,E); We don't need this
    return Mod64Prime89(r);
}

}

namespace nosiam {
INLINE __uint128_t mod127(__uint128_t x) {
    static constexpr __uint128_t mod = (__uint128_t(1) << 127) - 1;
    x = (x >> 127) + (x & mod);
    if(__builtin_expect(x == __uint128_t(-1), 0)) {
        return mod + 3;
        // = (x >> 127) + (x & mod);
    }
    if(x > mod) x -= mod;
    return x;
}


INLINE __uint128_t mod61(__uint128_t x) {
    static constexpr uint64_t mod = (__uint128_t(1) << 61) - 1;
    do x = (x >> 61) + (x & mod); while(x > mod * 2);
    if(x >= mod) x -= mod;
    return x;
}
INLINE uint64_t mod61(uint64_t x) {
    static constexpr uint64_t mod = (uint64_t(1) << 61) - 1;
    do x = (x >> 61) + (x & mod); while(x > mod * 2);
    if(x >= mod) x -= mod;
    return x;
}
INLINE uint64_t mulmod61(uint64_t x1, uint64_t x2) {
    __uint128_t tmp = x1; tmp *= x2;
    return mod61(tmp);
}

template<size_t n>
inline uint64_t i61hash(uint64_t x, const std::array<uint64_t, n> & keys) {
    uint64_t tsum = mulmod61(x, keys[1]);
    tsum += keys[0];
    uint64_t xp = x;
    for(size_t i = 2; i < n; ++i) {
        if(i % 8 == 0) tsum = mod61(tsum);
        xp = mulmod61(xp, x);
        tsum += mulmod61(xp, keys[i]);
    }
	return mod61(tsum);
}
template<size_t k>
inline uint64_t i128hash(uint64_t x, const std::array<uint64_t, k> & keys) {
    // Use 2**127
    __uint128_t sum = mod127(mod127(x * keys[1]) + keys[0]);
    __uint128_t xp = x * x;
    for(size_t i = 2; i < k; ++i) {
        xp = mod127(xp);
        sum = mod127(sum + mod127(xp * keys[i]));
        xp *= x;
    }
    return sum;
}

} // nosiam

template<size_t k>
class KWiseIndependentPolynomialHash {
    static_assert(k, "k must be positive");
#ifndef NO_USE_SIAM
    const std::array<int96_t, k> coeffs_;
#else
    const std::array<uint64_t, k> coeffs_;
#endif
	static constexpr uint64_t mod = (uint64_t(1) << 61) - 1;
    static constexpr bool is_kwise_independent(size_t val) {return val <= k;}
public:
    KWiseIndependentPolynomialHash(uint64_t seedseed=137): coeffs_(make_coefficients<k>(seedseed)) {
    }
    uint64_t operator()(uint64_t val) const {
#ifndef NO_USE_SIAM
		return siam::CWtrick64<k>(val, coeffs_);
#else
		return nosiam::i61hash<k>(val, coeffs_);
#endif
    }
    Type operator()(VType val) const {
        throw std::runtime_error("Should not be called... yet. TODO: this");
        // Data parallel across same coefficients.
        VType ret = Space::set1(coeffs_[0][0]);
        VType exp = val;
        for(size_t i = 1; i < k; ++i) {
#if HAS_AVX_512
            auto tmp = Space::mullo(exp.simd_, Space::set1(coeffs_[i]));
            tmp.for_each([](auto &x) {x %= mod;});
            ret = Space::add(ret, tmp.simd_);
            ret.for_each([](auto &x) {x %= mod;});
            exp = Space::mullo(exp.simd_, val.simd_);
            exp.for_each([](auto &x) {x %= mod;});
#else
            for(uint32_t j = 0; j < Space::COUNT; ++j) {
                ret.arr_[j] = (ret.arr_[j] + (exp.arr_[j] * coeffs_[i] % mod)) % mod;
                exp.arr_[j] = (exp.arr_[j] * val.arr_[j]) % mod;
            }
#endif
        }
        return ret.simd_;
    }
#ifdef DUMMY_INVERSE
    uint64_t inverse(uint64_t val) { return val;} // This is a lie for compatibility only
#endif
};
template<size_t k>
struct KWiseHasherSet {
    std::vector<KWiseIndependentPolynomialHash<k>> hashers_;
    KWiseHasherSet(size_t nh, uint64_t seedseed=137) {
        std::mt19937_64 mt(seedseed);
        while(hashers_.size() < nh)
            hashers_.emplace_back(mt());
    }
    uint64_t operator()(uint64_t v, unsigned ind) const {
        return hashers_[ind](v);
    }
    uint64_t operator()(uint64_t v) const {throw std::runtime_error("Should not be called.");}
};


struct MurFinHash {
    static constexpr uint64_t C1 = 0xff51afd7ed558ccduLL, C2 = 0xc4ceb9fe1a85ec53uLL;
    INLINE uint64_t operator()(uint64_t key) const {
        key ^= key >> 33;
        key *= C1;
        key ^= key >> 33;
        key *= C2;
        key ^= key >> 33;
        return key;
    }
#ifdef DUMMY_INVERSE
    INLINE uint64_t inverse(uint64_t key) const {return key;}
#endif
    INLINE Type operator()(Type key) const {
        return this->operator()(*(reinterpret_cast<VType *>(&key)));
    }
#if 0
&& VECTOR_WIDTH > 16
    INLINE auto operator()(__m128i key) const {
        using namespace vec;
        key ^= _mm_srli_epi64(key, 33);
        key = _mm_mul_epi64x(key, C1);
        key ^= _mm_srli_epi64(key,  33);
        key = _mm_mul_epi64x(key, C2);
        key ^= _mm_srli_epi64(key,  33);
        return key;
    }
#endif
    INLINE Type operator()(VType key) const {
#if 1
        key = Space::srli(key.simd_, 33) ^ key.simd_;  // h ^= h >> 33;
        key.for_each([](auto &x) {x *= C1;});
        key = Space::srli(key.simd_, 33) ^ key.simd_;  // h ^= h >> 33;
        key.for_each([](auto &x) {x *= C2;});
        key = Space::srli(key.simd_, 33) ^ key.simd_;  // h ^= h >> 33;
#else
        __m128i *p = (__m128i *)&key;
        for(unsigned i = 0; i < sizeof(key) / sizeof(*p); ++i)
            *p = this->operator()(*p);
#endif
        return key.simd_;
    }
};
static INLINE uint64_t finalize(uint64_t key) {
    return MurFinHash()(key);
}

namespace op {

template<typename T>
struct multiplies {
    T operator()(T x, T y) const { return x * y;}
    VType operator()(VType x, VType y) const {
#if HAS_AVX_512
        return Space::mullo(x.simd_, y.simd_);
#else
        uint64_t *p1 = reinterpret_cast<uint64_t *>(&x), *p2 = reinterpret_cast<uint64_t *>(&y);
        for(uint32_t i = 0; i < sizeof(VType) / sizeof(uint64_t); ++i) {
            p1[i] *= p2[i];
        }
        return x;
#endif
    }
};
template<typename T>
struct plus {
    T operator()(T x, T y) const { return x + y;}
    VType operator()(VType x, VType y) const { return Space::add(x.simd_, y.simd_);}
};
template<typename T>
struct bit_xor {
    T operator()(T x, T y) const { return x ^ y;}
    VType operator()(VType x, VType y) const { return Space::xor_fn(x.simd_, y.simd_);}
};
}
namespace multinv {
// From Daniel Lemire's Blog: https://github.com/lemire/Code-used-on-Daniel-Lemire-s-blog/blob/master/2017/09/18/inverse.c
// Blog post: https://lemire.me/blog/2017/09/18/computing-the-inverse-of-odd-integers/
// 25 cycles (longest it would take) + 1 cycle for a multiply is at least 3x as fast as performing an IDIV
// but it only works if the integer is odd.

static inline constexpr uint32_t f32(uint32_t x, uint32_t y) { return y * (2 - y * x); }
static constexpr uint32_t findInverse32(uint32_t x) {
  uint32_t y = (3 * x) ^ 2;
  y = f32(x, y);
  y = f32(x, y);
  y = f32(x, y);
  return y;
}
static inline constexpr uint64_t f64(uint64_t x, uint64_t y) { return y * (2 - y * x); }
static inline uint64_t findMultInverse64(uint64_t x) {
  assert(x & 1 || !std::fprintf(stderr, "Can't get multiplicative inverse of an even number."));
  uint64_t y = (3 * x) ^ 2;
  y = f64(x, y);
  y = f64(x, y);
  y = f64(x, y);
  y = f64(x, y);
  return y;
}
template<typename T> inline T findmultinv(T v) {return findmultinv<typename std::make_unsigned<T>::type>(v);}
template<> inline uint64_t findmultinv<uint64_t>(uint64_t v) {return findMultInverse64(v);}
template<> inline uint32_t findmultinv<uint32_t>(uint32_t v) {return findInverse32(v);}


template<typename T>
struct Inverse64 {
    uint64_t operator()(uint64_t x) const {return x;}
    template<typename T2> T2 apply(T2 x) const {return this->operator()(x);}
};

template<>
struct Inverse64<op::multiplies<uint64_t>> {
    uint64_t operator()(uint64_t x) const {return findMultInverse64(x);}
    uint64_t apply(uint64_t x) const {return this->operator()(x);}
};

template<>
struct Inverse64<op::plus<uint64_t>> {
    uint64_t operator()(uint64_t x) const {return std::numeric_limits<uint64_t>::max() - x + 1;}
    uint64_t apply(uint64_t x) const {return this->operator()(x);}
};

} // namespace multinv

template<size_t n>
struct RShiftXor;
template<size_t n>
struct InvRShiftXor;
template<size_t n>
struct InvRShiftXor {
    static constexpr size_t enditer = 64 / n;
    uint64_t constexpr operator()(uint64_t v) const {
        uint64_t ret = v ^ v >> n;
        for(size_t i = 1; i < enditer; ++i)
            ret = v ^ ret >> n;
        return ret;
    }
    uint64_t inverse(uint64_t x) {return InverseOperation()(x);}
    using InverseOperation = RShiftXor<n>;
};

template<size_t n>
struct RShiftXor {
    uint64_t constexpr operator()(uint64_t v) const {return v ^ (v >> n);}
    uint64_t inverse(uint64_t x) {return InverseOperation()(x);}
    using InverseOperation = InvRShiftXor<n>;
};
template<size_t n> using ShiftXor = RShiftXor<n>;
template<size_t n> using ShiftXor = RShiftXor<n>;
template<size_t n> using InvShiftXor = InvRShiftXor<n>;

template<size_t n>
struct LShiftXor;
template<size_t n>
struct InvLShiftXor;
template<size_t n>
struct LShiftXor {
    uint64_t constexpr operator()(uint64_t v) const {return v ^ (v << n);}
    uint64_t constexpr inverse(uint64_t v) const {return InverseOperation()(v);}
    using InverseOperation = InvLShiftXor<n>;
};
template<size_t n>
struct InvLShiftXor {
    static constexpr size_t enditer = 64 / n;
    uint64_t constexpr operator()(uint64_t v) const {
        uint64_t ret = v ^ v << n;
        for(size_t i = 1; i < enditer; ++i)
            ret = v ^ ret << n;
        return ret;
    }
    static constexpr uint64_t inverse(uint64_t v) {InverseOperation()(v);}
    using InverseOperation = LShiftXor<n>;
};


template<size_t n, bool left>
struct Rot {
    template<typename...Args>
    Rot(Args &&...args) {}
    template<typename T>
    INLINE T constexpr operator()(T val) const {
        static_assert(n < sizeof(T) * CHAR_BIT, "Can't shift more than the width of the type.");
        return left ? (val << n) ^ (val >> (64 - n))
                    : (val >> n) ^ (val << (64 - n));
    }
    template<typename T>
    INLINE T constexpr inverse(T val) const {
        return InverseOperation()(val);
    }
    template<typename T, typename T2>
    INLINE T constexpr operator()(T val, const T2 &oval) const { // DO nothing
        return this->operator()(val);
    }
    using InverseOperation = Rot<n, !left>;
};
template<size_t n> using RotL = Rot<n, true>;
template<size_t n> using RotR = Rot<n, false>;
struct BitFlip {
    template<typename...Args>
    BitFlip(Args &&...args) {}
    template<typename T>
    INLINE T constexpr operator()(T val) const {
        return ~val;
    }
    template<typename T>
    INLINE T constexpr inverse(T val) const {
        return InverseOperation()(val);
    }
    template<typename T, typename T2>
    INLINE T constexpr operator()(T val, const T2 &oval) const { // DO nothing
        return this->operator()(val);
    }
    using InverseOperation = BitFlip;
};

using RotL33 = RotL<33>;
using RotR33 = RotR<33>;
using RotL31 = RotL<31>;
using RotR31 = RotR<31>;

template<typename Operation, typename InverseOperation=Operation>
struct InvH {
    uint64_t seed_, inverse_; // Note: inverse_ is only used for op::multiplies
    const Operation op;
    const InverseOperation iop;

    InvH(uint64_t seed):
            seed_(seed | std::is_same<Operation, op::multiplies<uint64_t>>::value),
            inverse_(multinv::Inverse64<Operation>()(seed_)), op(), iop() {}
    // To ensure that it is actually reversible.
    INLINE uint64_t inverse(uint64_t hv) const {
        hv = iop(hv, inverse_);
        return hv;
    }
    INLINE VType inverse(VType hv) const {
        hv = iop(hv.simd_, Space::set1(inverse_));
        return hv;
    }
    INLINE uint64_t operator()(uint64_t h) const {
        h = op(h, seed_);
        return h;
    }
    INLINE VType operator()(VType h) const {
        const VType s = Space::set1(seed_);
        h = op(h, s);
        return h;
    }
};

// Reversible, runtime-configurable hashes


template<typename InvH1, typename InvH2>
struct FusedReversible {
    InvH1 op1;
    InvH2 op2;
    FusedReversible(uint64_t seed1, uint64_t seed2=0xe37e28c4271b5a1duLL):
        op1(seed1), op2(seed2) {}
    template<typename T>
    INLINE T operator()(T h) const {
        h = op1(h);
        h = op2(h);
        return h;
    }
    template<typename T>
    INLINE T inverse(T hv) const {
        hv = op1.inverse(op2.inverse(hv));
        return hv;
    }
};
template<typename InvH1, typename InvH2, typename InvH3>
struct FusedReversible3 {
    InvH1 op1;
    InvH2 op2;
    InvH3 op3;
    FusedReversible3(uint64_t seed1, uint64_t seed2=0xe37e28c4271b5a1duLL):
        op1(seed1), op2(seed2), op3((seed1 * seed2 + seed2) | 1) {}
    template<typename T>
    INLINE T operator()(T h) const {
        return op3(op2(op1(h)));
    }
    template<typename T>
    INLINE T inverse(T hv) const {
        hv = op1.inverse(op2.inverse(op3.inverse(hv)));
        return hv;
    }
};

using InvXor = InvH<op::bit_xor<uint64_t>>;
using InvMul = InvH<op::multiplies<uint64_t>>;
using InvAdd = InvH<op::plus<uint64_t>>;
//using InvAdd = InvH<op::plus<uint64_t>, op::minus<uint64_t>>;
template<size_t n>
using RotN = InvH<RotL<n>, RotR<n>>;
struct XorMultiply: public FusedReversible<InvXor, InvMul > {
    XorMultiply(uint64_t seed1, uint64_t seed2=0xe37e28c4271b5a1duLL): FusedReversible<InvXor, InvMul >(seed1, seed2) {}
};
struct MultiplyAdd: public FusedReversible<InvMul, InvAdd> {
    MultiplyAdd(uint64_t seed1, uint64_t seed2=0xe37e28c4271b5a1duLL): FusedReversible<InvMul, InvAdd>(seed1, seed2) {}
};
struct MultiplyAddXor: public FusedReversible3<InvMul,InvAdd,InvXor> {
    MultiplyAddXor(uint64_t seed1, uint64_t seed2=0xe37e28c4271b5a1duLL): FusedReversible3<InvMul,InvAdd,InvXor>(seed1, seed2) {}
};
template<size_t shift>
struct MultiplyAddXoRot: public FusedReversible3<InvMul,InvXor,RotN<shift>> {
    MultiplyAddXoRot(uint64_t seed1=1337, uint64_t seed2=0xe37e28c4271b5a1duLL): FusedReversible3<InvMul,InvXor, RotN<shift>>(seed1, seed2) {}
};

template<typename HashType>
struct RecursiveReversibleHash {
    std::vector<HashType> v_;
    template<typename... Args>
    RecursiveReversibleHash(size_t n, uint64_t seed1=1337, Args &&... args) {
        std::mt19937_64 mt(seed1);
        while(v_.size() < n)
            v_.emplace_back(mt() | 1, mt(), std::forward<Args>(args)...);
    }
    template<typename T>
    T operator()(T v) const {
        std::for_each(v_.begin(), v_.end(), [&](const auto &hash) {v = hash(v);});
        return v;
    }
    template<typename T>
    T inverse(T hv) const {
        std::for_each(v_.rbegin(), v_.rend(), [&](const auto &hash) {hv = hash.inverse(hv);});
        return hv;
    }
};

struct XorMultiplyNVec: public RecursiveReversibleHash<XorMultiply> {
    XorMultiplyNVec(size_t n, uint64_t seed1=0xB0BAF377D00Dc001uLL):
        RecursiveReversibleHash<XorMultiply>(n, seed1) {}
};
struct MultiplyAddNVec: public RecursiveReversibleHash<MultiplyAdd> {
    MultiplyAddNVec(size_t n, uint64_t seed1=0xB0BAF377D00Dc001uLL):
        RecursiveReversibleHash<MultiplyAdd>(n, seed1) {}
};
struct MultiplyAddXorNVec: public RecursiveReversibleHash<MultiplyAddXor> {
    MultiplyAddXorNVec(size_t n, uint64_t seed1=0xB0BAF377D00Dc001uLL):
        RecursiveReversibleHash<MultiplyAddXor>(n, seed1) {}
};
template<size_t shift>
struct MultiplyAddXoRotNVec: public RecursiveReversibleHash<MultiplyAddXoRot<shift>> {
    MultiplyAddXoRotNVec(size_t n, uint64_t seed1=0xB0BAF377D00Dc001uLL):
        RecursiveReversibleHash<MultiplyAddXoRot<shift>>(n, seed1) {}
};

template<size_t shift, size_t n>
struct MultiplyAddXoRotN: MultiplyAddXoRotNVec<shift> {
    MultiplyAddXoRotN(): MultiplyAddXoRotNVec<shift>(n) {}
};
template<size_t n>
struct MultiplyAddXorN: MultiplyAddXorNVec {
    MultiplyAddXorN(): MultiplyAddXorNVec(n) {}
};
template<size_t n>
struct MultiplyAddN: MultiplyAddNVec{
    MultiplyAddN(): MultiplyAddNVec(n) {}
};
template<size_t n>
struct XorMultiplyN: XorMultiplyNVec{
    XorMultiplyN(): XorMultiplyNVec(n) {}
};

} // namespace hash
} // namespace sketch

#endif /* #ifndef DBSKETCH_HASH_H__ */
