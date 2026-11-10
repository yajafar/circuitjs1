/*
    This file is part of CircuitJS1.

    CircuitJS1 is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    CircuitJS1 is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with CircuitJS1.  If not, see <http://www.gnu.org/licenses/>.
*/

#pragma once

#include <cstdint>
#include <cstring>

// (not in TS) Inline tests for NaN, infinity and finiteness of a double.
//
// MSVC compiles std::isnan, std::isinf and std::isfinite to a call to the C runtime's
// _dclass(), which classifies the number the slow, general way (about 10 ns); in the simulator's
// inner loops they showed up as ~5% of the run.  An IEEE-754 double is a sign bit, 11 exponent
// bits and 52 fraction bits; the exponent bits are all ones exactly for infinities (fraction 0)
// and NaNs (fraction not 0).  The results are the same as std:: for every input.

namespace floatbits {

constexpr uint64_t EXPONENT = 0x7FF0000000000000ull;
constexpr uint64_t NOT_SIGN = 0x7FFFFFFFFFFFFFFFull;

inline uint64_t bits(double x) {
    uint64_t b;
    std::memcpy(&b, &x, sizeof b);  // the portable way to read the bits; compiles to one move
    return b;
}

// not a NaN and not an infinity
inline bool isFinite(double x) { return (bits(x) & EXPONENT) != EXPONENT; }

// a NaN (exponent all ones and a nonzero fraction)
inline bool isNaN(double x) { return (bits(x) & NOT_SIGN) > EXPONENT; }

// plus or minus infinity
inline bool isInf(double x) { return (bits(x) & NOT_SIGN) == EXPONENT; }

}  // namespace floatbits
