/*
    Copyright (C) Paul Falstad and Iain Sharp

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

#include <memory>
#include <vector>

#include "DenseLU.h"
#include "matrix/SparseLU.h"

class CircuitNode;
class VoltageSource;

// one independent sub-matrix of the circuit (one per closure of matrix-connected nodes)
class CircuitMatrix {
public:
    std::vector<std::vector<double>> matrix;
    std::vector<double> rightSide;
    std::vector<double> origRightSide;
    std::vector<std::vector<double>> origMatrix;
    std::vector<int> permute;
    int size = 0;
    int nodeCount = 0;   // number of node rows (vs rows come after)
    bool nonLinear = false;
    std::vector<double> nodeVoltages;
    std::vector<double> lastNodeVoltages;
    std::unique_ptr<SparseLU> sparseLU;
    // (not in TS) the fast engine's dense factors; `matrix` itself is left as stamped
    DenseLU denseLU;
    std::vector<CircuitNode *> nodeList;
    std::vector<VoltageSource *> voltageSourceList;

    // (not in TS) the entries stamped since clear(), as col*size+row; every other entry of
    // matrix and origMatrix is zero, so the sparse solver resets and converts only these
    // instead of all size*size entries
    std::vector<int> stamped;
    bool stampedSorted = true;

    // (not in TS) fast engine: a linear matrix is LU-factored once, in stampCircuit(), and then
    // frozen, because a later stamp would leave the factors out of date.  Until the next clear(), stamp()
    // refuses to write to a frozen matrix and sets frozenStamped instead, so the simulator can
    // find the element that broke the promise of nonLinear() == false and recover (see
    // SimulationManager::checkFrozenStamps()).
    bool frozen = false;
    bool frozenStamped = false;
    // (not in TS) treat this matrix as nonlinear from now on, because something stamped it
    // while it was frozen.  Survives clear(), so restamping doesn't forget it.
    bool forceNonLinear = false;

    // (not in TS) fast engine: the right side of the last solve (before solving), so a solve
    // with the same factors and the same right side can be skipped; see
    // SimulationManager::solveIsUnchanged()
    std::vector<double> lastRightSide;
    bool lastRightSideValid = false;
    bool lastSolveHadNaN = false;
    // (not in TS) fast engine: this frozen matrix already passed runCircuit()'s NaN/inf check
    bool passedFiniteCheck = false;

    // (not in TS) fast engine: before each sub-iteration runCircuit() puts matrix and rightSide back
    // to origMatrix and origRightSide, then the nonlinear elements stamp again.  The reference
    // engine copies everything; here only what changed is put back:
    // - dirty lists the entries stamped since matrix last equalled origMatrix, as (row, col).  A
    //   nonlinear element stamps a few entries per sub-iteration, while the stamped list holds every
    //   entry of the matrix.  Tracked from the end of stampCircuit() on, when the two are equal.
    // - rightSideDirty says rightSide may differ from origRightSide: something stamped it, a solve
    //   overwrote it with the solution, or an element changed origRightSide (see
    //   SimulationManager::setVoltageSourceForStep()).
    struct Entry {
        int row, col;
    };
    std::vector<Entry> dirty;
    bool trackDirty = false;
    bool rightSideDirty = true;

    void restoreDirty() {
        for (const Entry &e : dirty) {
            matrix[e.row][e.col] = origMatrix[e.row][e.col];
            stampedFlags[(size_t) e.col * size + e.row] &= (unsigned char) ~DIRTY;
        }
        dirty.clear();
    }

    void restoreRightSide() {
        if (!rightSideDirty)
            return;
        for (int i = 0; i != size; i++)
            rightSide[i] = origRightSide[i];
        rightSideDirty = false;
    }

    void clear() {
        matrix.assign(size, std::vector<double>(size, 0.0));
        origMatrix.assign(size, std::vector<double>(size, 0.0));
        stampedFlags.assign((size_t) size * size, 0);
        stamped.clear();
        stampedSorted = true;
        frozen = frozenStamped = false;
        lastRightSideValid = false;
        passedFiniteCheck = false;
        dirty.clear();
        trackDirty = false;
        rightSideDirty = true;
        denseLU.invalidate();  // the stamped list starts over
    }

    void stamp(int row, int col, double x) {
        if (frozen) {
            frozenStamped = true;
            return;
        }
        matrix[row][col] += x;
        size_t k = (size_t) col * size + row;
        unsigned char &f = stampedFlags[k];
        if (!(f & STAMPED)) {
            f |= STAMPED;
            stamped.push_back((int) k);
            stampedSorted = false;
        }
        if (trackDirty && !(f & DIRTY)) {
            f |= DIRTY;
            dirty.push_back({row, col});
        }
    }

private:
    static constexpr unsigned char STAMPED = 1;  // entry is in the stamped list
    static constexpr unsigned char DIRTY = 2;    // entry is in the dirty list
    std::vector<unsigned char> stampedFlags;
};
