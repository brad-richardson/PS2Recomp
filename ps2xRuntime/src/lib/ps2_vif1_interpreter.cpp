// Based on Blackline Interactive implementation
#include "ps2_mtvu.h"
#include "ps2_vif_unpack_fast.h"
#include "runtime/ps2_memory.h"
#include <atomic>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include "ps2_e7.h"

enum VIFCmd : uint8_t
{
    VIF_NOP = 0x00,
    VIF_STCYCL = 0x01,
    VIF_OFFSET = 0x02,
    VIF_BASE = 0x03,
    VIF_ITOP = 0x04,
    VIF_STMOD = 0x05,
    VIF_MSKPATH3 = 0x06,
    VIF_MARK = 0x07,
    VIF_FLUSHE = 0x10,
    VIF_FLUSH = 0x11,
    VIF_FLUSHA = 0x13,
    VIF_MSCAL = 0x14,
    VIF_MSCALF = 0x15,
    VIF_MSCNT = 0x17,
    VIF_STMASK = 0x20,
    VIF_STROW = 0x30,
    VIF_STCOL = 0x31,
    VIF_MPG = 0x4A,
    VIF_DIRECT = 0x50,
    VIF_DIRECTHL = 0x51,
};

namespace
{
    // VS1: bulk UNPACK fast path, unconditional (CU4 B2: the
    // PS2X_VIF_FAST_UNPACK=0 bisection lever is deleted; the generic
    // per-vector loop below stays as the fallback for non-bulk cases).

    // VS1: true when a masked UNPACK selects source data on every lane of
    // every cycle position the WL window uses, i.e. the mask is a no-op and
    // the generic loop would store the decompressed vector verbatim. Mirrors
    // the generic maskSpec computation exactly (maskCycle caps at 3).
    bool vifUnpackMaskAllData(uint32_t mask, uint32_t wl)
    {
        for (uint32_t c = 0u; c < wl; ++c)
        {
            const uint32_t maskCycle = (c > 3u) ? 3u : c;
            for (uint32_t f = 0u; f < 4u; ++f)
            {
                const uint32_t shift = ((maskCycle * 4u) + f) * 2u;
                if (((mask >> shift) & 0x3u) != 0u)
                    return false;
            }
        }
        return true;
    }

    // VUP1: once-per-process engagement receipt (B legs show it, A legs don't).
    void vifUnpackFastLogOnce()
    {
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true, std::memory_order_relaxed))
            std::fprintf(stderr, "[VUP1] fast unpack engaged (PS2X_VIF_UNPACK_FAST=1)\n");
    }

    constexpr uint8_t kGifFmtImage = 2u;

    uint32_t pendingGifImageQwc(const uint8_t *data, uint32_t sizeBytes)
    {
        if (!data || sizeBytes < 16u)
            return 0u;

        uint32_t offset = 0u;
        while (offset + 16u <= sizeBytes)
        {
            uint64_t tagLo = 0u;
            std::memcpy(&tagLo, data + offset, sizeof(tagLo));
            offset += 16u;

            const uint32_t nloop = static_cast<uint32_t>(tagLo & 0x7FFFu);
            const uint8_t flg = static_cast<uint8_t>((tagLo >> 58) & 0x3u);
            uint32_t nreg = static_cast<uint32_t>((tagLo >> 60) & 0xFu);
            if (nreg == 0u)
                nreg = 16u;

            uint64_t payloadBytes = 0u;
            if (flg == 0u) // PACKED
            {
                payloadBytes = static_cast<uint64_t>(nloop) * nreg * 16ull;
            }
            else if (flg == 1u) // REGLIST, padded to a quadword
            {
                payloadBytes = static_cast<uint64_t>(nloop) * nreg * 8ull;
                payloadBytes = (payloadBytes + 15ull) & ~15ull;
            }
            else if (flg == kGifFmtImage)
            {
                payloadBytes = static_cast<uint64_t>(nloop) * 16ull;
                const uint64_t availableBytes = sizeBytes - offset;
                if (payloadBytes > availableBytes)
                {
                    return static_cast<uint32_t>((payloadBytes - availableBytes) / 16ull);
                }
            }
            else
            {
                return 0u;
            }

            if (payloadBytes > static_cast<uint64_t>(sizeBytes - offset))
                return 0u;
            offset += static_cast<uint32_t>(payloadBytes);
        }

        return 0u;
    }
}

void PS2Memory::processVIF0Data(uint32_t srcPhys, uint32_t sizeBytes)
{
    if (sizeBytes == 0u || srcPhys >= PS2_RAM_SIZE)
        return;

    const uint64_t requestedEnd = static_cast<uint64_t>(srcPhys) + static_cast<uint64_t>(sizeBytes);
    if (requestedEnd > static_cast<uint64_t>(PS2_RAM_SIZE))
        sizeBytes = PS2_RAM_SIZE - srcPhys;

    processVIF0Data(m_rdram + srcPhys, sizeBytes);
}

