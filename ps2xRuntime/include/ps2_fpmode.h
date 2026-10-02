// E53: host floating-point mode for guest EE code.
//
// PCSX2 runs EE FPU and VU0 math with round-toward-zero plus
// denormals-are-zero / flush-to-zero (Pcsx2Config.cpp,
// DEFAULT_FPU_FP_CONTROL_REGISTER / DEFAULT_VU_FP_CONTROL_REGISTER). The
// generated code does its float math with plain host float ops, so the EE
// executor thread sets the same host mode once and host-side subsystems that
// run on that thread (the software GS) switch back to IEEE for their own work.
//
// PS2X_EE_FPMODE=ps2 (default) | ieee selects the EE mode (read once).
#ifndef PS2_FPMODE_H
#define PS2_FPMODE_H

#include <cfenv>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include <xmmintrin.h>
#define PS2X_FPMODE_X86 1
#endif

namespace ps2_fpmode
{
    // Raw host control word: AArch64 FPCR or x86 MXCSR. Other hosts fall
    // back to the C rounding mode only (no flush-to-zero).
    inline uint64_t readControl()
    {
#if defined(__aarch64__) && (defined(__GNUC__) || defined(__clang__))
        uint64_t v;
        __asm__ __volatile__("mrs %0, fpcr" : "=r"(v));
        return v;
#elif defined(PS2X_FPMODE_X86)
        return _mm_getcsr();
#else
        return static_cast<uint64_t>(std::fegetround());
#endif
    }

    inline void writeControl(uint64_t v)
    {
#if defined(__aarch64__) && (defined(__GNUC__) || defined(__clang__))
        __asm__ __volatile__("msr fpcr, %0" : : "r"(v));
#elif defined(PS2X_FPMODE_X86)
        _mm_setcsr(static_cast<unsigned int>(v));
#else
        std::fesetround(static_cast<int>(v));
#endif
    }

    // Control word for the PS2 mode (round toward zero + FZ/DAZ) derived
    // from a host word.
    inline uint64_t ps2Control(uint64_t base)
    {
#if defined(__aarch64__) && (defined(__GNUC__) || defined(__clang__))
        // RMode bits 23:22 = 0b11 (RZ), FZ bit 24.
        return (base & ~(uint64_t{3} << 22)) | (uint64_t{3} << 22) | (uint64_t{1} << 24);
#elif defined(PS2X_FPMODE_X86)
        // RC bits 14:13 = 0b11 (chop), FTZ bit 15, DAZ bit 6.
        return (base & ~uint64_t{0x6000}) | 0x6000u | 0x8000u | 0x0040u;
#else
        (void)base;
        return static_cast<uint64_t>(FE_TOWARDZERO);
#endif
    }

    // Control word for plain IEEE (round to nearest, no flush) from a host word.
    inline uint64_t ieeeControl(uint64_t base)
    {
#if defined(__aarch64__) && (defined(__GNUC__) || defined(__clang__))
        return base & ~((uint64_t{3} << 22) | (uint64_t{1} << 24));
#elif defined(PS2X_FPMODE_X86)
        return base & ~uint64_t{0x6000 | 0x8000 | 0x0040};
#else
        (void)base;
        return static_cast<uint64_t>(FE_TONEAREST);
#endif
    }

    // True unless PS2X_EE_FPMODE=ieee. Read once per process.
    inline bool eeUsesPs2Mode()
    {
        static const bool ps2 = []()
        {
            const char *v = std::getenv("PS2X_EE_FPMODE");
            const bool ieee = v && std::strcmp(v, "ieee") == 0;
            std::fprintf(stderr, "[E53] PS2X_EE_FPMODE=%s (EE thread: %s)\n",
                         ieee ? "ieee" : "ps2",
                         ieee ? "host IEEE" : "round-toward-zero + flush-to-zero");
            return !ieee;
        }();
        return ps2;
    }

    // Sets a control word for a scope and restores the previous one. Skips
    // the write when the word already matches.
    class ScopedControl
    {
    public:
        explicit ScopedControl(uint64_t want) : m_saved(readControl())
        {
            m_changed = want != m_saved;
            if (m_changed)
                writeControl(want);
        }
        ~ScopedControl()
        {
            if (m_changed)
                writeControl(m_saved);
        }
        ScopedControl(const ScopedControl &) = delete;
        ScopedControl &operator=(const ScopedControl &) = delete;

    private:
        uint64_t m_saved;
        bool m_changed = false;
    };

    // EE executor scope: PS2 mode unless PS2X_EE_FPMODE=ieee.
    class ScopedEeMode : public ScopedControl
    {
    public:
        ScopedEeMode() : ScopedControl(eeUsesPs2Mode() ? ps2Control(readControl()) : readControl()) {}
    };

    // Forced PS2 mode (unit tests of the generated code's semantics).
    class ScopedPs2Mode : public ScopedControl
    {
    public:
        ScopedPs2Mode() : ScopedControl(ps2Control(readControl())) {}
    };

    // Host subsystem scope (software GS, …) on the EE thread: plain IEEE.
    class ScopedHostMode : public ScopedControl
    {
    public:
        ScopedHostMode() : ScopedControl(ieeeControl(readControl())) {}
    };
}

#endif // PS2_FPMODE_H
