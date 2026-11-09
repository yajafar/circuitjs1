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

// Stress test for DenseLU: random sparse matrices factored and solved by DenseLU and by the
// reference lu_factor_dense() / lu_solve_dense(), which must agree bit for bit (same singular
// verdict, same solution bits).  The matrices are built to provoke the hard cases: many +-1
// entries (exact pivot ties), entries that cancel exactly, stamped entries that are zero,
// singular matrices, and occasional NaN, inf and -0.0 inputs (which must take the fallbacks).
//
// usage: lutest [count] [seed]

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <random>
#include <vector>

#include "core/DenseLU.h"
#include "core/SimulationManager.h"

namespace {

bool sameBits(double a, double b) { return std::memcmp(&a, &b, sizeof a) == 0; }

}  // namespace

int main(int argc, char **argv) {
    const int count = argc > 1 ? std::atoi(argv[1]) : 20000;
    std::mt19937_64 rng(argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 12345);
    auto uniform = [&](double lo, double hi) { return std::uniform_real_distribution<double>(lo, hi)(rng); };
    auto chance = [&](double p) { return uniform(0, 1) < p; };

    int failures = 0, singular = 0, sparsePath = 0, reusedOk = 0;
    for (int t = 0; t < count; t++) {
        const int n = 1 + (int) (rng() % 48);
        const double density = uniform(0.03, 0.4);
        std::vector<std::vector<double>> a(n, std::vector<double>(n, 0.0));
        std::vector<int> stamped;
        std::vector<char> isStamped((size_t) n * n, 0);
        auto stamp = [&](int r, int c, double v) {
            a[r][c] += v;
            const int k = c * n + r;
            if (!isStamped[k]) {
                isStamped[k] = 1;
                stamped.push_back(k);
            }
        };
        // a diagonal most of the time, like a circuit's conductances
        for (int i = 0; i < n; i++)
            if (chance(0.9))
                stamp(i, i, chance(0.5) ? uniform(0.1, 10) : 1.0);
        for (int r = 0; r < n; r++)
            for (int c = 0; c < n; c++) {
                if (!chance(density))
                    continue;
                double v;
                switch (rng() % 6) {
                case 0: v = 1; break;
                case 1: v = -1; break;
                case 2: v = 0; break;  // stamped but zero
                case 3: v = uniform(-1, 1) * std::pow(10.0, (double) (rng() % 13) - 6); break;
                default: v = uniform(-5, 5); break;
                }
                stamp(r, c, v);
            }
        // exact cancellation: copy a row into another (makes equal rows, ties and zeros)
        if (n > 2 && chance(0.15)) {
            const int r1 = (int) (rng() % n), r2 = (int) (rng() % n);
            for (int c = 0; c < n; c++)
                if (a[r1][c] != 0)
                    stamp(r2, c, a[r1][c] - a[r2][c]);
        }
        // rare special values, to exercise the fallbacks
        if (chance(0.02)) {
            const int r = (int) (rng() % n), c = (int) (rng() % n);
            const double specials[] = {std::numeric_limits<double>::infinity(), std::nan(""), -0.0, 1e-310, 1e300};
            double v = specials[rng() % 5];
            const int k = c * n + r;
            if (!isStamped[k]) {
                isStamped[k] = 1;
                stamped.push_back(k);
            }
            a[r][c] = v;
        }

        std::vector<double> b(n);
        for (int i = 0; i < n; i++)
            b[i] = chance(0.3) ? 0.0 : (chance(0.2) ? 1.0 : uniform(-3, 3));

        // LUTEST_DUMP=t prints test t's matrix
        if (const char *d = std::getenv("LUTEST_DUMP"))
            if (std::atoi(d) == t) {
                std::printf("test %d, n=%d:\n", t, n);
                for (int r = 0; r < n; r++) {
                    for (int c = 0; c < n; c++)
                        std::printf(" %-24.17g", a[r][c]);
                    std::printf("\n");
                }
            }

        // Pass 0 factors and solves; pass 1 repeats it (the factors must be reused); passes 2-4
        // change the values but keep the stamped pattern, as a Newton iteration does, which
        // exercises the replay of the recorded structure, including pivots that change
        // (mismatches) and stamped zeros that become nonzero.
        DenseLU lu;
        std::vector<std::vector<double>> ra;
        std::vector<int> ripvt(n);
        std::vector<double> rb;
        bool refOk = false;
        for (int pass = 0; pass < 5; pass++) {
            if (pass >= 2) {
                for (int k : stamped) {
                    const int r = k % n, c = k / n;
                    double &v = a[r][c];
                    if (!std::isfinite(v) || v == 0.0) {
                        if (std::isfinite(v) && chance(0.2))
                            v = chance(0.5) ? 1.0 : uniform(-2, 2);
                        continue;
                    }
                    switch (rng() % 8) {
                    case 0: v = -v; break;                    // flips sign
                    case 1: v *= uniform(0.1, 10); break;     // may change which pivot wins
                    case 2: v = std::copysign(1.0, v); break; // makes ties
                    default: v *= uniform(0.999, 1.001); break;  // small Newton-like change
                    }
                }
            }
            if (pass != 1) {
                ra = a;
                refOk = SimulationManager::lu_factor_dense(ra, n, ripvt);
                rb = b;
                if (refOk)
                    SimulationManager::lu_solve_dense(ra, n, ripvt, rb);
            }
            const bool ok = lu.factor(a, n, stamped);
            if (ok != refOk) {
                std::printf("test %d (n=%d) pass %d: singular verdict differs: DenseLU %d, reference %d\n", t, n,
                            pass, ok, refOk);
                failures++;
                break;
            }
            if (!ok) {
                singular += pass == 0;
                if (pass == 0)
                    break;
                continue;  // a changed matrix may be singular; the next pass changes it again
            }
            if (pass == 0)
                sparsePath += lu.usedSparse();
            else if (pass == 1 && lu.reused())
                reusedOk++;
            std::vector<double> x = b;
            lu.solve(x);
            for (int i = 0; i < n; i++)
                if (!sameBits(x[i], rb[i])) {
                    std::printf("test %d (n=%d) pass %d: x[%d] = %.17g, reference %.17g\n", t, n, pass, i, x[i],
                                rb[i]);
                    failures++;
                    break;
                }
        }
        if (failures > 10)
            break;
    }
    std::printf("%d matrices: %d failures, %d singular, %d took the sparse path, %d reused correctly\n", count,
                failures, singular, sparsePath, reusedOk);
    std::printf("factorizations: %lld sparse full, %lld sparse replays (%lld pivot mismatches), %lld dense\n",
                (long long) DenseLU::counts.sparseFull, (long long) DenseLU::counts.sparseRefactor,
                (long long) DenseLU::counts.mismatches, (long long) DenseLU::counts.dense);
    return failures ? 1 : 0;
}
