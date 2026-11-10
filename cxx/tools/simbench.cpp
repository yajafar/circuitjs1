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

// Headless benchmark: runs a circuit for a given amount of *simulated* time as fast as
// possible and reports how fast the engine went.  If the circuit has logic-level 7-segment
// displays, their digits are decoded and printed whenever they change, so a digital clock
// shows its time ticking over.
//
// usage: simbench circuit.txt [--seconds S] [--engine fast|ref] [--frame-steps N] [--quiet]
//                             [--verbose] [--profile] [--blame]
//
//   --seconds S   simulated seconds to run (default 1)
//   --steps N     stop after N timesteps, whichever comes first (for circuits with tiny timesteps)
//   --frame-steps N  timesteps per runCircuit() call, i.e. per GUI frame (default 1000).  Like
//                 the app, the engine works out wire currents (for drawing) once per frame when
//                 no scope needs them every step.  1 reproduces simtrace's one step per call.
//   --engine      fast (default): the optimized engine; ref: the original algorithm,
//                 bit-identical to the TypeScript version
//   --quiet       don't print display changes, only the summary
//   --verbose     show the engine's console log (e.g. "converged after 8 iterations")
//   --profile     time each phase of runCircuit() with the CPU's time-stamp counter
//   --blame       list the elements that most often report "not converged yet"
//   --elements    time doStep() per element class (adds ~20 cycles per call)
//   --dense-lu    use only DenseLU's dense path (to compare it with the sparse one)
//   --wires-every-step  work out wire currents every step, as a scope showing a wire makes the app do
//   --watch-wires N  pretend scopes watch every Nth wire; prints a hash of their currents at every
//                 step (equal for --engine ref and fast)

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "core/CircuitElm.h"
#include "core/CircuitLoader.h"
#include "core/CycleCounter.h"
#include "core/DenseLU.h"
#include "core/ElementFactory.h"
#include "core/SimApp.h"
#include "core/SimulationManager.h"
#include "elements/SevenSegElm.h"

namespace {

// A SimApp whose clock simbench advances by hand, so each runCircuit() call does exactly
// frameSteps timesteps, as one GUI frame would.  runCircuit() keeps stepping while
//     steps * 1000 < steprate * (now - lastIterTime),   steprate = 160 * getIterCount()
// so with the clock advanced 1 ms per call, getIterCount() = frameSteps * 1000 / 160 gives
// frameSteps steps.  lastFrameTime is kept at "now" so the frame-time limit never cuts in.
class BenchApp : public SimApp {
public:
    double clockMs = 1e12;
    int frameSteps = 1;
    double getIterCount() override { return frameSteps * 1000.0 / 160.0; }
    double currentTimeMillis() override { return clockMs; }
    double randomDouble() override { return 0.5; }
    int randomInt() override { return 0; }
    // There are no scopes here, so wire currents (needed only for drawing) are worked out once
    // per frame, unless --wires-every-step asks for what a scope showing a wire causes in the app,
    // or --watch-wires pretends scopes watch some wires (whose currents are then hashed every step).
    bool wiresEveryStep = false;
    std::vector<CircuitElm *> watched;
    uint64_t watchHash = 1469598103934665603ull;  // FNV-1a offset basis
    bool canDelayWireProcessing() override { return !wiresEveryStep && watched.empty(); }
    void collectViewedWires(std::unordered_set<CircuitElm *> &wires) override {
        wires.insert(watched.begin(), watched.end());
    }
    void onTimeStep() override {
        // FNV-1a over the bits of each watched wire's current: equal hashes mean every value at
        // every step was bit-identical
        for (CircuitElm *w : watched) {
            double c = w->getCurrent();
            unsigned char bytes[sizeof c];
            std::memcpy(bytes, &c, sizeof c);
            for (unsigned char b : bytes)
                watchHash = (watchHash ^ b) * 1099511628211ull;
        }
    }
    void startFrame() {
        clockMs += 1;
        lastFrameTime = clockMs;
    }
};

// Segment patterns a..g (bit 0 = a) for the digits 0-9, as a 7-segment decoder lights them.
// The CD4511 draws 6 and 9 without tails; both variants are accepted.
char decodeSegments(int bits) {
    switch (bits) {
    case 0x3F: return '0';
    case 0x06: return '1';
    case 0x5B: return '2';
    case 0x4F: return '3';
    case 0x66: return '4';
    case 0x6D: return '5';
    case 0x7D: case 0x7C: return '6';
    case 0x07: case 0x27: return '7';
    case 0x7F: return '8';
    case 0x6F: case 0x67: return '9';
    case 0x00: return ' ';  // blank
    default: return '?';
    }
}

// The logic-level (no diode) 7-segment displays, in reading order: top row first, left to
// right within a row (HH MM SS for a clock with its digits in a line).
std::vector<SevenSegElm *> findDisplays(std::vector<CircuitElm *> &elms) {
    std::vector<SevenSegElm *> displays;
    for (CircuitElm *ce : elms)
        if (auto *d = dynamic_cast<SevenSegElm *>(ce))
            if (d->diodeDirection == 0 && d->segmentCount == 7)
                displays.push_back(d);
    std::sort(displays.begin(), displays.end(), [](SevenSegElm *a, SevenSegElm *b) {
        return a->y != b->y ? a->y < b->y : a->x < b->x;
    });
    return displays;
}

// read every display; pairs of digits are joined with ':' (HH:MM:SS for a clock)
std::string readDisplays(const std::vector<SevenSegElm *> &displays) {
    std::string s;
    for (size_t i = 0; i != displays.size(); i++) {
        if (i > 0 && i % 2 == 0)
            s += ':';
        int bits = 0;
        for (int seg = 0; seg != 7; seg++)
            if (displays[i]->getNode(seg)->v > 2.5)
                bits |= 1 << seg;
        s += decodeSegments(bits);
    }
    return s;
}

}  // namespace

