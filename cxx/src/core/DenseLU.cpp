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

#include "DenseLU.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <utility>

#include "SimulationManager.h"

// Why skipping zeros is exact (used by every path below)
// -------------------------------------------------------
// Each entry of the factorization is a running total: it starts as an input value and has
// products subtracted from it, x -= y * m.  When y or m is 0, the product is +0 or -0 (if the
// other factor is finite; inf * 0 is NaN), and x - (+-0) == x for every x except x = -0.0,
// where -0 - -0 = +0.  So such a subtraction can be skipped as long as nothing is inf or NaN and
// no running total is -0.0.  Running totals are never -0.0: the inputs aren't (stamping adds,
// and +0 + -0 = +0; checked anyway), and subtraction can only produce -0.0 from -0.0 (x - x =
// +0 in round-to-nearest; +0 - +-0 = +0).  The one operation that makes -0.0 is scaling by
// 1/pivot, and it applies to finished L entries, which are only ever the y above, never x.
//
// Notes on writing loops the compiler can vectorize (checked with MSVC's /Qvec-report:2 and
// by reading the generated assembly):
// - Loop with "i < count", not "i != count": with != the compiler can't be sure how many
//   times the loop runs, and MSVC refuses to vectorize it (reason 500).
// - Keep sizes and pointers in locals.  MSVC doesn't assume that storing a double can't change
//   an int, so a loop that stores through a double* re-reads a member like this->n from
//   memory on every iteration.
// - __restrict promises that two pointers never overlap, so loads and stores can be batched.

