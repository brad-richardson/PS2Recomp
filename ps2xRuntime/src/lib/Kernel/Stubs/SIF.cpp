#include "Common.h"
#include "ps2_e41_trace.h"
#include "ps2_snd_spike.h"
#include "SIF.h"
#include "../Syscalls/RPC.h"
#include "../../ps2_iop_transport.h"
#include "runtime/ps2_address.h"
#include "runtime/ee_scheduler.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <map>
#include <vector>

namespace ps2_stubs
{
    void sceSifCmdIntrHdlr(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceSifCmdIntrHdlr", rdram, ctx, runtime);
    }

    void sceSifLoadModule(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ps2_syscalls::SifLoadModule(rdram, ctx, runtime);
    }

    // UPR1: HLE IOP mode (SSX 3, PS2Runtime::hleIopMode) keeps the fork's SIF
    // semantics: the 0x04000000 SIF heap, EE-RAM aliasing for SIF DMA and
    // tracked-only module loads. Otherwise upstream's IOP emulator owns IOP
    // memory. A null runtime has no emulator, so it takes the HLE path.
    static bool sifUsesHleIop(const PS2Runtime *runtime)
    {
        return !runtime || runtime->hleIopMode();
    }

    void sceSifSendCmd(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t srcAddr = getRegU32(ctx, 7); // $a3
        const uint32_t dstAddr = readStackU32(rdram, ctx, 16);
        const uint32_t size = readStackU32(rdram, ctx, 20);
        if (size != 0u && srcAddr != 0u && dstAddr != 0u && sifUsesHleIop(runtime))
        {
            for (uint32_t i = 0; i < size; ++i)
            {
                const uint8_t *src = getConstMemPtr(rdram, srcAddr + i);
                uint8_t *dst = getMemPtr(rdram, dstAddr + i);
                if (!src || !dst)
                {
                    break;
                }
                *dst = *src;
            }
        }
        else if (size != 0u && srcAddr != 0u && dstAddr != 0u)
        {
            std::vector<uint8_t> payload(size);
            bool valid = true;
            for (uint32_t i = 0; i < size; ++i)
            {
                const uint8_t *src = getConstMemPtr(rdram, srcAddr + i);
                if (!src)
                {
                    valid = false;
                    break;
                }
                payload[i] = *src;
            }
            if (!valid || !runtime->writeIopMemory(dstAddr, payload.data(), payload.size()))
            {
                setReturnS32(ctx, 0);
                return;
            }
        }

        setReturnS32(ctx, 1);
    }

    namespace
    {
        struct Ps2SifDmaTransfer
        {
            uint32_t src = 0;
            uint32_t dest = 0;
            int32_t size = 0;
            int32_t attr = 0;
        };
        static_assert(sizeof(Ps2SifDmaTransfer) == 16u, "Unexpected SIF DMA descriptor size");

        std::mutex g_sifDmaTransferMutex;
        uint32_t g_nextSifDmaTransferId = 1u;
        std::mutex g_sifCmdStateMutex;
        std::mutex g_sifHeapMutex;
        std::unordered_map<uint32_t, uint32_t> g_sifRegs;
        std::unordered_map<uint32_t, uint32_t> g_sifSregs;
        struct SifCmdHandler
        {
            uint32_t function = 0u;
            uint32_t argument = 0u;
        };

        std::unordered_map<uint32_t, SifCmdHandler> g_sifCmdHandlers;
        std::map<uint32_t, uint32_t> g_sifHeapAllocations;
        std::array<uint8_t, kIopHeapLimit - kIopHeapBase> g_sifHeapStorage{};
        uint32_t g_sifCmdBuffer = 0u;
        uint32_t g_sifSysCmdBuffer = 0u;
        bool g_sifCmdInitialized = false;
        uint32_t g_sifGetRegLogCount = 0u;
        uint32_t g_sifSetRegLogCount = 0u;

        constexpr uint32_t kSifRegBootStatus = 0x4u;
        constexpr uint32_t kSifRegMainAddr = 0x80000000u;
        constexpr uint32_t kSifRegSubAddr = 0x80000001u;
        constexpr uint32_t kSifRegMsCom = 0x80000002u;
        constexpr uint32_t kSifBootReadyMask = 0x00020000u;

        void seedDefaultSifRegsLocked()
        {
            g_sifRegs.clear();
            g_sifSregs.clear();
            g_sifCmdHandlers.clear();
            g_sifCmdBuffer = 0u;
            g_sifSysCmdBuffer = 0u;
            g_sifCmdInitialized = false;
            g_sifGetRegLogCount = 0u;
            g_sifSetRegLogCount = 0u;

            g_sifRegs[kSifRegBootStatus] = kSifBootReadyMask;
            g_sifRegs[kSifRegMainAddr] = 0u;
            g_sifRegs[kSifRegSubAddr] = 0u;
            g_sifRegs[kSifRegMsCom] = 0u;
        }

        bool shouldTraceSifReg(uint32_t reg)
        {
            switch (reg)
            {
            case 0x2u:
            case 0x4u:
            case 0x80000000u:
            case 0x80000001u:
            case 0x80000002u:
                return true;
            default:
                return false;
            }
        }