void PS2Memory::processVIF0Data(const uint8_t *data, uint32_t sizeBytes)
{
    if (sizeBytes == 0u)
        return;

    uint32_t pos = 0;
    while (pos + 4 <= sizeBytes)
    {
        uint32_t cmd = 0u;
        std::memcpy(&cmd, data + pos, sizeof(cmd));
        pos += 4u;

        const uint8_t opcode = static_cast<uint8_t>((cmd >> 24) & 0x7Fu);
        const uint16_t imm = static_cast<uint16_t>(cmd & 0xFFFFu);
        const uint8_t num = static_cast<uint8_t>((cmd >> 16) & 0xFFu);
        const bool irq = (cmd & 0x80000000u) != 0u;

        vif0_regs.code = cmd;
        vif0_regs.num = num;
        if (irq)
            vif0_regs.stat |= (1u << 11);

        if (opcode == VIF_NOP)
        {
            continue;
        }
        else if (opcode == VIF_STCYCL)
        {
            vif0_regs.cycle = imm;
            continue;
        }
        else if (opcode == VIF_ITOP)
        {
            vif0_regs.itops = imm & 0x3FFu;
            continue;
        }
        else if (opcode == VIF_STMOD)
        {
            vif0_regs.mode = imm & 3u;
            continue;
        }
        else if (opcode == VIF_MARK)
        {
            vif0_regs.mark = imm;
            vif0_regs.stat |= (1u << 6);
            continue;
        }
        else if (opcode == VIF_FLUSHE || opcode == VIF_FLUSH || opcode == VIF_FLUSHA)
        {
            continue;
        }
        else if (opcode == VIF_STMASK)
        {
            if (pos + 4u > sizeBytes)
                break;
            std::memcpy(&vif0_regs.mask, data + pos, sizeof(vif0_regs.mask));
            pos += 4u;
            continue;
        }
        else if (opcode == VIF_STROW)
        {
            if (pos + 16u > sizeBytes)
                break;
            std::memcpy(vif0_regs.row, data + pos, 16u);
            pos += 16u;
            continue;
        }
        else if (opcode == VIF_STCOL)
        {
            if (pos + 16u > sizeBytes)
                break;
            std::memcpy(vif0_regs.col, data + pos, 16u);
            pos += 16u;
            continue;
        }
        else if (opcode == VIF_MPG)
        {
            const uint32_t destAddr = static_cast<uint32_t>(imm & 0x1FFu) * 8u;
            const uint32_t instructionCount = (num == 0u) ? 256u : static_cast<uint32_t>(num);
            const uint32_t mpgBytes = instructionCount * 8u;
            uint32_t copyBytes = 0u;
            if (m_vu0Code && destAddr < PS2_VU0_CODE_SIZE && mpgBytes > 0u)
            {
                copyBytes = mpgBytes;
                if (destAddr + copyBytes > PS2_VU0_CODE_SIZE)
                    copyBytes = PS2_VU0_CODE_SIZE - destAddr;
                if (pos + copyBytes <= sizeBytes)
                {
                    std::memcpy(m_vu0Code + destAddr, data + pos, copyBytes);
                    markVU0CodeModified();
                }
            }

            pos += mpgBytes;
            if (pos > sizeBytes)
                break;
            continue;
        }
        else if ((opcode & 0x60u) == 0x60u)
        {
            const uint8_t vn = static_cast<uint8_t>((opcode >> 2) & 0x3u);
            const uint8_t vl = static_cast<uint8_t>(opcode & 0x3u);
            const int components = static_cast<int>(vn) + 1;
            int bitsPerComponent = 32;
            switch (vl)
            {
            case 0:
                bitsPerComponent = 32;
                break;
            case 1:
                bitsPerComponent = 16;
                break;
            case 2:
                bitsPerComponent = 8;
                break;
            case 3:
                bitsPerComponent = (vn == 3u) ? 4 : 16;
                break;
            default:
                break;
            }
            const int bitsPerVector = (vl == 3u && vn == 3u) ? 16 : (components * bitsPerComponent);
            uint32_t bytesPerVector = static_cast<uint32_t>((bitsPerVector + 7) / 8);
            const uint32_t writeVectorCount = (num == 0u) ? 256u : static_cast<uint32_t>(num);
            uint32_t cl = vif0_regs.cycle & 0xFFu;
            uint32_t wl = (vif0_regs.cycle >> 8) & 0xFFu;
            if (cl == 0u)
                cl = 1u;
            if (wl == 0u)
                wl = 1u;
            uint32_t sourceVectorCount = writeVectorCount;
            if (cl < wl)
            {
                const uint32_t fullBlocks = writeVectorCount / wl;
                uint32_t remainder = writeVectorCount % wl;
                if (remainder > cl)
                    remainder = cl;
                sourceVectorCount = fullBlocks * cl + remainder;
            }
            uint32_t totalBytes = sourceVectorCount * bytesPerVector;
            totalBytes = (totalBytes + 3u) & ~3u;

            if (m_vu0Data && pos + totalBytes <= sizeBytes && vl == 0u)
            {
                uint32_t vuAddr = static_cast<uint32_t>(imm & 0x3FFu);
                if ((imm & 0x8000u) != 0u)
                    vuAddr = (vuAddr + (vif0_regs.tops & 0x3FFu)) & 0x3FFu;
                const uint8_t *srcBase = data + pos;
                uint32_t srcIndex = 0u;
                for (uint32_t writeIndex = 0; writeIndex < writeVectorCount; ++writeIndex)
                {
                    const uint32_t cyclePos = writeIndex % wl;
                    const bool sourceAvailable = (cl >= wl) || (cyclePos < cl);
                    uint32_t destVec = (cl >= wl) ? ((vuAddr + (writeIndex / wl) * cl + cyclePos) & 0x3FFu)
                                                  : ((vuAddr + writeIndex) & 0x3FFu);
                    const uint32_t destOff = destVec * 16u;
                    if (destOff + 16u > PS2_VU0_DATA_SIZE)
                    {
                        if (sourceAvailable && srcIndex < sourceVectorCount)
                            ++srcIndex;
                        continue;
                    }
                    if (!sourceAvailable || srcIndex >= sourceVectorCount)
                        continue;
                    const uint8_t *srcVec = srcBase + srcIndex * bytesPerVector;
                    ++srcIndex;
                    uint32_t lanes[4] = {0u, 0u, 0u, 0u};
                    std::memcpy(lanes, m_vu0Data + destOff, sizeof(lanes));
                    const uint32_t limit = (components > 4) ? 4u : static_cast<uint32_t>(components);
                    for (uint32_t c = 0; c < limit; ++c)
                    {
                        uint32_t scalar = 0u;
                        std::memcpy(&scalar, srcVec + c * 4u, sizeof(scalar));
                        lanes[c] = scalar;
                    }
                    _mm_storeu_si128(reinterpret_cast<__m128i *>(m_vu0Data + destOff), _mm_loadu_si128(reinterpret_cast<const __m128i *>(lanes)));
                }
            }
            pos += totalBytes;
            if (pos > sizeBytes)
                break;
            continue;
        }
        else
        {
            // No MSCAL/MSCALF/MSCNT/BASE/OFFSET branch exists, so a
            // VU0 program kicked here is silently dropped with the tail.
            break;
        }
    }
}

void PS2Memory::processVIF1Data(uint32_t srcPhys, uint32_t sizeBytes)
{
    if (sizeBytes == 0u || srcPhys >= PS2_RAM_SIZE)
        return;

    const uint64_t requestedEnd = static_cast<uint64_t>(srcPhys) + static_cast<uint64_t>(sizeBytes);
    if (requestedEnd > static_cast<uint64_t>(PS2_RAM_SIZE))
        sizeBytes = PS2_RAM_SIZE - srcPhys;

    processVIF1Data(m_rdram + srcPhys, sizeBytes);
}

void PS2Memory::processVIF1Data(const uint8_t *data, uint32_t sizeBytes)
{
    if (sizeBytes == 0u)
        return;
    if (ps2_mtvu::vifStageDefer())
    {
        // VPL2: parse here, the VU side runs from the log on the MTVU thread
        // (drainPath3IfUnmasked below becomes a record too).
        processVIF1DataStaged(data, sizeBytes);
        drainPath3IfUnmasked();
        return;
    }
    processVIF1DataImpl(data, sizeBytes);
    // RR1: PATH3 left unmasked at the end of a VIF1 delivery runs to completion.
    drainPath3IfUnmasked();
}

