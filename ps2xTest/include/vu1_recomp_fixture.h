#ifndef VU1_RECOMP_FIXTURE_H
#define VU1_RECOMP_FIXTURE_H

// VR2: synthetic VU1 code images for the generated-vs-interpreter differential
// test. vu1_fixture_gen emits their generated C++ at build time (random
// programs, not game data); ps2x_tests compiles it in and runs every program
// through the generated pairs and through the interpreter.
//
// Image 0 ("mix"): VB1's random mix of FMAC/ACC/CLIP uppers and LSU, VI,
// flag, FSSET/FCSET, DIV/WAITQ and forward-branch lowers.
// Image 1 ("flags"): an FMAC flag write followed, at every distance 0-5, by a
// flag reader (MAC/status/clip compares, FCGET, FSSET/FCSET), a branch that
// lands on a reader, or the program end, with overwriting FMACs, non-flag
// uppers and stalling lowers mixed into the gap.

#include <cstdint>
#include <cstring>
#include <vector>

namespace vu1_fixture
{
    constexpr uint32_t kCodeSize = 0x4000u;
    constexpr uint32_t kImageCount = 2u;
    constexpr uint64_t kImageHash[kImageCount] = {0x5652320000000000ull, 0x5652320000000001ull};
    constexpr uint32_t kUpperNop = 0x000002FFu;
    constexpr uint32_t kLowerNop = 0x8000033Cu;

    struct Program
    {
        uint32_t startPc;
        uint32_t length; // pairs, including the E-bit pair and its delay slot
    };

    inline uint32_t upper(uint8_t op, uint8_t dest, uint8_t ft, uint8_t fs, uint8_t fd)
    {
        return (static_cast<uint32_t>(dest & 0xFu) << 21) | (static_cast<uint32_t>(ft & 0x1Fu) << 16) |
               (static_cast<uint32_t>(fs & 0x1Fu) << 11) | (static_cast<uint32_t>(fd & 0x1Fu) << 6) |
               static_cast<uint32_t>(op & 0x3Fu);
    }

    inline uint32_t upperSpecial(uint8_t op, uint8_t dest, uint8_t ft, uint8_t fs)
    {
        return (static_cast<uint32_t>(dest & 0xFu) << 21) | (static_cast<uint32_t>(ft & 0x1Fu) << 16) |
               (static_cast<uint32_t>(fs & 0x1Fu) << 11) | (static_cast<uint32_t>(op & 0x7Cu) << 4) |
               static_cast<uint32_t>(op & 0x3u) | 0x3Cu;
    }

    inline uint32_t lowerSpecial(uint8_t op, uint8_t is, uint8_t it = 0u, uint8_t id = 0u, uint8_t dest = 0u)
    {
        return (0x40u << 25) | (static_cast<uint32_t>(dest & 0xFu) << 21) | (static_cast<uint32_t>(it & 0x1Fu) << 16) |
               (static_cast<uint32_t>(is & 0x1Fu) << 11) | (static_cast<uint32_t>(id & 0x1Fu) << 6) |
               (static_cast<uint32_t>(op & 0x7Cu) << 4) | static_cast<uint32_t>(op & 0x3u) | 0x3Cu;
    }

    inline uint32_t flagImmediate(uint8_t op, uint8_t vi, uint16_t imm)
    {
        return (static_cast<uint32_t>(op & 0x7Fu) << 25) | (static_cast<uint32_t>((imm >> 11) & 0x1u) << 21) |
               (static_cast<uint32_t>(vi & 0xFu) << 16) | static_cast<uint32_t>(imm & 0x7FFu);
    }

    inline uint32_t flagRegister(uint8_t op, uint8_t it, uint8_t is)
    {
        return (static_cast<uint32_t>(op & 0x7Fu) << 25) | (static_cast<uint32_t>(it & 0xFu) << 16) |
               (static_cast<uint32_t>(is & 0xFu) << 11);
    }

    inline uint32_t lq(uint8_t dest, uint8_t vf, uint8_t vi, int16_t imm)
    {
        return (static_cast<uint32_t>(dest & 0xFu) << 21) | (static_cast<uint32_t>(vf & 0x1Fu) << 16) |
               (static_cast<uint32_t>(vi & 0xFu) << 11) | (static_cast<uint32_t>(imm) & 0x7FFu);
    }

    inline uint32_t sq(uint8_t dest, uint8_t vf, uint8_t vi, int16_t imm)
    {
        return (0x01u << 25) | (static_cast<uint32_t>(dest & 0xFu) << 21) | (static_cast<uint32_t>(vi & 0xFu) << 16) |
               (static_cast<uint32_t>(vf & 0x1Fu) << 11) | (static_cast<uint32_t>(imm) & 0x7FFu);
    }

