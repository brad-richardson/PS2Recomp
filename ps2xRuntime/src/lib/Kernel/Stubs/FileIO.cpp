#include "Common.h"
#include "ps2_e3.h"
#include "FileIO.h"

namespace ps2_stubs
{
    void sceFsDbChk(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceFsDbChk", rdram, ctx, runtime);
    }

    void sceFsIntrSigSema(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceFsIntrSigSema", rdram, ctx, runtime);
    }

    void sceFsSemExit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceFsSemExit", rdram, ctx, runtime);
    }

    void sceFsSemInit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceFsSemInit", rdram, ctx, runtime);
    }

    void sceFsSigSema(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceFsSigSema", rdram, ctx, runtime);
    }

    void close(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ps2_syscalls::fioClose(rdram, ctx, runtime);
    }

    void fstat(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        uint32_t statAddr = getRegU32(ctx, 5);
        if (uint8_t *statBuf = getMemPtr(rdram, statAddr))
        {
            ps2_e3::Tap e3t = ps2_e3::tapBegin(rdram, statAddr, 128); // E3b R3c C8
            std::memset(statBuf, 0, 128);
            ps2_e3::tapEnd(std::move(e3t), "fstat", rdram, "fill=0");
            setReturnS32(ctx, 0);
            return;
        }
        ps2_log::emitDrop("stub/fstat", "error");
        setReturnS32(ctx, -1);
    }

    void lseek(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ps2_syscalls::fioLseek(rdram, ctx, runtime);
    }

    void open(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ps2_syscalls::fioOpen(rdram, ctx, runtime);
    }

    void read(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ps2_syscalls::fioRead(rdram, ctx, runtime);
    }

    void sceClose(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ps2_syscalls::fioClose(rdram, ctx, runtime);
    }

    void sceFsInit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void sceFsReset(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void sceIoctl(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const int32_t cmd = static_cast<int32_t>(getRegU32(ctx, 5));
        const uint32_t argAddr = getRegU32(ctx, 6);

        // HTCI wait paths poll sceIoctl(fd, 1, &state) and expect state to move
        // away from 1 once host-side I/O is no longer busy.
        if (cmd == 1 && argAddr != 0u)
        {
            uint8_t *argPtr = getMemPtr(rdram, argAddr);
            if (!argPtr)
            {
                ps2_log::emitDrop("stub/sceIoctl", "error");
                setReturnS32(ctx, -1);
                return;
            }

            const uint32_t ready = 0u;
            ps2_e3::Tap e3t = ps2_e3::tapBegin(rdram, argAddr, sizeof(ready)); // E3b R3c C9
            std::memcpy(argPtr, &ready, sizeof(ready));
            ps2_e3::tapEnd(std::move(e3t), "ioctl", rdram, "cmd=1");
        }

        setReturnS32(ctx, 0);
    }

    void sceLseek(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ps2_syscalls::fioLseek(rdram, ctx, runtime);
    }

    void sceOpen(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ps2_syscalls::fioOpen(rdram, ctx, runtime);
    }

    void sceRead(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ps2_syscalls::fioRead(rdram, ctx, runtime);
    }

    void sceWrite(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ps2_syscalls::fioWrite(rdram, ctx, runtime);
    }

    void stat(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t statAddr = getRegU32(ctx, 5);
        uint8_t *statBuf = getMemPtr(rdram, statAddr);
        if (!statBuf)
        {
            ps2_log::emitDrop("stub/stat", "error");
            setReturnS32(ctx, -1);
            return;
        }

        // Minimal fake stat payload: zeroed structure indicates a valid, readable file.
        ps2_e3::Tap e3t = ps2_e3::tapBegin(rdram, statAddr, 128); // E3b R3c C8
        std::memset(statBuf, 0, 128);
        ps2_e3::tapEnd(std::move(e3t), "stat", rdram, "fill=0");
        setReturnS32(ctx, 0);
    }

    void write(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ps2_syscalls::fioWrite(rdram, ctx, runtime);
    }

    void cvFsSetDefDev(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static int logCount = 0;
        if (logCount < 8)
        {
            RUNTIME_LOG("ps2_stub cvFsSetDefDev");
            ++logCount;
        }
        setReturnS32(ctx, 0);
    }
}
