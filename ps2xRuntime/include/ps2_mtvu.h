#ifndef PS2_MTVU_H
#define PS2_MTVU_H

// MT1: ownership hooks for the VIF1 -> VU1 -> GIF -> GS-frontend "unit"
// (local/research/MT1/REPORT.md). A future threaded build runs unit jobs
// (the GIF + VIF1 part of a DMA kick, VIF1 FIFO writes) on a worker; the EE
// thread must call sync() before it touches unit-owned state, and the
// objects the unit owns call touch() so a missed sync shows up.
//
// PS2X_MTVU unset/0 (default): every hook is one branch on a cached flag.
// PS2X_MTVU=census (stage 2', still synchronous, no behaviour change):
//   - counts sync-point hits per reason, and the first sync after each job;
//   - reports a touch() of unit-owned state on the EE thread after a job
//     with no sync in between (VIOLATION lines: a threaded build would race);
//   - with PS2X_MTVU_CENSUS_OUT=<file>, writes one event per line for the
//     offline two-timeline model (tau = EE host ns with unit work removed):
//       J <tau> <unit_ns> <d|f>   job (d = DMA kick, f = VIF1 FIFO write)
//       S <tau> <reason> <detail> every sync hit (guest pc / address in hex;
//                                 repeats with no job in between collapse)
//       V <tau> <tick>            VBlankStart (always a sync)
//     plus a summary line on stderr every 300 vsyncs.

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <vector>

namespace ps2_mtvu
{
    enum class Reason : uint8_t
    {
        VBlank,
        Vu1Mem,
        GsPrivRead,
        GsPrivReadMasked, // CSR load whose next insn masks away the unit's bits
        GsPrivWrite,
        GsPrivSync,
        Vif1Reg,
        Cmsar1,
        GsHle,
        NativeGif,
        SaveState,
        DtFallback,
        Count
    };

    enum class Site : uint8_t
    {
        GsProcess,
        GsNative,
        GsWriteReg,
        GsPriv,
        GsDrain,
        GsClear,
        GsReadback,
        GsReset,
        ArbSubmit,
        ArbDrain,
        Path3Fifo,
        Vu1Mem,
        Count
    };

    inline const char *reasonName(Reason r)
    {
        static const char *const names[] = {"vblank", "vu1mem", "gsprivread", "gsprivread-masked", "gsprivwrite", "gsprivsync",
                                            "vif1reg", "cmsar1", "gshle", "nativegif", "savestate", "dtfallback"};
        return names[static_cast<unsigned>(r)];
    }

    inline const char *siteName(Site s)
    {
        static const char *const names[] = {"gs-process", "gs-native", "gs-writereg", "gs-priv", "gs-drain",
                                            "gs-clear", "gs-readback", "gs-reset", "arb-submit", "arb-drain",
                                            "path3-fifo", "vu1-mem"};
        return names[static_cast<unsigned>(s)];
    }

    namespace detail
    {
        inline int readMode()
        {
            const char *e = std::getenv("PS2X_MTVU");
            if (e && std::strcmp(e, "census") == 0)
                return 2;
            return 0;
        }

        inline int mode()
        {
            static const int m = readMode();
            return m;
        }

