#ifndef VU1_RECOMP_FIXTURE_H
#define VU1_RECOMP_FIXTURE_H

// VR2: synthetic VU1 code images for the generated-vs-interpreter differential
// test. vu1_fixture_gen emits their generated C++ at build time (random
// programs, not game data); ps2x_tests compiles it in and runs every program
// through the generated pairs and through the interpreter.
//
// Image 0 ("mix"): VB1's random mix of FMAC/ACC/CLIP uppers and LSU, VI,
// flag, FSSET/FCSET, DIV/WAITQ and forward-branch lowers.
// Image 2 ("pipes", VR2 stage 4): the ops the block fast path admits beyond
// that mix, with pipelines active at block entries: EFU ops, WAITP, MFP, DIV,
// WAITQ and Q/I-reading uppers, I-bit pairs, JR/JALR to forward targets, and
// XGKICK of small GIF packets (with stores into them while PATH1 runs), all
// between forward branches, so blocks start with P/Q/PATH1 in flight and are
// sometimes reached as delay slots.
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
    constexpr uint32_t kImageCount = 3u;
    constexpr uint64_t kImageHash[kImageCount] = {0x5652320000000000ull, 0x5652320000000001ull,
                                                  0x5652320000000002ull};
    // Image 2: XGKICK sources. Qword kTagQword + 4k (k < kTagCount) holds a
    // one- or two-qword IMAGE GIF packet (the test writes them into VU data).
    constexpr uint32_t kTagQword = 128u;
    constexpr uint32_t kTagCount = 8u;
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

    // VR3: codeSize/vu0/salt/skip make the VU0 variants (vu0Image below);
    // the VU1 images use the defaults and are unchanged. vu0: image 2 leaves
    // out the ops VU0 reserves (EFU, WAITP, MFP, XGKICK); salt changes the
    // RNG seed; skip (image 1) drops the first skip programs of the list.
    inline std::vector<Program> buildImage(uint32_t image, uint8_t *code, uint32_t codeSize = kCodeSize,
                                           bool vu0 = false, uint64_t salt = 0u, uint32_t skip = 0u)
    {
        std::memset(code, 0, codeSize);
        std::vector<Program> programs;
        const uint32_t maxPairs = codeSize / 8u;
        uint32_t next = 0u;
        if (image == 0u)
        {
            Rng rnd{0x9E3779B97F4A7C15ull ^ salt};
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

        if (image == 2u)
        {
            Rng rnd{0xA0761D6478BD642Full ^ salt};
            for (uint32_t count = 0; count < 40u; ++count)
            {
                const uint32_t length = 14u + rnd(16u);
                if (next + length + 2u > maxPairs)
                    break;
                const uint32_t start = next;
                std::vector<uint32_t> lo(length, kLowerNop), up(length, kUpperNop);
                for (uint32_t pair = 0; pair < length; ++pair)
                {
                    const uint8_t vf = static_cast<uint8_t>(1u + rnd(8u));
                    uint32_t kind = rnd(12u);
                    if (vu0 && (kind <= 2u || kind == 5u))
                        kind = 8u; // quietLower
                    switch (kind)
                    {
                    case 0: // EFU op (all but the undefined 0x77 and WAITP 0x7B)
                    {
                        static const uint8_t kEfu[] = {0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x78, 0x79, 0x7A, 0x7C, 0x7D, 0x7E};
                        lo[pair] = lowerSpecial(kEfu[rnd(sizeof(kEfu))], vf, 0u, 0u, static_cast<uint8_t>(rnd(16u)));
                        break;
                    }
                    case 1: lo[pair] = lowerSpecial(0x7Bu, 0u); break;                                      // WAITP
                    case 2: lo[pair] = lowerSpecial(0x64u, 0u, vf, 0u, static_cast<uint8_t>(1u + rnd(15u))); break; // MFP
                    case 3: lo[pair] = div(vf, static_cast<uint8_t>(1u + rnd(8u)), static_cast<uint8_t>(rnd(4u)), static_cast<uint8_t>(rnd(4u))); break;
                    case 4: lo[pair] = lowerSpecial(0x3Bu, 0u); break;                                      // WAITQ
                    case 5: // XGKICK a tag (its VI set two pairs earlier), or a store into a tag's payload
                        if (pair >= 2u && rnd(2u) != 0u)
                        {
                            lo[pair - 2u] = iaddiu(4u, 0u, static_cast<int16_t>(kTagQword + 4u * rnd(kTagCount)));
                            lo[pair] = lowerSpecial(0x6Cu, 4u);
                        }
                        else
                            lo[pair] = sq(static_cast<uint8_t>(1u + rnd(15u)), vf, 0u,
                                          static_cast<int16_t>(kTagQword + 4u * rnd(kTagCount) + 1u));
                        break;
                    case 6: // JR/JALR forward (the jump reads VI set two pairs earlier)
                        if (pair >= 2u && pair + 6u < length)
                        {
                            const uint32_t target = start + pair + 2u + rnd(3u);
                            lo[pair - 2u] = iaddiu(5u, 0u, static_cast<int16_t>(target));
                            lo[pair] = rnd(2u) != 0u ? (0x24u << 25) | (5u << 11)
                                                     : (0x25u << 25) | (6u << 16) | (5u << 11); // JR vi5 / JALR vi6, vi5
                        }
                        break;
                    case 7: // forward branch
                        if (pair + 4u < length)
                        {
                            const int16_t forward = static_cast<int16_t>(1 + rnd(3u));
                            lo[pair] = rnd(2u) != 0u ? ibne(static_cast<uint8_t>(1u + rnd(4u)), static_cast<uint8_t>(rnd(5u)), forward)
                                                     : branch(forward);
                        }
                        break;
                    default: lo[pair] = quietLower(rnd); break;
                    }
                    const uint8_t dest = static_cast<uint8_t>(1u + rnd(15u));
                    switch (rnd(5u))
                    {
                    case 0: // I-bit: the lower word is the I immediate (ADDi/MADDi/MULi/SUBi)
                    {
                        static const uint8_t kIOps[] = {0x1E, 0x22, 0x23, 0x26};
                        up[pair] = upper(kIOps[rnd(sizeof(kIOps))], dest, 1u + rnd(8u), 1u + rnd(8u), 1u + rnd(8u)) | 0x80000000u;
                        const float imm = static_cast<float>(static_cast<int32_t>(rnd(2001u)) - 1000) / 16.0f;
                        std::memcpy(&lo[pair], &imm, sizeof(imm));
                        break;
                    }
                    case 1: // Q-reading upper (ADDq/MULq/MADDq)
                    {
                        static const uint8_t kQOps[] = {0x1C, 0x20, 0x21};
                        up[pair] = upper(kQOps[rnd(sizeof(kQOps))], dest, 1u + rnd(8u), 1u + rnd(8u), 1u + rnd(8u));
                        break;
                    }
                    default: up[pair] = randomUpper(rnd); break;
                    }
                }
                // A jump's delay slot and the pairs a branch skips stay whatever they are,
                // but no branch or jump may sit in a delay slot.
                for (uint32_t pair = 0; pair + 1u < length; ++pair)
                {
                    const uint8_t opHi = static_cast<uint8_t>((lo[pair] >> 25) & 0x7Fu);
                    const bool isBranch = (up[pair] & 0x80000000u) == 0u &&
                                          (opHi == 0x20u || opHi == 0x24u || opHi == 0x25u || opHi == 0x29u);
                    const uint8_t nextHi = static_cast<uint8_t>((lo[pair + 1u] >> 25) & 0x7Fu);
                    if (isBranch && (up[pair + 1u] & 0x80000000u) == 0u &&
                        (nextHi == 0x20u || nextHi == 0x24u || nextHi == 0x25u || nextHi == 0x29u))
                        lo[pair + 1u] = kLowerNop;
                }
                for (uint32_t pair = 0; pair < length; ++pair)
                    writePair(code, (start + pair) * 8u, lo[pair], up[pair]);
                const uint32_t total = length + writeEnd(code, (start + length) * 8u);
                programs.push_back({start * 8u, total});
                next = start + total;
            }
            return programs;
        }

        // Image 1: for every distance 0..5 and every reader kind, a few
        // variants. Reader kinds 0..10 are flagOpLower; 11 = the program ends
        // at that distance; 12 = a branch at that distance lands on a reader.
        Rng rnd{0xD1B54A32D192ED03ull ^ salt};
        uint32_t index = 0u;
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
                    if (index++ < skip)
                    {
                        std::memset(code + start * 8u, 0, total * 8u);
                        continue;
                    }
                    programs.push_back({start * 8u, total});
                    next = start + total;
                }
            }
        }
        return programs;
    }

    // VR3: VU0 differential images (4 KiB, VU0-legal ops): 0-1 mix (two
    // seeds), 2-3 pipes without the VU0-reserved ops (two seeds), 4..7 the
    // flag-distance list of image 1 in consecutive windows (each starts where
    // the previous one filled up).
    constexpr uint32_t kVu0CodeSize = 0x1000u;
    constexpr uint32_t kVu0ImageCount = 8u;
    constexpr uint64_t kVu0ImageHash[kVu0ImageCount] = {
        0x5652330000000000ull, 0x5652330000000001ull, 0x5652330000000002ull, 0x5652330000000003ull,
        0x5652330000000004ull, 0x5652330000000005ull, 0x5652330000000006ull, 0x5652330000000007ull};

    inline std::vector<Program> buildVu0Image(uint32_t image, uint8_t *code)
    {
        if (image < 4u)
            return buildImage(image < 2u ? 0u : 2u, code, kVu0CodeSize, true, image & 1u);
        uint32_t skip = 0u;
        for (uint32_t part = 4u; part < image; ++part)
            skip += static_cast<uint32_t>(buildImage(1u, code, kVu0CodeSize, true, 0u, skip).size());
        return buildImage(1u, code, kVu0CodeSize, true, 0u, skip);
    }
}

#endif