    inline uint32_t ilw(uint8_t dest, uint8_t it, uint8_t is, int16_t imm)
    {
        return (0x04u << 25) | (static_cast<uint32_t>(dest & 0xFu) << 21) | (static_cast<uint32_t>(it & 0xFu) << 16) |
               (static_cast<uint32_t>(is & 0xFu) << 11) | (static_cast<uint32_t>(imm) & 0x7FFu);
    }

    inline uint32_t iaddiu(uint8_t it, uint8_t is, int16_t imm)
    {
        return (0x08u << 25) | (static_cast<uint32_t>(it & 0xFu) << 16) | (static_cast<uint32_t>(is & 0xFu) << 11) |
               (static_cast<uint32_t>(imm) & 0x7FFu);
    }

    inline uint32_t branch(int16_t imm) { return (0x20u << 25) | (static_cast<uint32_t>(imm) & 0x7FFu); }

    inline uint32_t ibne(uint8_t is, uint8_t it, int16_t imm)
    {
        return (0x29u << 25) | (static_cast<uint32_t>(it & 0xFu) << 16) | (static_cast<uint32_t>(is & 0xFu) << 11) |
               (static_cast<uint32_t>(imm) & 0x7FFu);
    }

    inline uint32_t div(uint8_t fs, uint8_t ft, uint8_t fsf, uint8_t ftf)
    {
        return lowerSpecial(0x38u, fs, ft, 0u, static_cast<uint8_t>(((ftf & 0x3u) << 2) | (fsf & 0x3u)));
    }

    inline void writePair(uint8_t *code, uint32_t pc, uint32_t lower, uint32_t upperWord)
    {
        std::memcpy(code + pc, &lower, sizeof(lower));
        std::memcpy(code + pc + 4u, &upperWord, sizeof(upperWord));
    }

    // Same LCG as the VB1 differential test.
    struct Rng
    {
        uint64_t seed;
        uint32_t operator()(uint32_t n)
        {
            seed = seed * 6364136223846793005ull + 1442695040888963407ull;
            return static_cast<uint32_t>((seed >> 33) % n);
        }
    };

    inline uint32_t randomUpper(Rng &rnd)
    {
        const uint8_t dest = static_cast<uint8_t>(1u + rnd(15u));
        const uint8_t ft = static_cast<uint8_t>(1u + rnd(8u));
        const uint8_t fs = static_cast<uint8_t>(1u + rnd(8u));
        const uint8_t fd = static_cast<uint8_t>(1u + rnd(8u));
        switch (rnd(6u))
        {
        case 0: return kUpperNop;
        // OPMSUB/OPMULA (0x2E) only as .xyz, as real code writes them.
        case 1:
        {
            const uint8_t op = static_cast<uint8_t>(rnd(0x30u));
            return upper(op, op == 0x2Eu ? uint8_t{0xE} : dest, ft, fs, fd);
        }
        case 2:
        {
            const uint8_t op = static_cast<uint8_t>(0x28u + rnd(8u));
            return upper(op, op == 0x2Eu ? uint8_t{0xE} : dest, ft, fs, fd);
        }
        case 3:
        {
            static const uint8_t kAcc[] = {0x00, 0x03, 0x08, 0x0B, 0x18, 0x1A, 0x1C, 0x1E, 0x20, 0x21, 0x28, 0x29, 0x2A, 0x2D, 0x2E};
            const uint8_t op = kAcc[rnd(sizeof(kAcc))];
            return upperSpecial(op, op == 0x2Eu ? uint8_t{0xE} : dest, ft, fs);
        }
        case 4: return upperSpecial(0x1Fu, 0xEu, ft, fs); // CLIP
        default: return upperSpecial(static_cast<uint8_t>(0x10u + rnd(8u)), dest, ft, fs);
        }
    }

    // An upper that writes MAC/status (ADD/SUB/MUL/MADD/MSUB, broadcast or
    // full, any non-empty dest).
    inline uint32_t flagWriterUpper(Rng &rnd)
    {
        static const uint8_t kOps[] = {0x00, 0x01, 0x04, 0x07, 0x08, 0x0B, 0x0C, 0x18, 0x1B, 0x28, 0x29, 0x2A, 0x2C, 0x2D};
        return upper(kOps[rnd(sizeof(kOps))], static_cast<uint8_t>(1u + rnd(15u)), static_cast<uint8_t>(1u + rnd(8u)),
                     static_cast<uint8_t>(1u + rnd(8u)), static_cast<uint8_t>(1u + rnd(8u)));
    }