        struct SifStateInitializer
        {
            SifStateInitializer()
            {
                std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
                seedDefaultSifRegsLocked();
            }
        } g_sifStateInitializer;

        uint32_t allocateSifDmaTransferId()
        {
            std::lock_guard<std::mutex> lock(g_sifDmaTransferMutex);
            uint32_t id = g_nextSifDmaTransferId++;
            if (id == 0u)
            {
                id = g_nextSifDmaTransferId++;
            }
            return id;
        }

        uint32_t alignIopHeapSize(uint32_t size)
        {
            return (size + (kIopHeapAlign - 1u)) & ~(kIopHeapAlign - 1u);
        }

        uint32_t allocateSifHeapBlock(uint32_t requestSize)
        {
            const uint32_t alignedSize = alignIopHeapSize(requestSize);
            if (alignedSize == 0u)
            {
                return 0u;
            }

            std::lock_guard<std::mutex> lock(g_sifHeapMutex);
            uint32_t candidate = kIopHeapBase;
            for (const auto &[addr, size] : g_sifHeapAllocations)
            {
                if (candidate + alignedSize <= addr)
                {
                    break;
                }

                const uint32_t blockEnd = alignIopHeapSize(addr + size);
                if (blockEnd > candidate)
                {
                    candidate = blockEnd;
                }
            }

            if (candidate < kIopHeapBase || candidate + alignedSize > kIopHeapLimit)
            {
                return 0u;
            }

            g_sifHeapAllocations[candidate] = alignedSize;
            std::fill_n(g_sifHeapStorage.data() + (candidate - kIopHeapBase),
                        alignedSize,
                        uint8_t{0});
            g_iopHeapNext = candidate + alignedSize;
            return candidate;
        }

        bool freeSifHeapBlock(uint32_t addr)
        {
            std::lock_guard<std::mutex> lock(g_sifHeapMutex);
            const auto it = g_sifHeapAllocations.find(addr);
            if (it == g_sifHeapAllocations.end())
            {
                return false;
            }

            g_sifHeapAllocations.erase(it);
            if (g_sifHeapAllocations.empty())
            {
                g_iopHeapNext = kIopHeapBase;
            }
            return true;
        }

        void resetSifHeapState()
        {
            std::lock_guard<std::mutex> lock(g_sifHeapMutex);
            g_sifHeapAllocations.clear();
            g_sifHeapStorage.fill(0u);
            g_iopHeapNext = kIopHeapBase;
        }

        bool isAllocatedSifHeapRangeLocked(uint32_t address, size_t size)
        {
            if (address < kIopHeapBase || address >= kIopHeapLimit || size > static_cast<size_t>(kIopHeapLimit - address))
            {
                return false;
            }

            auto it = g_sifHeapAllocations.upper_bound(address);
            if (it == g_sifHeapAllocations.begin())
            {
                return false;
            }
            --it;

            const uint64_t allocationEnd = static_cast<uint64_t>(it->first) + it->second;
            const uint64_t rangeEnd = static_cast<uint64_t>(address) + size;
            return address >= it->first && rangeEnd <= allocationEnd;
        }

        bool isCopyableGuestAddress(uint32_t addr)
        {
            if (Ps2AddressInRange(addr, PS2_SCRATCHPAD_BASE, PS2_SCRATCHPAD_SIZE))
            {
                return true;
            }

            if (addr < PS2_EE_UNCACHED_RAM_MIRROR_BASE)
            {
                return true;
            }

            if (Ps2IsUncachedRamMirrorAddress(addr))
            {
                return true;
            }

            if (Ps2IsKseg01Address(addr))
            {
                return true;
            }

            return false;
        }

        bool canCopyAddressRange(const uint8_t *rdram, uint32_t address, uint32_t sizeBytes)
        {
            if (isSifIopHeapRange(address, sizeBytes))
            {
                return true;
            }
            if (isSifIopHeapAddress(address) || !rdram)
            {
                return false;
            }
            if (sizeBytes == 0u)
            {
                return true;
            }
            if (sizeBytes - 1u > std::numeric_limits<uint32_t>::max() - address)
            {
                return false;
            }
            for (uint32_t i = 0u; i < sizeBytes; ++i)
            {
                const uint32_t byteAddress = address + i;
                if (!isCopyableGuestAddress(byteAddress) ||getConstMemPtr(rdram, byteAddress) == nullptr)
                {
                    return false;
                }
            }
            return true;
        }

        bool canCopyGuestByteRange(const uint8_t *rdram, uint32_t dstAddr, uint32_t srcAddr, uint32_t sizeBytes)
        {
            return canCopyAddressRange(rdram, srcAddr, sizeBytes) && canCopyAddressRange(rdram, dstAddr, sizeBytes);
        }

