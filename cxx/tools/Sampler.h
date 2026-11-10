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

// A sampling profiler for simbench (Windows only; elsewhere it does nothing).
//
// A second thread wakes about once a millisecond, suspends the thread being profiled, reads its
// instruction pointer (RIP) and lets it go on.  After the run, each sampled address is looked up
// in the program's PDB debug file (build with /Zi, see build-prof in the notes) to find the
// function and source line it belongs to.  Most time is spent in a few places, so a few
// thousand samples show where.
//
// Inlining: the optimizer pastes small functions into their callers, so the sampled address
// belongs to the caller as far as a symbol lookup without inline information can tell.  With
// the inline-aware lookups (SymFromInlineContext) the innermost function that was pasted in is
// found too, and that is what "self" means below; "outer" is the real function the code sits in.
//
// Caveats: sampling is statistical (a function with 1% of the samples is real, one with 0.1%
// might be noise); suspending a thread is not free, so run times differ a little from an
// unprofiled run; the sampler only sees the thread that created it.

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#ifdef _WIN32

// NOGDI: windows.h's graphics API declares a function called Rectangle(), which clashes with
// the simulator's own Rectangle struct, and the sampler needs nothing from it.
// WIN32_LEAN_AND_MEAN leaves out mmsystem.h (timeBeginPeriod), which is included by hand.
#ifndef NOGDI
#define NOGDI
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
// these two need windows.h first
#include <mmsystem.h>
#include <dbghelp.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <thread>

class Sampler {
public:
    // profiles the calling thread from start() until stop()
    Sampler() {
        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &target, 0, FALSE,
                        DUPLICATE_SAME_ACCESS);
        samples.reserve(1 << 20);  // never grown while the target is suspended: a suspended thread
                                   // may hold the heap lock, and growing would deadlock
    }
    ~Sampler() {
        stop();
        CloseHandle(target);
    }

    void start() {
        timeBeginPeriod(1);  // a 1 ms Sleep() instead of the default 15.6 ms
        running = true;
        worker = std::thread([this] {
            CONTEXT ctx;
            while (running) {
                ctx.ContextFlags = CONTEXT_CONTROL;
                if (SuspendThread(target) != (DWORD) -1) {
                    bool ok = GetThreadContext(target, &ctx) != 0;
                    ResumeThread(target);
                    if (ok && samples.size() < samples.capacity())
                        samples.push_back(ctx.Rip);
                }
                Sleep(1);
            }
        });
    }

    void stop() {
        if (!running)
            return;
        running = false;
        worker.join();
        timeEndPeriod(1);
    }

    // print the top functions and source lines
    void report(int top = 25) {
        stop();
        if (samples.empty()) {
            std::printf("\nsampler: no samples\n");
            return;
        }
        HANDLE proc = GetCurrentProcess();
        SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_INCLUDE_32BIT_MODULES);
        SymInitialize(proc, nullptr, TRUE);

        struct Info {
            std::string self, outer, line;
        };
        std::map<uint64_t, Info> resolved;
        std::map<std::string, int64_t> bySelf, byOuter, byLine;

        alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 512];
        auto symbolName = [&](uint64_t addr, DWORD inlineContext, bool useInline) -> std::string {
            SYMBOL_INFO *sym = reinterpret_cast<SYMBOL_INFO *>(buf);
            std::memset(sym, 0, sizeof(SYMBOL_INFO));
            sym->SizeOfStruct = sizeof(SYMBOL_INFO);
            sym->MaxNameLen = 500;
            DWORD64 disp = 0;
            BOOL ok = useInline ? SymFromInlineContext(proc, addr, inlineContext, &disp, sym)
                                : SymFromAddr(proc, addr, &disp, sym);
            return ok ? std::string(sym->Name) : std::string("?");
        };

        for (uint64_t addr : samples) {
            auto it = resolved.find(addr);
            if (it == resolved.end()) {
                Info info;
                info.outer = symbolName(addr, 0, false);
                info.self = info.outer;
                info.line = "?";
                // innermost inlined function at this address, if the PDB knows of one
                DWORD inlineCount = SymAddrIncludeInlineTrace(proc, addr);
                DWORD inlineContext = 0;
                if (inlineCount > 0) {
                    DWORD ctx = 0, index = 0;
                    if (SymQueryInlineTrace(proc, addr, INLINE_FRAME_CONTEXT_INIT, addr, addr, &ctx, &index)) {
                        info.self = symbolName(addr, ctx, true);
                        inlineContext = ctx;
                    }
                }
                IMAGEHLP_LINE64 line;
                std::memset(&line, 0, sizeof line);
                line.SizeOfStruct = sizeof line;
                DWORD lineDisp = 0;
                BOOL haveLine = inlineCount > 0 && inlineContext != 0
                                    ? SymGetLineFromInlineContext(proc, addr, inlineContext, 0, &lineDisp, &line)
                                    : SymGetLineFromAddr64(proc, addr, &lineDisp, &line);
                if (haveLine) {
                    std::string file = line.FileName ? line.FileName : "?";
                    size_t slash = file.find_last_of("\\/");
                    if (slash != std::string::npos)
                        file = file.substr(slash + 1);
                    info.line = file + ":" + std::to_string(line.LineNumber);
                }
                it = resolved.emplace(addr, std::move(info)).first;
            }
            bySelf[it->second.self]++;
            byOuter[it->second.outer]++;
            byLine[it->second.line]++;
        }

        const double total = (double) samples.size();
        auto print = [&](const char *title, std::map<std::string, int64_t> &m) {
            std::vector<std::pair<std::string, int64_t>> rows(m.begin(), m.end());
            std::sort(rows.begin(), rows.end(), [](auto &a, auto &b) { return a.second > b.second; });
            std::printf("\n%s (%zu samples)\n", title, samples.size());
            for (int i = 0; i < top && i < (int) rows.size(); i++) {
                std::string name = rows[i].first;
                if (name.size() > 96)
                    name = name.substr(0, 93) + "...";
                std::printf("  %5.1f%%  %s\n", 100.0 * (double) rows[i].second / total, name.c_str());
            }
        };
        print("hot functions, innermost inlined function (self)", bySelf);
        print("hot functions, the function the code sits in (outer)", byOuter);
        print("hot source lines", byLine);
        SymCleanup(proc);
    }

private:
    HANDLE target = nullptr;
    std::thread worker;
    std::atomic<bool> running{false};
    std::vector<uint64_t> samples;
};

#else  // not Windows: no-op

class Sampler {
public:
    void start() {}
    void stop() {}
    void report(int = 25) { std::printf("\nsampler: only available on Windows\n"); }
};

#endif
