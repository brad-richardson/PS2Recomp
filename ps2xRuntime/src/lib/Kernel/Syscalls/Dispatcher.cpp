#include "Common.h"
#include "Dispatcher.h"
#include "System.h"

#include <cstdlib>

namespace
{
    // P1c steady-state diagnostics, gated on PS2X_DIAG_PERIOD_MS (unset =
    // compiled in, nothing printed, callers pay only a counter increment).
    uint64_t diagPeriodMs()
    {
        static const uint64_t period = [] {
            if (const char *env = std::getenv("PS2X_DIAG_PERIOD_MS"))
            {
                if (env[0] != '\0')
                {
                    char *end = nullptr;
                    const unsigned long long parsed = std::strtoull(env, &end, 10);
                    if (end != env)
                    {
                        return static_cast<uint64_t>(parsed);
                    }
                }
            }
            return static_cast<uint64_t>(0);
        }();
        return period;
    }

    uint64_t diagNowMs()
    {
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                         std::chrono::steady_clock::now().time_since_epoch())
                                         .count());
    }

    struct SyscallDiagEntry
    {
        uint64_t count = 0;
        uint32_t firstPc = 0;
        uint32_t lastPc = 0;
    };

    std::unordered_map<uint32_t, SyscallDiagEntry> g_diagSyscallCounts;
    uint64_t g_diagSyscallLastMs = 0;
    uint64_t g_diagSyscallBlock = 0;
}

namespace ps2_syscalls
{
    // Flushes the pending syscall histogram when a period boundary has
    // passed. Called from the dispatch hook below and from the scheduler
    // tick so a quiet steady state still emits (possibly empty) blocks.
    void diagSyscallsPeriodicFlush()
    {
        const uint64_t period = diagPeriodMs();
        if (period == 0u)
        {
            return;
        }
        const uint64_t now = diagNowMs();
        if (g_diagSyscallLastMs == 0u)
        {
            g_diagSyscallLastMs = now;
            return;
        }
        if (now - g_diagSyscallLastMs < period)
        {
            return;
        }
        g_diagSyscallLastMs = now;
        std::vector<std::pair<uint32_t, SyscallDiagEntry>> sorted(g_diagSyscallCounts.begin(), g_diagSyscallCounts.end());
        std::sort(sorted.begin(), sorted.end(),
                  [](const auto &a, const auto &b) { return a.second.count > b.second.count; });
        std::cerr << "[diag:syscalls] block=" << g_diagSyscallBlock++
                  << " distinct=" << sorted.size()
                  << " period_ms=" << period << std::endl;
        for (size_t i = 0; i < sorted.size() && i < 20u; ++i)
        {
            std::cerr << "[diag:syscall] id=0x" << std::hex << sorted[i].first << std::dec
                      << " count=" << sorted[i].second.count
                      << " first=0x" << std::hex << sorted[i].second.firstPc
                      << " last=0x" << std::hex << sorted[i].second.lastPc << std::dec << std::endl;
        }
        g_diagSyscallCounts.clear();
    }

