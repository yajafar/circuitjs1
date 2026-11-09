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
#include <vector>

// (not in TS) LU factorization for the fast engine.
//
// It gives bit-for-bit the same pivots and solutions as SimulationManager::lu_factor_dense() /
// lu_solve_dense() (Crout's method with partial pivoting): every number goes through the same
// arithmetic operations in the same order.  What it leaves out is only work that provably
// changes nothing: subtracting an exact zero (see factorDense() for when that is exact).
//
// There are two ways it gets there:
//
// - Sparse (factorSparse(), the usual path).  Circuit matrices are mostly zeros (the digital
//   clock's 43x43 matrix is 6% nonzero), so it does arithmetic only where values can be nonzero:
//   it keeps the factors as lists of nonzero entries, finds which earlier columns can affect a
//   column by following those lists (a graph search), and keeps rows where they are and only
//   relabels positions when pivoting swaps rows.
// - Dense (factorDense()): the factors in one column-major n*n array, worked column by column.
//   Used when the sparse path's exactness conditions don't hold (a NaN, inf or -0.0 anywhere),
//   and built on demand when a solve needs the dense fallback.
//
// The input is read through the list of stamped entries (CircuitMatrix::stamped); every other
// entry must be +0.0.  The input matrix is only read, never modified.
class DenseLU {
public:
    // false makes every factor() take the dense path (for benchmarks; results are the same)
    static inline bool sparseEnabled = true;

    // how factorizations went, over all DenseLU objects (for benchmarks and tests)
    struct Counts {
        int64_t sparseFull = 0;      // sparse, structure found from scratch
        int64_t sparseRefactor = 0;  // sparse, recorded structure replayed
        int64_t mismatches = 0;      // replays that found a different pivot (then done in full)
        int64_t dense = 0;           // dense path
    };
    static inline Counts counts;

    // Factor the n x n matrix a.  Returns false if the matrix is singular, exactly when
    // lu_factor_dense() would.
    //
    // stamped lists the entries that may be nonzero, as col * n + row (CircuitMatrix::stamped);
    // every entry not in it must be +0.0.  The list may grow between calls, but anything else
    // (a new list after CircuitMatrix::clear()) needs invalidate() first.
    //
    // If the listed entries are bit-for-bit those of the last successful factor(), the factors
    // from then are still exact, so they are reused and reused() returns true.
    bool factor(const std::vector<std::vector<double>> &a, int n, const std::vector<int> &stamped);

    // Solve A x = b in place, using the factors from the last successful factor().
    void solve(std::vector<double> &b);

    // whether the last factor() call reused the previous factors
    bool reused() const { return lastReused; }

    // whether the last successful factor() took the sparse path (for statistics and tests)
    bool usedSparse() const { return sparseValid; }

    // forget the previous factors and the stamped-list cache (call when the list is rebuilt)
    void invalidate() {
        haveFactors = sparseValid = denseValid = symbolicValid = false;
        stampRow.clear();
        stampCol.clear();
        groupedCount = 0;
    }

private:
    // Done: factored.  Singular: the matrix is singular.  Abandoned: an inf/NaN made skipping
    // zeros inexact, use the dense path.  Mismatch (refactor only): a pivot differs from the
    // recorded one, so the recorded structure doesn't apply; do the full sparse factorization.
    enum class SparseResult { Done, Singular, Abandoned, Mismatch };

    int n = 0;
    bool haveFactors = false;  // factors for inputValues exist (sparse, dense or both)
    bool sparseValid = false;  // the sparse factors below are for inputValues
    bool denseValid = false;   // lu/ipvt are for inputValues
    bool lastReused = false;

    // the input: the stamped entries, their rows and columns, and the values factored last
    std::vector<int> stampRow, stampCol;
    std::vector<double> inputValues;
    std::vector<unsigned char> rowHasNonzero;
    bool inputHasNegativeZero = false;
    bool inputHasNonFinite = false;
    // the stamped entries grouped by column: column c's are colEntry[colStart[c] .. colStart[c+1])
    std::vector<int> colStart, colEntry;
    size_t groupedCount = 0;  // how many stamped entries colEntry covers

    // pivots, shared by both paths: the position swapped with position j at step j
    std::vector<int> ipvt;

    // dense factors: column-major, entry (i, j) at lu[j * n + i]
    std::vector<double> lu;
    std::vector<unsigned char> colFinite;  // during factorDense(): finished column k has no inf/NaN
    bool factorsFinite = false;

    // sparse factors.  Rows are identified by their original index ("row id"); pivoting moves
    // rows between positions, recorded in rowAt[position] and posOf[row id].
    //
    // The structure is symbolic: it lists every entry that can be nonzero given the stamped
    // pattern and the pivot sequence, whatever the values (some may be exactly 0).  It depends
    // on nothing else, so while the pivots stay the same it is reused (sparseRefactor()).
    std::vector<int> rowAt, posOf;
    std::vector<int> lStart, lRow;  // L column j: rows lRow[lStart[j] .. lStart[j+1]) (row ids)
    std::vector<double> lVal;       //   and their values
    std::vector<int> lPos;          //   their final positions, for solve()
    std::vector<int> reachStart, reachPos;  // column j's U entries: positions k, ascending
    std::vector<int> uSlot;         //   where each one goes in uRowVal
    std::vector<int> uRowStart, uRowCol;  // U above the diagonal by row, ascending column, for solve()
    std::vector<double> uRowVal;
    std::vector<double> diag;     // U's diagonal, by position
    std::vector<double> uColVal;  // factorSparse(): U values in reachPos order, until placed
    std::vector<int> pivotRow;    // the row id chosen as pivot for each column
    bool symbolicValid = false;   // the structure above matches the stamped pattern
    size_t symbolicCount = 0;     // how many stamped entries it was built from

    // work space for factorSparse(), kept between calls to avoid allocating
    std::vector<double> work;              // the column being built, by row id; zero between columns
    std::vector<int64_t> touchMark, seenMark;  // == generation: touched / visited in this column
    int64_t generation = 0;
    std::vector<int> touched, reach, stack, nextSlot;
    std::vector<double> scratch;  // solve()'s permuted right side

    bool sameAsInput(const std::vector<std::vector<double>> &a) const;
    void groupByColumn();
    SparseResult factorSparse();
    SparseResult sparseRefactor();
    bool factorDense();
    void ensureDense();
    void solveSparse(std::vector<double> &b);
    void solveDenseFast(std::vector<double> &b) const;
    void solveDense(std::vector<double> &b) const;
};