void PS2Memory::processVIF1DataImpl(const uint8_t *data, uint32_t sizeBytes)
{
    ps2_mtvu::noteVif1Bytes(sizeBytes); // MU2 counter: det-neutral, logged only

    uint32_t pos = 0;

    while (pos + 4 <= sizeBytes)
    {
        if (m_vif1PendingPath2ImageQwc != 0u)
        {
            const uint32_t availableQw = (sizeBytes - pos) / 16u;
            if (availableQw == 0u)
            {
                break;
            }

            const uint32_t chunkQw = std::min<uint32_t>(m_vif1PendingPath2ImageQwc, availableQw);
            // TRM1: pooled buffer; the two memcpys below overwrite every byte.
            std::vector<uint8_t> imagePacket =
                acquireStageBytes(16u + static_cast<size_t>(chunkQw) * 16u);
            imagePacket.resize(16u + static_cast<size_t>(chunkQw) * 16u);
            const uint64_t imageTag =
                static_cast<uint64_t>(chunkQw & 0x7FFFu) |
                ((m_vif1PendingPath2ImageQwc == chunkQw) ? (1ull << 15) : 0ull) |
                (static_cast<uint64_t>(kGifFmtImage) << 58);
            std::memcpy(imagePacket.data(), &imageTag, sizeof(imageTag));
            std::memcpy(imagePacket.data() + 16u, data + pos, static_cast<size_t>(chunkQw) * 16u);
            submitGifPacket(GifPathId::Path2,
                            imagePacket.data(),
                            static_cast<uint32_t>(imagePacket.size()),
                            true,
                            m_vif1PendingPath2DirectHl);
            // TRM1: synchronous consume above; the buffer returns to the pool.
            releaseStageBytes(std::move(imagePacket));

            pos += chunkQw * 16u;
            m_vif1PendingPath2ImageQwc -= chunkQw;
            if (m_vif1PendingPath2ImageQwc == 0u)
            {
                m_vif1PendingPath2DirectHl = false;
            }
            continue;
        }

        uint32_t cmd;
        memcpy(&cmd, data + pos, 4);
        pos += 4;

        uint8_t opcode = (cmd >> 24) & 0x7F;
        uint16_t imm = cmd & 0xFFFF;
        uint8_t num = (cmd >> 16) & 0xFF;
        const bool irq = (cmd & 0x80000000u) != 0u;

        // Track most-recent command for VIFn_CODE emulation.
        vif1_regs.code = cmd;
        vif1_regs.num = num;
        if (irq)
            vif1_regs.stat |= (1u << 11); // INT

        if (opcode == VIF_NOP)
        {
            continue;
        }
        else if (opcode == VIF_STCYCL)
        {
            vif1_regs.cycle = imm;
            continue;
        }
        else if (opcode == VIF_OFFSET)
        {
            // VIF double-buffer setup. OFFSET clears DBF and resets TOPS to BASE.
            // Do not rewrite BASE from the previous TOPS value.
            vif1_regs.ofst = imm & 0x3FFu;
            vif1_regs.tops = vif1_regs.base & 0x3FFu;
            vif1_regs.stat &= ~(1u << 7); // clear DBF
            continue;
        }
        else if (opcode == VIF_BASE)
        {
            // BASE only updates the base register. TOPS changes on OFFSET/MSCAL.
            vif1_regs.base = imm & 0x3FFu;
            continue;
        }
        else if (opcode == VIF_ITOP)
        {
            // ITOP VIFcode writes pending ITOPS; VU XITOP observes it after MSCAL/MSCNT.
            vif1_regs.itops = imm & 0x3FFu;
            continue;
        }
        else if (opcode == VIF_STMOD)
        {
            vif1_regs.mode = imm & 3u;
            continue;
        }
        else if (opcode == VIF_MSKPATH3)
        {
            // VIF command docs: MSKPATH3 uses IMMEDIATE bit 15.
            const uint32_t offset = pos - 4u;
            const bool wasMasked = m_path3Masked;
            m_path3Masked = (imm & 0x8000u) != 0u;
            if (ps2_e7::enabled())
                ps2_e7::event(gs_regs.vsyncTick.load(), "vif-mask", "cmd=0x%x offset=%u was=%u now=%u queued=%zu", cmd, offset, wasMasked, m_path3Masked, m_path3MaskedFifo.size());
            if (wasMasked && !m_path3Masked)
                releaseOneMaskedPath3Packet();
            continue;
        }
        else if (opcode == VIF_MARK)
        {
            vif1_regs.mark = imm;
            vif1_regs.stat |= (1u << 6); // MRK
            continue;
        }
        else if (opcode == VIF_FLUSHE || opcode == VIF_FLUSH || opcode == VIF_FLUSHA)
        {
            const char *flushName = (opcode == VIF_FLUSHE) ? "FLUSHE" : ((opcode == VIF_FLUSH) ? "FLUSH" : "FLUSHA");
            continue;
        }
        else if (opcode == VIF_MSCAL || opcode == VIF_MSCALF)
        {
            uint32_t startPC = (uint32_t)imm * 8u;

            // Values visible to the VU program for this MSCAL.
            // DobieStation semantics: ITOP = ITOPS; TOP = current TOPS;
            // then TOPS/DBF are prepared for the next buffer.
            const uint32_t runTop = vif1_regs.tops & 0x3FFu;
            const uint32_t runItop = vif1_regs.itops & 0x3FFu;
            vif1_regs.top = runTop;
            vif1_regs.itop = runItop;

            const bool dbf = (vif1_regs.stat & (1u << 7)) != 0u;
            if (dbf)
                vif1_regs.tops = vif1_regs.base & 0x3FFu;
            else
                vif1_regs.tops = (vif1_regs.base + vif1_regs.ofst) & 0x3FFu;
            vif1_regs.stat ^= (1u << 7); // toggle DBF

            if (m_vu1MscalCallback)
                m_vu1MscalCallback(startPC, runTop, runItop);
            continue;
        }
        else if (opcode == VIF_MSCNT)
        {
            const uint32_t runTop = vif1_regs.tops & 0x3FFu;
            const uint32_t runItop = vif1_regs.itops & 0x3FFu;
            vif1_regs.top = runTop;
            vif1_regs.itop = runItop;

            const bool dbf = (vif1_regs.stat & (1u << 7)) != 0u;
            if (dbf)
                vif1_regs.tops = vif1_regs.base & 0x3FFu;
            else
                vif1_regs.tops = (vif1_regs.base + vif1_regs.ofst) & 0x3FFu;
            vif1_regs.stat ^= (1u << 7); // toggle DBF

            if (m_vu1MscntCallback)
                m_vu1MscntCallback(runTop, runItop);
            continue;
        }
        else if (opcode == VIF_STMASK)
        {
            if (pos + 4 > sizeBytes)
                break;
            uint32_t maskValue = 0;
            std::memcpy(&maskValue, data + pos, sizeof(maskValue));
            vif1_regs.mask = maskValue;
            pos += 4;
            continue;
        }
        else if (opcode == VIF_STROW)
        {
            if (pos + 16 > sizeBytes)
                break;
            std::memcpy(vif1_regs.row, data + pos, 16);
            pos += 16;
            continue;
        }
        else if (opcode == VIF_STCOL)
        {
            if (pos + 16 > sizeBytes)
                break;
            std::memcpy(vif1_regs.col, data + pos, 16);
            pos += 16;
            continue;
        }
        else if (opcode == VIF_MPG)
        {
            uint32_t destAddr = (uint32_t)imm * 8u;
            // VIF MPG semantics: NUM==0 means 256 instructions (2048 bytes).
            // MPG payload is instruction-packed and should not be QW-aligned.
            const uint32_t instructionCount = (num == 0u) ? 256u : static_cast<uint32_t>(num);
            const uint32_t mpgBytes = instructionCount * 8u;
            if (m_vu1Code && destAddr < PS2_VU1_CODE_SIZE && mpgBytes > 0)
            {
                uint32_t copyBytes = mpgBytes;
                if (destAddr + copyBytes > PS2_VU1_CODE_SIZE)
                    copyBytes = PS2_VU1_CODE_SIZE - destAddr;
                if (pos + copyBytes <= sizeBytes)
                {
                    std::memcpy(m_vu1Code + destAddr, data + pos, copyBytes);
                    markVU1CodeModified();
                }
            }
            pos += mpgBytes;
            if (pos > sizeBytes)
                break;
            continue;
        }
        else if (opcode == VIF_DIRECT || opcode == VIF_DIRECTHL)
        {
            uint32_t qwCount = imm;
            if (qwCount == 0)
                qwCount = 65536;
            const uint32_t availableQw = (sizeBytes - pos) / 16u;
            const bool truncated = qwCount > availableQw;
            if (qwCount > availableQw)
                qwCount = availableQw;

            if (qwCount > 0)
            {
                const bool directHl = (opcode == VIF_DIRECTHL);
                submitGifPacket(GifPathId::Path2, data + pos, qwCount * 16, true, directHl);

                const uint32_t pendingImageQw = pendingGifImageQwc(data + pos, qwCount * 16u);
                if (pendingImageQw != 0u)
                {
                    m_vif1PendingPath2ImageQwc = pendingImageQw;
                    m_vif1PendingPath2DirectHl = directHl;
                }
            }

            pos += qwCount * 16;
            if (truncated)
            {
                pos = sizeBytes;
                break;
            }
            continue;
        }
        else if ((opcode & 0x60) == 0x60)
        {
            ps2_mtvu::noteVif1Unpack(); // MU2 counter: det-neutral, logged only
            uint8_t vn = (opcode >> 2) & 0x3;
            uint8_t vl = opcode & 0x3;
            const bool maskEnable = (opcode & 0x10u) != 0u;
            int components = vn + 1;
            int bitsPerComponent = 32;
            switch (vl)
            {
            case 0:
                bitsPerComponent = 32;
                break;
            case 1:
                bitsPerComponent = 16;
                break;
            case 2:
                bitsPerComponent = 8;
                break;
            case 3:
                bitsPerComponent = (vn == 3) ? 4 : 16;
                break;
            default:
                break;
            }
            int bitsPerVector = (vl == 3 && vn == 3) ? 16 : (components * bitsPerComponent);
            uint32_t bytesPerVector = (bitsPerVector + 7) / 8;
            // UNPACK semantics: NUM is 8-bit and NUM==0 means 256 vectors (writes).
            const uint32_t writeVectorCount = (num == 0u) ? 256u : static_cast<uint32_t>(num);

            // STCYCL controls write cycles for UNPACK.
            uint32_t cl = vif1_regs.cycle & 0xFFu;
            uint32_t wl = (vif1_regs.cycle >> 8) & 0xFFu;
            if (cl == 0u)
                cl = 1u;
            if (wl == 0u)
                wl = 1u;

            uint32_t sourceVectorCount = writeVectorCount;
            if (cl < wl)
            {
                const uint32_t fullBlocks = writeVectorCount / wl;
                uint32_t remainder = writeVectorCount % wl;
                if (remainder > cl)
                    remainder = cl;
                sourceVectorCount = fullBlocks * cl + remainder;
            }

            uint32_t totalBytes = sourceVectorCount * bytesPerVector;
            totalBytes = (totalBytes + 3) & ~3u;

            uint32_t vuAddr = (uint32_t)imm & 0x3FFu;
            if ((imm & 0x8000u) != 0u)
                vuAddr = (vuAddr + (vif1_regs.tops & 0x3FFu)) & 0x3FFu;

            const bool zeroExtend = (imm & 0x4000u) != 0u;

            // UV1 Part 2: stream phase of this UNPACK's data start. Buffers
            // concatenate whole QW payloads, so pos&15 is the VIF stream QW
            // phase iff the first payload starts guest-QW-aligned (true for
            // the game's QW-granular VIF chains; assumed here). PCSX2 tracks
            // the same phase as start_aligned (Vif_Unpack.cpp:248).
            const uint32_t uv1DataStartPos = pos;
            const bool uv1UnpackQwAligned = ((uv1DataStartPos & 15u) == 0u);


            // VS1: bulk fast path for the common V4_32 stream case (UV1: 31% of
            // UNPACK commands; the per-vector loop below is ~0.6 ms/frame on
            // the Odin MTVU thread). V4_32 writes all four lanes from source,
            // mode 0 adds nothing, cl==wl makes source available every cycle
            // with a contiguous destination, and an all-data mask selects
            // source on every lane. Anything else (fills, skips, masked
            // fills/protects) keeps the generic loop.
            const bool unpackBulk =
                vl == 0u && vn == 3u &&
                (vif1_regs.mode & 3u) == 0u && cl == wl && m_vu1Data != nullptr &&
                totalBytes > 0u && pos + totalBytes <= sizeBytes &&
                (!maskEnable || vifUnpackMaskAllData(vif1_regs.mask, wl));
            if (unpackBulk)
            {
                const uint8_t *bulkSrc = data + pos;
                const uint32_t firstRun = 0x400u - vuAddr;
                const uint32_t bulkFirst =
                    (writeVectorCount < firstRun) ? writeVectorCount : firstRun;
                std::memcpy(m_vu1Data + static_cast<size_t>(vuAddr) * 16u, bulkSrc,
                            static_cast<size_t>(bulkFirst) * 16u);
                if (writeVectorCount > bulkFirst)
                {
                    std::memcpy(m_vu1Data, bulkSrc + static_cast<size_t>(bulkFirst) * 16u,
                                static_cast<size_t>(writeVectorCount - bulkFirst) * 16u);
                }
            }
            else if (m_vifUnpackFast && totalBytes > 0u && pos + totalBytes <= sizeBytes &&
                     ps2_vif_unpack_fast::eligible(vl, static_cast<uint32_t>(components), maskEnable,
                                                   vif1_regs.mode & 3u, vif1_regs.mask, wl))
            {
                // VUP1 (PS2X_VIF_UNPACK_FAST=1): specialized decoder for the
                // heavy-window shapes (masked V3/V2-16/32, V4-5/16, V2-8 at
                // wl == 1, mode 0). wl == 1 makes cyclePos always 0 (only mask
                // row 0 applies) with source available every cycle, and the
                // eligibility gate allows only data/row lane specs, so every
                // lane of every vector is written: no read-modify-write
                // preload, per-command format/mask/cycle work hoisted. Stores
                // are byte-identical to the generic loop below.
                ps2_vif_unpack_fast::noteFastUnpack();
                vifUnpackFastLogOnce();
                const uint8_t *srcBase = data + pos;
                const uint32_t row0 = vif1_regs.mask & 0xFFu;
                uint32_t spec[4];
                for (uint32_t f = 0u; f < 4u; ++f)
                    spec[f] = maskEnable ? ((row0 >> (f * 2u)) & 0x3u) : 0u;
                for (uint32_t i = 0u; i < writeVectorCount; ++i)
                {
                    const uint8_t *srcVec = srcBase + i * bytesPerVector;
                    const uint32_t destVec = (vuAddr + i * cl) & 0x3FFu;
                    uint32_t dec[4] = {0u, 0u, 0u, 0u};
                    ps2_vif_unpack_fast::decodeVector(vl, static_cast<uint32_t>(components), srcVec,
                                                      zeroExtend, uv1UnpackQwAligned, srcBase, i,
                                                      bytesPerVector, uv1DataStartPos, sizeBytes, dec);
                    uint32_t lanes[4];
                    for (uint32_t f = 0u; f < 4u; ++f)
                        lanes[f] = (spec[f] == 0u) ? dec[f] : vif1_regs.row[f];
                    const uint32_t destOff = destVec * 16u;
                    std::memcpy(m_vu1Data + destOff, lanes, sizeof(lanes));
                }
            }
            else if (m_vu1Data && totalBytes > 0 && pos + totalBytes <= sizeBytes)
            {
                const uint8_t *srcBase = data + pos;
                uint32_t srcIndex = 0u;
                // VS1: strength-reduce the per-vector cycle division. Observed
                // (cl,wl) pairs are 1x1/3x1/4x4 (UV1); every power-of-two WL
                // reduces exactly, anything else keeps the generic division.
                const bool wlPow2 = (wl & (wl - 1u)) == 0u;
                const uint32_t wlShift =
                    wlPow2 ? static_cast<uint32_t>(std::countr_zero(wl)) : 0u;
                const uint32_t wlMask = wl - 1u;
                const bool clGeWl = (cl >= wl);
                for (uint32_t writeIndex = 0; writeIndex < writeVectorCount; ++writeIndex)
                {
                    const uint32_t cyclePos =
                        wlPow2 ? (writeIndex & wlMask) : (writeIndex % wl);
                    const bool sourceAvailable = clGeWl || (cyclePos < cl);

                    uint32_t destVec = 0;
                    if (clGeWl)
                    {
                        const uint32_t wlGroup =
                            wlPow2 ? (writeIndex >> wlShift) : (writeIndex / wl);
                        destVec = (vuAddr + wlGroup * cl + cyclePos) & 0x3FFu;
                    }
                    else
                    {
                        destVec = (vuAddr + writeIndex) & 0x3FFu;
                    }

                    uint32_t destOff = destVec * 16u;
                    if (destOff + 16u > PS2_VU1_DATA_SIZE)
                    {
                        if (sourceAvailable && srcIndex < sourceVectorCount)
                            ++srcIndex;
                        continue;
                    }

                    uint32_t lanes[4] = {0u, 0u, 0u, 0u};
                    std::memcpy(lanes, m_vu1Data + destOff, sizeof(lanes));
                    uint32_t decompressed[4] = {lanes[0], lanes[1], lanes[2], lanes[3]};
                    bool decoded = false;

                    const uint8_t *srcVec = nullptr;
                    uint32_t uv1MySrc = 0u;
                    if (sourceAvailable && srcIndex < sourceVectorCount)
                    {
                        srcVec = srcBase + srcIndex * bytesPerVector;
                        uv1MySrc = srcIndex;
                        ++srcIndex;
                        decoded = true;
                    }

                    auto extend16 = [&](uint16_t raw) -> uint32_t
                    {
                        if (zeroExtend)
                            return static_cast<uint32_t>(raw);
                        return static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(raw)));
                    };

                    auto extend8 = [&](uint8_t raw) -> uint32_t
                    {
                        if (zeroExtend)
                            return static_cast<uint32_t>(raw);
                        return static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(raw)));
                    };

                    bool handledFormat = true;
                    if (!decoded)
                    {
                        handledFormat = false;
                    }
                    else if (vl == 0u)
                    {
                        if (components == 1)
                        {
                            uint32_t scalar = 0;
                            std::memcpy(&scalar, srcVec, sizeof(scalar));
                            decompressed[0] = scalar;
                            decompressed[1] = scalar;
                            decompressed[2] = scalar;
                            decompressed[3] = scalar;
                        }
                        else
                        {
                            const uint32_t limit = (components > 4) ? 4u : static_cast<uint32_t>(components);
                            for (uint32_t c = 0; c < limit; ++c)
                            {
                                uint32_t scalar = 0;
                                std::memcpy(&scalar, srcVec + c * 4u, sizeof(scalar));
                                decompressed[c] = scalar;
                            }
                        }
                    }
                    else if (vl == 1u)
                    {
                        if (components == 1)
                        {
                            uint16_t raw = 0;
                            std::memcpy(&raw, srcVec, sizeof(raw));
                            const uint32_t scalar = extend16(raw);
                            decompressed[0] = scalar;
                            decompressed[1] = scalar;
                            decompressed[2] = scalar;
                            decompressed[3] = scalar;
                        }
                        else
                        {
                            const uint32_t limit = (components > 4) ? 4u : static_cast<uint32_t>(components);
                            for (uint32_t c = 0; c < limit; ++c)
                            {
                                uint16_t raw = 0;
                                std::memcpy(&raw, srcVec + c * 2u, sizeof(raw));
                                decompressed[c] = extend16(raw);
                            }
                        }
                    }
                    else if (vl == 2u)
                    {
                        if (components == 1)
                        {
                            const uint32_t scalar = extend8(srcVec[0]);
                            decompressed[0] = scalar;
                            decompressed[1] = scalar;
                            decompressed[2] = scalar;
                            decompressed[3] = scalar;
                        }
                        else
                        {
                            const uint32_t limit = (components > 4) ? 4u : static_cast<uint32_t>(components);
                            for (uint32_t c = 0; c < limit; ++c)
                            {
                                decompressed[c] = extend8(srcVec[c]);
                            }
                        }
                    }
                    else if (vl == 3u && vn == 3u)
                    {
                        // V4-5: RGBA5551 in one 16-bit value, expanded to 8-bit
                        // channels (RGB << 3, A << 7), as in PCSX2's UNPACK_V4_5.
                        uint16_t packed = 0;
                        std::memcpy(&packed, srcVec, sizeof(packed));
                        decompressed[0] = (packed & 0x1Fu) << 3;
                        decompressed[1] = ((packed >> 5) & 0x1Fu) << 3;
                        decompressed[2] = ((packed >> 10) & 0x1Fu) << 3;
                        decompressed[3] = ((packed >> 15) & 0x01u) << 7;
                    }
                    else
                    {
                        handledFormat = false;
                    }

                    // UV1 Part 2: PCSX2 V2/V3 lane rules. V2 writes v1v0v1v0
                    // (Vif_Unpack.cpp UNPACK_V2 :76-83; xUPK_V2_* in
                    // x86/Vif_UnpackSSE.cpp :139-201), except V2-32 zeroes w
                    // when the unpack data starts QW-aligned (xUPK_V2_32
                    // :139-151; PCSX2 hardcodes the aligned case for every
                    // non-V3-16 type via key1, x86/Vif_Dynarec.cpp :471-473 —
                    // here the real stream phase above is evaluated). V3
                    // takes w from the next source vector's first element, 0
                    // when that read crosses a source QW boundary (xUPK_V3_*
                    // :203-238) or runs past the buffer end. Mask/mode
                    // handling below is unchanged and applies to the new
                    // lanes exactly as PCSX2's writeXYZW does (:24-59).
                    if (decoded && handledFormat && (components == 2 || components == 3))
                    {
                        if (components == 2)
                        {
                            decompressed[2] = decompressed[0];
                            if (vl == 0u && uv1UnpackQwAligned)
                                decompressed[3] = 0u;
                            else
                                decompressed[3] = decompressed[1];
                        }
                        else
                        {
                            const uint32_t uv1ReadLen = (vl == 0u) ? 4u : ((vl == 1u) ? 2u : 1u);
                            const uint64_t uv1ReadOff = static_cast<uint64_t>(uv1MySrc + 1u) *
                                                        static_cast<uint64_t>(bytesPerVector);
                            const uint64_t uv1Avail = static_cast<uint64_t>(sizeBytes - uv1DataStartPos);
                            const uint32_t uv1Phase = static_cast<uint32_t>(
                                (static_cast<uint64_t>(uv1DataStartPos) + uv1ReadOff) & 15u);
                            uint32_t uv1W = 0u;
                            if (uv1Phase + uv1ReadLen <= 16u && uv1ReadOff + uv1ReadLen <= uv1Avail)
                            {
                                const uint8_t *uv1Next = srcBase + uv1ReadOff;
                                if (vl == 0u)
                                {
                                    std::memcpy(&uv1W, uv1Next, sizeof(uv1W));
                                }
                                else if (vl == 1u)
                                {
                                    uint16_t uv1Raw = 0u;
                                    std::memcpy(&uv1Raw, uv1Next, sizeof(uv1Raw));
                                    uv1W = extend16(uv1Raw);
                                }
                                else
                                {
                                    uv1W = extend8(uv1Next[0]);
                                }
                            }
                            decompressed[3] = uv1W;
                        }
                    }

                    // Unknown compressed format fallback: preserve legacy raw-copy behavior.
                    if (!handledFormat && decoded && !maskEnable && (vif1_regs.mode == 0u || vif1_regs.mode == 3u))
                    {
                        uint32_t copyBytes = (bytesPerVector < 16u) ? bytesPerVector : 16u;
                        std::memcpy(m_vu1Data + destOff, srcVec, copyBytes);
                        continue;
                    }

                    const bool canAdd = (vl != 3u || vn != 3u);
                    const uint32_t mode = vif1_regs.mode & 3u;
                    const uint32_t colIdx = (cyclePos > 3u) ? 3u : cyclePos;
                    const uint32_t maskCycle = (cyclePos > 3u) ? 3u : cyclePos;

                    for (uint32_t field = 0u; field < 4u; ++field)
                    {
                        uint32_t maskSpec = 0u;
                        if (maskEnable)
                        {
                            const uint32_t shift = ((maskCycle * 4u) + field) * 2u;
                            maskSpec = (vif1_regs.mask >> shift) & 0x3u;
                        }

                        // In fill-write cycles with suspended source reads, treat raw-data selections as row-fill.
                        if (!decoded && maskSpec == 0u)
                            maskSpec = 1u;

                        uint32_t writeVal = lanes[field];
                        if (maskSpec == 0u)
                        {
                            if (handledFormat)
                            {
                                writeVal = decompressed[field];
                                if (canAdd && (mode == 1u || mode == 2u))
                                {
                                    writeVal = writeVal + vif1_regs.row[field];
                                    if (mode == 2u)
                                        vif1_regs.row[field] = writeVal;
                                }
                            }
                        }
                        else if (maskSpec == 1u)
                        {
                            writeVal = vif1_regs.row[field];
                        }
                        else if (maskSpec == 2u)
                        {
                            writeVal = vif1_regs.col[colIdx];
                        }
                        else
                        {
                            continue; // write-protect
                        }

                        lanes[field] = writeVal;
                    }

                    std::memcpy(m_vu1Data + destOff, lanes, sizeof(lanes));
                }
            }
            pos += totalBytes;

            if (pos > sizeBytes)
                break;
            continue;
        }
        else
        {
            continue;
        }
    }
}

