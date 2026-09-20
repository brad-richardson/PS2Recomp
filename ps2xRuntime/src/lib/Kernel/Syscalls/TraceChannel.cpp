#include "TraceChannel.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <chrono>

// T18 runtime EE-syscall trace channel (Brief 2).
//
// Name table is PCSX2 R5900::bios[0..0x7F] verbatim
// (REF pcsx2/R5900OpcodeImpl.cpp:84, @1275b25a; all 42 T4-census names
// match it). Call-number derivation replicates PCSX2 SYSCALL()
// (REF pcsx2/R5900OpcodeImpl.cpp:906-917): negative $v1 negates,
// positive takes the low byte, logged as lowercase %x.

namespace
{
    const char *const kEeBiosNames[128] = {
        // 0x00
        "RFU000_FullReset", "ResetEE", "SetGsCrt", "RFU003",
        "Exit", "RFU005", "LoadExecPS2", "ExecPS2",
        "RFU008", "RFU009", "AddSbusIntcHandler", "RemoveSbusIntcHandler",
        "Interrupt2Iop", "SetVTLBRefillHandler", "SetVCommonHandler", "SetVInterruptHandler",
        // 0x10
        "AddIntcHandler", "RemoveIntcHandler", "AddDmacHandler", "RemoveDmacHandler",
        "_EnableIntc", "_DisableIntc", "_EnableDmac", "_DisableDmac",
        "_SetAlarm", "_ReleaseAlarm", "_iEnableIntc", "_iDisableIntc",
        "_iEnableDmac", "_iDisableDmac", "_iSetAlarm", "_iReleaseAlarm",
        // 0x20
        "CreateThread", "DeleteThread", "StartThread", "ExitThread",
        "ExitDeleteThread", "TerminateThread", "iTerminateThread", "DisableDispatchThread",
        "EnableDispatchThread", "ChangeThreadPriority", "iChangeThreadPriority", "RotateThreadReadyQueue",
        "iRotateThreadReadyQueue", "ReleaseWaitThread", "iReleaseWaitThread", "GetThreadId",
        // 0x30
        "ReferThreadStatus", "iReferThreadStatus", "SleepThread", "WakeupThread",
        "_iWakeupThread", "CancelWakeupThread", "iCancelWakeupThread", "SuspendThread",
        "iSuspendThread", "ResumeThread", "iResumeThread", "JoinThread",
        "RFU060", "RFU061", "EndOfHeap", "RFU063",
        // 0x40
        "CreateSema", "DeleteSema", "SignalSema", "iSignalSema",
        "WaitSema", "PollSema", "iPollSema", "ReferSemaStatus",
        "iReferSemaStatus", "RFU073", "SetOsdConfigParam", "GetOsdConfigParam",
        "GetGsHParam", "GetGsVParam", "SetGsHParam", "SetGsVParam",
        // 0x50
        "RFU080_CreateEventFlag", "RFU081_DeleteEventFlag",
        "RFU082_SetEventFlag", "RFU083_iSetEventFlag",
        "RFU084_ClearEventFlag", "RFU085_iClearEventFlag",
        "RFU086_WaitEventFlag", "RFU087_PollEventFlag",
        "RFU088_iPollEventFlag", "RFU089_ReferEventFlagStatus",
        "RFU090_iReferEventFlagStatus", "RFU091_GetEntryAddress",
        "EnableIntcHandler_iEnableIntcHandler",
        "DisableIntcHandler_iDisableIntcHandler",
        "EnableDmacHandler_iEnableDmacHandler",
        "DisableDmacHandler_iDisableDmacHandler",
        // 0x60
        "KSeg0", "EnableCache", "DisableCache", "GetCop0",
        "FlushCache", "RFU101", "CpuConfig", "iGetCop0",
        "iFlushCache", "RFU105", "iCpuConfig", "sceSifStopDma",
        "SetCPUTimerHandler", "SetCPUTimer", "SetOsdConfigParam2", "GetOsdConfigParam2",
        // 0x70
        "GsGetIMR_iGsGetIMR", "GsGetIMR_iGsPutIMR", "SetPgifHandler", "SetVSyncFlag",
        "RFU116", "print", "sceSifDmaStat_isceSifDmaStat", "sceSifSetDma_isceSifSetDma",
        "sceSifSetDChain_isceSifSetDChain", "sceSifSetReg", "sceSifGetReg", "ExecOSD",
        "Deci2Call", "PSMode", "MachineType", "GetMemorySize",
    };

    struct TraceChannelState
    {
        bool enabled = false;
        FILE *file = nullptr;
        std::mutex mutex;
        std::chrono::steady_clock::time_point openedAt;

        TraceChannelState()
        {
            const char *path = std::getenv("PS2X_TRACE_SYSCALLS");
            if (path == nullptr || path[0] == '\0')
            {
                return;
            }
            FILE *f = std::fopen(path, "w");
            if (f == nullptr)
            {
                std::fprintf(stderr, "[trace:syscalls] open FAILED path=%s\n", path);
                return;
            }
            // Line-buffered: every line hits the file even under SIGTERM
            // (proof boots end by timeout); no signal handler needed.
            std::setvbuf(f, nullptr, _IOLBF, 0);
            file = f;
            openedAt = std::chrono::steady_clock::now();
            enabled = true;
            std::fprintf(stderr, "[trace:syscalls] open path=%s\n", path);
        }

        ~TraceChannelState()
        {
            if (file != nullptr)
            {
                std::fclose(file);
                file = nullptr;
            }
        }
    };

    TraceChannelState &channelState()
    {
        static TraceChannelState state;
        return state;
    }
}

namespace ps2_syscalls
{
    void traceChannelEmit(uint32_t syscallId)
    {
        TraceChannelState &state = channelState();
        if (!state.enabled)
        {
            return;
        }
        // PCSX2 SYSCALL() derivation, verbatim semantics.
        const int32_t signedId = static_cast<int32_t>(syscallId);
        const uint8_t call = (signedId < 0)
                                 ? static_cast<uint8_t>(-signedId)
                                 : static_cast<uint8_t>(syscallId);
        char rfuName[16];
        const char *name = nullptr;
        if (call < 128u)
        {
            name = kEeBiosNames[call];
        }
        else
        {
            // PCSX2's table has no entries >= 0x80 (null there); emit an
            // RFUddd fallback in PCSX2's decimal-RFU convention so the line
            // shape still parses. Tabled as a deviation if it ever fires.
            std::snprintf(rfuName, sizeof(rfuName), "RFU%03u", call);
            name = rfuName;
        }
        const double elapsed = std::chrono::duration<double>(
                                   std::chrono::steady_clock::now() - state.openedAt)
                                   .count();
        std::lock_guard<std::mutex> lock(state.mutex);
        std::fprintf(state.file, "[%8.4f] Bios    : Bios call: %s (%x)\n",
                     elapsed, name, call);
    }
}