    bool dispatchNumericSyscall(uint32_t syscallNumber, uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static uint64_t s_diagTick = 0;
        ++s_diagTick;
        if (diagPeriodMs() != 0u)
        {
            const uint32_t callerPc = (ctx != nullptr) ? ctx->pc : 0u;
            SyscallDiagEntry &entry = g_diagSyscallCounts[syscallNumber];
            if (entry.count == 0u)
            {
                entry.firstPc = callerPc;
            }
            entry.lastPc = callerPc;
            ++entry.count;
            diagSyscallsPeriodicFlush();
        }

        if (dispatchSyscallOverride(syscallNumber, rdram, ctx, runtime))
        {
            return true;
        }

        switch (syscallNumber)
        {
        case 0x01:
            ResetEE(rdram, ctx, runtime);
            return true;
        case 0x02:
            GsSetCrt(rdram, ctx, runtime);
            return true;
        case 0x04:
            ExitThread(rdram, ctx, runtime);
            return true;
        case 0x10:
            AddIntcHandler(rdram, ctx, runtime);
            return true;
        case 0x11:
            RemoveIntcHandler(rdram, ctx, runtime);
            return true;
        case 0x12:
            AddDmacHandler(rdram, ctx, runtime);
            return true;
        case 0x13:
            RemoveDmacHandler(rdram, ctx, runtime);
            return true;
        case 0x14:
            EnableIntc(rdram, ctx, runtime);
            return true;
        case 0x15:
            DisableIntc(rdram, ctx, runtime);
            return true;
        case 0x16:
            EnableDmac(rdram, ctx, runtime);
            return true;
        case 0x17:
            DisableDmac(rdram, ctx, runtime);
            return true;
        case static_cast<uint32_t>(-0x1A):
            iEnableIntc(rdram, ctx, runtime);
            return true;
        case static_cast<uint32_t>(-0x1B):
            iDisableIntc(rdram, ctx, runtime);
            return true;
        case static_cast<uint32_t>(-0x1C):
            iEnableDmac(rdram, ctx, runtime);
            return true;
        case static_cast<uint32_t>(-0x1D):
            iDisableDmac(rdram, ctx, runtime);
            return true;
        case 0x18:
        case 0xFC:
            SetAlarm(rdram, ctx, runtime);
            return true;
        case 0x19:
        case 0xFE:
            CancelAlarm(rdram, ctx, runtime);
            return true;
        case static_cast<uint32_t>(-0x1E):
        case static_cast<uint32_t>(-0xFD):
            iSetAlarm(rdram, ctx, runtime);
            return true;
        case static_cast<uint32_t>(-0x1F):
        case static_cast<uint32_t>(-0xFF):
            iCancelAlarm(rdram, ctx, runtime);
            return true;
        case 0x20:
            CreateThread(rdram, ctx, runtime);
            return true;
        case 0x21:
            DeleteThread(rdram, ctx, runtime);
            return true;
        case 0x22:
            StartThread(rdram, ctx, runtime);
            return true;
        case 0x23:
            ExitThread(rdram, ctx, runtime);
            return true;
        case 0x24:
            ExitDeleteThread(rdram, ctx, runtime);
            return true;
        case 0x25:
        case static_cast<uint32_t>(-0x26):
            TerminateThread(rdram, ctx, runtime);
            return true;
        case 0x29:
            ChangeThreadPriority(rdram, ctx, runtime);
            return true;
        case static_cast<uint32_t>(-0x2A):
            iChangeThreadPriority(rdram, ctx, runtime);
            return true;
        case 0x2B:
            RotateThreadReadyQueue(rdram, ctx, runtime);
            return true;
        case static_cast<uint32_t>(-0x2C):
            iRotateThreadReadyQueue(rdram, ctx, runtime);
            return true;
        case 0x2D:
            ReleaseWaitThread(rdram, ctx, runtime);
            return true;
        case static_cast<uint32_t>(-0x2E):
            iReleaseWaitThread(rdram, ctx, runtime);
            return true;
        case 0x2F:
        case static_cast<uint32_t>(-0x2F):
            GetThreadId(rdram, ctx, runtime);
            return true;
        case 0x30:
            ReferThreadStatus(rdram, ctx, runtime);
            return true;
        case static_cast<uint32_t>(-0x31):
            iReferThreadStatus(rdram, ctx, runtime);
            return true;
        case 0x32:
            SleepThread(rdram, ctx, runtime);
            return true;
        case 0x33:
            WakeupThread(rdram, ctx, runtime);
            return true;
        case static_cast<uint32_t>(-0x34):
            iWakeupThread(rdram, ctx, runtime);
            return true;
        case 0x35:
            CancelWakeupThread(rdram, ctx, runtime);
            return true;
        case static_cast<uint32_t>(-0x36):
            iCancelWakeupThread(rdram, ctx, runtime);
            return true;
        case 0x37:
        case static_cast<uint32_t>(-0x38):
            SuspendThread(rdram, ctx, runtime);
            return true;
        case 0x39:
        case static_cast<uint32_t>(-0x3A):
            ResumeThread(rdram, ctx, runtime);
            return true;
        case 0x3C:
            SetupThread(rdram, ctx, runtime);
            return true;
        case 0x3D:
            SetupHeap(rdram, ctx, runtime);
            return true;
        case 0x3E:
            EndOfHeap(rdram, ctx, runtime);
            return true;
        case 0x40:
            CreateSema(rdram, ctx, runtime);
            return true;
        case 0x41:
            DeleteSema(rdram, ctx, runtime);
            return true;
        case static_cast<uint32_t>(-0x49):
            iDeleteSema(rdram, ctx, runtime);
            return true;
        case 0x42:
            SignalSema(rdram, ctx, runtime);
            return true;
        case static_cast<uint32_t>(-0x43):
            iSignalSema(rdram, ctx, runtime);
            return true;
        case 0x44:
            WaitSema(rdram, ctx, runtime);
            return true;
        case 0x45:
            PollSema(rdram, ctx, runtime);
            return true;
        case static_cast<uint32_t>(-0x46):
            iPollSema(rdram, ctx, runtime);
            return true;
        case 0x47:
            ReferSemaStatus(rdram, ctx, runtime);
            return true;
        case static_cast<uint32_t>(-0x48):
            iReferSemaStatus(rdram, ctx, runtime);
            return true;
        case 0x4A:
            SetOsdConfigParam(rdram, ctx, runtime);
            return true;
        case 0x4B:
            GetOsdConfigParam(rdram, ctx, runtime);
            return true;
        case 0x50:
            CreateEventFlag(rdram, ctx, runtime);
            return true;
        case 0x51:
            DeleteEventFlag(rdram, ctx, runtime);
            return true;
        case 0x52:
            SetEventFlag(rdram, ctx, runtime);
            return true;
        case static_cast<uint32_t>(-0x53):
            iSetEventFlag(rdram, ctx, runtime);
            return true;
        case 0x54:
            ClearEventFlag(rdram, ctx, runtime);
            return true;
        case static_cast<uint32_t>(-0x55):
            iClearEventFlag(rdram, ctx, runtime);
            return true;
        case 0x56:
            WaitEventFlag(rdram, ctx, runtime);
            return true;
        case 0x57:
            PollEventFlag(rdram, ctx, runtime);
            return true;
        case static_cast<uint32_t>(-0x58):
            iPollEventFlag(rdram, ctx, runtime);
            return true;
        case 0x59:
            ReferEventFlagStatus(rdram, ctx, runtime);
            return true;
        case static_cast<uint32_t>(-0x5A):
            iReferEventFlagStatus(rdram, ctx, runtime);
            return true;
        case 0x5A:
            Copy(rdram, ctx, runtime);
            return true;
        case 0x5B:
            GetEntryAddress(rdram, ctx, runtime);
            return true;
        case 0x5C:
        case static_cast<uint32_t>(-0x5C):
            EnableIntcHandler(rdram, ctx, runtime);
            return true;
        case 0x5D:
        case static_cast<uint32_t>(-0x5D):
            DisableIntcHandler(rdram, ctx, runtime);
            return true;
        case 0x5E:
        case static_cast<uint32_t>(-0x5E):
            EnableDmacHandler(rdram, ctx, runtime);
            return true;
        case 0x5F:
        case static_cast<uint32_t>(-0x5F):
            DisableDmacHandler(rdram, ctx, runtime);
            return true;
        case 0x61:
            EnableCache(rdram, ctx, runtime);
            return true;
        case 0x62:
            DisableCache(rdram, ctx, runtime);
            return true;
        case 0x64:
            FlushCache(rdram, ctx, runtime);
            return true;
        case 0x6E:
            SetOsdConfigParam2(rdram, ctx, runtime);
            return true;
        case 0x6F:
            GetOsdConfigParam2(rdram, ctx, runtime);
            return true;
        case 0x70:
            GsGetIMR(rdram, ctx, runtime);
            return true;
        case static_cast<uint32_t>(-0x70):
            iGsGetIMR(rdram, ctx, runtime);
            return true;
        case 0x71:
            GsPutIMR(rdram, ctx, runtime);
            return true;
        case static_cast<uint32_t>(-0x71):
            iGsPutIMR(rdram, ctx, runtime);
            return true;
        case 0x73:
            SetVSyncFlag(rdram, ctx, runtime);
            return true;
        case 0x74:
            SetSyscall(rdram, ctx, runtime);
            return true;
        case 0x76:
        case static_cast<uint32_t>(-0x76):
            ps2_stubs::sceSifDmaStat(rdram, ctx, runtime);
            return true;
        case 0x77:
        case static_cast<uint32_t>(-0x77):
            ps2_stubs::sceSifSetDma(rdram, ctx, runtime);
            return true;
        case 0x78:
        case static_cast<uint32_t>(-0x78):
            ps2_stubs::sceSifSetDChain(rdram, ctx, runtime);
            return true;
        case 0x7F:
            GetMemorySize(rdram, ctx, runtime);
            return true;
        case 0x82:
            InitTLB(rdram, ctx, runtime);
            return true;
        case 0x7C:
        case static_cast<uint32_t>(-0x7C):
            Deci2Call(rdram, ctx, runtime);
            return true;
        case 0x83:
            FindAddress(rdram, ctx, runtime);
            return true;
        case 0x85:
            SetMemoryMode(rdram, ctx, runtime);
            return true;
        default:
        {
            // P1w: unhandled syscall number. The guest gets no handler and
            // (unless an override was installed) no answer. Trace it.
            char dropArgs[64];
            std::snprintf(dropArgs, sizeof(dropArgs), "id=0x%x pc=0x%x", syscallNumber,
                          (ctx != nullptr) ? ctx->pc : 0u);
            ps2_log::emitDrop("dispatch/numeric", "unknown-syscall", dropArgs);
            return false;
        }
        }
    }
}