    // Flag readers/setters (lower): FCEQ/FCSET/FCAND/FCOR, FSEQ/FSSET/FSAND/
    // FSOR, FMEQ/FMAND/FMOR, FCGET.
    inline uint32_t flagOpLower(Rng &rnd, uint32_t which)
    {
        const uint8_t vi = static_cast<uint8_t>(1u + rnd(4u));
        switch (which % 11u)
        {
        case 0: return (0x10u << 25) | rnd(0x1000000u);
        case 1: return (0x11u << 25) | rnd(0x1000000u);
        case 2: return (0x12u << 25) | rnd(0x1000000u);
        case 3: return (0x13u << 25) | rnd(0x1000000u);
        case 4: return flagImmediate(0x14u, vi, static_cast<uint16_t>(rnd(0x1000u)));
        case 5: return flagImmediate(0x15u, 0u, static_cast<uint16_t>(rnd(0x1000u)));
        case 6: return flagImmediate(0x16u, vi, static_cast<uint16_t>(rnd(0x1000u)));
        case 7: return flagImmediate(0x17u, vi, static_cast<uint16_t>(rnd(0x1000u)));
        case 8: return flagRegister(0x18u, vi, static_cast<uint8_t>(rnd(5u)));
        case 9: return flagRegister(0x1Au + rnd(2u), vi, static_cast<uint8_t>(rnd(5u)));
        default: return flagImmediate(0x1Cu, vi, 0u);
        }
    }

    inline uint32_t randomLower(Rng &rnd, uint32_t pair, uint32_t length)
    {
        const uint8_t dest = static_cast<uint8_t>(1u + rnd(15u));
        const uint8_t vf = static_cast<uint8_t>(1u + rnd(8u));
        const uint8_t vi = static_cast<uint8_t>(1u + rnd(4u));
        switch (rnd(16u))
        {
        case 0: return lq(dest, vf, 0u, static_cast<int16_t>(rnd(64u)));
        case 1: return sq(dest, vf, 0u, static_cast<int16_t>(rnd(64u)));
        case 2: return lowerSpecial(0x34u, vi, vf, 0u, dest); // LQI
        case 3: return lowerSpecial(0x35u, vf, vi, 0u, dest); // SQI
        case 4: return ilw(0x8u, vi, 0u, static_cast<int16_t>(rnd(64u)));
        case 5: return iaddiu(vi, static_cast<uint8_t>(rnd(5u)), static_cast<int16_t>(rnd(8u)));
        case 6: return flagRegister(static_cast<uint8_t>(0x18u + 2u * rnd(2u)), vi, static_cast<uint8_t>(rnd(5u)));
        case 7: return flagImmediate(static_cast<uint8_t>(0x14u + 2u * rnd(2u)), vi, static_cast<uint16_t>(rnd(0x1000u)));
        case 8: return flagImmediate(0x15u, 0u, static_cast<uint16_t>(rnd(0x1000u))); // FSSET
        case 9: return (0x12u << 25) | rnd(0x1000000u);                                // FCAND vi1
        case 10: return (0x11u << 25) | rnd(0x1000000u);                               // FCSET
        case 11: return div(static_cast<uint8_t>(1u + rnd(8u)), static_cast<uint8_t>(1u + rnd(8u)),
                            static_cast<uint8_t>(rnd(4u)), static_cast<uint8_t>(rnd(4u)));
        case 12: return lowerSpecial(0x3Bu, 0u); // WAITQ
        case 13:
        {
            if (pair + 3u >= length)
                return kLowerNop;
            const int16_t forward = static_cast<int16_t>(1 + rnd(3u));
            return rnd(2u) != 0u ? ibne(vi, static_cast<uint8_t>(rnd(5u)), forward) : branch(forward);
        }
        case 14: return flagImmediate(0x1Cu, vi, 0u); // FCGET
        default: return kLowerNop;
        }
    }