        bool copyGuestByteRange(uint8_t *rdram, uint32_t dstAddr, uint32_t srcAddr, uint32_t sizeBytes,
                                          const R5900Context *ctx = nullptr)
        {
            if (!canCopyGuestByteRange(rdram, dstAddr, srcAddr, sizeBytes))
            {
                return false;
            }

            if (sizeBytes == 0u)
            {
                return true;
            }

            // E3b R3b (before-slices; the destinationIsIop early-return below
            // writes the host-side mirror only, so it emits no row).

            const bool sourceIsIop = isSifIopHeapRange(srcAddr, sizeBytes);
            const bool destinationIsIop = isSifIopHeapRange(dstAddr, sizeBytes);
            if (sourceIsIop || destinationIsIop)
            {
                std::vector<uint8_t> payload(sizeBytes);
                if (sourceIsIop)
                {
                    if (!readSifIopHeap(srcAddr, payload.data(), payload.size()))
                    {
                        return false;
                    }
                }
                else
                {
                    for (uint32_t i = 0u; i < sizeBytes; ++i)
                    {
                        const uint8_t *src = getConstMemPtr(rdram, srcAddr + i);
                        if (!src)
                        {
                            return false;
                        }
                        payload[i] = *src;
                    }
                }

                if (destinationIsIop)
                {
                    return writeSifIopHeap(dstAddr, payload.data(), payload.size());
                }

                ps2TraceGuestRangeWrite(rdram, dstAddr, sizeBytes, "sifCopyGuestByteRange", nullptr);
                for (uint32_t i = 0u; i < sizeBytes; ++i)
                {
                    uint8_t *dst = getMemPtr(rdram, dstAddr + i);
                    if (!dst)
                    {
                        return false;
                    }
                    *dst = payload[i];
                }
                return true;
            }

            ps2TraceGuestRangeWrite(rdram, dstAddr, sizeBytes, "sifCopyGuestByteRange", nullptr);

            const uint64_t srcBegin = srcAddr;
            const uint64_t srcEnd = srcBegin + static_cast<uint64_t>(sizeBytes);
            const uint64_t dstBegin = dstAddr;
            const bool copyBackward = (dstBegin > srcBegin) && (dstBegin < srcEnd);

            if (copyBackward)
            {
                for (uint32_t i = sizeBytes; i > 0u; --i)
                {
                    const uint32_t index = i - 1u;
                    const uint8_t *src = getConstMemPtr(rdram, srcAddr + index);
                    uint8_t *dst = getMemPtr(rdram, dstAddr + index);
                    if (!src || !dst)
                    {
                        return false;
                    }
                    *dst = *src;
                }
                return true;
            }

            for (uint32_t i = 0; i < sizeBytes; ++i)
            {
                const uint8_t *src = getConstMemPtr(rdram, srcAddr + i);
                uint8_t *dst = getMemPtr(rdram, dstAddr + i);
                if (!src || !dst)
                {
                    return false;
                }
                *dst = *src;
            }
            return true;
        }
    }

    namespace
    {
        bool canAccessEeRange(const uint8_t *rdram, uint32_t address, uint32_t sizeBytes)
        {
            if (!rdram)
            {
                return false;
            }

            if (sizeBytes == 0u)
            {
                return true;
            }
            if (sizeBytes - 1u > std::numeric_limits<uint32_t>::max() - address)
            {
                return false;
            }
            for (uint32_t i = 0u; i < sizeBytes; ++i)
            {
                const uint32_t byteAddress = address + i;
                if (!isCopyableGuestAddress(byteAddress) || getConstMemPtr(rdram, byteAddress) == nullptr)
                {
                    return false;
                }
            }
            return true;
        }

        bool readEeRange(const uint8_t *rdram, uint32_t address, void *destination, uint32_t sizeBytes)
        {
            if ((!destination && sizeBytes != 0u) || !canAccessEeRange(rdram, address, sizeBytes))
                return false;
            auto *bytes = static_cast<uint8_t *>(destination);
            for (uint32_t i = 0u; i < sizeBytes; ++i)
            {
                const uint8_t *source = getConstMemPtr(rdram, address + i);
                if (!source)
                    return false;
                bytes[i] = *source;
            }
            return true;
        }

        bool writeEeRange(uint8_t *rdram, uint32_t address, const void *source, uint32_t sizeBytes)
        {
            if ((!source && sizeBytes != 0u) || !canAccessEeRange(rdram, address, sizeBytes))
                return false;
            ps2TraceGuestRangeWrite(rdram, address, sizeBytes, "SIF IOP-to-EE DMA", nullptr);
            const auto *bytes = static_cast<const uint8_t *>(source);
            for (uint32_t i = 0u; i < sizeBytes; ++i)
            {
                uint8_t *destination = getMemPtr(rdram, address + i);
                if (!destination)
                    return false;
                *destination = bytes[i];
            }
            return true;
        }
    }

    bool isSifIopHeapAddress(uint32_t address)
    {
        return address >= kIopHeapBase && address < kIopHeapLimit;
    }

    bool isSifIopHeapRange(uint32_t address, size_t size)
    {
        std::lock_guard<std::mutex> lock(g_sifHeapMutex);
        return isAllocatedSifHeapRangeLocked(address, size);
    }

    bool readSifIopHeap(uint32_t address, void *destination, size_t size)
    {
        if (!destination && size != 0u)
        {
            return false;
        }
        std::lock_guard<std::mutex> lock(g_sifHeapMutex);
        if (!isAllocatedSifHeapRangeLocked(address, size))
        {
            return false;
        }
        if (size != 0u)
        {
            std::memcpy(destination,
                        g_sifHeapStorage.data() + (address - kIopHeapBase),
                        size);
        }
        return true;
    }