// VPL2 VIF stage (PS2X_MTVU_VIF_STAGE=1): processVIF1DataImpl on the
// MTVU-VIF thread. The parse, the VIF1 register effects (CYCLE, MASK, MODE,
// ROW incl. mode-2 accumulate, COL, BASE/OFST/TOPS/DBF, ITOPS/TOP/ITOP,
// CODE/NUM/STAT, MARK) and the pending PATH2 image state are identical and
// stay here. Every effect on VU-side state becomes a record, in program
// order (local/research/VPL2/REPORT.md §1.3):
//   UNPACK -> Block (bulk path) or Masked (per vector: the bytes today's
//             read-modify-write stores, with a byte mask for the rest),
//   MPG -> Mpg, MSCAL/MSCALF/MSCNT -> Mscal, MSKPATH3 -> Msk3,
//   DIRECT/DIRECTHL + image continuation -> GifCopy (submitGifPacket).
// It never reads VU1 data or code. The dev taps of the inline loop (E36/
// E37/E39/E40, UV1, RR1, E7, gfx-stats) force MTVU off, so they are absent.
void PS2Memory::processVIF1DataStaged(const uint8_t *data, uint32_t sizeBytes)
{
    using K = ps2_mtvu::VifRecKind;
    ps2_mtvu::noteVif1Bytes(sizeBytes); // MU2 counter: det-neutral, logged only

    uint32_t pos = 0;

    while (pos + 4 <= sizeBytes)
    {
        if (m_vif1PendingPath2ImageQwc != 0u)
        {
            const uint32_t availableQw = (sizeBytes - pos) / 16u;
            if (availableQw == 0u)
            {
                break;
            }

            const uint32_t chunkQw = std::min<uint32_t>(m_vif1PendingPath2ImageQwc, availableQw);
            // TRM1: pooled buffer (returns at GifCopy consume); the two
            // memcpys below overwrite every byte.
            std::vector<uint8_t> imagePacket =
                acquireStageBytes(16u + static_cast<size_t>(chunkQw) * 16u);
            imagePacket.resize(16u + static_cast<size_t>(chunkQw) * 16u);
            const uint64_t imageTag =
                static_cast<uint64_t>(chunkQw & 0x7FFFu) |
                ((m_vif1PendingPath2ImageQwc == chunkQw) ? (1ull << 15) : 0ull) |
                (static_cast<uint64_t>(kGifFmtImage) << 58);
            std::memcpy(imagePacket.data(), &imageTag, sizeof(imageTag));
            std::memcpy(imagePacket.data() + 16u, data + pos, static_cast<size_t>(chunkQw) * 16u);
            vifStageGif(GifPathId::Path2, std::move(imagePacket), true, m_vif1PendingPath2DirectHl);

            pos += chunkQw * 16u;
            m_vif1PendingPath2ImageQwc -= chunkQw;
            if (m_vif1PendingPath2ImageQwc == 0u)
            {
                m_vif1PendingPath2DirectHl = false;
            }
            continue;
        }

        uint32_t cmd;
        memcpy(&cmd, data + pos, 4);
        pos += 4;

        uint8_t opcode = (cmd >> 24) & 0x7F;
        uint16_t imm = cmd & 0xFFFF;
        uint8_t num = (cmd >> 16) & 0xFF;
        const bool irq = (cmd & 0x80000000u) != 0u;

        vif1_regs.code = cmd;
        vif1_regs.num = num;
        if (irq)
            vif1_regs.stat |= (1u << 11); // INT

        if (opcode == VIF_NOP)
        {
            continue;
        }
        else if (opcode == VIF_STCYCL)
        {
            vif1_regs.cycle = imm;
            continue;
        }
        else if (opcode == VIF_OFFSET)
        {
            vif1_regs.ofst = imm & 0x3FFu;
            vif1_regs.tops = vif1_regs.base & 0x3FFu;
            vif1_regs.stat &= ~(1u << 7); // clear DBF
            continue;
        }
        else if (opcode == VIF_BASE)
        {
            vif1_regs.base = imm & 0x3FFu;
            continue;
        }
        else if (opcode == VIF_ITOP)
        {
            vif1_regs.itops = imm & 0x3FFu;
            continue;
        }
        else if (opcode == VIF_STMOD)
        {
            vif1_regs.mode = imm & 3u;
            continue;
        }
        else if (opcode == VIF_MSKPATH3)
        {
            vifStageMsk3(imm);
            continue;
        }
        else if (opcode == VIF_MARK)
        {
            vif1_regs.mark = imm;
            vif1_regs.stat |= (1u << 6); // MRK
            continue;
        }
        else if (opcode == VIF_FLUSHE || opcode == VIF_FLUSH || opcode == VIF_FLUSHA)
        {
            continue;
        }
        else if (opcode == VIF_MSCAL || opcode == VIF_MSCALF || opcode == VIF_MSCNT)
        {
            const bool mscnt = (opcode == VIF_MSCNT);
            const uint32_t startPC = mscnt ? 0u : static_cast<uint32_t>(imm) * 8u;
            const uint32_t runTop = vif1_regs.tops & 0x3FFu;
            const uint32_t runItop = vif1_regs.itops & 0x3FFu;
            vif1_regs.top = runTop;
            vif1_regs.itop = runItop;

            const bool dbf = (vif1_regs.stat & (1u << 7)) != 0u;
            if (dbf)
                vif1_regs.tops = vif1_regs.base & 0x3FFu;
            else
                vif1_regs.tops = (vif1_regs.base + vif1_regs.ofst) & 0x3FFu;
            vif1_regs.stat ^= (1u << 7); // toggle DBF

            // Published at once: the MTVU thread runs this program while the
            // VIF thread parses on.
            ps2_mtvu::vifStagePush(K::Mscal, mscnt ? 1u : 0u, startPC, runTop | (runItop << 16), true);
            continue;
        }
        else if (opcode == VIF_STMASK)
        {
            if (pos + 4 > sizeBytes)
                break;
            uint32_t maskValue = 0;
            std::memcpy(&maskValue, data + pos, sizeof(maskValue));
            vif1_regs.mask = maskValue;
            pos += 4;
            continue;
        }
        else if (opcode == VIF_STROW)
        {
            if (pos + 16 > sizeBytes)
                break;
            std::memcpy(vif1_regs.row, data + pos, 16);
            pos += 16;
            continue;
        }
        else if (opcode == VIF_STCOL)
        {
            if (pos + 16 > sizeBytes)
                break;
            std::memcpy(vif1_regs.col, data + pos, 16);
            pos += 16;
            continue;
        }
        else if (opcode == VIF_MPG)
        {
            uint32_t destAddr = (uint32_t)imm * 8u;
            const uint32_t instructionCount = (num == 0u) ? 256u : static_cast<uint32_t>(num);
            const uint32_t mpgBytes = instructionCount * 8u;
            if (m_vu1Code && destAddr < PS2_VU1_CODE_SIZE && mpgBytes > 0)
            {
                uint32_t copyBytes = mpgBytes;
                if (destAddr + copyBytes > PS2_VU1_CODE_SIZE)
                    copyBytes = PS2_VU1_CODE_SIZE - destAddr;
                if (pos + copyBytes <= sizeBytes)
                {
                    const uint32_t size = 16u + ((copyBytes + 15u) & ~15u);
                    if (uint8_t *p = ps2_mtvu::vifStageReserve(size))
                    {
                        const ps2_mtvu::VifRec r{size, K::Mpg, 0u, 0u, destAddr, copyBytes};
                        std::memcpy(p, &r, sizeof(r));
                        std::memcpy(p + 16u, data + pos, copyBytes);
                        ps2_mtvu::vifStageCommit(size, false);
                    }
                }
            }
            pos += mpgBytes;
            if (pos > sizeBytes)
                break;
            continue;
        }
        else if (opcode == VIF_DIRECT || opcode == VIF_DIRECTHL)
        {
            uint32_t qwCount = imm;
            if (qwCount == 0)
                qwCount = 65536;
            const uint32_t availableQw = (sizeBytes - pos) / 16u;
            const bool truncated = qwCount > availableQw;
            if (qwCount > availableQw)
                qwCount = availableQw;

            if (qwCount > 0)
            {
                const bool directHl = (opcode == VIF_DIRECTHL);
                // TRM1: pooled buffer (returns at GifCopy consume);
                // assign() overwrites every byte.
                std::vector<uint8_t> directBytes = acquireStageBytes(static_cast<size_t>(qwCount) * 16u);
                directBytes.assign(data + pos, data + pos + static_cast<size_t>(qwCount) * 16u);
                vifStageGif(GifPathId::Path2, std::move(directBytes), true, directHl);

                const uint32_t pendingImageQw = pendingGifImageQwc(data + pos, qwCount * 16u);
                if (pendingImageQw != 0u)
                {
                    m_vif1PendingPath2ImageQwc = pendingImageQw;
                    m_vif1PendingPath2DirectHl = directHl;
                }
            }

            pos += qwCount * 16;
            if (truncated)
            {
                pos = sizeBytes;
                break;
            }
            continue;
        }
        else if ((opcode & 0x60) == 0x60)
        {
            ps2_mtvu::noteVif1Unpack(); // MU2 counter: det-neutral, logged only
            uint8_t vn = (opcode >> 2) & 0x3;
            uint8_t vl = opcode & 0x3;
            const bool maskEnable = (opcode & 0x10u) != 0u;
            int components = vn + 1;
            int bitsPerComponent = 32;
            switch (vl)
            {
            case 0:
                bitsPerComponent = 32;
                break;
            case 1:
                bitsPerComponent = 16;
                break;
            case 2:
                bitsPerComponent = 8;
                break;
            case 3:
                bitsPerComponent = (vn == 3) ? 4 : 16;
                break;
            default:
                break;
            }
            int bitsPerVector = (vl == 3 && vn == 3) ? 16 : (components * bitsPerComponent);
            uint32_t bytesPerVector = (bitsPerVector + 7) / 8;
            const uint32_t writeVectorCount = (num == 0u) ? 256u : static_cast<uint32_t>(num);

            uint32_t cl = vif1_regs.cycle & 0xFFu;
            uint32_t wl = (vif1_regs.cycle >> 8) & 0xFFu;
            if (cl == 0u)
                cl = 1u;
            if (wl == 0u)
                wl = 1u;

            uint32_t sourceVectorCount = writeVectorCount;
            if (cl < wl)
            {
                const uint32_t fullBlocks = writeVectorCount / wl;
                uint32_t remainder = writeVectorCount % wl;
                if (remainder > cl)
                    remainder = cl;
                sourceVectorCount = fullBlocks * cl + remainder;
            }

            uint32_t totalBytes = sourceVectorCount * bytesPerVector;
            totalBytes = (totalBytes + 3) & ~3u;

            uint32_t vuAddr = (uint32_t)imm & 0x3FFu;
            if ((imm & 0x8000u) != 0u)
                vuAddr = (vuAddr + (vif1_regs.tops & 0x3FFu)) & 0x3FFu;

            const bool zeroExtend = (imm & 0x4000u) != 0u;
            const uint32_t uv1DataStartPos = pos;
            const bool uv1UnpackQwAligned = ((uv1DataStartPos & 15u) == 0u);

            const bool unpackBulk =
                vl == 0u && vn == 3u &&
                (vif1_regs.mode & 3u) == 0u && cl == wl && m_vu1Data != nullptr &&
                totalBytes > 0u && pos + totalBytes <= sizeBytes &&
                (!maskEnable || vifUnpackMaskAllData(vif1_regs.mask, wl));
            if (unpackBulk)
            {
                // V4-32 with cl == wl: writeVectorCount source qwords land at
                // consecutive VU qwords (wrapping); totalBytes covers them.
                const uint32_t size = 16u + writeVectorCount * 16u;
                if (uint8_t *p = ps2_mtvu::vifStageReserve(size))
                {
                    const ps2_mtvu::VifRec r{size, K::Block, 0u, static_cast<uint16_t>(writeVectorCount), vuAddr, 0u};
                    std::memcpy(p, &r, sizeof(r));
                    std::memcpy(p + 16u, data + pos, static_cast<size_t>(writeVectorCount) * 16u);
                    ps2_mtvu::vifStageCommit(size, false);
                }
            }
            else if (m_vifUnpackFast && totalBytes > 0u && pos + totalBytes <= sizeBytes &&
                     ps2_vif_unpack_fast::eligible(vl, static_cast<uint32_t>(components), maskEnable,
                                                   vif1_regs.mode & 3u, vif1_regs.mask, wl))
            {
                // VUP1 (PS2X_VIF_UNPACK_FAST=1): the same specialized decode
                // as the inline loop, emitting one full-mask Masked entry per
                // vector. The record bytes are identical to the generic loop's
                // (same reserve size, header, entry order and values).
                ps2_vif_unpack_fast::noteFastUnpack();
                vifUnpackFastLogOnce();
                const uint32_t fastSize = (16u + writeVectorCount * 20u + 15u) & ~15u;
                if (uint8_t *rec = ps2_mtvu::vifStageReserve(fastSize))
                {
                    const uint8_t *srcBase = data + pos;
                    const uint32_t row0 = vif1_regs.mask & 0xFFu;
                    uint32_t spec[4];
                    for (uint32_t f = 0u; f < 4u; ++f)
                        spec[f] = maskEnable ? ((row0 >> (f * 2u)) & 0x3u) : 0u;
                    uint8_t *out = rec + 16u;
                    for (uint32_t i = 0u; i < writeVectorCount; ++i)
                    {
                        const uint8_t *srcVec = srcBase + i * bytesPerVector;
                        const uint32_t destVec = (vuAddr + i * cl) & 0x3FFu;
                        uint32_t dec[4] = {0u, 0u, 0u, 0u};
                        ps2_vif_unpack_fast::decodeVector(vl, static_cast<uint32_t>(components), srcVec,
                                                          zeroExtend, uv1UnpackQwAligned, srcBase, i,
                                                          bytesPerVector, uv1DataStartPos, sizeBytes,
                                                          dec);
                        uint32_t vals[4];
                        for (uint32_t f = 0u; f < 4u; ++f)
                            vals[f] = (spec[f] == 0u) ? dec[f] : vif1_regs.row[f];
                        const uint16_t q16 = static_cast<uint16_t>(destVec);
                        const uint16_t m16 = 0xFFFFu;
                        std::memcpy(out, &q16, 2u);
                        std::memcpy(out + 2u, &m16, 2u);
                        std::memcpy(out + 4u, vals, 16u);
                        out += 20u;
                    }
                    const ps2_mtvu::VifRec r{fastSize, K::Masked, 0u,
                                             static_cast<uint16_t>(writeVectorCount), 0u, 0u};
                    std::memcpy(rec, &r, sizeof(r));
                    ps2_mtvu::vifStageCommit(fastSize, false);
                }
            }
            else if (m_vu1Data && totalBytes > 0 && pos + totalBytes <= sizeBytes)
            {
                // One Masked record: <= writeVectorCount entries of 20 bytes.
                const uint32_t maxSize = (16u + writeVectorCount * 20u + 15u) & ~15u;
                uint8_t *rec = ps2_mtvu::vifStageReserve(maxSize);
                uint8_t *out = rec ? rec + 16u : nullptr;
                uint32_t entries = 0u;
                auto emit = [&](uint32_t qw, uint32_t byteMask, const uint32_t (&vals)[4])
                {
                    if (!out || byteMask == 0u)
                        return;
                    const uint16_t q16 = static_cast<uint16_t>(qw);
                    const uint16_t m16 = static_cast<uint16_t>(byteMask);
                    std::memcpy(out, &q16, 2u);
                    std::memcpy(out + 2u, &m16, 2u);
                    std::memcpy(out + 4u, vals, 16u);
                    out += 20u;
                    ++entries;
                };

                const uint8_t *srcBase = data + pos;
                uint32_t srcIndex = 0u;
                const bool wlPow2 = (wl & (wl - 1u)) == 0u;
                const uint32_t wlShift =
                    wlPow2 ? static_cast<uint32_t>(std::countr_zero(wl)) : 0u;
                const uint32_t wlMask = wl - 1u;
                const bool clGeWl = (cl >= wl);
                for (uint32_t writeIndex = 0; writeIndex < writeVectorCount; ++writeIndex)
                {
                    const uint32_t cyclePos =
                        wlPow2 ? (writeIndex & wlMask) : (writeIndex % wl);
                    const bool sourceAvailable = clGeWl || (cyclePos < cl);

                    uint32_t destVec = 0;
                    if (clGeWl)
                    {
                        const uint32_t wlGroup =
                            wlPow2 ? (writeIndex >> wlShift) : (writeIndex / wl);
                        destVec = (vuAddr + wlGroup * cl + cyclePos) & 0x3FFu;
                    }
                    else
                    {
                        destVec = (vuAddr + writeIndex) & 0x3FFu;
                    }

                    uint32_t destOff = destVec * 16u;
                    if (destOff + 16u > PS2_VU1_DATA_SIZE)
                    {
                        if (sourceAvailable && srcIndex < sourceVectorCount)
                            ++srcIndex;
                        continue;
                    }

                    // Every lane a handled format decodes is set below before
                    // use; unhandled/undecoded lanes are never stored.
                    uint32_t decompressed[4] = {0u, 0u, 0u, 0u};
                    bool decoded = false;

                    const uint8_t *srcVec = nullptr;
                    uint32_t uv1MySrc = 0u;
                    if (sourceAvailable && srcIndex < sourceVectorCount)
                    {
                        srcVec = srcBase + srcIndex * bytesPerVector;
                        uv1MySrc = srcIndex;
                        ++srcIndex;
                        decoded = true;
                    }

                    auto extend16 = [&](uint16_t raw) -> uint32_t
                    {
                        if (zeroExtend)
                            return static_cast<uint32_t>(raw);
                        return static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(raw)));
                    };

                    auto extend8 = [&](uint8_t raw) -> uint32_t
                    {
                        if (zeroExtend)
                            return static_cast<uint32_t>(raw);
                        return static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(raw)));
                    };

                    bool handledFormat = true;
                    if (!decoded)
                    {
                        handledFormat = false;
                    }
                    else if (vl == 0u)
                    {
                        if (components == 1)
                        {
                            uint32_t scalar = 0;
                            std::memcpy(&scalar, srcVec, sizeof(scalar));
                            decompressed[0] = scalar;
                            decompressed[1] = scalar;
                            decompressed[2] = scalar;
                            decompressed[3] = scalar;
                        }
                        else
                        {
                            const uint32_t limit = (components > 4) ? 4u : static_cast<uint32_t>(components);
                            for (uint32_t c = 0; c < limit; ++c)
                            {
                                uint32_t scalar = 0;
                                std::memcpy(&scalar, srcVec + c * 4u, sizeof(scalar));
                                decompressed[c] = scalar;
                            }
                        }
                    }
                    else if (vl == 1u)
                    {
                        if (components == 1)
                        {
                            uint16_t raw = 0;
                            std::memcpy(&raw, srcVec, sizeof(raw));
                            const uint32_t scalar = extend16(raw);
                            decompressed[0] = scalar;
                            decompressed[1] = scalar;
                            decompressed[2] = scalar;
                            decompressed[3] = scalar;
                        }
                        else
                        {
                            const uint32_t limit = (components > 4) ? 4u : static_cast<uint32_t>(components);
                            for (uint32_t c = 0; c < limit; ++c)
                            {
                                uint16_t raw = 0;
                                std::memcpy(&raw, srcVec + c * 2u, sizeof(raw));
                                decompressed[c] = extend16(raw);
                            }
                        }
                    }
                    else if (vl == 2u)
                    {
                        if (components == 1)
                        {
                            const uint32_t scalar = extend8(srcVec[0]);
                            decompressed[0] = scalar;
                            decompressed[1] = scalar;
                            decompressed[2] = scalar;
                            decompressed[3] = scalar;
                        }
                        else
                        {
                            const uint32_t limit = (components > 4) ? 4u : static_cast<uint32_t>(components);
                            for (uint32_t c = 0; c < limit; ++c)
                            {
                                decompressed[c] = extend8(srcVec[c]);
                            }
                        }
                    }
                    else if (vl == 3u && vn == 3u)
                    {
                        uint16_t packed = 0;
                        std::memcpy(&packed, srcVec, sizeof(packed));
                        decompressed[0] = (packed & 0x1Fu) << 3;
                        decompressed[1] = ((packed >> 5) & 0x1Fu) << 3;
                        decompressed[2] = ((packed >> 10) & 0x1Fu) << 3;
                        decompressed[3] = ((packed >> 15) & 0x01u) << 7;
                    }
                    else
                    {
                        handledFormat = false;
                    }

                    // UV1 Part 2: PCSX2 V2/V3 lane rules (see processVIF1DataImpl).
                    if (decoded && handledFormat && (components == 2 || components == 3))
                    {
                        if (components == 2)
                        {
                            decompressed[2] = decompressed[0];
                            if (vl == 0u && uv1UnpackQwAligned)
                                decompressed[3] = 0u;
                            else
                                decompressed[3] = decompressed[1];
                        }
                        else
                        {
                            const uint32_t uv1ReadLen = (vl == 0u) ? 4u : ((vl == 1u) ? 2u : 1u);
                            const uint64_t uv1ReadOff = static_cast<uint64_t>(uv1MySrc + 1u) *
                                                        static_cast<uint64_t>(bytesPerVector);
                            const uint64_t uv1Avail = static_cast<uint64_t>(sizeBytes - uv1DataStartPos);
                            const uint32_t uv1Phase = static_cast<uint32_t>(
                                (static_cast<uint64_t>(uv1DataStartPos) + uv1ReadOff) & 15u);
                            uint32_t uv1W = 0u;
                            if (uv1Phase + uv1ReadLen <= 16u && uv1ReadOff + uv1ReadLen <= uv1Avail)
                            {
                                const uint8_t *uv1Next = srcBase + uv1ReadOff;
                                if (vl == 0u)
                                {
                                    std::memcpy(&uv1W, uv1Next, sizeof(uv1W));
                                }
                                else if (vl == 1u)
                                {
                                    uint16_t uv1Raw = 0u;
                                    std::memcpy(&uv1Raw, uv1Next, sizeof(uv1Raw));
                                    uv1W = extend16(uv1Raw);
                                }
                                else
                                {
                                    uv1W = extend8(uv1Next[0]);
                                }
                            }
                            decompressed[3] = uv1W;
                        }
                    }

                    // Unknown compressed format fallback: the legacy raw copy
                    // of the first bytesPerVector bytes (a byte-masked store).
                    if (!handledFormat && decoded && !maskEnable && (vif1_regs.mode == 0u || vif1_regs.mode == 3u))
                    {
                        const uint32_t copyBytes = (bytesPerVector < 16u) ? bytesPerVector : 16u;
                        uint32_t raw[4] = {0u, 0u, 0u, 0u};
                        std::memcpy(raw, srcVec, copyBytes);
                        emit(destVec, (copyBytes >= 16u) ? 0xFFFFu : ((1u << copyBytes) - 1u), raw);
                        continue;
                    }

                    const bool canAdd = (vl != 3u || vn != 3u);
                    const uint32_t mode = vif1_regs.mode & 3u;
                    const uint32_t colIdx = (cyclePos > 3u) ? 3u : cyclePos;
                    const uint32_t maskCycle = (cyclePos > 3u) ? 3u : cyclePos;

                    uint32_t vals[4] = {0u, 0u, 0u, 0u};
                    uint32_t byteMask = 0u;
                    for (uint32_t field = 0u; field < 4u; ++field)
                    {
                        uint32_t maskSpec = 0u;
                        if (maskEnable)
                        {
                            const uint32_t shift = ((maskCycle * 4u) + field) * 2u;
                            maskSpec = (vif1_regs.mask >> shift) & 0x3u;
                        }

                        if (!decoded && maskSpec == 0u)
                            maskSpec = 1u;

                        uint32_t writeVal = 0u;
                        if (maskSpec == 0u)
                        {
                            if (!handledFormat)
                                continue; // today: stores the lane's old value back
                            writeVal = decompressed[field];
                            if (canAdd && (mode == 1u || mode == 2u))
                            {
                                writeVal = writeVal + vif1_regs.row[field];
                                if (mode == 2u)
                                    vif1_regs.row[field] = writeVal;
                            }
                        }
                        else if (maskSpec == 1u)
                        {
                            writeVal = vif1_regs.row[field];
                        }
                        else if (maskSpec == 2u)
                        {
                            writeVal = vif1_regs.col[colIdx];
                        }
                        else
                        {
                            continue; // write-protect
                        }

                        vals[field] = writeVal;
                        byteMask |= 0xFu << (field * 4u);
                    }
                    emit(destVec, byteMask, vals);
                }
                if (rec)
                {
                    const uint32_t size = (16u + entries * 20u + 15u) & ~15u;
                    const ps2_mtvu::VifRec r{size, K::Masked, 0u, static_cast<uint16_t>(entries), 0u, 0u};
                    std::memcpy(rec, &r, sizeof(r));
                    ps2_mtvu::vifStageCommit(size, false);
                }
            }
            pos += totalBytes;

            if (pos > sizeBytes)
                break;
            continue;
        }
        else
        {
            continue;
        }
    }
}