    // A lower that does not touch flags: NOP, LSU, VI add, DIV/WAITQ stalls.
    inline uint32_t quietLower(Rng &rnd)
    {
        const uint8_t dest = static_cast<uint8_t>(1u + rnd(15u));
        const uint8_t vf = static_cast<uint8_t>(1u + rnd(8u));
        switch (rnd(7u))
        {
        case 0: return lq(dest, vf, 0u, static_cast<int16_t>(rnd(64u)));
        case 1: return sq(dest, vf, 0u, static_cast<int16_t>(rnd(64u)));
        case 2: return iaddiu(static_cast<uint8_t>(1u + rnd(4u)), 0u, static_cast<int16_t>(rnd(8u)));
        case 3: return div(static_cast<uint8_t>(1u + rnd(8u)), static_cast<uint8_t>(1u + rnd(8u)),
                           static_cast<uint8_t>(rnd(4u)), static_cast<uint8_t>(rnd(4u)));
        case 4: return lowerSpecial(0x3Bu, 0u); // WAITQ
        default: return kLowerNop;
        }
    }

    // Upper in the gap after the flag write: NOP, a non-flag op, or an
    // FMAC/CLIP that overwrites the flags.
    inline uint32_t gapUpper(Rng &rnd)
    {
        switch (rnd(5u))
        {
        case 0: return kUpperNop;
        case 1: return upperSpecial(static_cast<uint8_t>(0x10u + rnd(8u)), static_cast<uint8_t>(1u + rnd(15u)),
                                    static_cast<uint8_t>(1u + rnd(8u)), static_cast<uint8_t>(1u + rnd(8u)));
        case 2: return upperSpecial(0x1Fu, 0xEu, static_cast<uint8_t>(1u + rnd(8u)), static_cast<uint8_t>(1u + rnd(8u)));
        default: return flagWriterUpper(rnd);
        }
    }

    // Writes one E-bit pair and its delay slot at pc; returns the pairs used.
    inline uint32_t writeEnd(uint8_t *code, uint32_t pc)
    {
        writePair(code, pc, kLowerNop, kUpperNop | 0x40000000u);
        writePair(code, pc + 8u, kLowerNop, kUpperNop);
        return 2u;
    }

    inline std::vector<Program> buildImage(uint32_t image, uint8_t *code)
    {
        std::memset(code, 0, kCodeSize);
        std::vector<Program> programs;
        const uint32_t maxPairs = kCodeSize / 8u;
        uint32_t next = 0u;
        if (image == 0u)
        {
            Rng rnd{0x9E3779B97F4A7C15ull};
            for (;;)
            {
                const uint32_t length = 12u + rnd(28u);
                if (next + length + 2u > maxPairs)
                    break;
                const uint32_t start = next;
                for (uint32_t pair = 0; pair < length; ++pair)
                {
                    const uint32_t up = randomUpper(rnd);
                    writePair(code, (start + pair) * 8u, randomLower(rnd, pair, length), up);
                }
                const uint32_t total = length + writeEnd(code, (start + length) * 8u);
                programs.push_back({start * 8u, total});
                next = start + total;
            }
            return programs;
        }

        // Image 1: for every distance 0..5 and every reader kind, a few
        // variants. Reader kinds 0..10 are flagOpLower; 11 = the program ends
        // at that distance; 12 = a branch at that distance lands on a reader.
        Rng rnd{0xD1B54A32D192ED03ull};
        for (uint32_t variant = 0; variant < 2u; ++variant)
        {
            for (uint32_t distance = 0; distance <= 5u; ++distance)
            {
                for (uint32_t kind = 0; kind < 13u; ++kind)
                {
                    const uint32_t lead = rnd(3u);
                    const uint32_t tail = rnd(3u);
                    const uint32_t length = lead + 1u + distance + 3u + tail;
                    if (next + length + 2u > maxPairs)
                        return programs;
                    const uint32_t start = next;
                    uint32_t pair = 0u;
                    for (; pair < lead; ++pair)
                        writePair(code, (start + pair) * 8u, quietLower(rnd), randomUpper(rnd));
                    const uint32_t writer = pair;
                    for (; pair < length; ++pair)
                    {
                        const uint32_t at = pair - writer; // distance from the flag write
                        uint32_t up = at == 0u ? flagWriterUpper(rnd) : gapUpper(rnd);
                        uint32_t lo = quietLower(rnd);
                        if (at == distance)
                        {
                            if (kind <= 10u)
                                lo = flagOpLower(rnd, kind);
                            else if (kind == 11u)
                                up |= 0x40000000u; // E bit: the program ends one pair later
                            else
                                lo = branch(1); // delay slot, then skip one pair to a reader
                        }
                        else if (kind == 12u && at == distance + 2u)
                            lo = flagOpLower(rnd, rnd(11u));
                        writePair(code, (start + pair) * 8u, lo, up);
                    }
                    const uint32_t total = length + writeEnd(code, (start + length) * 8u);
                    programs.push_back({start * 8u, total});
                    next = start + total;
                }
            }
        }
        return programs;
    }
}

#endif