    bool writeSifIopHeap(uint32_t address, const void *source, size_t size)
    {
        if (!source && size != 0u)
        {
            return false;
        }
        std::lock_guard<std::mutex> lock(g_sifHeapMutex);
        if (!isAllocatedSifHeapRangeLocked(address, size))
        {
            return false;
        }
        if (size != 0u)
        {
            std::memcpy(g_sifHeapStorage.data() + (address - kIopHeapBase),
                        source,
                        size);
        }
        return true;
    }

    bool zeroSifIopHeap(uint32_t address, size_t size)
    {
        std::lock_guard<std::mutex> lock(g_sifHeapMutex);
        if (!isAllocatedSifHeapRangeLocked(address, size))
        {
            return false;
        }
        if (size != 0u)
        {
            std::memset(g_sifHeapStorage.data() + (address - kIopHeapBase), 0, size);
        }
        return true;
    }

    void resetSifState()
    {
        std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
        seedDefaultSifRegsLocked();
        resetSifHeapState();
    }

    bool dispatchSifCommand(uint8_t *rdram,
                            PS2Runtime *runtime,
                            uint32_t commandId,
                            const void *packet,
                            size_t packetSize) noexcept
    {
        if (!rdram || !runtime || !packet || packetSize < 16u || packetSize > 112u)
            return false;

        SifCmdHandler registered{};
        {
            std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
            const auto handler = g_sifCmdHandlers.find(commandId);
            if (handler == g_sifCmdHandlers.end() || handler->second.function == 0u)
                return false;
            registered = handler->second;
        }

        if (!runtime->hasFunction(registered.function))
            return false;

        const uint32_t packetAddress = runtime->guestMalloc(static_cast<uint32_t>(packetSize), 16u);
        if (packetAddress == 0u)
            return false;

        uint8_t *const first = getMemPtr(rdram, packetAddress);
        uint8_t *const last = getMemPtr(rdram, packetAddress + static_cast<uint32_t>(packetSize - 1u));
        if (!first || !last || last < first || static_cast<size_t>(last - first) != packetSize - 1u)
        {
            runtime->guestFree(packetAddress);
            return false;
        }

        ps2TraceGuestRangeWrite(rdram, packetAddress, static_cast<uint32_t>(packetSize), "SIF command packet", nullptr);
        std::memcpy(first, packet, packetSize);

        try
        {
            GuestInvocation invocation{};
            invocation.kind = GuestInvocationKind::SifCommand;
            invocation.tag = commandId;
            invocation.context = runtime->cpu();
            invocation.context.pc = registered.function;
            SET_GPR_U32(&invocation.context, 4, packetAddress);
            SET_GPR_U32(&invocation.context, 5, registered.argument);
            SET_GPR_U32(&invocation.context, 6, 0u);
            SET_GPR_U32(&invocation.context, 7, 0u);
            SET_GPR_U32(&invocation.context, 29, 0u);
            SET_GPR_U32(&invocation.context, 31, 0u);
            invocation.onComplete = [runtime, packetAddress](const R5900Context &, R5900Context &)
            {
                runtime->guestFree(packetAddress);
            };
            runtime->eeScheduler().queueInvocation(std::move(invocation));
            return true;
        }
        catch (...)
        {
            runtime->guestFree(packetAddress);
            return false;
        }
    }

    void sceSifAddCmdHandler(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t cid = getRegU32(ctx, 4);
        const uint32_t handler = getRegU32(ctx, 5);
        const uint32_t argument = getRegU32(ctx, 6);
        if (ps2_snd_spike::noteAddCmdHandler(cid, handler, argument, getRegU32(ctx, 28)) && runtime)
            runtime->eeScheduler().startSoundClock();
        std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
        g_sifCmdHandlers[cid] = SifCmdHandler{handler, argument};
        setReturnS32(ctx, 0);
    }

    void sceSifAllocIopHeap(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;

        const uint32_t reqSize = getRegU32(ctx, 4);
        if (sifUsesHleIop(runtime))
        {
            setReturnU32(ctx, allocateSifHeapBlock(reqSize));
            return;
        }
        setReturnU32(ctx, runtime->allocateIopMemory(reqSize, 64u));
    }

    void sceSifAllocSysMemory(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;

        const uint32_t size = getRegU32(ctx, 5);
        if (sifUsesHleIop(runtime))
        {
            setReturnU32(ctx, allocateSifHeapBlock(size));
            return;
        }
        setReturnU32(ctx, runtime->allocateIopMemory(size, 64u));
    }

