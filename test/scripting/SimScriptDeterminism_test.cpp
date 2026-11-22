#include "luminumbra_common/core/DeterministicScriptMath.h"
#include "luminumbra_common/scripting/SimScriptHost.h"

#include <gtest/gtest.h>

#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <stdexcept>

namespace {
namespace dm = Luminumbra::DeterministicScriptMath;
using namespace Luminumbra::scripting;

std::uint64_t Bits(double value) {
    return std::bit_cast<std::uint64_t>(value);
}
double Value(std::uint64_t bits) {
    return std::bit_cast<double>(bits);
}

struct TrigGolden {
    std::uint64_t input;
    std::uint64_t sine;
    std::uint64_t cosine;
    std::uint64_t tangent;
};
struct UnaryGolden {
    std::uint64_t input;
    std::uint64_t output;
};
struct BinaryGolden {
    std::uint64_t left;
    std::uint64_t right;
    std::uint64_t output;
};

// Golden bits checked against independent high-precision reference values.
// The fdlibm cosine kernel can differ by one ULP from correct rounding. These
// pin signed zeros, domain errors, reduction boundaries and the exponent range.
constexpr TrigGolden kTrigGolden[] = {
    {UINT64_C(0x0000000000000000),
     UINT64_C(0x0000000000000000),
     UINT64_C(0x3ff0000000000000),
     UINT64_C(0x0000000000000000)},
    {UINT64_C(0x8000000000000000),
     UINT64_C(0x8000000000000000),
     UINT64_C(0x3ff0000000000000),
     UINT64_C(0x8000000000000000)},
    {UINT64_C(0x0000000000000001),
     UINT64_C(0x0000000000000001),
     UINT64_C(0x3ff0000000000000),
     UINT64_C(0x0000000000000001)},
    {UINT64_C(0x8000000000000001),
     UINT64_C(0x8000000000000001),
     UINT64_C(0x3ff0000000000000),
     UINT64_C(0x8000000000000001)},
    {UINT64_C(0x000fffffffffffff),
     UINT64_C(0x000fffffffffffff),
     UINT64_C(0x3ff0000000000000),
     UINT64_C(0x000fffffffffffff)},
    {UINT64_C(0x0010000000000000),
     UINT64_C(0x0010000000000000),
     UINT64_C(0x3ff0000000000000),
     UINT64_C(0x0010000000000000)},
    {UINT64_C(0x3e40000000000000),
     UINT64_C(0x3e40000000000000),
     UINT64_C(0x3ff0000000000000),
     UINT64_C(0x3e40000000000000)},
    {UINT64_C(0x3fe0000000000000),
     UINT64_C(0x3fdeaee8744b05f0),
     UINT64_C(0x3fec1528065b7d50),
     UINT64_C(0x3fe17b4f5bf3474a)},
    {UINT64_C(0x3ff0000000000000),
     UINT64_C(0x3feaed548f090cee),
     UINT64_C(0x3fe14a280fb5068c),
     UINT64_C(0x3ff8eb245cbee3a6)},
    {UINT64_C(0xbff0000000000000),
     UINT64_C(0xbfeaed548f090cee),
     UINT64_C(0x3fe14a280fb5068c),
     UINT64_C(0xbff8eb245cbee3a6)},
    {UINT64_C(0x3fe921fb54442d18),
     UINT64_C(0x3fe6a09e667f3bcc),
     UINT64_C(0x3fe6a09e667f3bcd),
     UINT64_C(0x3fefffffffffffff)},
    {UINT64_C(0x3ff921fb54442d17),
     UINT64_C(0x3ff0000000000000),
     UINT64_C(0x3cb469898cc51702),
     UINT64_C(0x4329153d9443ed0b)},
    {UINT64_C(0x3ff921fb54442d18),
     UINT64_C(0x3ff0000000000000),
     UINT64_C(0x3c91a62633145c07),
     UINT64_C(0x434d02967c31cdb5)},
    {UINT64_C(0x3ff921fb54442d19),
     UINT64_C(0x3ff0000000000000),
     UINT64_C(0xbca72cece675d1fd),
     UINT64_C(0xc33617a15494767a)},
    {UINT64_C(0x400921fb54442d18),
     UINT64_C(0x3ca1a62633145c07),
     UINT64_C(0xbff0000000000000),
     UINT64_C(0xbca1a62633145c07)},
    {UINT64_C(0xc00921fb54442d18),
     UINT64_C(0xbca1a62633145c07),
     UINT64_C(0xbff0000000000000),
     UINT64_C(0x3ca1a62633145c07)},
    {UINT64_C(0x401921fb54442d18),
     UINT64_C(0xbcb1a62633145c07),
     UINT64_C(0x3ff0000000000000),
     UINT64_C(0xbcb1a62633145c07)},
    {UINT64_C(0x4024000000000000),
     UINT64_C(0xbfe1689ef5f34f52),
     UINT64_C(0xbfead9ac890c6b1f),
     UINT64_C(0x3fe4bf5f34be3782)},
    {UINT64_C(0xc024000000000000),
     UINT64_C(0x3fe1689ef5f34f52),
     UINT64_C(0xbfead9ac890c6b1f),
     UINT64_C(0xbfe4bf5f34be3782)},
    {UINT64_C(0x4130000000000000),
     UINT64_C(0x3fd526ccb2fc8656),
     UINT64_C(0x3fee33ada92fe2ae),
     UINT64_C(0x3fd6692e5779206f)},
    {UINT64_C(0x4340000000000000),
     UINT64_C(0xbfeb2a66c8f35586),
     UINT64_C(0xbfe0e9918bb35aac),
     UINT64_C(0x3ff9b33af5ae241f)},
    {UINT64_C(0x4415af1d78b58c40),
     UINT64_C(0xbfe4a5e605fd6450),
     UINT64_C(0x3fe872720fc60d3d),
     UINT64_C(0xbfeb06fbbe995394)},
    {UINT64_C(0x54b249ad2594c37d),
     UINT64_C(0xbfd85c5e5b929359),
     UINT64_C(0x3fed9757496841f5),
     UINT64_C(0xbfda5807d6f76f7d)},
    {UINT64_C(0x7fefffffffffffff),
     UINT64_C(0x3f7452fc98b34e97),
     UINT64_C(0xbfefffe62ecfab75),
     UINT64_C(0xbf74530cfe729484)},
    {UINT64_C(0xffefffffffffffff),
     UINT64_C(0xbf7452fc98b34e97),
     UINT64_C(0xbfefffe62ecfab75),
     UINT64_C(0x3f74530cfe729484)},
    {UINT64_C(0x3fd3333333333333),
     UINT64_C(0x3fd2e9cd95baba33),
     UINT64_C(0x3fee921dd42f09ba),
     UINT64_C(0x3fd3cc2a44e29998)},
    {UINT64_C(0xbfd3333333333333),
     UINT64_C(0xbfd2e9cd95baba33),
     UINT64_C(0x3fee921dd42f09ba),
     UINT64_C(0xbfd3cc2a44e29998)},
    {UINT64_C(0x3fe594af4f0d844d),
     UINT64_C(0x3fe3fb5210a73cf2),
     UINT64_C(0x3fe8fe9f26eae490),
     UINT64_C(0x3fe995054ea00c37)},
    {UINT64_C(0xbfe594af4f0d844d),
     UINT64_C(0xbfe3fb5210a73cf2),
     UINT64_C(0x3fe8fe9f26eae490),
     UINT64_C(0xbfe995054ea00c37)},
    {UINT64_C(0x3fe9000000000000),
     UINT64_C(0x3fe6888a4e134b2f),
     UINT64_C(0x3fe6b898fa9efb5d),
     UINT64_C(0x3fefbc511df5917f)},
    {UINT64_C(0xbfe9000000000000),
     UINT64_C(0xbfe6888a4e134b2f),
     UINT64_C(0x3fe6b898fa9efb5d),
     UINT64_C(0xbfefbc511df5917f)},
    {UINT64_C(0x3fe594af4f0d844c),
     UINT64_C(0x3fe3fb5210a73cf2),
     UINT64_C(0x3fe8fe9f26eae491),
     UINT64_C(0x3fe995054ea00c35)},
    {UINT64_C(0x3fe594af4f0d844e),
     UINT64_C(0x3fe3fb5210a73cf3),
     UINT64_C(0x3fe8fe9f26eae48f),
     UINT64_C(0x3fe995054ea00c38)},
    {UINT64_C(0x3fe8ffffffffffff),
     UINT64_C(0x3fe6888a4e134b2e),
     UINT64_C(0x3fe6b898fa9efb5e),
     UINT64_C(0x3fefbc511df5917d)},
    {UINT64_C(0x3fe9000000000001),
     UINT64_C(0x3fe6888a4e134b2f),
     UINT64_C(0x3fe6b898fa9efb5c),
     UINT64_C(0x3fefbc511df59181)},
    {UINT64_C(0x7ff0000000000000),
     UINT64_C(0x7ff8000000000000),
     UINT64_C(0x7ff8000000000000),
     UINT64_C(0x7ff8000000000000)},
    {UINT64_C(0xfff0000000000000),
     UINT64_C(0x7ff8000000000000),
     UINT64_C(0x7ff8000000000000),
     UINT64_C(0x7ff8000000000000)},
    {UINT64_C(0x7ff8000000000042),
     UINT64_C(0x7ff8000000000000),
     UINT64_C(0x7ff8000000000000),
     UINT64_C(0x7ff8000000000000)},
    {UINT64_C(0x7ff0000000000001),
     UINT64_C(0x7ff8000000000000),
     UINT64_C(0x7ff8000000000000),
     UINT64_C(0x7ff8000000000000)},
};

constexpr UnaryGolden kLogGolden[] = {
    {UINT64_C(0x0000000000000001), UINT64_C(0xc0874385446d71c3)},
    {UINT64_C(0x000fffffffffffff), UINT64_C(0xc086232bdd7abcd2)},
    {UINT64_C(0x0010000000000000), UINT64_C(0xc086232bdd7abcd2)},
    {UINT64_C(0x3fefffffffffffff), UINT64_C(0xbca0000000000000)},
    {UINT64_C(0x3ff0000000000000), UINT64_C(0x0000000000000000)},
    {UINT64_C(0x3ff0000000000001), UINT64_C(0x3cafffffffffffff)},
    {UINT64_C(0x3ff8000000000000), UINT64_C(0x3fd9f323ecbf984c)},
    {UINT64_C(0x4000000000000000), UINT64_C(0x3fe62e42fefa39ef)},
    {UINT64_C(0x4024000000000000), UINT64_C(0x40026bb1bbb55516)},
    {UINT64_C(0x7fefffffffffffff), UINT64_C(0x40862e42fefa39ef)},
    {UINT64_C(0x0000000000000000), UINT64_C(0xfff0000000000000)},
    {UINT64_C(0x8000000000000000), UINT64_C(0xfff0000000000000)},
    {UINT64_C(0xbff0000000000000), UINT64_C(0x7ff8000000000000)},
    {UINT64_C(0x7ff0000000000000), UINT64_C(0x7ff0000000000000)},
    {UINT64_C(0xfff0000000000000), UINT64_C(0x7ff8000000000000)},
    {UINT64_C(0x7ff8000000000042), UINT64_C(0x7ff8000000000000)},
    {UINT64_C(0x3ff6a09e667f3bcd), UINT64_C(0x3fd62e42fefa39f0)},
    {UINT64_C(0x4008000000000000), UINT64_C(0x3ff193ea7aad030b)},
    {UINT64_C(0x3fe8000000000000), UINT64_C(0xbfd269621134db92)},
    {UINT64_C(0x3ff6a09e667f3bcc), UINT64_C(0x3fd62e42fefa39ee)},
};

constexpr UnaryGolden kExpGolden[] = {
    {UINT64_C(0x0000000000000000), UINT64_C(0x3ff0000000000000)},
    {UINT64_C(0x8000000000000000), UINT64_C(0x3ff0000000000000)},
    {UINT64_C(0x0000000000000001), UINT64_C(0x3ff0000000000000)},
    {UINT64_C(0x8000000000000001), UINT64_C(0x3ff0000000000000)},
    {UINT64_C(0x3ff0000000000000), UINT64_C(0x4005bf0a8b145769)},
    {UINT64_C(0xbff0000000000000), UINT64_C(0x3fd78b56362cef38)},
    {UINT64_C(0x3fe62e42fefa39ef), UINT64_C(0x4000000000000000)},
    {UINT64_C(0x40862e42fefa39ef), UINT64_C(0x7fefffffffffff2a)},
    {UINT64_C(0x40862e42fefa39f0), UINT64_C(0x7ff0000000000000)},
    {UINT64_C(0xc0874385446d71c3), UINT64_C(0x0000000000000001)},
    {UINT64_C(0xc0874910d52d3051), UINT64_C(0x0000000000000001)},
    {UINT64_C(0xc0874910d52d3052), UINT64_C(0x0000000000000000)},
    {UINT64_C(0xc087500000000000), UINT64_C(0x0000000000000000)},
    {UINT64_C(0x7fefffffffffffff), UINT64_C(0x7ff0000000000000)},
    {UINT64_C(0xffefffffffffffff), UINT64_C(0x0000000000000000)},
    {UINT64_C(0x7ff0000000000000), UINT64_C(0x7ff0000000000000)},
    {UINT64_C(0xfff0000000000000), UINT64_C(0x0000000000000000)},
    {UINT64_C(0x7ff8000000000042), UINT64_C(0x7ff8000000000000)},
    {UINT64_C(0x3fb999999999999a), UINT64_C(0x3ff1aec7b35a00d4)},
    {UINT64_C(0xbfb999999999999a), UINT64_C(0x3fecf46d99d52b3a)},
    {UINT64_C(0xc086232bdd7abcd2), UINT64_C(0x001000000000007c)},
    {UINT64_C(0xc086280000000000), UINT64_C(0x0008bfe55de02338)},
    {UINT64_C(0xc085e00000000000), UINT64_C(0x00d14f2b0fb9307f)},
    {UINT64_C(0x4086200000000000), UINT64_C(0x7fc586f6bf260cf1)},
    {UINT64_C(0x4086280000000000), UINT64_C(0x7fdd422d2be5dc9b)},
};

constexpr UnaryGolden kSqrtGolden[] = {
    {UINT64_C(0x0000000000000000), UINT64_C(0x0000000000000000)},
    {UINT64_C(0x8000000000000000), UINT64_C(0x8000000000000000)},
    {UINT64_C(0x0000000000000001), UINT64_C(0x1e60000000000000)},
    {UINT64_C(0x000fffffffffffff), UINT64_C(0x1fffffffffffffff)},
    {UINT64_C(0x0010000000000000), UINT64_C(0x2000000000000000)},
    {UINT64_C(0x3ff0000000000000), UINT64_C(0x3ff0000000000000)},
    {UINT64_C(0x4000000000000000), UINT64_C(0x3ff6a09e667f3bcd)},
    {UINT64_C(0x4010000000000000), UINT64_C(0x4000000000000000)},
    {UINT64_C(0x7fefffffffffffff), UINT64_C(0x5fefffffffffffff)},
    {UINT64_C(0xbff0000000000000), UINT64_C(0x7ff8000000000000)},
    {UINT64_C(0x7ff0000000000000), UINT64_C(0x7ff0000000000000)},
    {UINT64_C(0xfff0000000000000), UINT64_C(0x7ff8000000000000)},
    {UINT64_C(0x7ff8000000000042), UINT64_C(0x7ff8000000000000)},
    {UINT64_C(0x7ff0000000000001), UINT64_C(0x7ff8000000000000)},
};

constexpr BinaryGolden kAtan2Golden[] = {
    {UINT64_C(0x0000000000000000), UINT64_C(0x0000000000000000), UINT64_C(0x0000000000000000)},
    {UINT64_C(0x0000000000000000), UINT64_C(0x8000000000000000), UINT64_C(0x400921fb54442d18)},
    {UINT64_C(0x8000000000000000), UINT64_C(0x0000000000000000), UINT64_C(0x8000000000000000)},
    {UINT64_C(0x8000000000000000), UINT64_C(0x8000000000000000), UINT64_C(0xc00921fb54442d18)},
    {UINT64_C(0x3ff0000000000000), UINT64_C(0x3ff0000000000000), UINT64_C(0x3fe921fb54442d18)},
    {UINT64_C(0x3ff0000000000000), UINT64_C(0xbff0000000000000), UINT64_C(0x4002d97c7f3321d2)},
    {UINT64_C(0x3ff0000000000000), UINT64_C(0x0000000000000000), UINT64_C(0x3ff921fb54442d18)},
    {UINT64_C(0x3ff0000000000000), UINT64_C(0x8000000000000000), UINT64_C(0x3ff921fb54442d18)},
    {UINT64_C(0x3ff0000000000000), UINT64_C(0x7ff0000000000000), UINT64_C(0x0000000000000000)},
    {UINT64_C(0x3ff0000000000000), UINT64_C(0xfff0000000000000), UINT64_C(0x400921fb54442d18)},
    {UINT64_C(0xbff0000000000000), UINT64_C(0x3ff0000000000000), UINT64_C(0xbfe921fb54442d18)},
    {UINT64_C(0xbff0000000000000), UINT64_C(0xbff0000000000000), UINT64_C(0xc002d97c7f3321d2)},
    {UINT64_C(0xbff0000000000000), UINT64_C(0x0000000000000000), UINT64_C(0xbff921fb54442d18)},
    {UINT64_C(0xbff0000000000000), UINT64_C(0x8000000000000000), UINT64_C(0xbff921fb54442d18)},
    {UINT64_C(0xbff0000000000000), UINT64_C(0x7ff0000000000000), UINT64_C(0x8000000000000000)},
    {UINT64_C(0xbff0000000000000), UINT64_C(0xfff0000000000000), UINT64_C(0xc00921fb54442d18)},
    {UINT64_C(0x0000000000000000), UINT64_C(0x3ff0000000000000), UINT64_C(0x0000000000000000)},
    {UINT64_C(0x0000000000000000), UINT64_C(0xbff0000000000000), UINT64_C(0x400921fb54442d18)},
    {UINT64_C(0x0000000000000000), UINT64_C(0x0000000000000000), UINT64_C(0x0000000000000000)},
    {UINT64_C(0x0000000000000000), UINT64_C(0x8000000000000000), UINT64_C(0x400921fb54442d18)},
    {UINT64_C(0x0000000000000000), UINT64_C(0x7ff0000000000000), UINT64_C(0x0000000000000000)},
    {UINT64_C(0x0000000000000000), UINT64_C(0xfff0000000000000), UINT64_C(0x400921fb54442d18)},
    {UINT64_C(0x8000000000000000), UINT64_C(0x3ff0000000000000), UINT64_C(0x8000000000000000)},
    {UINT64_C(0x8000000000000000), UINT64_C(0xbff0000000000000), UINT64_C(0xc00921fb54442d18)},
    {UINT64_C(0x8000000000000000), UINT64_C(0x0000000000000000), UINT64_C(0x8000000000000000)},
    {UINT64_C(0x8000000000000000), UINT64_C(0x8000000000000000), UINT64_C(0xc00921fb54442d18)},
    {UINT64_C(0x8000000000000000), UINT64_C(0x7ff0000000000000), UINT64_C(0x8000000000000000)},
    {UINT64_C(0x8000000000000000), UINT64_C(0xfff0000000000000), UINT64_C(0xc00921fb54442d18)},
    {UINT64_C(0x7ff0000000000000), UINT64_C(0x3ff0000000000000), UINT64_C(0x3ff921fb54442d18)},
    {UINT64_C(0x7ff0000000000000), UINT64_C(0xbff0000000000000), UINT64_C(0x3ff921fb54442d18)},
    {UINT64_C(0x7ff0000000000000), UINT64_C(0x0000000000000000), UINT64_C(0x3ff921fb54442d18)},
    {UINT64_C(0x7ff0000000000000), UINT64_C(0x8000000000000000), UINT64_C(0x3ff921fb54442d18)},
    {UINT64_C(0x7ff0000000000000), UINT64_C(0x7ff0000000000000), UINT64_C(0x3fe921fb54442d18)},
    {UINT64_C(0x7ff0000000000000), UINT64_C(0xfff0000000000000), UINT64_C(0x4002d97c7f3321d2)},
    {UINT64_C(0xfff0000000000000), UINT64_C(0x3ff0000000000000), UINT64_C(0xbff921fb54442d18)},
    {UINT64_C(0xfff0000000000000), UINT64_C(0xbff0000000000000), UINT64_C(0xbff921fb54442d18)},
    {UINT64_C(0xfff0000000000000), UINT64_C(0x0000000000000000), UINT64_C(0xbff921fb54442d18)},
    {UINT64_C(0xfff0000000000000), UINT64_C(0x8000000000000000), UINT64_C(0xbff921fb54442d18)},
    {UINT64_C(0xfff0000000000000), UINT64_C(0x7ff0000000000000), UINT64_C(0xbfe921fb54442d18)},
    {UINT64_C(0xfff0000000000000), UINT64_C(0xfff0000000000000), UINT64_C(0xc002d97c7f3321d2)},
    {UINT64_C(0x4008000000000000), UINT64_C(0x4010000000000000), UINT64_C(0x3fe4978fa3269ee1)},
    {UINT64_C(0x0000000000000001), UINT64_C(0x7fefffffffffffff), UINT64_C(0x0000000000000000)},
    {UINT64_C(0x7fefffffffffffff), UINT64_C(0x0000000000000001), UINT64_C(0x3ff921fb54442d18)},
    {UINT64_C(0x3fe0000000000000), UINT64_C(0x4000000000000000), UINT64_C(0x3fcf5b75f92c80dd)},
    {UINT64_C(0x3fdc000000000000), UINT64_C(0x3ff0000000000000), UINT64_C(0x3fda64eec3cc23fd)},
    {UINT64_C(0x3fe6000000000000), UINT64_C(0x3ff0000000000000), UINT64_C(0x3fe345f01cce37bb)},
    {UINT64_C(0x3ff3000000000000), UINT64_C(0x3ff0000000000000), UINT64_C(0x3febde70ed439fe7)},
    {UINT64_C(0x4003800000000000), UINT64_C(0x3ff0000000000000), UINT64_C(0x3ff2e75728833a54)},
    {UINT64_C(0x7ff8000000000042), UINT64_C(0x3ff0000000000000), UINT64_C(0x7ff8000000000000)},
    {UINT64_C(0x3ff0000000000000), UINT64_C(0x7ff8000000000042), UINT64_C(0x7ff8000000000000)},
    {UINT64_C(0x7ff0000000000001), UINT64_C(0x3ff0000000000000), UINT64_C(0x7ff8000000000000)},
};

constexpr BinaryGolden kPowGolden[] = {
    {UINT64_C(0x4000000000000000), UINT64_C(0x4008000000000000), UINT64_C(0x4020000000000000)},
    {UINT64_C(0x4000000000000000), UINT64_C(0xc008000000000000), UINT64_C(0x3fc0000000000000)},
    {UINT64_C(0x4000000000000000), UINT64_C(0x3fe0000000000000), UINT64_C(0x3ff6a09e667f3bcd)},
    {UINT64_C(0xc000000000000000), UINT64_C(0x4008000000000000), UINT64_C(0xc020000000000000)},
    {UINT64_C(0xc000000000000000), UINT64_C(0x4010000000000000), UINT64_C(0x4030000000000000)},
    {UINT64_C(0xc000000000000000), UINT64_C(0x3fe0000000000000), UINT64_C(0x7ff8000000000000)},
    {UINT64_C(0x8000000000000000), UINT64_C(0x4008000000000000), UINT64_C(0x8000000000000000)},
    {UINT64_C(0x8000000000000000), UINT64_C(0xc008000000000000), UINT64_C(0xfff0000000000000)},
    {UINT64_C(0x8000000000000000), UINT64_C(0x3fe0000000000000), UINT64_C(0x0000000000000000)},
    {UINT64_C(0x0000000000000000), UINT64_C(0xbff0000000000000), UINT64_C(0x7ff0000000000000)},
    {UINT64_C(0x0000000000000000), UINT64_C(0x0000000000000000), UINT64_C(0x3ff0000000000000)},
    {UINT64_C(0x7ff8000000000000), UINT64_C(0x0000000000000000), UINT64_C(0x3ff0000000000000)},
    {UINT64_C(0x3ff0000000000000), UINT64_C(0x7ff8000000000000), UINT64_C(0x3ff0000000000000)},
    {UINT64_C(0xbff0000000000000), UINT64_C(0x7ff0000000000000), UINT64_C(0x3ff0000000000000)},
    {UINT64_C(0x3fe0000000000000), UINT64_C(0x7ff0000000000000), UINT64_C(0x0000000000000000)},
    {UINT64_C(0x3fe0000000000000), UINT64_C(0xfff0000000000000), UINT64_C(0x7ff0000000000000)},
    {UINT64_C(0x4000000000000000), UINT64_C(0x7ff0000000000000), UINT64_C(0x7ff0000000000000)},
    {UINT64_C(0x4000000000000000), UINT64_C(0xfff0000000000000), UINT64_C(0x0000000000000000)},
    {UINT64_C(0x7ff0000000000000), UINT64_C(0x4000000000000000), UINT64_C(0x7ff0000000000000)},
    {UINT64_C(0xfff0000000000000), UINT64_C(0x4008000000000000), UINT64_C(0xfff0000000000000)},
    {UINT64_C(0xfff0000000000000), UINT64_C(0x3fe0000000000000), UINT64_C(0x7ff0000000000000)},
    {UINT64_C(0xfff0000000000000), UINT64_C(0xc008000000000000), UINT64_C(0x8000000000000000)},
    {UINT64_C(0x4000000000000000), UINT64_C(0x408ff80000000000), UINT64_C(0x7fe0000000000000)},
    {UINT64_C(0x4000000000000000), UINT64_C(0x4090000000000000), UINT64_C(0x7ff0000000000000)},
    {UINT64_C(0x4000000000000000), UINT64_C(0xc090c80000000000), UINT64_C(0x0000000000000001)},
    {UINT64_C(0x4000000000000000), UINT64_C(0xc090cc0000000000), UINT64_C(0x0000000000000000)},
    {UINT64_C(0x4024000000000000), UINT64_C(0x3fb999999999999a), UINT64_C(0x3ff4248ef8fc2604)},
    {UINT64_C(0x3ff0000000000001), UINT64_C(0x4330000000000000), UINT64_C(0x4005bf0a8b145769)},
    {UINT64_C(0x3fefffffffffffff), UINT64_C(0x4340000000000000), UINT64_C(0x3fd78b56362cef37)},
    {UINT64_C(0xbff0000000000000), UINT64_C(0x433fffffffffffff), UINT64_C(0xbff0000000000000)},
    {UINT64_C(0xbff0000000000000), UINT64_C(0x4340000000000000), UINT64_C(0x3ff0000000000000)},
    {UINT64_C(0xbff0000000000000), UINT64_C(0x432fffffffffffff), UINT64_C(0x7ff8000000000000)},
    {UINT64_C(0x3ff0000000000001), UINT64_C(0x54b249ad2594c37d), UINT64_C(0x7ff0000000000000)},
    {UINT64_C(0x3fefffffffffffff), UINT64_C(0x54b249ad2594c37d), UINT64_C(0x0000000000000000)},
    {UINT64_C(0x3ff0000000000001), UINT64_C(0xd4b249ad2594c37d), UINT64_C(0x0000000000000000)},
    {UINT64_C(0x3fefffffffffffff), UINT64_C(0xd4b249ad2594c37d), UINT64_C(0x7ff0000000000000)},
    {UINT64_C(0x7ff8000000000000), UINT64_C(0x4000000000000000), UINT64_C(0x7ff8000000000000)},
    {UINT64_C(0x4000000000000000), UINT64_C(0x7ff8000000000000), UINT64_C(0x7ff8000000000000)},
    {UINT64_C(0x4010000000000000), UINT64_C(0xbfe0000000000000), UINT64_C(0x3fe0000000000000)},
    {UINT64_C(0xc010000000000000), UINT64_C(0xbfe0000000000000), UINT64_C(0x7ff8000000000000)},
    {UINT64_C(0x4010000000000000), UINT64_C(0xbff0000000000000), UINT64_C(0x3fd0000000000000)},
    {UINT64_C(0xc010000000000000), UINT64_C(0xbff0000000000000), UINT64_C(0xbfd0000000000000)},
    {UINT64_C(0x0000000000000000), UINT64_C(0xbfe0000000000000), UINT64_C(0x7ff0000000000000)},
    {UINT64_C(0x7ff0000000000000), UINT64_C(0xbfe0000000000000), UINT64_C(0x0000000000000000)},
    {UINT64_C(0x0000000000000001), UINT64_C(0x3ff0000000000000), UINT64_C(0x0000000000000001)},
    {UINT64_C(0x0000000000000001), UINT64_C(0xbff0000000000000), UINT64_C(0x7ff0000000000000)},
    {UINT64_C(0x7fefffffffffffff), UINT64_C(0x3ff0000000000000), UINT64_C(0x7fefffffffffffff)},
    {UINT64_C(0x7fefffffffffffff), UINT64_C(0x3fe0000000000000), UINT64_C(0x5fefffffffffffff)},
    {UINT64_C(0xc000000000000000), UINT64_C(0xc008000000000000), UINT64_C(0xbfc0000000000000)},
    {UINT64_C(0x3fe0000000000000), UINT64_C(0xc090c80000000000), UINT64_C(0x7ff0000000000000)},
    {UINT64_C(0x0000000000000000), UINT64_C(0x4008000000000000), UINT64_C(0x0000000000000000)},
    {UINT64_C(0x8000000000000000), UINT64_C(0x4010000000000000), UINT64_C(0x0000000000000000)},
    {UINT64_C(0x8000000000000000), UINT64_C(0xc010000000000000), UINT64_C(0x7ff0000000000000)},
};

TEST(SimScriptDeterminism, TrigonometricGoldenBits) {
    for (const auto& golden : kTrigGolden) {
        SCOPED_TRACE(golden.input);
        EXPECT_EQ(Bits(dm::Sin(Value(golden.input))), golden.sine);
        EXPECT_EQ(Bits(dm::Cos(Value(golden.input))), golden.cosine);
        EXPECT_EQ(Bits(dm::Tan(Value(golden.input))), golden.tangent);
    }
}
TEST(SimScriptDeterminism, LogGoldenBits) {
    for (const auto& golden : kLogGolden) {
        SCOPED_TRACE(golden.input);
        EXPECT_EQ(Bits(dm::Log(Value(golden.input))), golden.output);
    }
}
TEST(SimScriptDeterminism, ExpGoldenBits) {
    for (const auto& golden : kExpGolden) {
        SCOPED_TRACE(golden.input);
        EXPECT_EQ(Bits(dm::Exp(Value(golden.input))), golden.output);
    }
}
TEST(SimScriptDeterminism, SqrtGoldenBits) {
    for (const auto& golden : kSqrtGolden) {
        SCOPED_TRACE(golden.input);
        EXPECT_EQ(Bits(dm::Sqrt(Value(golden.input))), golden.output);
    }
}
TEST(SimScriptDeterminism, Atan2GoldenBits) {
    for (const auto& golden : kAtan2Golden) {
        SCOPED_TRACE(golden.left);
        SCOPED_TRACE(golden.right);
        EXPECT_EQ(Bits(dm::Atan2(Value(golden.left), Value(golden.right))), golden.output);
    }
}
TEST(SimScriptDeterminism, PowGoldenBits) {
    for (const auto& golden : kPowGolden) {
        SCOPED_TRACE(golden.left);
        SCOPED_TRACE(golden.right);
        EXPECT_EQ(Bits(dm::Pow(Value(golden.left), Value(golden.right))), golden.output);
    }
}

SimCommand Command(std::uint64_t target = 1) {
    SimCommand command;
    command.kind = SimCommandKind::SetComponent;
    command.target = target;
    command.key = 17;
    command.payload_size = 1;
    command.payload[0] = std::byte{42};
    return command;
}

TEST(SimScriptCommands, SuccessPublishesOwnedCommandsInOrder) {
    SimCommandBuffer committed;
    ASSERT_TRUE(committed.stage(Command(7)));
    const auto result = SimScriptHost::Run(
        [](SimScriptCall& call) {
            auto command = Command(8);
            EXPECT_TRUE(call.stage(command));
            command.target = 9;
            command.payload[0] = std::byte{3};
            EXPECT_TRUE(call.stage(command));
            EXPECT_EQ(call.remaining_fuel(), 0U);
            return SimScriptStatus::Succeeded;
        },
        2,
        committed);
    ASSERT_EQ(result, SimScriptStatus::Succeeded);
    ASSERT_EQ(committed.commands().size(), 3U);
    EXPECT_EQ(committed.commands()[0].target, 7U);
    EXPECT_EQ(committed.commands()[1].target, 8U);
    EXPECT_EQ(committed.commands()[1].payload[0], std::byte{42});
    EXPECT_EQ(committed.commands()[2].target, 9U);
    EXPECT_EQ(committed.commands()[2].payload[0], std::byte{3});
}

TEST(SimScriptCommands, ReportedFailuresDiscardEveryStagedCommand) {
    constexpr SimScriptStatus failures[] = {SimScriptStatus::RuntimeError,
                                            SimScriptStatus::BudgetExceeded,
                                            SimScriptStatus::MemoryExceeded,
                                            SimScriptStatus::CommandLimit,
                                            SimScriptStatus::InvalidCommand};
    for (const auto failure : failures) {
        SCOPED_TRACE(static_cast<unsigned>(failure));
        SimCommandBuffer committed;
        ASSERT_TRUE(committed.stage(Command(7)));
        EXPECT_EQ(SimScriptHost::Run(
                      [failure](SimScriptCall& call) {
                          EXPECT_TRUE(call.stage(Command(8)));
                          EXPECT_TRUE(call.stage(Command(9)));
                          return failure;
                      },
                      10,
                      committed),
                  failure);
        ASSERT_EQ(committed.commands().size(), 1U);
        EXPECT_EQ(committed.commands()[0].target, 7U);
        EXPECT_EQ(committed.status(), SimScriptStatus::Succeeded);
    }
}

TEST(SimScriptCommands, ExceptionsDiscardAndNextCallCanSucceed) {
    for (const bool outOfMemory : {false, true}) {
        SimCommandBuffer committed;
        ASSERT_TRUE(committed.stage(Command(7)));
        const auto result = SimScriptHost::Run(
            [outOfMemory](SimScriptCall& call) -> SimScriptStatus {
                EXPECT_TRUE(call.stage(Command(8)));
                if (outOfMemory) {
                    throw std::bad_alloc{};
                }
                throw std::runtime_error("script failure");
            },
            10,
            committed);
        EXPECT_EQ(result,
                  outOfMemory ? SimScriptStatus::MemoryExceeded : SimScriptStatus::RuntimeError);
        ASSERT_EQ(committed.commands().size(), 1U);
        EXPECT_EQ(committed.commands()[0].target, 7U);
        EXPECT_EQ(SimScriptHost::Run(
                      [](SimScriptCall& call) {
                          EXPECT_TRUE(call.stage(Command(9)));
                          return SimScriptStatus::Succeeded;
                      },
                      1,
                      committed),
                  SimScriptStatus::Succeeded);
        ASSERT_EQ(committed.commands().size(), 2U);
        EXPECT_EQ(committed.commands()[1].target, 9U);
    }
}

TEST(SimScriptCommands, FuelExhaustionIsStickyEvenWhenIgnored) {
    SimCommandBuffer committed;
    ASSERT_TRUE(committed.stage(Command(7)));
    EXPECT_EQ(SimScriptHost::Run(
                  [](SimScriptCall& call) {
                      EXPECT_TRUE(call.stage(Command(8)));
                      EXPECT_TRUE(call.consume(1));
                      EXPECT_FALSE(call.stage(Command(9)));
                      EXPECT_FALSE(call.consume(0));
                      EXPECT_FALSE(call.stage(Command(10)));
                      EXPECT_TRUE(call.staged().commands().empty());
                      return SimScriptStatus::Succeeded;
                  },
                  2,
                  committed),
              SimScriptStatus::BudgetExceeded);
    ASSERT_EQ(committed.commands().size(), 1U);
    EXPECT_EQ(committed.commands()[0].target, 7U);
}

TEST(SimScriptCommands, ZeroAndMaximumFuelDoNotWrap) {
    SimCommandBuffer committed;
    EXPECT_EQ(SimScriptHost::Run(
                  [](SimScriptCall& call) {
                      EXPECT_TRUE(call.consume(0));
                      EXPECT_FALSE(call.stage(Command()));
                      return SimScriptStatus::Succeeded;
                  },
                  0,
                  committed),
              SimScriptStatus::BudgetExceeded);
    EXPECT_TRUE(committed.commands().empty());
    EXPECT_EQ(SimScriptHost::Run(
                  [](SimScriptCall& call) {
                      EXPECT_TRUE(call.consume(std::numeric_limits<std::uint64_t>::max()));
                      EXPECT_EQ(call.remaining_fuel(), 0U);
                      EXPECT_FALSE(call.consume(1));
                      return SimScriptStatus::Succeeded;
                  },
                  std::numeric_limits<std::uint64_t>::max(),
                  committed),
              SimScriptStatus::BudgetExceeded);
    EXPECT_EQ(SimScriptHost::Run(
                  [](SimScriptCall& call) {
                      EXPECT_FALSE(call.consume(std::numeric_limits<std::uint64_t>::max()));
                      return SimScriptStatus::Succeeded;
                  },
                  1,
                  committed),
              SimScriptStatus::BudgetExceeded);
    EXPECT_TRUE(committed.commands().empty());
}

TEST(SimScriptCommands, InvalidKindsAndOversizePayloadPoisonWholeCall) {
    for (const bool badKind : {false, true}) {
        SimCommandBuffer committed;
        ASSERT_TRUE(committed.stage(Command(7)));
        EXPECT_EQ(SimScriptHost::Run(
                      [badKind](SimScriptCall& call) {
                          EXPECT_TRUE(call.stage(Command(8)));
                          auto invalid = Command(9);
                          if (badKind) {
                              invalid.kind = static_cast<SimCommandKind>(255);
                          } else {
                              invalid.payload_size = std::numeric_limits<std::size_t>::max();
                          }
                          EXPECT_FALSE(call.stage(invalid));
                          EXPECT_FALSE(call.stage(Command(10)));
                          EXPECT_TRUE(call.staged().commands().empty());
                          return SimScriptStatus::Succeeded;
                      },
                      10,
                      committed),
                  SimScriptStatus::InvalidCommand);
        ASSERT_EQ(committed.commands().size(), 1U);
        EXPECT_EQ(committed.commands()[0].target, 7U);
    }
}

TEST(SimScriptCommands, AllKindsAndPayloadBoundariesAreOwnedAndCanonical) {
    constexpr SimCommandKind kinds[] = {SimCommandKind::Spawn,
                                        SimCommandKind::Destroy,
                                        SimCommandKind::SetComponent,
                                        SimCommandKind::EmitEvent,
                                        SimCommandKind::DepositField};
    for (const auto kind : kinds) {
        for (const std::size_t size : {std::size_t{0}, std::size_t{64}}) {
            SimCommandBuffer staged;
            auto command = Command();
            command.kind = kind;
            command.payload.fill(std::byte{11});
            command.payload_size = size;
            ASSERT_TRUE(staged.stage(command));
            command.payload.fill(std::byte{22});
            ASSERT_EQ(staged.commands().size(), 1U);
            const auto& stored = staged.commands()[0];
            EXPECT_EQ(stored.kind, kind);
            EXPECT_EQ(stored.key, command.key);
            EXPECT_EQ(stored.payload_size, size);
            for (const auto byte : stored.payload) {
                EXPECT_EQ(byte, size == 0 ? std::byte{} : std::byte{11});
            }
        }
    }
}

TEST(SimScriptCommands, StagingCapacityRefusalClearsPrefix) {
    SimCommandBuffer committed;
    ASSERT_TRUE(committed.stage(Command(7)));
    EXPECT_EQ(SimScriptHost::Run(
                  [](SimScriptCall& call) {
                      for (std::size_t index = 0; index < SimCommandBuffer::kMaxCommands; ++index) {
                          EXPECT_TRUE(call.stage(Command(index)));
                      }
                      EXPECT_EQ(call.staged().commands().size(), SimCommandBuffer::kMaxCommands);
                      EXPECT_FALSE(call.stage(Command()));
                      EXPECT_FALSE(call.stage(Command()));
                      EXPECT_TRUE(call.staged().commands().empty());
                      return SimScriptStatus::Succeeded;
                  },
                  SimCommandBuffer::kMaxCommands + 2,
                  committed),
              SimScriptStatus::CommandLimit);
    ASSERT_EQ(committed.commands().size(), 1U);
    EXPECT_EQ(committed.commands()[0].target, 7U);
}

TEST(SimScriptCommands, PublicationCapacityRefusalKeepsDestinationUnchanged) {
    SimCommandBuffer committed;
    for (std::size_t index = 0; index < SimCommandBuffer::kMaxCommands - 1; ++index) {
        ASSERT_TRUE(committed.stage(Command(index)));
    }
    EXPECT_EQ(SimScriptHost::Run(
                  [](SimScriptCall& call) {
                      EXPECT_TRUE(call.stage(Command(900)));
                      EXPECT_TRUE(call.stage(Command(901)));
                      return SimScriptStatus::Succeeded;
                  },
                  2,
                  committed),
              SimScriptStatus::CommandLimit);
    ASSERT_EQ(committed.commands().size(), SimCommandBuffer::kMaxCommands - 1);
    EXPECT_EQ(committed.status(), SimScriptStatus::Succeeded);
    for (std::size_t index = 0; index < committed.commands().size(); ++index) {
        EXPECT_EQ(committed.commands()[index].target, index);
    }
    EXPECT_EQ(SimScriptHost::Run(
                  [](SimScriptCall& call) {
                      EXPECT_TRUE(call.stage(Command(902)));
                      return SimScriptStatus::Succeeded;
                  },
                  1,
                  committed),
              SimScriptStatus::Succeeded);
    ASSERT_EQ(committed.commands().size(), SimCommandBuffer::kMaxCommands);
    EXPECT_EQ(committed.commands().back().target, 902U);
    EXPECT_EQ(
        SimScriptHost::Run([](SimScriptCall&) { return SimScriptStatus::Succeeded; }, 0, committed),
        SimScriptStatus::Succeeded);
    EXPECT_EQ(committed.commands().size(), SimCommandBuffer::kMaxCommands);
}

TEST(SimScriptCommands, DiscardAndFailedJournalsCannotPublish) {
    SimCommandBuffer committed;
    ASSERT_TRUE(committed.stage(Command(7)));
    SimCommandBuffer discarded;
    ASSERT_TRUE(discarded.stage(Command(8)));
    discarded.discard(SimScriptStatus::Succeeded);
    EXPECT_EQ(discarded.status(), SimScriptStatus::RuntimeError);
    EXPECT_TRUE(discarded.commands().empty());
    discarded.discard(SimScriptStatus::BudgetExceeded);
    EXPECT_EQ(discarded.status(), SimScriptStatus::RuntimeError);
    EXPECT_FALSE(discarded.stage(Command(9)));
    EXPECT_FALSE(committed.append_atomic(discarded));
    ASSERT_EQ(committed.commands().size(), 1U);
    EXPECT_EQ(committed.commands()[0].target, 7U);
    EXPECT_FALSE(discarded.append_atomic(committed));
    EXPECT_EQ(SimScriptHost::Run(
                  [](SimScriptCall& call) {
                      EXPECT_TRUE(call.stage(Command(10)));
                      return SimScriptStatus::Succeeded;
                  },
                  1,
                  discarded),
              SimScriptStatus::RuntimeError);
    EXPECT_TRUE(discarded.commands().empty());
}

TEST(SimScriptCommands, SelfAppendCopiesCompleteBatchOrRefuses) {
    SimCommandBuffer committed;
    ASSERT_TRUE(committed.stage(Command(7)));
    ASSERT_TRUE(committed.stage(Command(8)));
    ASSERT_TRUE(committed.append_atomic(committed));
    ASSERT_EQ(committed.commands().size(), 4U);
    EXPECT_EQ(committed.commands()[2].target, 7U);
    EXPECT_EQ(committed.commands()[3].target, 8U);
    while (committed.commands().size() < SimCommandBuffer::kMaxCommands) {
        ASSERT_TRUE(committed.append_atomic(committed));
    }
    EXPECT_FALSE(committed.append_atomic(committed));
    EXPECT_EQ(committed.commands().size(), SimCommandBuffer::kMaxCommands);
}

} // namespace
