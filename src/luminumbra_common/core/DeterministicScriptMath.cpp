/*
 * Trigonometric kernels and coefficients adapted from fdlibm.
 * Copyright (C) 1993 by Sun Microsystems, Inc. All rights reserved.
 *
 * Developed at SunPro, a Sun Microsystems, Inc. business.
 * Permission to use, copy, modify, and distribute this
 * software is freely granted, provided that this notice
 * is preserved.
 */
#include "DeterministicScriptMath.h"

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>

namespace Luminumbra::DeterministicMath::Script {
namespace {

static_assert(sizeof(double) == sizeof(std::uint64_t));
static_assert(std::numeric_limits<double>::is_iec559 && std::numeric_limits<double>::digits == 53);
constexpr std::uint64_t kSign = UINT64_C(0x8000000000000000);
constexpr std::uint64_t kMagnitude = kSign - 1;
constexpr std::uint64_t kInfinity = UINT64_C(0x7ff0000000000000);
constexpr double kPi = 3.14159265358979311600;
constexpr double kPiLow = 1.22464679914735317723e-16;
constexpr double kHalfPi = 1.57079632679489655800;
constexpr double kHalfPiLow = 6.12323399573676588613e-17;
constexpr double kLn2 = 6.93147180559945309417e-1;
constexpr double kLn2Low = 2.31904681384629955842e-17;

std::uint64_t Bits(double value) noexcept {
    return std::bit_cast<std::uint64_t>(value);
}
double FromBits(std::uint64_t bits) noexcept {
    return std::bit_cast<double>(bits);
}
double Abs(double value) noexcept {
    return FromBits(Bits(value) & kMagnitude);
}
bool IsNan(double value) noexcept {
    return (Bits(value) & kMagnitude) > kInfinity;
}
double Nan() noexcept {
    return FromBits(UINT64_C(0x7ff8000000000000));
}
double WithSign(double value, double sign) noexcept {
    return FromBits((Bits(value) & kMagnitude) | (Bits(sign) & kSign));
}

// Scale a finite value by 2^power. Callers bound power to [-2200, 2200].
// Normal power-of-two factors avoid premature underflow during scaling.
double Scale(double value, int power) noexcept {
    if (power > 1023) {
        value *= 0x1p1023;
        power -= 1023;
        if (power > 1023) {
            value *= 0x1p1023;
            power -= 1023;
        }
    } else if (power < -1022) {
        value *= 0x1p-969;
        power += 969;
        if (power < -1022) {
            value *= 0x1p-969;
            power += 969;
        }
    }
    return value * FromBits(static_cast<std::uint64_t>(power + 1023) << 52);
}

struct DoublePair {
    double high;
    double low;
};

DoublePair Sum(double left, double right) noexcept {
    const double high = left + right;
    const double tail = high - left;
    return {high, (left - (high - tail)) + (right - tail)};
}
DoublePair Add(const DoublePair& left, const DoublePair& right) noexcept {
    const auto sum = Sum(left.high, right.high);
    return Sum(sum.high, (sum.low + left.low) + right.low);
}
DoublePair Product(double left, double right) noexcept {
    // Splitting the significands by bits also works for very large multipliers.
    const double leftHigh = FromBits(Bits(left) & UINT64_C(0xfffffffff8000000));
    const double rightHigh = FromBits(Bits(right) & UINT64_C(0xfffffffff8000000));
    const double leftLow = left - leftHigh;
    const double rightLow = right - rightHigh;
    const double high = left * right;
    return {high,
            ((leftHigh * rightHigh - high) + leftHigh * rightLow + leftLow * rightHigh) +
                leftLow * rightLow};
}
DoublePair Multiply(const DoublePair& left, const DoublePair& right) noexcept {
    const auto product = Product(left.high, right.high);
    return Sum(product.high, (product.low + left.high * right.low) + left.low * right.high);
}
DoublePair Divide(const DoublePair& numerator, const DoublePair& denominator) noexcept {
    const double high = numerator.high / denominator.high;
    const auto product = Multiply({high, 0.0}, denominator);
    const auto residue = Add(numerator, {-product.high, -product.low});
    return Sum(high, (residue.high + residue.low) / denominator.high);
}

struct ReducedAngle {
    double high;
    double low;
    unsigned quadrant;
};

// 1584 fractional bits of 2/pi, in fdlibm's base-2^24 representation.
constexpr std::array<std::uint32_t, 66> kTwoOverPi = {
    0xA2F983, 0x6E4E44, 0x1529FC, 0x2757D1, 0xF534DD, 0xC0DB62, 0x95993C, 0x439041,
    0xFE5163, 0xABDEBB, 0xC561B7, 0x246E3A, 0x424DD2, 0xE00649, 0x2EEA09, 0xD1921C,
    0xFE1DEB, 0x1CB129, 0xA73EE8, 0x8235F5, 0x2EBB44, 0x84E99C, 0x7026B4, 0x5F7E41,
    0x3991D6, 0x398353, 0x39F49C, 0x845F8B, 0xBDF928, 0x3B1FF8, 0x97FFDE, 0x05980F,
    0xEF2F11, 0x8B5A0A, 0x6D1F6D, 0x367ECF, 0x27CB09, 0xB74F46, 0x3F669E, 0x5FEA2D,
    0x7527BA, 0xC7EBE5, 0xF17B3D, 0x0739F7, 0x8A5292, 0xEA6BFB, 0x5FB11F, 0x8D5D08,
    0x560330, 0x46FC7B, 0x6BABF0, 0xCFBC20, 0x9AF436, 0x1DA9E3, 0x91615E, 0xE61B08,
    0x659985, 0x5F14A0, 0x68408D, 0xFFD880, 0x4D7327, 0x310606, 0x1556CA, 0x73A8C9};

using Limbs = std::array<std::uint32_t, 69>;
unsigned GetBit(const Limbs& limbs, int index) noexcept {
    if (index < 0 || index >= 1656) {
        return 0;
    }
    const auto position = static_cast<unsigned>(index);
    return (limbs[position / 24] >> (position % 24)) & 1U;
}

double FractionPart(const Limbs& limbs, int start, int scale) noexcept {
    std::uint64_t significand = 0;
    // Exactly 53 bits; start is inside the 1656-bit product.
    for (int offset = 0; offset < 53; ++offset) {
        significand = (significand << 1) | GetBit(limbs, start - offset);
    }
    return Scale(static_cast<double>(significand), scale - 52);
}

ReducedAngle Reduce(double value) noexcept {
    if (Abs(value) <= 0x1.921fb54442d18p-1) {
        return {value, 0.0, 0};
    }
    const auto bits = Bits(value);
    const int exponent = static_cast<int>((bits & kMagnitude) >> 52) - 1023;
    const std::uint64_t significand = (bits & UINT64_C(0xfffffffffffff)) | (UINT64_C(1) << 52);
    Limbs product{};
    // Three significand limbs times 66 constant limbs. Every accumulator is
    // below 2^49; the 69-limb result covers every finite binary64 input.
    for (unsigned part = 0; part < 3; ++part) {
        const auto digit = (significand >> (24 * part)) & UINT64_C(0xffffff);
        std::uint64_t carry = 0;
        for (unsigned index = 0; index < 66; ++index) {
            const auto combined = product[part + index] + digit * kTwoOverPi[65 - index] + carry;
            product[part + index] = static_cast<std::uint32_t>(combined & UINT64_C(0xffffff));
            carry = combined >> 24;
        }
        product[part + 66] = static_cast<std::uint32_t>(carry);
    }
    const int shift = 1584 + 52 - exponent;
    unsigned quadrant = GetBit(product, shift) | (GetBit(product, shift + 1) << 1);
    const bool roundUp = GetBit(product, shift - 1) != 0;
    if (roundUp) {
        quadrant = (quadrant + 1) & 3U;
        std::uint32_t carry = 1;
        for (auto& digit : product) {
            const auto complement = (digit ^ 0xffffffU) + carry;
            digit = complement & 0xffffffU;
            carry = complement >> 24;
        }
    }
    int first = shift - 1;
    // At most 1637 bits, independent of the magnitude of the input.
    while (first >= 0 && GetBit(product, first) == 0) {
        --first;
    }
    const double high = FractionPart(product, first, first - shift);
    const double low = FractionPart(product, first - 53, first - shift - 53);
    const auto reduced = Multiply({high, low}, {kHalfPi, kHalfPiLow});
    const bool negative = (bits & kSign) != 0;
    const double sign = negative != roundUp ? -1.0 : 1.0;
    if (negative) {
        quadrant = (4U - quadrant) & 3U;
    }
    return {sign * reduced.high, sign * reduced.low, quadrant};
}

double SinKernel(double value, double tail) noexcept {
    constexpr double s1 = -1.66666666666666324348e-01;
    constexpr double s2 = 8.33333333332248946124e-03;
    constexpr double s3 = -1.98412698298579493134e-04;
    constexpr double s4 = 2.75573137070700676789e-06;
    constexpr double s5 = -2.50507602534068634195e-08;
    constexpr double s6 = 1.58969099521155010221e-10;
    const double square = value * value;
    const double cube = square * value;
    const double remainder = s2 + square * (s3 + square * (s4 + square * (s5 + square * s6)));
    return value - ((square * (0.5 * tail - cube * remainder) - tail) - cube * s1);
}

double CosKernel(double value, double tail) noexcept {
    constexpr double c1 = 4.16666666666666019037e-02;
    constexpr double c2 = -1.38888888888741095749e-03;
    constexpr double c3 = 2.48015872894767294178e-05;
    constexpr double c4 = -2.75573143513906633035e-07;
    constexpr double c5 = 2.08757232129817482790e-09;
    constexpr double c6 = -1.13596475577881948265e-11;
    const double square = value * value;
    const double remainder =
        square * (c1 + square * (c2 + square * (c3 + square * (c4 + square * (c5 + square * c6)))));
    if (Abs(value) < 0.3) {
        return 1.0 - (0.5 * square - (square * remainder - value * tail));
    }
    const double quarter =
        Abs(value) > 0.78125
            ? 0.28125
            : FromBits((Bits(Abs(value)) - (UINT64_C(2) << 52)) & UINT64_C(0xffffffff00000000));
    const double halfSquare = 0.5 * square - quarter;
    return (1.0 - quarter) - (halfSquare - (square * remainder - value * tail));
}

double TanKernel(double value, double tail, bool reciprocal) noexcept {
    constexpr std::array<double, 13> coefficients = {3.33333333333334091986e-01,
                                                     1.33333333333201242699e-01,
                                                     5.39682539762260521377e-02,
                                                     2.18694882948595424599e-02,
                                                     8.86323982359930005737e-03,
                                                     3.59207910759131235356e-03,
                                                     1.45620945432529025516e-03,
                                                     5.88041240820264096874e-04,
                                                     2.46463134818469906812e-04,
                                                     7.81794442939557092300e-05,
                                                     7.14072491382608190305e-05,
                                                     -1.85586374855275456654e-05,
                                                     2.59073051863633712884e-05};
    const bool negative = (Bits(value) & kSign) != 0;
    const bool transform = Abs(value) >= 0.6744;
    if (transform) {
        if (negative) {
            value = -value;
            tail = -tail;
        }
        value = (kPi / 4.0 - value) + (kPiLow / 4.0 - tail);
        tail = 0.0;
    }
    const double square = value * value;
    const double fourth = square * square;
    const double odd =
        coefficients[1] +
        fourth * (coefficients[3] +
                  fourth * (coefficients[5] +
                            fourth * (coefficients[7] +
                                      fourth * (coefficients[9] + fourth * coefficients[11]))));
    const double even =
        square *
        (coefficients[2] +
         fourth * (coefficients[4] +
                   fourth * (coefficients[6] +
                             fourth * (coefficients[8] +
                                       fourth * (coefficients[10] + fourth * coefficients[12])))));
    const double cube = square * value;
    const double remainder =
        (tail + square * (cube * (odd + even) + tail)) + coefficients[0] * cube;
    const double tangent = value + remainder;
    if (transform) {
        const double direction = reciprocal ? -1.0 : 1.0;
        const double result =
            direction - 2.0 * (value - (tangent * tangent / (tangent + direction) - remainder));
        return negative ? -result : result;
    }
    if (!reciprocal) {
        return tangent;
    }
    const double high = FromBits(Bits(tangent) & UINT64_C(0xffffffff00000000));
    const double low = remainder - (high - value);
    const double inverse = -1.0 / tangent;
    const double inverseHigh = FromBits(Bits(inverse) & UINT64_C(0xffffffff00000000));
    const double residue = 1.0 + inverseHigh * high;
    return inverseHigh + inverse * (residue + inverseHigh * low);
}

double Atan(double value) noexcept {
    constexpr std::array<double, 4> high = {4.63647609000806093515e-01,
                                            7.85398163397448278999e-01,
                                            9.82793723247329054082e-01,
                                            kHalfPi};
    constexpr std::array<double, 4> low = {2.26987774529616870924e-17,
                                           3.06161699786838301793e-17,
                                           1.39033110312309984516e-17,
                                           kHalfPiLow};
    constexpr std::array<double, 11> coefficients = {3.33333333333329318027e-01,
                                                     -1.99999999998764832476e-01,
                                                     1.42857142725034663711e-01,
                                                     -1.11111104054623557880e-01,
                                                     9.09088713343650656196e-02,
                                                     -7.69187620504482999495e-02,
                                                     6.66107313738753120669e-02,
                                                     -5.83357013379057348645e-02,
                                                     4.97687799461593236017e-02,
                                                     -3.65315727442169155270e-02,
                                                     1.62858201153657823623e-02};
    double reduced = Abs(value);
    int region = -1;
    if (reduced >= 0.4375) {
        if (reduced < 1.1875) {
            if (reduced < 0.6875) {
                region = 0;
                reduced = (2.0 * reduced - 1.0) / (2.0 + reduced);
            } else {
                region = 1;
                reduced = (reduced - 1.0) / (reduced + 1.0);
            }
        } else if (reduced < 2.4375) {
            region = 2;
            reduced = (reduced - 1.5) / (1.0 + 1.5 * reduced);
        } else {
            region = 3;
            reduced = -1.0 / reduced;
        }
    }
    const double square = reduced * reduced;
    const double fourth = square * square;
    const double odd =
        square *
        (coefficients[0] +
         fourth * (coefficients[2] +
                   fourth * (coefficients[4] +
                             fourth * (coefficients[6] +
                                       fourth * (coefficients[8] + fourth * coefficients[10])))));
    const double even =
        fourth * (coefficients[1] +
                  fourth * (coefficients[3] +
                            fourth * (coefficients[5] +
                                      fourth * (coefficients[7] + fourth * coefficients[9]))));
    if (region < 0) {
        return WithSign(reduced - reduced * (odd + even), value);
    }
    const auto index = static_cast<unsigned>(region);
    return WithSign(high[index] - ((reduced * (odd + even) - low[index]) - reduced), value);
}

// Double-double log via 2*atanh((m-1)/(m+1)), with m in [sqrt(1/2),sqrt(2)].
// 24 terms bound truncation below 2^-124. Used by pow to retain low product bits.
DoublePair LogPair(double value) noexcept {
    int correction = 0;
    if (value < 0x1p-1022) {
        value *= 0x1p54;
        correction = -54;
    }
    const auto bits = Bits(value);
    int exponent = static_cast<int>(bits >> 52) - 1023 + correction;
    double mantissa = FromBits((bits & UINT64_C(0xfffffffffffff)) | UINT64_C(0x3ff0000000000000));
    if (mantissa > 0x1.6a09e667f3bcdp0) {
        mantissa *= 0.5;
        ++exponent;
    }
    const auto ratio = Divide(Sum(mantissa, -1.0), Sum(mantissa, 1.0));
    const auto square = Multiply(ratio, ratio);
    auto term = ratio;
    auto series = ratio;
    for (int index = 1; index < 24; ++index) {
        term = Multiply(term, square);
        series = Add(series, Divide(term, {static_cast<double>(2 * index + 1), 0.0}));
    }
    return Add(Multiply({2.0, 0.0}, series),
               Multiply({static_cast<double>(exponent), 0.0}, {kLn2, kLn2Low}));
}

double ExpPair(const DoublePair& value) noexcept {
    if (value.high > 710.0) {
        return FromBits(kInfinity);
    }
    if (value.high < -746.0) {
        return 0.0;
    }
    const int exponent =
        static_cast<int>(value.high * 1.44269504088896340736 + (value.high < 0.0 ? -0.5 : 0.5));
    const auto multiple = Multiply({static_cast<double>(exponent), 0.0}, {kLn2, kLn2Low});
    const auto reduced = Add(value, {-multiple.high, -multiple.low});
    DoublePair term{1.0, 0.0};
    DoublePair sum{1.0, 0.0};
    // |reduced| <= ln(2)/2; 26 terms leave a remainder below 2^-130.
    for (int index = 1; index <= 26; ++index) {
        term = Divide(Multiply(term, reduced), {static_cast<double>(index), 0.0});
        sum = Add(sum, term);
    }
    // Scale through the smallest normal factor to avoid premature underflow.
    if (exponent < -1022) {
        return (Scale(sum.high, exponent + 1022) + Scale(sum.low, exponent + 1022)) * 0x1p-1022;
    }
    if (exponent == 1024) {
        return (2.0 * sum.high + 2.0 * sum.low) * 0x1p1023;
    }
    return Scale(sum.high + sum.low, exponent);
}

// 0: noninteger, 1: odd integer, 2: even integer. No floating-to-integer cast.
unsigned IntegerKind(double value) noexcept {
    const auto bits = Bits(Abs(value));
    const int exponent = static_cast<int>(bits >> 52) - 1023;
    if (exponent < 0) {
        return 0;
    }
    if (exponent > 52) {
        return 2;
    }
    const unsigned shift = static_cast<unsigned>(52 - exponent);
    const auto fractionMask = (UINT64_C(1) << shift) - 1;
    if ((bits & fractionMask) != 0) {
        return 0;
    }
    return ((bits >> shift) & 1U) != 0 ? 1 : 2;
}

} // namespace

double Sin(double value) noexcept {
    if ((Bits(value) & kMagnitude) >= kInfinity) {
        return Nan();
    }
    if (Abs(value) < 0x1p-27) {
        return value;
    }
    const auto reduced = Reduce(value);
    switch (reduced.quadrant) {
        case 0:
            return SinKernel(reduced.high, reduced.low);
        case 1:
            return CosKernel(reduced.high, reduced.low);
        case 2:
            return -SinKernel(reduced.high, reduced.low);
        default:
            return -CosKernel(reduced.high, reduced.low);
    }
}
double Cos(double value) noexcept {
    if ((Bits(value) & kMagnitude) >= kInfinity) {
        return Nan();
    }
    const auto reduced = Reduce(value);
    switch (reduced.quadrant) {
        case 0:
            return CosKernel(reduced.high, reduced.low);
        case 1:
            return -SinKernel(reduced.high, reduced.low);
        case 2:
            return -CosKernel(reduced.high, reduced.low);
        default:
            return SinKernel(reduced.high, reduced.low);
    }
}
double Tan(double value) noexcept {
    if ((Bits(value) & kMagnitude) >= kInfinity) {
        return Nan();
    }
    if (Abs(value) < 0x1p-27) {
        return value;
    }
    const auto reduced = Reduce(value);
    return TanKernel(reduced.high, reduced.low, (reduced.quadrant & 1U) != 0);
}
double Atan2(double ordinate, double abscissa) noexcept {
    if (IsNan(ordinate) || IsNan(abscissa)) {
        return Nan();
    }
    const bool negativeX = (Bits(abscissa) & kSign) != 0;
    const double absY = Abs(ordinate);
    const double absX = Abs(abscissa);
    if (absY == 0.0) {
        return negativeX ? WithSign(kPi, ordinate) : ordinate;
    }
    if (absX == 0.0) {
        return WithSign(kHalfPi, ordinate);
    }
    if (absX == FromBits(kInfinity)) {
        if (absY == FromBits(kInfinity)) {
            return WithSign(negativeX ? 3.0 * (kPi / 4.0) : kPi / 4.0, ordinate);
        }
        return WithSign(negativeX ? kPi : 0.0, ordinate);
    }
    if (absY == FromBits(kInfinity)) {
        return WithSign(kHalfPi, ordinate);
    }
    double angle = 0.0;
    if (absY > absX) {
        angle = kHalfPi - (Atan(absX / absY) - kHalfPiLow);
    } else {
        angle = Atan(absY / absX);
    }
    if (negativeX) {
        angle = kPi - (angle - kPiLow);
    }
    return WithSign(angle, ordinate);
}
double Log(double value) noexcept {
    if (IsNan(value) || value < 0.0) {
        return Nan();
    }
    if (value == 0.0) {
        return -FromBits(kInfinity);
    }
    if (Bits(value) == kInfinity) {
        return value;
    }
    const auto result = LogPair(value);
    return result.high + result.low;
}
double Exp(double value) noexcept {
    if (IsNan(value)) {
        return Nan();
    }
    return ExpPair({value, 0.0});
}
double Pow(double base, double exponent) noexcept {
    if (exponent == 0.0 || base == 1.0) {
        return 1.0;
    }
    if (IsNan(base) || IsNan(exponent)) {
        return Nan();
    }
    const double magnitude = Abs(base);
    if (Abs(exponent) == FromBits(kInfinity)) {
        if (magnitude == 1.0) {
            return 1.0;
        }
        return (magnitude > 1.0) == (exponent > 0.0) ? FromBits(kInfinity) : 0.0;
    }
    const auto integer = IntegerKind(exponent);
    const bool negative = (Bits(base) & kSign) != 0 && integer == 1;
    if (magnitude == 0.0 || magnitude == FromBits(kInfinity)) {
        const bool infinite = (magnitude == 0.0) == (exponent < 0.0);
        const double result = infinite ? FromBits(kInfinity) : 0.0;
        return negative ? -result : result;
    }
    if (base < 0.0 && integer == 0) {
        return Nan();
    }
    if (magnitude == 1.0) {
        return negative ? -1.0 : 1.0;
    }
    if (exponent == 1.0) {
        return base;
    }
    if (exponent == -1.0) {
        return 1.0 / base;
    }
    if (exponent == 0.5) {
        return Sqrt(base);
    }
    if (exponent == -0.5) {
        return 1.0 / Sqrt(base);
    }
    // For distinct binary64 values adjacent to 1, |log(base)| >= 2^-54.
    // This bound also prevents overflow inside the double-double product.
    if (Abs(exponent) > 0x1p64) {
        const double result = (magnitude > 1.0) == (exponent > 0.0) ? FromBits(kInfinity) : 0.0;
        return negative ? -result : result;
    }
    const double result = ExpPair(Multiply(LogPair(magnitude), {exponent, 0.0}));
    return negative ? -result : result;
}
double Sqrt(double value) noexcept {
    if (IsNan(value) || value < 0.0) {
        return Nan();
    }
    return std::sqrt(value);
}

} // namespace Luminumbra::DeterministicMath::Script