namespace {

// y[i] -= x[i] * s for i in [0, count).  Each element is independent, so this vectorizes, and
// each element gets exactly one multiply and one subtract, so it rounds like scalar code.
inline void subtractScaled(double *__restrict y, const double *__restrict x, double s, int count) {
    for (int i = 0; i < count; i++)
        y[i] -= x[i] * s;
}

// Bit tests on doubles, done with integer operations: std::isfinite/std::signbit compile to
// branchy scalar code under MSVC.  An IEEE-754 double is inf or NaN exactly when its 11
// exponent bits are all ones, and -0.0 is exactly the sign bit alone.
constexpr uint64_t EXPONENT_BITS = 0x7FF0000000000000ull;
constexpr uint64_t NEGATIVE_ZERO = 0x8000000000000000ull;

inline uint64_t bitsOf(double x) {
    uint64_t b;
    std::memcpy(&b, &x, sizeof b);  // the portable way to reinterpret; compiles to one move
    return b;
}

inline bool isNonFinite(double x) { return (bitsOf(x) & EXPONENT_BITS) == EXPONENT_BITS; }

inline bool anyNonFinite(const double *x, int count) {
    int bad = 0;
    for (int i = 0; i < count; i++)
        bad |= isNonFinite(x[i]);
    return bad != 0;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------------------------

// The first column whose entries differ from the input of the last factorization: n if none
// does (the factors can be reused as they are), 0 if there is nothing to compare with.  Only
// the stamped entries can differ (every other entry is +0.0 both times).
int DenseLU::firstChangedColumn(const std::vector<std::vector<double>> &a) const {
    if (!haveFactors || stampRow.size() != inputValues.size())
        return 0;
    int first = n;
    for (size_t p = 0; p < stampRow.size(); p++)
        // compare bit patterns, not values: -0.0 == 0.0 but they can factor differently,
        // and NaN != NaN (a NaN matrix never gets this far anyway)
        if (stampCol[p] < first && std::memcmp(&a[stampRow[p]][stampCol[p]], &inputValues[p], sizeof(double)) != 0)
            first = stampCol[p];
    return first;
}

// Group the stamped entries by column (a counting sort), for factorSparse().  Only needed when
// the list has grown since last time.
void DenseLU::groupByColumn() {
    const int N = n;
    if (groupedCount == stampRow.size() && (int) colStart.size() == N + 1)
        return;
    colStart.assign(N + 1, 0);
    for (int c : stampCol)
        colStart[c + 1]++;
    for (int c = 0; c < N; c++)
        colStart[c + 1] += colStart[c];
    colEntry.resize(stampCol.size());
    std::vector<int> next(colStart.begin(), colStart.end() - 1);
    for (size_t p = 0; p < stampCol.size(); p++)
        colEntry[next[stampCol[p]]++] = (int) p;
    groupedCount = stampRow.size();
}

bool DenseLU::factor(const std::vector<std::vector<double>> &a, int size, const std::vector<int> &stamped) {
    const int N = size;
    lastReused = false;
    if (N != n) {
        invalidate();
        n = N;
    }
    // extend the row/column cache to entries stamped since the last call (the list only grows)
    for (size_t p = stampRow.size(); p < stamped.size(); p++) {
        stampRow.push_back(stamped[p] % N);
        stampCol.push_back(stamped[p] / N);
    }
    const int firstChanged = firstChangedColumn(a);
    if (firstChanged == N) {
        lastReused = true;
        return true;
    }
    // Left-looking factorization builds column j from input column j and columns 0..j-1 only.
    // So if the last factorization was a successful sparse one, its columns before firstChanged
    // are exactly what they'd be now, and a replay can start at firstChanged.
    const int replayFrom = sparseValid ? firstChanged : 0;
    haveFactors = sparseValid = denseValid = false;

    // read the stamped entries, keeping their values for the next firstChangedColumn() and noting
    // which rows have a nonzero entry and whether anything is -0.0, inf or NaN
    rowHasNonzero.assign(N, 0);
    inputValues.resize(stampRow.size());
    inputHasNegativeZero = inputHasNonFinite = false;
    for (size_t p = 0; p < stampRow.size(); p++) {
        const double x = a[stampRow[p]][stampCol[p]];
        inputValues[p] = x;
        rowHasNonzero[stampRow[p]] |= (x != 0);  // true for NaN too, as "a[i][j] != 0" is
        inputHasNegativeZero |= bitsOf(x) == NEGATIVE_ZERO;
        inputHasNonFinite |= isNonFinite(x);
    }
    ipvt.resize(N);

    // a row of zeroes means a singular matrix (lu_factor_dense() checks this before anything)
    for (int i = 0; i < N; i++)
        if (!rowHasNonzero[i])
            return false;

    if (sparseEnabled && !inputHasNegativeZero && !inputHasNonFinite) {
        // replay the recorded structure if it's for this stamped pattern; else find it
        SparseResult r = SparseResult::Mismatch;
        if (symbolicValid && symbolicCount == stampRow.size())
            r = sparseRefactor(replayFrom);
        if (r == SparseResult::Mismatch)
            r = factorSparse();
        if (r == SparseResult::Done) {
            haveFactors = sparseValid = true;
            return true;
        }
        if (r == SparseResult::Singular)
            return false;
        // abandoned (an inf or NaN appeared): do it the dense way, which handles anything
    }
    counts.dense++;
    if (!factorDense())
        return false;
    haveFactors = denseValid = true;
    return true;
}

// ---------------------------------------------------------------------------------------------
// Sparse factorization
// ---------------------------------------------------------------------------------------------
//
// Left-looking, like factorDense(): column j is built from the input column by subtracting
// earlier columns, then a pivot is chosen and the part below the diagonal is scaled.  The
// differences are only in which work is visited:
//
// - The column is a "work" vector indexed by row id, with a list of the rows touched so far;
//   every other row is exactly zero.
// - Which earlier columns k can give a nonzero multiplier U[k][j]?  Only those reachable from
//   column j's nonzero inputs: an input in a row already pivoted to position k makes k
//   reachable, and subtracting column k touches the rows of L's column k, some of which were
//   pivoted to later positions, which are then reachable too.  A depth-first search finds them
//   all (it may find some whose value turns out to be exactly 0, which are then skipped, as in
//   factorDense()).
// - They must then be visited in ascending k.  The textbook sparse algorithm (Gilbert-Peierls)
//   uses the search's topological order, which is also correct, but it changes the order in
//   which each entry receives its subtractions, and so the rounding; sorting restores Crout's
//   order exactly.
// - Pivoting swaps two positions.  lu_factor_dense() swaps the two rows' entries in every
//   column; here entries stay keyed by row id and only rowAt[] / posOf[] change, O(1).  The
//   pivot rule is reproduced exactly: the largest magnitude among unpivoted rows, ties going
//   to the row at the later position (lu_factor_dense() scans positions in order with ">=").
//   Ties do happen in circuit matrices (a voltage source column holds +1 and -1).
DenseLU::SparseResult DenseLU::factorSparse() {
    const int N = n;
    groupByColumn();
    counts.sparseFull++;
    symbolicValid = false;

    rowAt.resize(N);
    posOf.resize(N);
    for (int i = 0; i < N; i++)
        rowAt[i] = posOf[i] = i;
    work.assign(N, 0.0);
    if ((int) touchMark.size() != N) {
        touchMark.assign(N, 0);
        seenMark.assign(N, 0);
    }
    lStart.assign(N + 1, 0);
    reachStart.assign(N + 1, 0);
    lRow.clear();
    lVal.clear();
    reachPos.clear();
    uColVal.clear();
    diag.resize(N);
    pivotRow.resize(N);

    double *const w = work.data();
    auto clearWork = [&] {
        for (int r : touched)
            w[r] = 0.0;
    };

    for (int j = 0; j < N; j++) {
        const int64_t gen = ++generation;
        touched.clear();
        reach.clear();

        // scatter column j of the input (every stamped entry, zero or not, so the structure
        // found below doesn't depend on values)
        for (int q = colStart[j]; q < colStart[j + 1]; q++) {
            const int p = colEntry[q];
            const int r = stampRow[p];
            w[r] = inputValues[p];
            if (touchMark[r] != gen) {
                touchMark[r] = gen;
                touched.push_back(r);
            }
        }

        // find the earlier columns that can contribute (rows already pivoted, reachable from
        // the inputs through L's structure)
        const size_t inputs = touched.size();
        for (size_t t = 0; t < inputs; t++) {
            const int start = touched[t];
            if (posOf[start] >= j || seenMark[start] == gen)
                continue;
            seenMark[start] = gen;
            stack.push_back(start);
            while (!stack.empty()) {
                const int x = stack.back();
                stack.pop_back();
                const int k = posOf[x];
                reach.push_back(k);
                for (int e = lStart[k]; e < lStart[k + 1]; e++) {
                    const int y = lRow[e];
                    if (posOf[y] < j && seenMark[y] != gen) {
                        seenMark[y] = gen;
                        stack.push_back(y);
                    }
                }
            }
        }
        std::sort(reach.begin(), reach.end());

        // subtract them, in ascending k (Crout's order for every entry).  The rows of L's
        // column k are touched even when the multiplier is 0, to keep the structure symbolic.
        for (int k : reach) {
            const double akj = w[rowAt[k]];
            for (int e = lStart[k]; e < lStart[k + 1]; e++) {
                const int y = lRow[e];
                if (touchMark[y] != gen) {
                    touchMark[y] = gen;
                    touched.push_back(y);
                }
            }
            if (akj == 0.0)
                continue;
            for (int e = lStart[k]; e < lStart[k + 1]; e++)
                w[lRow[e]] -= lVal[e] * akj;
        }

        // the column must be finite for the zero-skipping above to have been exact
        for (int r : touched)
            if (isNonFinite(w[r])) {
                clearWork();
                return SparseResult::Abandoned;
            }

        // pivot: largest magnitude among unpivoted rows, ties to the later position.  Untouched
        // rows are 0, so they can only matter when every candidate is 0, and then the matrix is
        // singular whichever one lu_factor_dense() would pick.
        double largest = 0;
        int best = -1, bestPos = -1;
        for (int r : touched) {
            const int pos = posOf[r];
            if (pos < j)
                continue;
            const double x = std::abs(w[r]);
            if (x > largest || (x == largest && pos > bestPos)) {
                largest = x;
                best = r;
                bestPos = pos;
            }
        }
        if (largest == 0.0) {
            SimulationManager::console("didn't avoid zero");
            clearWork();
            return SparseResult::Singular;
        }
        if (bestPos != j) {
            const int displaced = rowAt[j];
            rowAt[j] = best;
            rowAt[bestPos] = displaced;
            posOf[best] = j;
            posOf[displaced] = bestPos;
        }
        ipvt[j] = bestPos;
        pivotRow[j] = best;
        const double pivot = w[best];
        diag[j] = pivot;

        // U's column j above the diagonal: one entry per reached position
        for (int k : reach) {
            reachPos.push_back(k);
            uColVal.push_back(w[rowAt[k]]);
        }
        reachStart[j + 1] = (int) reachPos.size();

        // L's column j: the unpivoted rows touched, scaled by 1/pivot
        if (j != N - 1) {
            const double mult = 1.0 / pivot;
            // A tiny (denormal) pivot makes 1/pivot overflow to inf.  lu_factor_dense() then
            // scales the zero entries too, and 0 * inf = NaN, which spreads to later columns;
            // skipping them is exact only for a finite multiplier.  (Found by tools/lutest.)
            if (isNonFinite(mult)) {
                clearWork();
                return SparseResult::Abandoned;
            }
            for (int r : touched) {
                if (posOf[r] <= j)
                    continue;
                const double v = w[r] * mult;
                if (isNonFinite(v)) {
                    clearWork();
                    return SparseResult::Abandoned;
                }
                lRow.push_back(r);
                lVal.push_back(v);
            }
        }
        lStart[j + 1] = (int) lRow.size();
        clearWork();
    }

    // arrange the structure for solve(): L entries by final position, U by rows (each row's
    // entries in ascending column, since the columns are walked in order), and where each U
    // entry lands, so sparseRefactor() can write values straight there
    lPos.resize(lRow.size());
    for (size_t e = 0; e < lRow.size(); e++)
        lPos[e] = posOf[lRow[e]];
    uRowStart.assign(N + 1, 0);
    for (int k : reachPos)
        uRowStart[k + 1]++;
    for (int i = 0; i < N; i++)
        uRowStart[i + 1] += uRowStart[i];
    uRowCol.resize(reachPos.size());
    uRowVal.resize(reachPos.size());
    uSlot.resize(reachPos.size());
    nextSlot.assign(uRowStart.begin(), uRowStart.end() - 1);
    for (int j = 0; j < N; j++)
        for (int e = reachStart[j]; e < reachStart[j + 1]; e++) {
            const int q = nextSlot[reachPos[e]]++;
            uSlot[e] = q;
            uRowCol[q] = j;
            uRowVal[q] = uColVal[e];
        }
    symbolicValid = true;
    symbolicCount = stampRow.size();
    return SparseResult::Done;
}

// Replay the structure recorded by factorSparse() with new values.  While every pivot comes out
// the same as recorded, the structure is exactly the one factorSparse() would find again (it
// depends only on the stamped pattern and the pivots), so only the arithmetic is left: no
// search, no sorting, no allocation.  Each column's pivot is chosen by the same rule as before
// and compared with the recorded one; if it differs, the rest of the structure doesn't apply
// and the caller does the full factorization.
//
// Columns before startCol are known to be unchanged from the last factorization (see factor()),
// so only their row swaps are redone, to bring rowAt[]/posOf[] to where they were at startCol.
DenseLU::SparseResult DenseLU::sparseRefactor(int startCol) {
    const int N = n;
    counts.sparseRefactor++;
    counts.columnsSkipped += startCol;
    counts.columnsReplayed += N - startCol;
    for (int i = 0; i < N; i++)
        rowAt[i] = posOf[i] = i;
    for (int j = 0; j < startCol; j++) {
        const int bestPos = ipvt[j];
        if (bestPos != j) {
            const int best = rowAt[bestPos], displaced = rowAt[j];
            rowAt[j] = best;
            rowAt[bestPos] = displaced;
            posOf[best] = j;
            posOf[displaced] = bestPos;
        }
    }
    double *const w = work.data();  // all zero between columns

    // the rows column j touched: its L rows, its pivot row and the rows of its U entries
    auto clearColumn = [&](int j) {
        for (int q = lStart[j]; q < lStart[j + 1]; q++)
            w[lRow[q]] = 0.0;
        w[pivotRow[j]] = 0.0;
        for (int e = reachStart[j]; e < reachStart[j + 1]; e++)
            w[rowAt[reachPos[e]]] = 0.0;
    };

    for (int j = startCol; j < N; j++) {
        for (int q = colStart[j]; q < colStart[j + 1]; q++) {
            const int p = colEntry[q];
            w[stampRow[p]] = inputValues[p];
        }
        for (int e = reachStart[j]; e < reachStart[j + 1]; e++) {
            const int k = reachPos[e];
            const double akj = w[rowAt[k]];
            if (akj == 0.0)
                continue;
            for (int q = lStart[k]; q < lStart[k + 1]; q++)
                w[lRow[q]] -= lVal[q] * akj;
        }

        // finite, as in factorSparse(); the candidates are column j's L rows and its pivot row
        bool finite = !isNonFinite(w[pivotRow[j]]);
        for (int q = lStart[j]; q < lStart[j + 1]; q++)
            finite &= !isNonFinite(w[lRow[q]]);
        for (int e = reachStart[j]; e < reachStart[j + 1]; e++)
            finite &= !isNonFinite(w[rowAt[reachPos[e]]]);
        if (!finite) {
            clearColumn(j);
            return SparseResult::Abandoned;
        }

        double largest = std::abs(w[pivotRow[j]]);
        int best = pivotRow[j], bestPos = posOf[best];
        for (int q = lStart[j]; q < lStart[j + 1]; q++) {
            const int r = lRow[q];
            const double x = std::abs(w[r]);
            const int pos = posOf[r];
            if (x > largest || (x == largest && pos > bestPos)) {
                largest = x;
                best = r;
                bestPos = pos;
            }
        }
        if (largest == 0.0) {
            SimulationManager::console("didn't avoid zero");
            clearColumn(j);
            return SparseResult::Singular;
        }
        if (best != pivotRow[j]) {
            counts.mismatches++;
            clearColumn(j);
            return SparseResult::Mismatch;
        }
        if (bestPos != j) {
            const int displaced = rowAt[j];
            rowAt[j] = best;
            rowAt[bestPos] = displaced;
            posOf[best] = j;
            posOf[displaced] = bestPos;
        }
        ipvt[j] = bestPos;
        const double pivot = w[best];
        diag[j] = pivot;

        for (int e = reachStart[j]; e < reachStart[j + 1]; e++)
            uRowVal[uSlot[e]] = w[rowAt[reachPos[e]]];

        if (j != N - 1) {
            const double mult = 1.0 / pivot;
            if (isNonFinite(mult)) {
                clearColumn(j);
                return SparseResult::Abandoned;
            }
            bool ok = true;
            for (int q = lStart[j]; q < lStart[j + 1]; q++) {
                const double v = w[lRow[q]] * mult;
                ok &= !isNonFinite(v);
                lVal[q] = v;
            }
            if (!ok) {
                clearColumn(j);
                return SparseResult::Abandoned;
            }
        }
        clearColumn(j);
    }
    return SparseResult::Done;
}

// ---------------------------------------------------------------------------------------------
// Dense factorization
// ---------------------------------------------------------------------------------------------

// Crout's method laid out column-major and left-looking: for each earlier column k, subtract
// column k times a[k][j] from all of column j (an AXPY).  Every entry gets the same subtractions
// in the same order (k ascending) as lu_factor_dense()'s dot products, so the bits are the same,
// and an earlier column with a zero multiplier is skipped (exact when nothing is inf, NaN or
// -0.0; see the top of the file).
bool DenseLU::factorDense() {
    const int N = n;
    lu.assign((size_t) N * N, 0.0);
    colFinite.assign(N, 0);
    double *const L = lu.data();
    for (size_t p = 0; p < stampRow.size(); p++)
        L[(size_t) stampCol[p] * N + stampRow[p]] = inputValues[p];
    const bool skipZeros = !inputHasNegativeZero;

    for (int j = 0; j < N; j++) {
        double *const colj = L + (size_t) j * N;
        // when we reach column k, colj[k] = a[k][j] has all of its own subtractions, so it is
        // final; rows above the diagonal stop at k = i - 1 and the rest at k = j - 1, which the
        // bounds "k < j" and "i > k" give
        for (int k = 0; k < j; k++) {
            const double akj = colj[k];
            if (skipZeros && akj == 0.0 && colFinite[k])
                continue;
            const double *colk = L + (size_t) k * N;
            subtractScaled(colj + k + 1, colk + k + 1, akj, N - k - 1);
        }

        // pivot: the largest magnitude on or below the diagonal; ">=" makes the last of equal
        // candidates win, as in lu_factor_dense()
        double largest = 0;
        int largestRow = -1;
        for (int i = j; i < N; i++) {
            double x = std::abs(colj[i]);
            if (x >= largest) {
                largest = x;
                largestRow = i;
            }
        }
        if (j != largestRow) {
            if (largestRow == -1) {
                SimulationManager::console("largestRow == -1");
                return false;
            }
            // swap whole rows, as lu_factor_dense() swaps its row vectors
            for (int c = 0; c < N; c++)
                std::swap(L[(size_t) c * N + j], L[(size_t) c * N + largestRow]);
        }
        ipvt[j] = largestRow;

        if (colj[j] == 0.0) {
            SimulationManager::console("didn't avoid zero");
            return false;
        }
        if (j != N - 1) {
            const double mult = 1.0 / colj[j];
            for (int i = j + 1; i < N; i++)
                colj[i] *= mult;
        }

        // column j is finished (later row swaps only reorder it, which keeps this true)
        colFinite[j] = !anyNonFinite(colj, N);
    }
    factorsFinite = true;
    for (int j = 0; j < N; j++)
        if (!colFinite[j])
            factorsFinite = false;
    return true;
}

// Build the dense factors when only the sparse ones exist (for the solve fallback).  They come
// out identical in every pivot, since both paths compute the same values.
void DenseLU::ensureDense() {
    if (denseValid)
        return;
    factorDense();
    denseValid = true;
}

// ---------------------------------------------------------------------------------------------
// Solving
// ---------------------------------------------------------------------------------------------
//
// All the solves match lu_solve_dense() bit for bit.  Forward substitution goes column by
// column: once b[j] is final, subtract L[i][j] * b[j] from every b[i] below it.  Each b[i] still
// gets its subtractions in ascending j, as in the row-by-row original, and a zero b[j] skips a
// whole column.  The row swaps, which lu_solve_dense() applies lazily as it reaches each row,
// are applied first; that's the same, since swap i only touches entries at or after i, which
// the lazy version hasn't used yet.  Back substitution must stay row by row (by columns, each
// b[i] would get its subtractions in descending j and round differently); it skips zero U
// entries.
//
// Skipping zeros needs a finite right side without -0.0 (the running totals start from it) and
// finite factors.  Any inf or NaN on the way ends up in the result (it never becomes finite
// again), so a result with one is recomputed the original way from the untouched right side.
void DenseLU::solve(std::vector<double> &bv) {
    const int N = n;
    const double *const b = bv.data();
    bool usable = true;
    for (int i = 0; i < N && usable; i++) {
        const uint64_t bits = bitsOf(b[i]);
        if ((bits & EXPONENT_BITS) == EXPONENT_BITS || bits == NEGATIVE_ZERO)
            usable = false;
    }
    if (usable && sparseValid) {
        solveSparse(bv);
        return;
    }
    ensureDense();
    if (usable && factorsFinite)
        solveDenseFast(bv);
    else
        solveDense(bv);
}

void DenseLU::solveSparse(std::vector<double> &bv) {
    const int N = n;
    scratch.resize(N);
    double *const x = scratch.data();
    const double *const b = bv.data();
    for (int p = 0; p < N; p++)
        x[p] = b[rowAt[p]];

    // forward substitution, by columns
    for (int j = 0; j < N; j++) {
        const double xj = x[j];
        if (xj == 0.0)
            continue;
        for (int e = lStart[j]; e < lStart[j + 1]; e++)
            x[lPos[e]] -= lVal[e] * xj;
    }

    // back substitution, by rows, ascending column
    for (int i = N - 1; i >= 0; i--) {
        double tot = x[i];
        for (int q = uRowStart[i]; q < uRowStart[i + 1]; q++)
            tot -= uRowVal[q] * x[uRowCol[q]];
        x[i] = tot / diag[i];
    }

    if (anyNonFinite(x, N)) {
        ensureDense();
        solveDense(bv);  // bv is still the right side
        return;
    }
    std::copy(x, x + N, bv.begin());
}

void DenseLU::solveDenseFast(std::vector<double> &bv) const {
    const int N = n;
    double *const b = bv.data();
    std::vector<double> saved(b, b + N);

    const double *const L = lu.data();
    const int *const piv = ipvt.data();
    for (int i = 0; i < N; i++)
        std::swap(b[i], b[piv[i]]);

    // forward substitution, by columns
    for (int j = 0; j < N - 1; j++) {
        const double bj = b[j];
        if (bj == 0.0)
            continue;
        subtractScaled(b + j + 1, L + (size_t) j * N + j + 1, bj, N - j - 1);
    }

    // back substitution, by rows, ascending j
    for (int i = N - 1; i >= 0; i--) {
        double tot = b[i];
        for (int j = i + 1; j < N; j++) {
            const double u = L[(size_t) j * N + i];
            if (u != 0.0)
                tot -= u * b[j];
        }
        b[i] = tot / L[(size_t) i * N + i];
    }

    if (anyNonFinite(b, N)) {
        bv.assign(saved.begin(), saved.end());
        solveDense(bv);
    }
}

// The same steps as lu_solve_dense(), reading entry (i, j) from lu[j * n + i].
void DenseLU::solveDense(std::vector<double> &bv) const {
    const int N = n;
    const double *const L = lu.data();
    const int *const piv = ipvt.data();
    double *const b = bv.data();
    int i;

    // find first nonzero b element
    for (i = 0; i < N; i++) {
        int row = piv[i];
        double swap = b[row];
        b[row] = b[i];
        b[i] = swap;
        if (swap != 0)
            break;
    }

    // forward substitution using the lower triangular matrix
    int bi = i++;
    for (; i < N; i++) {
        int row = piv[i];
        double tot = b[row];
        b[row] = b[i];
        for (int j = bi; j < i; j++)
            tot -= L[(size_t) j * N + i] * b[j];
        b[i] = tot;
    }

    // back substitution using the upper triangular matrix
    for (i = N - 1; i >= 0; i--) {
        double tot = b[i];
        for (int j = i + 1; j < N; j++)
            tot -= L[(size_t) j * N + i] * b[j];
        b[i] = tot / L[(size_t) i * N + i];
    }
}