        inline uint64_t nowNs()
        {
            return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                             std::chrono::steady_clock::now().time_since_epoch())
                                             .count());
        }

        // Unit depth > 0: this thread is doing unit work (hooks are no-ops).
        inline thread_local int t_unitDepth = 0;
        // Set on the thread that runs VBlankStart / submits jobs (the EE
        // executor). touch() only checks there: the GS worker's own calls and
        // host presentation are downstream consumers, as today.
        inline thread_local bool t_isEe = false;

        struct Census
        {
            bool dirty = false;       // a job ran and no sync followed yet
            uint64_t unitNs = 0;      // total unit work (removed from tau)
            uint64_t jobs = 0;
            uint64_t fifoJobs = 0;
            uint64_t snapshotBytes = 0;
            uint64_t snapshotNs = 0;
            uint64_t violationsTotal = 0;
            std::array<uint64_t, static_cast<size_t>(Reason::Count)> hits{};
            std::array<uint64_t, static_cast<size_t>(Reason::Count)> first{};
            std::array<uint64_t, static_cast<size_t>(Site::Count)> violations{};
            uint64_t tick = 0;
            Reason lastReason = Reason::Count;
            uint32_t lastDetail = 0;
            FILE *out = nullptr;
            bool outTried = false;
            std::function<bool()> dtFallback;
            std::vector<uint8_t> scratch;
        };

        inline Census &census()
        {
            static Census c;
            return c;
        }

        inline uint64_t tau()
        {
            return nowNs() - census().unitNs;
        }

        inline FILE *out()
        {
            Census &c = census();
            if (!c.outTried)
            {
                c.outTried = true;
                if (const char *path = std::getenv("PS2X_MTVU_CENSUS_OUT"))
                {
                    c.out = std::fopen(path, "w");
                    if (c.out)
                        std::setvbuf(c.out, nullptr, _IOFBF, 1u << 20);
                }
            }
            return c.out;
        }

        inline void syncSlow(Reason r, uint32_t detail)
        {
            Census &c = census();
            ++c.hits[static_cast<size_t>(r)];
            if (c.dirty)
            {
                c.dirty = false;
                ++c.first[static_cast<size_t>(r)];
            }
            // Spin loops re-hit the same site with no job between: log once.
            if (r == c.lastReason && detail == c.lastDetail)
                return;
            c.lastReason = r;
            c.lastDetail = detail;
            if (FILE *f = out())
                std::fprintf(f, "S %llu %s %x\n", static_cast<unsigned long long>(tau()), reasonName(r), detail);
        }

        inline void touchSlow(Site s)
        {
            Census &c = census();
            if (!c.dirty)
                return;
            ++c.violations[static_cast<size_t>(s)];
            if (c.violationsTotal++ < 16u)
                std::fprintf(stderr, "[mtvu] VIOLATION site=%s tick=%llu (unit state touched after a job, no sync)\n",
                             siteName(s), static_cast<unsigned long long>(c.tick));
        }

        inline void summary()
        {
            Census &c = census();
            std::fprintf(stderr, "[mtvu] census tick=%llu jobs=%llu fifo=%llu unit_ms=%.1f snap_bytes=%llu snap_ms=%.2f violations=%llu hits:",
                         static_cast<unsigned long long>(c.tick), static_cast<unsigned long long>(c.jobs),
                         static_cast<unsigned long long>(c.fifoJobs), c.unitNs / 1e6,
                         static_cast<unsigned long long>(c.snapshotBytes), c.snapshotNs / 1e6,
                         static_cast<unsigned long long>(c.violationsTotal));
            for (size_t i = 0; i < c.hits.size(); ++i)
                if (c.hits[i] != 0u)
                    std::fprintf(stderr, " %s=%llu/%llu", reasonName(static_cast<Reason>(i)),
                                 static_cast<unsigned long long>(c.hits[i]),
                                 static_cast<unsigned long long>(c.first[i]));
            for (size_t i = 0; i < c.violations.size(); ++i)
                if (c.violations[i] != 0u)
                    std::fprintf(stderr, " V:%s=%llu", siteName(static_cast<Site>(i)),
                                 static_cast<unsigned long long>(c.violations[i]));
            std::fprintf(stderr, "\n");
        }
    }

    inline bool active()
    {
        return detail::mode() != 0;
    }

    // EE side, before touching unit-owned state.
    inline void sync(Reason r, uint32_t detail = 0u)
    {
        if (!active() || detail::t_unitDepth != 0)
            return;
        detail::syncSlow(r, detail);
    }

    // Census classification of a guest load from the GS priv range at pc.
    // The unit writes only CSR bits 0-1 (SIGNAL/FINISH) and SIGLBLID
    // (gs_frontend.cpp); CSR FIFO is HLE'd constant and VSINT/FIELD come from
    // the EE's VBlank store. A CSR load whose next instruction is
    // `andi rt, rt, imm` with imm clear of the unit bits leaves architectural
    // state that cannot depend on the unit (-> GsPrivReadMasked).
    inline Reason privReadReason(const uint8_t *rdram, uint32_t pc, uint32_t vaddr, uint32_t bytes)
    {
        const uint32_t phys = vaddr & 0x1FFFFFFFu;
        if ((phys & ~7u) != 0x12001000u || !rdram)
            return Reason::GsPrivRead;
        const uint32_t pcPhys = pc & 0x01FFFFFCu; // 32 MB RDRAM
        if (pcPhys + 8u > 0x02000000u)
            return Reason::GsPrivRead;
        uint32_t load = 0, next = 0;
        std::memcpy(&load, rdram + pcPhys, 4);
        std::memcpy(&next, rdram + pcPhys + 4u, 4);
        const uint32_t rt = (load >> 16) & 31u;
        uint64_t mask = bytes >= 8u ? ~0ull : ((1ull << (bytes * 8u)) - 1u);
        if ((next >> 26) == 0x0Cu && ((next >> 21) & 31u) == rt && ((next >> 16) & 31u) == rt && rt != 0u)
            mask &= static_cast<uint64_t>(next & 0xFFFFu);
        mask <<= (phys & 7u) * 8u;
        return (mask & 0x3ull) == 0u ? Reason::GsPrivReadMasked : Reason::GsPrivRead;
    }

    // Inside a unit-owned object: must be unit work or follow a sync.
    inline void touch(Site s)
    {
        if (!active() || detail::t_unitDepth != 0 || !detail::t_isEe)
            return;
        detail::touchSlow(s);
    }

    // PS2Runtime installs the D/T rule (REPORT.md map #10): true = the
    // current guest context has VU1 D/T stops enabled or pending, so VIF1
    // work runs inline on the EE after a sync instead of as a job.
    inline void setDtFallbackFn(std::function<bool()> fn)
    {
        detail::census().dtFallback = std::move(fn);
    }

    inline bool dtFallback()
    {
        const auto &fn = detail::census().dtFallback;
        return fn && fn();
    }

    // Census only: time the copy a threaded job would take of `bytes` of
    // non-chain DMA source (EE-side cost, stays in tau).
    inline void noteSnapshot(const uint8_t *src, size_t bytes)
    {
        if (!active() || bytes == 0u || !src)
            return;
        detail::Census &c = detail::census();
        const uint64_t t0 = detail::nowNs();
        if (c.scratch.size() < bytes)
            c.scratch.resize(bytes);
        std::memcpy(c.scratch.data(), src, bytes);
        c.snapshotNs += detail::nowNs() - t0;
        c.snapshotBytes += bytes;
    }

    // One unit job (possibly in segments: pause() around EE-only work).
    class JobScope
    {
    public:
        JobScope(bool on, char kind)
            : m_on(on && active() && detail::t_unitDepth == 0), m_kind(kind)
        {
            if (!m_on)
                return;
            detail::t_isEe = true;
            m_tau = detail::tau();
            resume();
        }
        ~JobScope()
        {
            if (!m_on)
                return;
            pause();
            detail::Census &c = detail::census();
            c.unitNs += m_ns;
            ++c.jobs;
            if (m_kind == 'f')
                ++c.fifoJobs;
            c.dirty = true;
            if (FILE *f = detail::out())
                std::fprintf(f, "J %llu %llu %c\n", static_cast<unsigned long long>(m_tau),
                             static_cast<unsigned long long>(m_ns), m_kind);
        }
        void pause()
        {
            if (!m_on || !m_running)
                return;
            m_ns += detail::nowNs() - m_start;
            --detail::t_unitDepth;
            m_running = false;
        }
        void resume()
        {
            if (!m_on || m_running)
                return;
            ++detail::t_unitDepth;
            m_start = detail::nowNs();
            m_running = true;
        }
        JobScope(const JobScope &) = delete;
        JobScope &operator=(const JobScope &) = delete;

    private:
        bool m_on = false;
        bool m_running = false;
        char m_kind = 'd';
        uint64_t m_tau = 0;
        uint64_t m_start = 0;
        uint64_t m_ns = 0;
    };

    // EeScheduler VBlankStart (after the pacer sleep): always a sync point.
    inline void vblank(uint64_t tick)
    {
        if (!active())
            return;
        detail::t_isEe = true;
        detail::Census &c = detail::census();
        c.tick = tick;
        sync(Reason::VBlank);
        if (FILE *f = detail::out())
        {
            std::fprintf(f, "V %llu %llu\n", static_cast<unsigned long long>(detail::tau()),
                         static_cast<unsigned long long>(tick));
            if ((tick % 60u) == 0u)
                std::fflush(f);
        }
        if ((tick % 300u) == 0u)
            detail::summary();
    }
}

#endif