int main(int argc, char **argv) {
    if (argc < 2) {
        std::fprintf(stderr,
                     "usage: simbench circuit.txt [--seconds S] [--engine fast|ref] [--frame-steps N] [--quiet]\n"
                     "                [--verbose] [--profile] [--blame] [--elements]\n");
        return 1;
    }
    double seconds = 1;
    long maxSteps = 0;  // --steps N; 0 = no limit
    bool refEngine = false, quiet = false, verbose = false, profile = false, blame = false, elements = false;
    bool wiresEveryStep = false;
    int watchEvery = 0;  // --watch-wires N: watch every Nth wire-equivalent element
    int frameSteps = 1000;
    for (int i = 2; i < argc; i++) {
        if (!std::strcmp(argv[i], "--seconds") && i + 1 < argc)
            seconds = std::atof(argv[++i]);
        else if (!std::strcmp(argv[i], "--steps") && i + 1 < argc)
            maxSteps = std::atol(argv[++i]);
        else if (!std::strcmp(argv[i], "--frame-steps") && i + 1 < argc)
            frameSteps = std::max(1, std::atoi(argv[++i]));
        else if (!std::strcmp(argv[i], "--engine") && i + 1 < argc)
            refEngine = !std::strcmp(argv[++i], "ref");
        else if (!std::strcmp(argv[i], "--quiet"))
            quiet = true;
        else if (!std::strcmp(argv[i], "--verbose"))
            verbose = true;
        else if (!std::strcmp(argv[i], "--profile"))
            profile = true;
        else if (!std::strcmp(argv[i], "--blame"))
            blame = true;
        else if (!std::strcmp(argv[i], "--elements"))
            elements = true;
        else if (!std::strcmp(argv[i], "--dense-lu"))
            DenseLU::sparseEnabled = false;  // fast engine with only the dense LU path
        else if (!std::strcmp(argv[i], "--wires-every-step"))
            wiresEveryStep = true;
        else if (!std::strcmp(argv[i], "--watch-wires") && i + 1 < argc)
            watchEvery = std::max(1, std::atoi(argv[++i]));
        else {
            std::fprintf(stderr, "unknown option %s\n", argv[i]);
            return 1;
        }
    }
    // The engine logs to std::cerr (SimulationManager::console).  Unless --verbose, swap in a
    // stream buffer that accepts every character and drops it.  (Setting a null buffer would
    // also discard output, but it puts the stream in a failed state.)
    struct NullBuf : std::streambuf {
        int overflow(int c) override { return traits_type::not_eof(c); }
    } discard;
    std::streambuf *origCerr = std::cerr.rdbuf();
    if (!verbose)
        std::cerr.rdbuf(&discard);
    struct RestoreCerr {
        std::streambuf *buf;
        ~RestoreCerr() { std::cerr.rdbuf(buf); }
    } restoreCerr{origCerr};

    std::ifstream in(argv[1], std::ios::binary);
    if (!in) {
        std::fprintf(stderr, "can't open %s\n", argv[1]);
        return 1;
    }
    std::stringstream text;
    text << in.rdbuf();

    BenchApp app;
    SimulationManager sim(&app);
    CircuitElm::app = &app;
    CircuitElm::sim = &sim;
    registerElements();
    sim.fastEngine = !refEngine;
    app.frameSteps = frameSteps;
    app.wiresEveryStep = wiresEveryStep;
    app.developerMode = verbose;  // --verbose: also log the engine's per-step progress messages

    CircuitLoader loader(&app, &sim);
    loader.readCircuit(text.str());
    for (CircuitElm *ce : app.elmList)
        ce->setPoints();
    sim.lastIterTime = app.clockMs;  // so the first runCircuit() call doesn't just initialize the clock
    sim.analyzeCircuit();
    sim.preStampAndStampCircuit();
    if (watchEvery > 0) {
        int k = 0;
        for (CircuitElm *ce : app.elmList)
            if (ce->isWireEquivalent() && k++ % watchEvery == 0)
                app.watched.push_back(ce);
    }
    if (app.stopMessage) {
        std::printf("stopped before running: %s\n", app.stopMessage->c_str());
        return 1;
    }

    int linear = 0, maxSize = 0;
    for (auto &m : sim.matrices) {
        linear += !m->nonLinear;
        maxSize = std::max(maxSize, m->size);
    }
    std::printf("%s engine | %zu elements | %zu matrices (%d linear, %zu nonlinear, largest %d) | timestep %g s\n",
                refEngine ? "ref" : "fast", sim.elmList.size(), sim.matrices.size(), linear,
                sim.matrices.size() - linear, maxSize, sim.maxTimeStep);

    std::vector<SevenSegElm *> displays = findDisplays(app.elmList);
    std::string shown = readDisplays(displays);

    sim.stats = {};
    sim.profile = {};
    sim.profiling = profile;
    std::unordered_map<CircuitElm *, int64_t> blameCounts;
    if (blame)
        sim.convergenceBlame = &blameCounts;
    std::unordered_map<CircuitElm *, SimulationManager::ElmTicks> elmTicks;
    if (elements)
        sim.doStepTicks = &elmTicks;
    const long stepsAtStart = app.steps;
    uint64_t startTicks = cycleCount();
    auto start = std::chrono::steady_clock::now();
    auto wall = [&] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(); };
    while (sim.t < seconds && (maxSteps == 0 || app.steps - stepsAtStart < maxSteps)) {
        app.startFrame();
        const long stepsBefore = app.steps;
        sim.runCircuit(true);
        if (app.stopMessage) {
            std::printf("stopped at t=%.6f s: %s\n", sim.t, app.stopMessage->c_str());
            break;
        }
        if (app.steps == stepsBefore) {
            // an empty circuit (or one that can't run) never advances time
            std::printf("stopped at t=%.6f s: no progress\n", sim.t);
            break;
        }
        if (!displays.empty() && !quiet) {
            std::string now = readDisplays(displays);
            if (now != shown) {
                shown = now;
                std::printf("  sim %9.4f s  wall %8.3f s  display %s\n", sim.t, wall(), shown.c_str());
            }
        }
    }
    double elapsed = wall();
    const int64_t steps = app.steps - stepsAtStart;
    // calibrate the time-stamp counter against the wall clock over the whole run
    double ticksPerSecond = (double) (cycleCount() - startTicks) / elapsed;
    if (!displays.empty())
        shown = readDisplays(displays);

    std::printf("%lld steps, %.4f simulated s in %.3f wall s\n", (long long) steps, sim.t, elapsed);
    std::printf("speed: %.0f steps/s = %.3fx real time\n", steps / elapsed, sim.t / elapsed);
    std::printf("per step: %.2f subiterations, %.1f LU factors (%.1f reused), %.1f LU solves (%.1f skipped), %.2f us\n",
                (double) sim.stats.subIterations / (double) steps, (double) sim.stats.factors / (double) steps,
                (double) sim.stats.factorsReused / (double) steps, (double) sim.stats.solves / (double) steps,
                (double) sim.stats.solvesSkipped / (double) steps, elapsed * 1e6 / (double) steps);
    if (!refEngine) {
        const DenseLU::Counts &c = DenseLU::counts;
        std::printf("LU per step: %.2f sparse full, %.2f sparse replays (%.3f pivot mismatches), %.2f dense\n",
                    (double) c.sparseFull / (double) steps, (double) c.sparseRefactor / (double) steps,
                    (double) c.mismatches / (double) steps, (double) c.dense / (double) steps);
        const int64_t cols = c.columnsSkipped + c.columnsReplayed;
        if (cols > 0)
            std::printf("replays: %.1f%% of columns unchanged and skipped\n",
                        100.0 * (double) c.columnsSkipped / (double) cols);
    }
    if (!displays.empty())
        std::printf("final display: %s\n", shown.c_str());
    if (watchEvery > 0)
        std::printf("watched %zu wires; hash of their currents at every step: %016llx\n", app.watched.size(),
                    (unsigned long long) app.watchHash);

    if (profile) {
        const SimulationManager::Profile &p = sim.profile;
        struct Row {
            const char *name;
            uint64_t ticks;
        } rows[] = {{"startIteration", p.startIteration}, {"restore matrices", p.restore},
                    {"doStep", p.doStep},                 {"lu_factor (+NaN check)", p.factor},
                    {"lu_solve", p.solve},                {"apply solution", p.apply},
                    {"stepFinished", p.stepFinished},     {"wire currents", p.wireCurrents},
                    {"other", p.other}};
        uint64_t sum = 0;
        for (const Row &r : rows)
            sum += r.ticks;
        auto usPerStep = [&](uint64_t ticks) { return ticks / ticksPerSecond * 1e6 / (double) steps; };
        std::printf("\nprofile (counter at %.2f GHz; runCircuit %.1f us/step; phases cover %.1f%% of it):\n",
                    ticksPerSecond / 1e9, usPerStep(p.total), 100.0 * (double) sum / (double) p.total);
        for (const Row &r : rows)
            std::printf("  %-24s %5.1f%%  %8.2f us/step\n", r.name, 100.0 * (double) r.ticks / (double) p.total,
                        usPerStep(r.ticks));
    }
    if (blame) {
        std::vector<std::pair<CircuitElm *, int64_t>> top(blameCounts.begin(), blameCounts.end());
        std::sort(top.begin(), top.end(), [](auto &a, auto &b) { return a.second > b.second; });
        std::printf("\nnot-converged reports per step (%.2f sub-iterations per step):\n",
                    (double) sim.stats.subIterations / (double) steps);
        for (size_t i = 0; i != top.size() && i != 15; i++) {
            CircuitElm *ce = top[i].first;
            int index = (int) (std::find(sim.elmList.begin(), sim.elmList.end(), ce) - sim.elmList.begin());
            std::printf("  %6.3f  #%-4d %-22s at (%d,%d)\n", (double) top[i].second / (double) steps, index,
                        ce->getClassName().c_str(), ce->x, ce->y);
        }
    }
    if (elements) {
        struct ClassTotal {
            std::string name;
            uint64_t ticks = 0;
            int64_t calls = 0;
            int count = 0;
        };
        std::unordered_map<std::string, ClassTotal> byClass;
        uint64_t all = 0;
        for (auto &[ce, t] : elmTicks) {
            ClassTotal &c = byClass[ce->getClassName()];
            c.name = ce->getClassName();
            c.ticks += t.ticks;
            c.calls += t.calls;
            c.count++;
            all += t.ticks;
        }
        std::vector<ClassTotal> rows;
        for (auto &kv : byClass)
            rows.push_back(kv.second);
        std::sort(rows.begin(), rows.end(), [](auto &a, auto &b) { return a.ticks > b.ticks; });
        std::printf("\ndoStep() by element class (counter ticks include ~20 per call of timing overhead):\n");
        std::printf("  %-24s %5s %7s %10s %12s\n", "class", "count", "share", "us/step", "ticks/call");
        for (auto &r : rows)
            std::printf("  %-24s %5d %6.1f%% %10.2f %12.1f\n", r.name.c_str(), r.count,
                        100.0 * (double) r.ticks / (double) all, r.ticks / ticksPerSecond * 1e6 / (double) steps,
                        (double) r.ticks / (double) r.calls);
    }
    return 0;
}
