#ifndef PS2_SYSCALLS_H
#define PS2_SYSCALLS_H

#include "ps2_runtime.h"
#include "ps2_call_list.h"
#include <mutex>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

std::string translatePs2Path(const char *ps2Path);

namespace ps2_syscalls
{
#define PS2_DECLARE_SYSCALL(name) void name(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    PS2_SYSCALL_LIST(PS2_DECLARE_SYSCALL)
#undef PS2_DECLARE_SYSCALL
    // UPR1: GetRomName is no longer a name-stubbed handler (upstream #244
    // recompiles ABI-incompatible variants), but pinned codegen (SSX 3) still
    // calls this HLE version directly.
    void GetRomName(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);

    void iDeleteSema(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void EnableIntcHandler(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void DisableIntcHandler(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void EnableDmacHandler(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void DisableDmacHandler(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);

    bool dispatchNumericSyscall(uint32_t syscallNumber, uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void dispatchDmacHandlersForCause(uint8_t *rdram, PS2Runtime *runtime, uint32_t cause);
    void initializeGuestKernelState(uint8_t *rdram, PS2Runtime *runtime);
    void TODO(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t encodedSyscallId);
    uint64_t GetCurrentVSyncTick(PS2Runtime *runtime);
    void WaitVSyncTick(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, int fixedResult = -1);

    // snd-seam-1 family A: `sceSifSendCmd` argument decoding. The public
    // six-argument entry reads (cid, packet, size, src, dst, esize) from
    // a0/a1/a2/a3+stack; the private `0x426078` entry reads the same fields
    // from seven registers (cid, mode, pkt, size, src, dst, esize). Moved
    // here from the SSX 3 SND header: it is generic SendCmd ABI knowledge,
    // needed by both the kernel entry points and the module's SND tests.
    struct SendCmdArgs
    {
        uint32_t cid = 0;
        uint32_t packet = 0;
        uint32_t packetSize = 0;
        uint32_t srcExtra = 0;
        uint32_t dstExtra = 0;
        uint32_t extraSize = 0;
    };

    inline SendCmdArgs decodeSendCmdArgs(const uint32_t *gpr, size_t count)
    {
        if (!gpr || count < 11u)
            return {};
        return {gpr[4], gpr[6], gpr[7], gpr[8], gpr[9], gpr[10]};
    }
}

#endif // PS2_SYSCALLS_H

