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

// (not in TS) A very cheap timestamp for profiling hot loops.
//
// On x86-64 this is the RDTSC instruction (0F 31), which puts the CPU's 64-bit time-stamp
// counter in EDX:EAX in roughly 20 cycles.  On every CPU of the last decade the counter ticks
// at a constant rate whatever the current clock speed ("invariant TSC"), so differences are
// proportional to wall time; convert them to seconds by comparing against a real clock over
// a long interval.  Elsewhere (e.g. ARM Macs) it falls back to std::chrono in nanoseconds.

#include <cstdint>

#if defined(_M_X64) || defined(__x86_64__)
#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <x86intrin.h>
#endif
inline uint64_t cycleCount() { return __rdtsc(); }
#else
#include <chrono>
inline uint64_t cycleCount() {
    return (uint64_t) std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}
#endif