    void sceSifBindRpc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ps2_syscalls::SifBindRpc(rdram, ctx, runtime);
    }

    void sceSifCheckStatRpc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ps2_syscalls::SifCheckStatRpc(rdram, ctx, runtime);
    }

    void sceSifDmaStat(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        (void)getRegU32(ctx, 4); // trid

        // Transfers are applied immediately by sceSifSetDma in this runtime.
        ps2_log::emitDrop("stub/sceSifDmaStat", "error");
        setReturnS32(ctx, -1);
    }

    void sceSifExecRequest(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void sceSifExitCmd(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
        seedDefaultSifRegsLocked();
        setReturnS32(ctx, 0);
    }

    void sceSifExitRpc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void sceSifFreeIopHeap(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        const uint32_t addr = getRegU32(ctx, 4);
        const int freeResult = (sifUsesHleIop(runtime) ? freeSifHeapBlock(addr)
                                                      : runtime->freeIopMemory(addr))
                                   ? 0
                                   : -1;
        if (freeResult != 0)
        {
            char dropArgs[32];
            std::snprintf(dropArgs, sizeof(dropArgs), "addr=0x%x", addr);
            ps2_log::emitDrop("stub/sceSifFreeIopHeap", "error", dropArgs);
        }
        setReturnS32(ctx, freeResult);
    }

    void sceSifFreeSysMemory(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        const uint32_t addr = getRegU32(ctx, 4);
        const int freeResult = (sifUsesHleIop(runtime) ? freeSifHeapBlock(addr)
                                                      : runtime->freeIopMemory(addr))
                                   ? 0
                                   : -1;
        if (freeResult != 0)
        {
            char dropArgs[32];
            std::snprintf(dropArgs, sizeof(dropArgs), "addr=0x%x", addr);
            ps2_log::emitDrop("stub/sceSifFreeSysMemory", "error", dropArgs);
        }
        setReturnS32(ctx, freeResult);
    }

    void sceSifGetDataTable(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
        setReturnU32(ctx, g_sifCmdBuffer);
    }

    void sceSifGetIopAddr(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnU32(ctx, getRegU32(ctx, 4));
    }

    void sceSifGetNextRequest(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void sceSifGetOtherData(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t rdAddr = getRegU32(ctx, 4);
        const uint32_t srcAddr = getRegU32(ctx, 5);
        const uint32_t dstAddr = getRegU32(ctx, 6);
        const int32_t sizeSigned = static_cast<int32_t>(getRegU32(ctx, 7));

        if (sizeSigned <= 0)
        {
            setReturnS32(ctx, 0);
            return;
        }

        const uint32_t size = static_cast<uint32_t>(sizeSigned);
        if (size > PS2_RAM_SIZE)
        {
            static uint32_t warnCount = 0;
            if (warnCount < 32u)
            {
                std::cerr << "sceSifGetOtherData rejected oversized transfer size=0x"
                          << std::hex << size << std::dec << std::endl;
                ++warnCount;
            }
            ps2_log::emitDrop("stub/sceSifGetOtherData", "error");
            setReturnS32(ctx, -1);
            return;
        }

        if (runtime)
        {
            PS2IopTransport::notifyTransfer(runtime, rdram, {
                ps2x::iop::SifTransferKind::GetOtherData,
                ps2x::iop::SifTransferPhase::BeforeCopy,
                srcAddr,
                dstAddr,
                size,
            });
        }

        bool copied = false;
        if (sifUsesHleIop(runtime))
        {
            copied = copyGuestByteRange(rdram, dstAddr, srcAddr, size, ctx);
        }
        else
        {
            std::vector<uint8_t> payload(size);
            copied = runtime->isIopMemoryRange(srcAddr, size) &&
                     canAccessEeRange(rdram, dstAddr, size) &&
                     runtime->readIopMemory(srcAddr, payload.data(), payload.size()) &&
                     writeEeRange(rdram, dstAddr, payload.data(), size);
        }
        if (!copied)
        {
            static uint32_t warnCount = 0;
            if (warnCount < 32u)
            {
                PS2_IF_AGRESSIVE_LOGS({
                    std::cerr << "sceSifGetOtherData copy failed src=0x" << std::hex << srcAddr
                              << " dst=0x" << dstAddr
                              << " size=0x" << size
                              << std::dec << std::endl;
                });
                ++warnCount;
            }
            ps2_log::emitDrop("stub/sceSifGetOtherData", "error");
            setReturnS32(ctx, -1);
            return;
        }

        // SifRpcReceiveData_t keeps src/dest/size at offsets 0x10/0x14/0x18.
        if (uint8_t *rd = getMemPtr(rdram, rdAddr))
        {
            std::memcpy(rd + 0x10u, &srcAddr, sizeof(srcAddr));
        // E44 Part-3 EE watch (dev-only, default off).
            std::memcpy(rd + 0x14u, &dstAddr, sizeof(dstAddr));
            std::memcpy(rd + 0x18u, &size, sizeof(size));
            if (ps2_e41_trace::plantArmed()) // E41 plant watch
            {
                const uint64_t tick = ps2_e41_trace::lastVsyncTick();
                char src[32];
                std::snprintf(src, sizeof(src), "src=0x%x", srcAddr);
                ps2_e41_trace::notePlantRange(tick, dstAddr, size, rdram,
                                              "sif-dma", src, 0u);
                ps2_e41_trace::notePlantRange(tick, rdAddr + 0x10u, 12u, rdram,
                                              "sif-recvdata", "recvdata", 0u);
            }
        }

        if (runtime)
        {
            PS2IopTransport::notifyTransfer(runtime, rdram, {
                ps2x::iop::SifTransferKind::GetOtherData,
                ps2x::iop::SifTransferPhase::AfterCopy,
                srcAddr,
                dstAddr,
                size,
            });
        }

        setReturnS32(ctx, 0);
    }

    void sceSifGetReg(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t reg = getRegU32(ctx, 4);
        uint32_t value = 0u;
        bool shouldLog = false;
        {
            std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
            auto it = g_sifRegs.find(reg);
            if (it != g_sifRegs.end())
            {
                value = it->second;
            }
            shouldLog = shouldTraceSifReg(reg) && g_sifGetRegLogCount < 128u;
            if (shouldLog)
            {
                ++g_sifGetRegLogCount;
            }
        }
        if (shouldLog)
        {
            PS2_IF_AGRESSIVE_LOGS({
                auto flags = std::cerr.flags();
                std::cerr << "[sceSifGetReg] reg=0x" << std::hex << reg
                          << " value=0x" << value
                          << " pc=0x" << (ctx ? ctx->pc : 0u)
                          << " ra=0x" << (ctx ? getRegU32(ctx, 31) : 0u)
                          << std::dec << std::endl;
                std::cerr.flags(flags);
            });
        }
        setReturnU32(ctx, value);
    }

    void sceSifGetSreg(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t reg = getRegU32(ctx, 4);
        uint32_t value = 0u;
        {
            std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
            auto it = g_sifSregs.find(reg);
            if (it != g_sifSregs.end())
            {
                value = it->second;
            }
        }
        setReturnU32(ctx, value);
    }

    void sceSifInitCmd(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
        g_sifCmdInitialized = true;
        setReturnS32(ctx, 0);
    }

    void sceSifInitIopHeap(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        // With the IOP emulator the physical IOP allocator is initialized by
        // IopSubsystem::reset().
        if (sifUsesHleIop(runtime))
            resetSifHeapState();
        setReturnS32(ctx, 0);
    }

    void sceSifInitRpc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ps2_syscalls::SifInitRpc(rdram, ctx, runtime);
    }

    void sceSifIsAliveIop(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    void sceSifLoadElf(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ps2_syscalls::sceSifLoadElf(rdram, ctx, runtime);
    }

    void sceSifLoadElfPart(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ps2_syscalls::sceSifLoadElfPart(rdram, ctx, runtime);
    }

    void sceSifLoadFileReset(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void sceSifLoadIopHeap(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void sceSifLoadModuleBuffer(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ps2_syscalls::sceSifLoadModuleBuffer(rdram, ctx, runtime);
    }

    void sceSifRebootIop(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (!sifUsesHleIop(runtime))
            PS2IopTransport::reset(runtime);
        setReturnS32(ctx, 1);
    }

    void sceSifRegisterRpc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ps2_syscalls::SifRegisterRpc(rdram, ctx, runtime);
    }

    void sceSifRemoveCmdHandler(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t cid = getRegU32(ctx, 4);
        std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
        g_sifCmdHandlers.erase(cid);
        setReturnS32(ctx, 0);
    }

    void sceSifRemoveRpc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ps2_syscalls::SifRemoveRpc(rdram, ctx, runtime);
    }

    void sceSifRemoveRpcQueue(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ps2_syscalls::SifRemoveRpcQueue(rdram, ctx, runtime);
    }

    void sceSifResetIop(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (!sifUsesHleIop(runtime))
            PS2IopTransport::reset(runtime);
        setReturnS32(ctx, 1);
    }

    void sceSifRpcLoop(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void sceSifSetCmdBuffer(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t newBuffer = getRegU32(ctx, 4);
        uint32_t prev = 0u;
        {
            std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
            prev = g_sifCmdBuffer;
            g_sifCmdBuffer = newBuffer;
        }
        setReturnU32(ctx, prev);
    }

    void isceSifSetDChain(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        sceSifSetDChain(rdram, ctx, runtime);
    }

    void isceSifSetDma(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        sceSifSetDma(rdram, ctx, runtime);
    }

    void sceSifSetDChain(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        setReturnS32(ctx, 0);
    }

    void sceSifSetDma(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t dmatAddr = getRegU32(ctx, 4);
        const uint32_t count = getRegU32(ctx, 5);

        const uint32_t listAddr = getRegU32(ctx, 4);
        PS2_IF_AGRESSIVE_LOGS({
            std::cerr << "[sceSifSetDma:CALL] pc=0x" << std::hex << ctx->pc
                      << " ra=0x" << getRegU32(ctx, 31)
                      << " list=0x" << listAddr
                      << " count=" << std::dec << count
                      << std::endl;

            for (uint32_t i = 0; i < count; ++i)
            {
                const uint32_t desc = listAddr + i * 16;
                const uint32_t src = READ32(desc + 0);
                const uint32_t dst = READ32(desc + 4);
                const uint32_t size = READ32(desc + 8);
                const uint32_t attr = READ32(desc + 12);

                std::cerr << "[sceSifSetDma:DESC] i=" << i
                          << " src=0x" << std::hex << src
                          << " dst=0x" << dst
                          << " size=0x" << size
                          << " attr=0x" << attr
                          << " pc=0x" << ctx->pc
                          << " ra=0x" << getRegU32(ctx, 31)
                          << std::dec << std::endl;
            }
        });

        if (!dmatAddr || count == 0u || count > 32u)
        {
            setReturnS32(ctx, 0);
            return;
        }

        std::array<Ps2SifDmaTransfer, 32u> pending{};
        uint32_t pendingCount = 0u;
        bool ok = true;
        for (uint32_t i = 0; i < count; ++i)
        {
            const uint32_t entryAddr = dmatAddr + (i * static_cast<uint32_t>(sizeof(Ps2SifDmaTransfer)));
            const uint8_t *entry = getConstMemPtr(rdram, entryAddr);
            if (!entry)
            {
                ok = false;
                break;
            }

            Ps2SifDmaTransfer xfer{};
            std::memcpy(&xfer, entry, sizeof(xfer));
            if (xfer.size <= 0)
            {
                continue;
            }

            const uint32_t sizeBytes = static_cast<uint32_t>(xfer.size);
            if (sizeBytes > PS2_RAM_SIZE)
            {
                ok = false;
                break;
            }
            const bool rangeOk = sifUsesHleIop(runtime)
                                     ? canCopyGuestByteRange(rdram, xfer.dest, xfer.src, sizeBytes)
                                     : (canAccessEeRange(rdram, xfer.src, sizeBytes) &&
                                        runtime->isIopMemoryRange(xfer.dest, sizeBytes));
            if (!rangeOk)
            {
                ok = false;
                break;
            }

            pending[pendingCount++] = xfer;
        }

        if (ok)
        {
            for (uint32_t i = 0; i < pendingCount; ++i)
            {
                const Ps2SifDmaTransfer &xfer = pending[i];
                // SND HLE: SND-library transfers go to the spike's IOP
                // capture instead of low EE RDRAM (always on, AU10).
                if (ps2_snd_spike::onSetDma(rdram, ps2_e41_trace::lastVsyncTick(), getRegU32(ctx, 31),
                                            xfer.src, xfer.dest, static_cast<uint32_t>(xfer.size),
                                            static_cast<uint32_t>(xfer.attr)))
                {
                    continue;
                }
                if (runtime)
                {
                    PS2IopTransport::notifyTransfer(runtime, rdram, {
                        ps2x::iop::SifTransferKind::SetDma,
                        ps2x::iop::SifTransferPhase::BeforeCopy,
                        xfer.src,
                        xfer.dest,
                        static_cast<uint32_t>(xfer.size),
                    });
                }
                bool copied = false;
                if (sifUsesHleIop(runtime))
                {
                    copied = copyGuestByteRange(rdram, xfer.dest, xfer.src, static_cast<uint32_t>(xfer.size), ctx);
                }
                else
                {
                    const uint32_t sizeBytes = static_cast<uint32_t>(xfer.size);
                    std::vector<uint8_t> payload(sizeBytes);
                    copied = readEeRange(rdram, xfer.src, payload.data(), sizeBytes) &&
                             runtime->writeIopMemory(xfer.dest, payload.data(), payload.size());
                }
                if (!copied)
                {
                    ok = false;
                    break;
                }
                if (ps2_e41_trace::plantArmed()) // E41 plant watch
                {
                    char src[32];
                    std::snprintf(src, sizeof(src), "src=0x%x", xfer.src);
                    ps2_e41_trace::notePlantRange(ps2_e41_trace::lastVsyncTick(), xfer.dest,
                                                  static_cast<uint32_t>(xfer.size), rdram,
                                                  "sif-dma", src, 0u);
                }
                if (runtime)
                {
                    PS2IopTransport::notifyTransfer(runtime, rdram, {
                        ps2x::iop::SifTransferKind::SetDma,
                        ps2x::iop::SifTransferPhase::AfterCopy,
                        xfer.src,
                        xfer.dest,
                        static_cast<uint32_t>(xfer.size),
                    });
                }
            }
        }

        if (!ok)
        {
            static uint32_t warnCount = 0;
            if (warnCount < 32u)
            {
                PS2_IF_AGRESSIVE_LOGS({
                    std::cerr << "sceSifSetDma failed dmat=0x" << std::hex << dmatAddr
                              << " count=0x" << count
                              << std::dec << std::endl;
                });
                ++warnCount;
            }
            setReturnS32(ctx, 0);
            return;
        }

        ps2_syscalls::dispatchDmacHandlersForCause(rdram, runtime, 5u);

        setReturnS32(ctx, static_cast<int32_t>(allocateSifDmaTransferId()));
    }

    void sceSifSetIopAddr(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnU32(ctx, getRegU32(ctx, 5));
    }

    void sceSifSetReg(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t reg = getRegU32(ctx, 4);
        const uint32_t value = getRegU32(ctx, 5);
        uint32_t prev = 0u;
        bool shouldLog = false;
        {
            std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
            auto it = g_sifRegs.find(reg);
            if (it != g_sifRegs.end())
            {
                prev = it->second;
            }
            g_sifRegs[reg] = value;
            shouldLog = shouldTraceSifReg(reg) && g_sifSetRegLogCount < 128u;
            if (shouldLog)
            {
                ++g_sifSetRegLogCount;
            }
        }
        if (shouldLog)
        {
            PS2_IF_AGRESSIVE_LOGS({
                auto flags = std::cerr.flags();
                std::cerr << "[sceSifSetReg] reg=0x" << std::hex << reg
                          << " prev=0x" << prev
                          << " value=0x" << value
                          << " pc=0x" << (ctx ? ctx->pc : 0u)
                          << " ra=0x" << (ctx ? getRegU32(ctx, 31) : 0u)
                          << std::dec << std::endl;
                std::cerr.flags(flags);
            });
        }
        setReturnU32(ctx, prev);
    }

    void sceSifSetRpcQueue(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ps2_syscalls::SifSetRpcQueue(rdram, ctx, runtime);
    }

    void sceSifSetSreg(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t reg = getRegU32(ctx, 4);
        const uint32_t value = getRegU32(ctx, 5);
        uint32_t prev = 0u;
        {
            std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
            auto it = g_sifSregs.find(reg);
            if (it != g_sifSregs.end())
            {
                prev = it->second;
            }
            g_sifSregs[reg] = value;
        }
        setReturnU32(ctx, prev);
    }

    void sceSifSetSysCmdBuffer(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t newBuffer = getRegU32(ctx, 4);
        uint32_t prev = 0u;
        {
            std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
            prev = g_sifSysCmdBuffer;
            g_sifSysCmdBuffer = newBuffer;
        }
        setReturnU32(ctx, prev);
    }

    void sceSifStopDma(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void sceSifSyncIop(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    void sceSifWriteBackDCache(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }
}

// SS1 save states: SIF regs, command handlers and the IOP heap (its storage
// is guest-visible through readSifIopHeap and the IOP host).
#include "runtime/ps2_savestate.h"
namespace
{
    void sifSavestateSave(ps2_savestate::Writer &w)
    {
        using namespace ps2_stubs;
        std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
        std::lock_guard<std::mutex> heapLock(g_sifHeapMutex);
        w.u32(g_nextSifDmaTransferId);
        ps2_savestate::writeOrderedPod(w, g_sifRegs);
        ps2_savestate::writeOrderedPod(w, g_sifSregs);
        ps2_savestate::writeOrderedPod(w, g_sifCmdHandlers);
        w.u64(g_sifHeapAllocations.size());
        for (const auto &[addr, size] : g_sifHeapAllocations)
        {
            w.u32(addr);
            w.u32(size);
        }
        w.bytes(g_sifHeapStorage.data(), g_sifHeapStorage.size());
        w.u32(g_sifCmdBuffer);
        w.u32(g_sifSysCmdBuffer);
        w.b(g_sifCmdInitialized);
    }
    bool sifSavestateLoad(ps2_savestate::Reader &r)
    {
        using namespace ps2_stubs;
        // RBF1: accept v1 (pre-rebase) payloads. v1 stored each command
        // handler as one word (the function address); v2 stores
        // {function, argument}. The old code never dispatched through a
        // stored argument — sceSifAddCmdHandler kept only $a1 (the handler)
        // and sceSifSendCmd never invoked handlers — so a migrated handler
        // runs with argument 0, exactly what the old machine passed (nothing).
        // Layout is otherwise identical, so only the handler map reads
        // versioned; the loader stamps the file's version (1 or 2).
        const uint32_t version = ps2_savestate::loadingSectionVersion();
        if (version != 0u && version != 1u && version != 2u)
            return r.fail("stub:sif loader got version");
        std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
        std::lock_guard<std::mutex> heapLock(g_sifHeapMutex);
        g_nextSifDmaTransferId = r.u32();
        ps2_savestate::readOrderedPod(r, g_sifRegs);
        ps2_savestate::readOrderedPod(r, g_sifSregs);
        if (version == 1u)
        {
            ps2_savestate::readOrdered(r, g_sifCmdHandlers, [](ps2_savestate::Reader &rr, auto &e) {
                rr.pod(e.first);
                const uint32_t function = rr.u32();
                e.second.function = function;
                e.second.argument = 0u;
            });
        }
        else
        {
            ps2_savestate::readOrderedPod(r, g_sifCmdHandlers);
        }
        g_sifHeapAllocations.clear();
        const uint64_t n = r.count(1u << 20);
        for (uint64_t i = 0; i < n && r.ok(); ++i)
        {
            const uint32_t addr = r.u32();
            g_sifHeapAllocations[addr] = r.u32();
        }
        r.bytes(g_sifHeapStorage.data(), g_sifHeapStorage.size());
        g_sifCmdBuffer = r.u32();
        g_sifSysCmdBuffer = r.u32();
        g_sifCmdInitialized = r.b();
        return r.ok();
    }
    const bool kSifSavestateRegistered =
        ps2_savestate::registerSection("stub:sif", {2u, &sifSavestateSave, &sifSavestateLoad, nullptr, 1u});
}

namespace ps2_stubs
{
    bool sifCmdHandlerForTest(uint32_t commandId, uint32_t &function, uint32_t &argument)
    {
        std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
        const auto it = g_sifCmdHandlers.find(commandId);
        if (it == g_sifCmdHandlers.end())
            return false;
        function = it->second.function;
        argument = it->second.argument;
        return true;
    }
}
