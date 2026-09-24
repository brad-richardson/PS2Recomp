#include "Common.h"
#include "ps2_e3.h"
#include "RPC.h"
#include "../../ps2_iop_transport.h"
#include "game_overrides.h"
#include "ps2_park_snapshot.h"
#include "ps2_e41_trace.h"
#include "ps2_snd_spike.h"

namespace ps2_syscalls
{
    namespace
    {
        SifRpcDebugEvent makeRpcDebugEvent(const char *op, R5900Context *ctx, PS2Runtime *runtime)
        {
            SifRpcDebugEvent event{};
            event.op = op;
            event.pc = ctx ? ctx->pc : 0u;
            event.ra = ctx ? getRegU32(ctx, 31) : 0u;
            event.threadId = runtime ? static_cast<uint32_t>(runtime->eeScheduler().currentThreadId()) : 0u;
            return event;
        }

        void pushSifRpcDebugEventLocked(SifRpcDebugEvent event)
        {
            event.seq = ++g_sif_rpc_debug_next_seq;
            g_sif_rpc_debug_history[event.seq % kSifRpcDebugHistoryCount] = event;
        }

        void pushSifRpcDebugEvent(SifRpcDebugEvent event)
        {
            std::lock_guard<std::mutex> lock(g_rpc_mutex);
            pushSifRpcDebugEventLocked(event);
        }

        void fillRpcDebugPreview(const uint8_t *rdram, uint32_t addr, uint32_t size, uint8_t *preview, uint32_t &previewSize)
        {
            previewSize = 0u;
            if (!rdram || !addr || !preview || size == 0u)
            {
                return;
            }

            const uint32_t count = std::min<uint32_t>(size, static_cast<uint32_t>(kSifRpcDebugPreviewBytes));
            for (uint32_t i = 0; i < count; ++i)
            {
                const uint8_t *ptr = getConstMemPtr(rdram, addr + i);
                if (!ptr)
                {
                    break;
                }
                preview[i] = *ptr;
                ++previewSize;
            }
        }

        std::string formatRpcTraceBytes(const uint8_t *bytes, uint32_t count)
        {
            std::string out;
            if (!bytes || count == 0u)
            {
                return out;
            }

            char item[4] = {};
            for (uint32_t i = 0; i < count; ++i)
            {
                std::snprintf(item, sizeof(item), "%02X", bytes[i]);
                if (!out.empty())
                {
                    out.push_back(' ');
                }
                out += item;
            }
            return out;
        }

#ifndef PS2X_ENABLE_IOP_RPC_TRACE
#define PS2X_ENABLE_IOP_RPC_TRACE 1
#endif

#if PS2X_ENABLE_IOP_RPC_TRACE
        std::string loadedIopModuleTraceSummary()
        {
            std::lock_guard<std::mutex> lock(g_sif_module_mutex);
            std::string out;
            uint32_t count = 0u;
            for (const auto &entry : g_sif_modules_by_id)
            {
                const SifModuleRecord &module = entry.second;
                if (!module.loaded)
                {
                    continue;
                }

                if (!out.empty())
                {
                    out += "; ";
                }
                out += module.pathKey.empty() ? module.path : module.pathKey;
                ++count;
                if (count >= 6u)
                {
                    break;
                }
            }
            return out;
        }

        void logUnhandledRpcTrace(const SifRpcDebugEvent &event)
        {
            static std::unordered_set<uint64_t> loggedSignatures;
            if (std::strcmp(event.op ? event.op : "", "CallRpc") != 0)
            {
                return;
            }

            uint64_t signature = 1469598103934665603ull;
            auto mixSignature = [&](uint32_t value)
            {
                signature ^= static_cast<uint64_t>(value);
                signature *= 1099511628211ull;
            };
            mixSignature(event.sid);
            mixSignature(event.rpcNum);
            mixSignature(event.sendSize);
            mixSignature(event.recvSize);
            if (!loggedSignatures.insert(signature).second || loggedSignatures.size() > 128u)
            {
                return;
            }

            const std::string modules = loadedIopModuleTraceSummary();
            std::cerr << "[IOP/RPC trace:unhandled]"
                      << " sid=0x" << std::hex << event.sid
                      << " rpc=0x" << event.rpcNum
                      << " pc=0x" << event.pc
                      << " ra=0x" << event.ra
                      << " send=0x" << event.sendBuf << "/" << std::dec << event.sendSize
                      << " recv=0x" << std::hex << event.recvBuf << "/" << std::dec << event.recvSize
                      << " sendBytes=[" << formatRpcTraceBytes(event.sendPreview, event.sendPreviewSize) << "]"
                      << " loadedModules=[" << modules << "]"
                      << std::dec << std::endl;
        }
#endif

        bool signalRpcCompletionSema(PS2Runtime *runtime, uint32_t semaId)
        {
            if (!runtime || semaId == 0u || semaId > 0xFFFFu)
            {
                return false;
            }
            return runtime->eeScheduler().signalSemaphore(static_cast<int>(semaId), true) >= 0;
        }

    } // namespace

    namespace
    {
        // P1ac: SSX3-only gate for the SIF ready-handshake completion in
        // sceSifSendCmd below. Set by the ps2_game_overrides descriptor
        // (SLUS_207.72, entry 0x100008); the HLE process loads one ELF, so
        // a process-wide flag matches the registry's apply-once-at-load
        // model.
        std::atomic<bool> g_ssx3SifHandshakeEnabled{false};

        void applySsx3SifHandshake(PS2Runtime &runtime)
        {
            (void)runtime;
            g_ssx3SifHandshakeEnabled.store(true, std::memory_order_relaxed);
        }
    } // namespace

    PS2_REGISTER_GAME_OVERRIDE("ssx3-sif-handshake",
                               "SLUS_207.72",
                               0x00100008u,
                               0u,
                               applySsx3SifHandshake);

    void resetSsx3SifHandshakeForTesting()
    {
        g_ssx3SifHandshakeEnabled.store(false, std::memory_order_relaxed);
    }

    void SifStopModule(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const int32_t moduleId = static_cast<int32_t>(getRegU32(ctx, 4)); // $a0
        const uint32_t resultAddr = getRegU32(ctx, 7);                    // $a3 (int* result, optional)

        uint32_t refsLeft = 0;
        const bool knownModule = trackSifModuleStop(moduleId, &refsLeft);
        const int32_t ret = knownModule ? 0 : -1;

        if (resultAddr != 0)
        {
            int32_t *hostResult = reinterpret_cast<int32_t *>(getMemPtr(rdram, resultAddr));
            if (hostResult)
            {
                ps2_e3::Tap e3t = ps2_e3::tapBegin(rdram, resultAddr, sizeof(int32_t)); // E3b R3b B14
                *hostResult = knownModule ? 0 : -1;
                ps2_e3::tapEnd(std::move(e3t), "sif-stopmod", rdram, "-");
            }
        }

        if (knownModule)
        {
            std::string modulePath;
            {
                std::lock_guard<std::mutex> lock(g_sif_module_mutex);
                auto it = g_sif_modules_by_id.find(moduleId);
                if (it != g_sif_modules_by_id.end())
                {
                    modulePath = it->second.path;
                }
            }
            logSifModuleAction("stop", moduleId, modulePath, refsLeft);
        }

        setReturnS32(ctx, ret);
    }

    void SifLoadModule(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t pathAddr = getRegU32(ctx, 4); // $a0
        const std::string modulePath = readGuestCStringBounded(rdram, pathAddr, kMaxSifModulePathBytes);
        const int parkTid = runtime ? runtime->eeScheduler().currentThreadId() : 0;
        if (modulePath.empty())
        {
            ps2_log::emitDrop("syscall/SifLoadModule", "error");
            ps2_park::ParkRpcEvent parkLoad;
            parkLoad.op = "load";
            parkLoad.tid = parkTid;
            parkLoad.claimed = false;
            ps2_park::tallyRpcEvent(std::move(parkLoad));
            setReturnS32(ctx, -1);
            return;
        }

        const int32_t moduleId = trackSifModuleLoad(modulePath);
        if (moduleId <= 0)
        {
            ps2_log::emitDrop("syscall/SifLoadModule", "error");
            ps2_park::ParkRpcEvent parkLoad;
            parkLoad.op = "load";
            parkLoad.tid = parkTid;
            parkLoad.claimed = false;
            parkLoad.path = modulePath;
            ps2_park::tallyRpcEvent(std::move(parkLoad));
            setReturnS32(ctx, -1);
            return;
        }

        uint32_t refs = 0;
        {
            std::lock_guard<std::mutex> lock(g_sif_module_mutex);
            auto it = g_sif_modules_by_id.find(moduleId);
            if (it != g_sif_modules_by_id.end())
            {
                refs = it->second.refCount;
            }
        }
        logSifModuleAction("load", moduleId, modulePath, refs);

        // T1: every LoadModule call lands in the snapshot tally.
        ps2_park::ParkRpcEvent parkLoad;
        parkLoad.op = "load";
        parkLoad.sid = static_cast<uint32_t>(moduleId);
        parkLoad.tid = parkTid;
        parkLoad.claimed = true;
        parkLoad.path = modulePath;
        ps2_park::tallyRpcEvent(std::move(parkLoad));

        setReturnS32(ctx, moduleId);
    }

    void SifInitRpc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        std::lock_guard<std::mutex> lock(g_rpc_mutex);
        if (runtime)
        {
            PS2IopTransport::reset(runtime);
        }
        if (!g_rpc_initialized)
        {
            g_rpc_servers.clear();
            g_rpc_clients.clear();
            g_rpc_next_id = 1;
            g_rpc_packet_index = 0;
            g_rpc_server_index = 0;
            g_rpc_active_queue = 0;
            g_sif_rpc_debug_next_seq = 0;
            for (size_t i = 0; i < kSifRpcDebugHistoryCount; ++i)
            {
                g_sif_rpc_debug_history[i] = SifRpcDebugEvent{};
            }
            g_rpc_initialized = true;
            RUNTIME_LOG("[SifInitRpc] Initialized");
        }

        SifRpcDebugEvent event = makeRpcDebugEvent("InitRpc", ctx, runtime);
        event.result = 0;
        pushSifRpcDebugEventLocked(event);
        setReturnS32(ctx, 0);
    }

    void SifBindRpc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        uint32_t clientPtr = getRegU32(ctx, 4);
        uint32_t rpcId = getRegU32(ctx, 5);
        uint32_t mode = getRegU32(ctx, 6);

        t_SifRpcClientData *client = reinterpret_cast<t_SifRpcClientData *>(getMemPtr(rdram, clientPtr));

        if (!client)
        {
            SifRpcDebugEvent event = makeRpcDebugEvent("BindRpc", ctx, runtime);
            event.clientPtr = clientPtr;
            event.sid = rpcId;
            event.mode = mode;
            event.flags = kSifRpcDebugFlagMissingClient;
            event.result = -1;
            pushSifRpcDebugEvent(event);
            ps2_log::emitDrop("syscall/SifBindRpc", "error");
            ps2_park::ParkRpcEvent parkBind;
            parkBind.op = "bind";
            parkBind.sid = rpcId;
            parkBind.fno = mode;
            parkBind.tid = runtime ? runtime->eeScheduler().currentThreadId() : 0;
            parkBind.claimed = false;
            ps2_park::tallyRpcEvent(std::move(parkBind));
            setReturnS32(ctx, -1);
            return;
        }

        ps2_e3::Tap e3client = ps2_e3::tapBegin(rdram, clientPtr, sizeof(t_SifRpcClientData)); // E3b R3b B7
        client->command = 0;
        client->buf = 0;
        client->cbuf = 0;
        client->end_function = 0;
        client->end_param = 0;
        client->server = 0;
        client->hdr.pkt_addr = 0;
        client->hdr.sema_id = -1;
        client->hdr.mode = mode;

        uint32_t serverPtr = 0;
        bool parkServerPrebound = false;
        {
            std::lock_guard<std::mutex> lock(g_rpc_mutex);
            client->hdr.rpc_id = g_rpc_next_id++;
            auto it = g_rpc_servers.find(rpcId);
            if (it != g_rpc_servers.end())
            {
                serverPtr = it->second.sd_ptr;
                parkServerPrebound = true;
            }
            g_rpc_clients[clientPtr] = {};
            g_rpc_clients[clientPtr].sid = rpcId;
        }

        if (!serverPtr)
        {
            // Allocate a dummy server so bind loops can proceed.
            serverPtr = rpcAllocServerAddr(rdram);
            if (serverPtr)
            {
                t_SifRpcServerData *dummy = reinterpret_cast<t_SifRpcServerData *>(getMemPtr(rdram, serverPtr));
                if (dummy)
                {
                    ps2_e3::Tap e3t = ps2_e3::tapBegin(rdram, serverPtr, sizeof(*dummy)); // E3b R3b B7
                    std::memset(dummy, 0, sizeof(*dummy));
                    dummy->sid = static_cast<int>(rpcId);
                    ps2_e3::tapEnd(std::move(e3t), "sif-bind", rdram, "f=dummy");
                }
                std::lock_guard<std::mutex> lock(g_rpc_mutex);
                g_rpc_servers[rpcId] = {rpcId, serverPtr};
            }
        }

        if (serverPtr)
        {
            t_SifRpcServerData *sd = reinterpret_cast<t_SifRpcServerData *>(getMemPtr(rdram, serverPtr));
            client->server = serverPtr;
            client->buf = sd ? sd->buf : 0;
            client->cbuf = sd ? sd->cbuf : 0;
        }
        else
        {
            client->server = 0;
            client->buf = 0;
            client->cbuf = 0;
        }
        ps2_e3::tapEnd(std::move(e3client), "sif-bind", rdram, "f=client");

        SifRpcDebugEvent event = makeRpcDebugEvent("BindRpc", ctx, runtime);
        event.clientPtr = clientPtr;
        event.serverPtr = serverPtr;
        event.sid = rpcId;
        event.mode = mode;
        event.result = 0;
        pushSifRpcDebugEvent(event);
        // T1: claimed = a server was already registered for this sid
        // (vs the dummy allocated above so the bind loop proceeds).
        ps2_park::ParkRpcEvent parkBind;
        parkBind.op = "bind";
        parkBind.sid = rpcId;
        parkBind.fno = mode;
        parkBind.tid = runtime ? runtime->eeScheduler().currentThreadId() : 0;
        parkBind.claimed = parkServerPrebound;
        ps2_park::tallyRpcEvent(std::move(parkBind));
        setReturnS32(ctx, 0);
    }

    void SifCallRpc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        std::lock_guard<std::recursive_mutex> rpcCallLock(g_sif_call_rpc_mutex);

        const uint32_t clientPtr = getRegU32(ctx, 4);
        const uint32_t rpcNum = getRegU32(ctx, 5);
        const uint32_t mode = getRegU32(ctx, 6);
        const uint32_t sendBuf = getRegU32(ctx, 7);
        const uint32_t stackPointer = getRegU32(ctx, 29);

        const uint32_t sendSizeRegisters = getRegU32(ctx, 8);
        const uint32_t receiveBufferRegisters = getRegU32(ctx, 9);
        const uint32_t receiveSizeRegisters = getRegU32(ctx, 10);
        const uint32_t endFunctionRegisters = getRegU32(ctx, 11);
        uint32_t endParameterRegisters = 0u;
        (void)readStackU32(rdram, stackPointer, 0x0u, endParameterRegisters);

        uint32_t sendSizeStack = 0u;
        uint32_t receiveBufferStack = 0u;
        uint32_t receiveSizeStack = 0u;
        uint32_t endFunctionStack = 0u;
        uint32_t endParameterStack = 0u;
        (void)readStackU32(rdram, stackPointer, 0x10u, sendSizeStack);
        (void)readStackU32(rdram, stackPointer, 0x14u, receiveBufferStack);
        (void)readStackU32(rdram, stackPointer, 0x18u, receiveSizeStack);
        (void)readStackU32(rdram, stackPointer, 0x1Cu, endFunctionStack);
        (void)readStackU32(rdram, stackPointer, 0x20u, endParameterStack);

        const auto looksLikeGuestPointer = [](uint32_t value)
        {
            if (value == 0u)
            {
                return true;
            }
            const uint32_t normalized = value & 0x1FFFFFFFu;
            return normalized >= 0x10000u && normalized < PS2_RAM_SIZE;
        };
        const auto looksLikeSize = [](uint32_t value)
        {
            return value <= 0x02000000u;
        };
        const auto plausiblePack = [&](uint32_t sendSize,
                                       uint32_t receiveBuffer,
                                       uint32_t receiveSize,
                                       uint32_t endFunction)
        {
            return looksLikeSize(sendSize) &&
                   looksLikeGuestPointer(receiveBuffer) &&
                   looksLikeSize(receiveSize) &&
                   (endFunction == 0u || looksLikeGuestPointer(endFunction));
        };

        const bool registerPackPlausible =
            plausiblePack(sendSizeRegisters,
                          receiveBufferRegisters,
                          receiveSizeRegisters,
                          endFunctionRegisters);
        const bool stackPackPlausible =
            plausiblePack(sendSizeStack,
                          receiveBufferStack,
                          receiveSizeStack,
                          endFunctionStack);

        uint32_t sidHint = 0u;
        {
            std::lock_guard<std::mutex> lock(g_rpc_mutex);
            const auto clientIt = g_rpc_clients.find(clientPtr);
            if (clientIt != g_rpc_clients.end())
            {
                sidHint = clientIt->second.sid;
            }
        }

        ps2x::iop::RpcAbi selectedAbi = ps2x::iop::RpcAbi::RuntimeDefault;
        {
            ps2x::iop::RpcAbiRequest request{};
            request.boundSid = sidHint;
            request.function = rpcNum;
            request.registers = {
                sendSizeRegisters,
                receiveBufferRegisters,
                receiveSizeRegisters,
                endFunctionRegisters,
                endParameterRegisters,
                registerPackPlausible,
            };
            request.stack = {
                sendSizeStack,
                receiveBufferStack,
                receiveSizeStack,
                endFunctionStack,
                endParameterStack,
                stackPackPlausible,
            };
            selectedAbi = PS2IopTransport::selectRpcAbi(runtime, request);
        }

        bool useRegisterConvention = selectedAbi != ps2x::iop::RpcAbi::Stack;
        if (selectedAbi == ps2x::iop::RpcAbi::RuntimeDefault && !registerPackPlausible && stackPackPlausible)
        {
            const bool registersHaveCallback = endFunctionRegisters != 0u && looksLikeGuestPointer(endFunctionRegisters);
            const bool stackHasCallback = endFunctionStack != 0u && looksLikeGuestPointer(endFunctionStack);
            if (!(registersHaveCallback && !stackHasCallback))
            {
                useRegisterConvention = false;
            }
        }
        else if (selectedAbi == ps2x::iop::RpcAbi::Registers)
        {
            useRegisterConvention = true;
        }

        const uint32_t sendSize = useRegisterConvention ? sendSizeRegisters : sendSizeStack;
        const uint32_t receiveBuffer = useRegisterConvention ? receiveBufferRegisters : receiveBufferStack;
        const uint32_t receiveSize = useRegisterConvention ? receiveSizeRegisters : receiveSizeStack;
        const uint32_t endFunction = useRegisterConvention ? endFunctionRegisters : endFunctionStack;
        const uint32_t endParameter = useRegisterConvention ? endParameterRegisters : endParameterStack;

        auto *client = reinterpret_cast<t_SifRpcClientData *>(getMemPtr(rdram, clientPtr));
        if (!client)
        {
            SifRpcDebugEvent event = makeRpcDebugEvent("CallRpc", ctx, runtime);
            event.clientPtr = clientPtr;
            event.sid = sidHint;
            event.rpcNum = rpcNum;
            event.mode = mode;
            event.sendBuf = sendBuf;
            event.sendSize = sendSize;
            event.recvBuf = receiveBuffer;
            event.recvSize = receiveSize;
            event.endFunc = endFunction;
            event.endParam = endParameter;
            event.flags = kSifRpcDebugFlagMissingClient | ((mode & kSifRpcModeNowait) ? kSifRpcDebugFlagNowait : 0u);
            event.result = -1;
            pushSifRpcDebugEvent(event);
            char dropArgs[64];
            std::snprintf(dropArgs, sizeof(dropArgs), "sid=0x%x rpc=0x%x mode=0x%x",
                          sidHint, rpcNum, mode);
            ps2_log::emitDrop("syscall/SifCallRpc", "missing-client", dropArgs);
            ps2_park::ParkRpcEvent parkCall;
            parkCall.op = "call";
            parkCall.sid = sidHint;
            parkCall.fno = rpcNum;
            parkCall.sendSize = sendSize;
            parkCall.recvSize = receiveSize;
            parkCall.tid = runtime ? runtime->eeScheduler().currentThreadId() : 0;
            parkCall.claimed = false;
            ps2_park::tallyRpcEvent(std::move(parkCall));
            setReturnS32(ctx, -1);
            return;
        }

        client->command = rpcNum;
        client->end_function = endFunction;
        client->end_param = endParameter;
        client->hdr.mode = mode;

        uint32_t sid = 0u;
        {
            std::lock_guard<std::mutex> lock(g_rpc_mutex);
            auto &state = g_rpc_clients[clientPtr];
            state.busy = true;
            state.last_rpc = rpcNum;
            sid = state.sid;
            if (sid != 0u)
            {
                const auto serverIt = g_rpc_servers.find(sid);
                if (serverIt != g_rpc_servers.end() &&
                    serverIt->second.sd_ptr != 0u)
                {
                    client->server = serverIt->second.sd_ptr;
                }
            }
        }

        ps2_snd_spike::noteRpc(rdram, sid, rpcNum, sendBuf, sendSize); // AU2 spike (default off)

        const uint32_t serverPtr = client->server;
        auto *server = serverPtr
                           ? reinterpret_cast<t_SifRpcServerData *>(getMemPtr(rdram, serverPtr))
                           : nullptr;
        if (server)
        {
            server->client = clientPtr;
            server->pkt_addr = client->hdr.pkt_addr;
            server->rpc_number = rpcNum;
            server->size = static_cast<int>(sendSize);
            server->recvbuf = receiveBuffer;
            server->rsize = static_cast<int>(receiveSize);
            server->rmode = ((mode & kSifRpcModeNowait) && endFunction == 0u) ? 0 : 1;
            server->rid = 0;

            if (server->buf != 0u && sendBuf != 0u && sendSize != 0u)
            {
                rpcCopyToRdram(rdram, server->buf, sendBuf, sendSize);
            }
        }

        ps2x::iop::RpcResult iopResult{};
        uint32_t completionSemaphore = static_cast<uint32_t>(client->hdr.sema_id);
        if (completionSemaphore == 0xFFFFFFFFu || completionSemaphore == 0u)
        {
            completionSemaphore = endParameter;
        }

        {
            ps2x::iop::RpcRequest request{};
            request.clientAddress = clientPtr;
            request.serverAddress = serverPtr;
            request.serverFunction = server ? server->func : 0u;
            request.serverBuffer = server ? server->buf : 0u;
            request.sid = sid;
            request.function = rpcNum;
            request.mode = mode;
            request.send = {sendBuf, sendSize};
            request.receive = {receiveBuffer, receiveSize};
            request.endFunction = endFunction;
            request.endParameter = endParameter;

            iopResult = PS2IopTransport::handleRpc(runtime, rdram, ctx, request);

            if (iopResult.signalNowaitCompletion &&
                (mode & kSifRpcModeNowait) != 0u)
            {
                (void)signalRpcCompletionSema(runtime, completionSemaphore);
            }
            if (iopResult.signalCompletion)
            {
                (void)signalRpcCompletionSema(runtime, completionSemaphore);
            }
        }

        uint32_t guestFunction = iopResult.guestFunction;
        uint32_t guestA0 = iopResult.guestArguments[0];
        uint32_t guestA1 = iopResult.guestArguments[1];
        uint32_t guestA2 = iopResult.guestArguments[2];
        uint32_t guestA3 = iopResult.guestArguments[3];
        uint32_t guestDefaultResult = iopResult.guestDefaultResultAddress;
        if (guestFunction == 0u && server && server->func != 0u &&
            iopResult.serverDispatchPolicy != ps2x::iop::ServerDispatchPolicy::Suppress)
        {
            guestFunction = server->func;
            guestA0 = rpcNum;
            guestA1 = server->buf;
            guestA2 = sendSize;
            guestDefaultResult = server->buf != 0u ? server->buf : receiveBuffer;
        }
        const bool serverDispatched = guestFunction != 0u && runtime->hasFunction(guestFunction);

        auto finishCall = [=](const R5900Context *guestResult, R5900Context &parent)
        {
            bool handled = iopResult.handled;
            uint32_t resultPointer = iopResult.resultAddress;
            bool copiedFallback = false;
            bool zeroedFallback = false;
            if (guestResult)
            {
                handled = true;
                resultPointer = getRegU32(guestResult, 2);
                if (resultPointer == 0u)
                {
                    resultPointer = guestDefaultResult;
                }
            }

            if (receiveBuffer != 0u && receiveSize != 0u)
            {
                if (handled && resultPointer != 0u && resultPointer != receiveBuffer)
                {
                    rpcCopyToRdram(rdram, receiveBuffer, resultPointer, receiveSize);
                }
                else if (!handled && sendBuf != 0u && sendSize != 0u && sendBuf != receiveBuffer)
                {
                    rpcCopyToRdram(rdram, receiveBuffer, sendBuf, std::min(sendSize, receiveSize));
                    copiedFallback = true;
                }
                else if (!handled)
                {
                    rpcZeroRdram(rdram, receiveBuffer, receiveSize);
                    zeroedFallback = true;
                }
            }

            auto completeClient = [=](R5900Context &base, bool callbackCompleted)
            {
                {
                    std::lock_guard<std::mutex> lock(g_rpc_mutex);
                    g_rpc_clients[clientPtr].busy = false;
                }
                SifRpcDebugEvent event = makeRpcDebugEvent("CallRpc", &base, runtime);
                event.clientPtr = clientPtr;
                event.serverPtr = serverPtr;
                event.sid = sid;
                event.rpcNum = rpcNum;
                event.mode = mode;
                event.sendBuf = sendBuf;
                event.sendSize = sendSize;
                event.recvBuf = receiveBuffer;
                event.recvSize = receiveSize;
                event.resultPtr = resultPointer;
                event.endFunc = endFunction;
                event.endParam = endParameter;
                event.semaId = completionSemaphore;
                event.flags =
                    ((mode & kSifRpcModeNowait) ? kSifRpcDebugFlagNowait : 0u) |
                    (iopResult.handled ? kSifRpcDebugFlagHandledByHle : 0u) |
                    (callbackCompleted ? kSifRpcDebugFlagCallback : 0u) |
                    (serverDispatched ? kSifRpcDebugFlagServerDispatch : 0u) |
                    (!handled ? kSifRpcDebugFlagUnhandled : 0u) |
                    (copiedFallback ? kSifRpcDebugFlagFallbackCopy : 0u) |
                    (zeroedFallback ? kSifRpcDebugFlagFallbackZero : 0u);
                fillRpcDebugPreview(rdram, sendBuf, sendSize, event.sendPreview, event.sendPreviewSize);
                fillRpcDebugPreview(rdram, receiveBuffer, receiveSize, event.recvPreview, event.recvPreviewSize);
                event.result = 0;
                if (!handled && runtime)
                    runtime->noteUnhandledRpc(sid, rpcNum);
#if PS2X_ENABLE_IOP_RPC_TRACE
                if ((event.flags & kSifRpcDebugFlagUnhandled) != 0u)
                {
                    logUnhandledRpcTrace(event);
                }
#endif
                pushSifRpcDebugEvent(event);
                // T1: one tally row per completed call; claimed = the HLE
                // handled it or a server function ran (same `handled`).
                ps2_park::ParkRpcEvent parkCall;
                parkCall.op = "call";
                parkCall.sid = sid;
                parkCall.fno = rpcNum;
                parkCall.sendSize = sendSize;
                parkCall.recvSize = receiveSize;
                parkCall.tid = runtime ? runtime->eeScheduler().currentThreadId() : 0;
                parkCall.claimed = handled;
                ps2_park::tallyRpcEvent(std::move(parkCall));
            };

            setReturnS32(&parent, 0);
            if (endFunction == 0u || iopResult.callbackPolicy == ps2x::iop::CallbackPolicy::Suppress)
            {
                completeClient(parent, endFunction != 0u);
                return;
            }

            uint32_t callbackFunction = endFunction;
            if (!runtime->hasFunction(callbackFunction) && callbackFunction >= 0x10000u &&
                runtime->hasFunction(callbackFunction - 0x10000u))
            {
                callbackFunction -= 0x10000u;
            }
            if (!runtime->hasFunction(callbackFunction))
            {
                (void)signalRpcCompletionSema(runtime, completionSemaphore);
                completeClient(parent, false);
                return;
            }

            GuestInvocation callback{};
            callback.kind = GuestInvocationKind::RpcCallback;
            callback.context = parent;
            callback.context.pc = callbackFunction;
            SET_GPR_U32(&callback.context, 4, endParameter);
            SET_GPR_U32(&callback.context, 29, runtime->eeScheduler().invocationStackTop());
            SET_GPR_U32(&callback.context, 31, 0u);
            callback.onComplete = [completeClient](const R5900Context &, R5900Context &base)
            {
                completeClient(base, true);
            };
            runtime->eeScheduler().invokeCurrent(std::move(callback));
        };

        if (serverDispatched)
        {
            GuestInvocation invocation{};
            invocation.kind = GuestInvocationKind::RpcCallback;
            invocation.context = *ctx;
            invocation.context.pc = guestFunction;
            SET_GPR_U32(&invocation.context, 4, guestA0);
            SET_GPR_U32(&invocation.context, 5, guestA1);
            SET_GPR_U32(&invocation.context, 6, guestA2);
            SET_GPR_U32(&invocation.context, 7, guestA3);
            SET_GPR_U32(&invocation.context, 29, runtime->eeScheduler().invocationStackTop());
            SET_GPR_U32(&invocation.context, 31, 0u);
            invocation.onComplete = [finishCall](const R5900Context &completed, R5900Context &parent)
            {
                finishCall(&completed, parent);
            };
            runtime->eeScheduler().invokeCurrent(std::move(invocation));
        }
        finishCall(nullptr, *ctx);
    }

    void SifRegisterRpc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        uint32_t sdPtr = getRegU32(ctx, 4);
        uint32_t sid = getRegU32(ctx, 5);
        uint32_t func = getRegU32(ctx, 6);
        uint32_t buf = getRegU32(ctx, 7);
        // stack args: cfunc, cbuf, qd...
        uint32_t sp = getRegU32(ctx, 29);
        uint32_t cfunc = 0;
        uint32_t cbuf = 0;
        uint32_t qd = 0;
        readStackU32(rdram, sp, 0x10, cfunc);
        readStackU32(rdram, sp, 0x14, cbuf);
        readStackU32(rdram, sp, 0x18, qd);

        t_SifRpcServerData *sd = reinterpret_cast<t_SifRpcServerData *>(getMemPtr(rdram, sdPtr));
        if (!sd)
        {
            SifRpcDebugEvent event = makeRpcDebugEvent("RegisterRpc", ctx, runtime);
            event.serverPtr = sdPtr;
            event.sid = sid;
            event.sendBuf = buf;
            event.recvBuf = cbuf;
            event.resultPtr = qd;
            event.endFunc = cfunc;
            event.flags = kSifRpcDebugFlagMissingClient;
            event.result = -1;
            pushSifRpcDebugEvent(event);
            ps2_log::emitDrop("syscall/SifRegisterRpc", "error");
            setReturnS32(ctx, -1);
            return;
        }

        ps2_e3::Tap e3sd = ps2_e3::tapBegin(rdram, sdPtr, sizeof(t_SifRpcServerData)); // E3b R3b B7
        sd->sid = static_cast<int>(sid);
        sd->func = func;
        sd->buf = buf;
        sd->size = 0;
        sd->cfunc = cfunc;
        sd->cbuf = cbuf;
        sd->size2 = 0;
        sd->client = 0;
        sd->pkt_addr = 0;
        sd->rpc_number = 0;
        sd->recvbuf = 0;
        sd->rsize = 0;
        sd->rmode = 0;
        sd->rid = 0;
        sd->base = qd;
        sd->link = 0;
        sd->next = 0;
        ps2_e3::tapEnd(std::move(e3sd), "sif-reg", rdram, "f=server");

        {
            std::lock_guard<std::mutex> lock(g_rpc_mutex);

            if (qd)
            {
                t_SifRpcDataQueue *queue = reinterpret_cast<t_SifRpcDataQueue *>(getMemPtr(rdram, qd));
                if (queue)
                {
                    if (!queue->link)
                    {
                        ps2_e3::Tap e3t = ps2_e3::tapBegin(rdram, qd, sizeof(t_SifRpcDataQueue)); // E3b R3b B7
                        queue->link = sdPtr;
                        ps2_e3::tapEnd(std::move(e3t), "sif-reg", rdram, "f=qlink");
                    }
                    else
                    {
                        uint32_t curPtr = queue->link;
                        for (int guard = 0; guard < 1024 && curPtr; ++guard)
                        {
                            t_SifRpcServerData *cur = reinterpret_cast<t_SifRpcServerData *>(getMemPtr(rdram, curPtr));
                            if (!cur)
                                break;
                            if (!cur->link)
                            {
                                ps2_e3::Tap e3t = // E3b R3b B7
                                    ps2_e3::tapBegin(rdram, curPtr, sizeof(t_SifRpcServerData));
                                cur->link = sdPtr;
                                ps2_e3::tapEnd(std::move(e3t), "sif-reg", rdram, "f=clink");
                                break;
                            }
                            if (cur->link == sdPtr)
                                break;
                            curPtr = cur->link;
                        }
                    }
                }
            }

            g_rpc_servers[sid] = {sid, sdPtr};
            for (auto &entry : g_rpc_clients)
            {
                if (entry.second.sid == sid)
                {
                    t_SifRpcClientData *cd = reinterpret_cast<t_SifRpcClientData *>(getMemPtr(rdram, entry.first));
                    if (cd)
                    {
                        ps2_e3::Tap e3t = // E3b R3b B7
                            ps2_e3::tapBegin(rdram, entry.first, sizeof(t_SifRpcClientData));
                        cd->server = sdPtr;
                        cd->buf = sd->buf;
                        cd->cbuf = sd->cbuf;
                        ps2_e3::tapEnd(std::move(e3t), "sif-reg", rdram, "f=client");
                    }
                }
            }
        }

        RUNTIME_LOG("[SifRegisterRpc] sid=0x" << std::hex << sid << " sd=0x" << sdPtr << std::dec);
        SifRpcDebugEvent event = makeRpcDebugEvent("RegisterRpc", ctx, runtime);
        event.serverPtr = sdPtr;
        event.sid = sid;
        event.sendBuf = buf;
        event.recvBuf = cbuf;
        event.resultPtr = qd;
        event.endFunc = cfunc;
        event.result = 0;
        pushSifRpcDebugEvent(event);
        setReturnS32(ctx, 0);
    }

    void SifCheckStatRpc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        uint32_t clientPtr = getRegU32(ctx, 4);
        std::lock_guard<std::mutex> lock(g_rpc_mutex);
        auto it = g_rpc_clients.find(clientPtr);
        if (it == g_rpc_clients.end())
        {
            setReturnS32(ctx, 0);
            return;
        }
        setReturnS32(ctx, it->second.busy ? 1 : 0);
    }

    void SifSetRpcQueue(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        uint32_t qdPtr = getRegU32(ctx, 4);
        int threadId = static_cast<int>(getRegU32(ctx, 5));

        t_SifRpcDataQueue *qd = reinterpret_cast<t_SifRpcDataQueue *>(getMemPtr(rdram, qdPtr));
        if (!qd)
        {
            ps2_log::emitDrop("syscall/SifSetRpcQueue", "error");
            setReturnS32(ctx, -1);
            return;
        }

        ps2_e3::Tap e3qd = ps2_e3::tapBegin(rdram, qdPtr, sizeof(t_SifRpcDataQueue)); // E3b R3b B7
        qd->thread_id = threadId;
        qd->active = 0;
        qd->link = 0;
        qd->start = 0;
        qd->end = 0;
        qd->next = 0;
        ps2_e3::tapEnd(std::move(e3qd), "sif-setq", rdram, "f=queue");

        {
            std::lock_guard<std::mutex> lock(g_rpc_mutex);
            if (!g_rpc_active_queue)
            {
                g_rpc_active_queue = qdPtr;
            }
            else
            {
                uint32_t curPtr = g_rpc_active_queue;
                for (int guard = 0; guard < 1024 && curPtr; ++guard)
                {
                    if (curPtr == qdPtr)
                        break;
                    t_SifRpcDataQueue *cur = reinterpret_cast<t_SifRpcDataQueue *>(getMemPtr(rdram, curPtr));
                    if (!cur)
                        break;
                    if (!cur->next)
                    {
                        ps2_e3::Tap e3t = // E3b R3b B7
                            ps2_e3::tapBegin(rdram, curPtr, sizeof(t_SifRpcDataQueue));
                        cur->next = qdPtr;
                        ps2_e3::tapEnd(std::move(e3t), "sif-setq", rdram, "f=cnext");
                        break;
                    }
                    curPtr = cur->next;
                }
            }
        }

        setReturnS32(ctx, 0);
    }

    void SifRemoveRpcQueue(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        uint32_t qdPtr = getRegU32(ctx, 4);
        if (!qdPtr)
        {
            setReturnU32(ctx, 0);
            return;
        }

        std::lock_guard<std::mutex> lock(g_rpc_mutex);
        if (!g_rpc_active_queue)
        {
            setReturnU32(ctx, 0);
            return;
        }

        if (g_rpc_active_queue == qdPtr)
        {
            t_SifRpcDataQueue *qd = reinterpret_cast<t_SifRpcDataQueue *>(getMemPtr(rdram, qdPtr));
            g_rpc_active_queue = qd ? qd->next : 0;
            setReturnU32(ctx, qdPtr);
            return;
        }

        uint32_t curPtr = g_rpc_active_queue;
        for (int guard = 0; guard < 1024 && curPtr; ++guard)
        {
            t_SifRpcDataQueue *cur = reinterpret_cast<t_SifRpcDataQueue *>(getMemPtr(rdram, curPtr));
            if (!cur)
                break;
            if (cur->next == qdPtr)
            {
                t_SifRpcDataQueue *rem = reinterpret_cast<t_SifRpcDataQueue *>(getMemPtr(rdram, qdPtr));
                ps2_e3::Tap e3t = ps2_e3::tapBegin(rdram, curPtr, sizeof(t_SifRpcDataQueue)); // E3b R3b B7
                cur->next = rem ? rem->next : 0;
                ps2_e3::tapEnd(std::move(e3t), "sif-remq", rdram, "f=splice");
                setReturnU32(ctx, qdPtr);
                return;
            }
            curPtr = cur->next;
        }

        setReturnU32(ctx, 0);
    }

    void SifRemoveRpc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        uint32_t sdPtr = getRegU32(ctx, 4);
        uint32_t qdPtr = getRegU32(ctx, 5);

        t_SifRpcDataQueue *qd = reinterpret_cast<t_SifRpcDataQueue *>(getMemPtr(rdram, qdPtr));
        if (!qd || !sdPtr)
        {
            setReturnU32(ctx, 0);
            return;
        }

        std::lock_guard<std::mutex> lock(g_rpc_mutex);

        if (qd->link == sdPtr)
        {
            t_SifRpcServerData *sd = reinterpret_cast<t_SifRpcServerData *>(getMemPtr(rdram, sdPtr));
            ps2_e3::Tap e3t = ps2_e3::tapBegin(rdram, qdPtr, sizeof(t_SifRpcDataQueue)); // E3b R3b B7
            qd->link = sd ? sd->link : 0;
            ps2_e3::tapEnd(std::move(e3t), "sif-rem", rdram, "f=qlink");
            if (sd)
            {
                ps2_e3::Tap e3s = ps2_e3::tapBegin(rdram, sdPtr, sizeof(t_SifRpcServerData));
                sd->link = 0;
                ps2_e3::tapEnd(std::move(e3s), "sif-rem", rdram, "f=slink");
            }
            setReturnU32(ctx, sdPtr);
            return;
        }

        uint32_t curPtr = qd->link;
        for (int guard = 0; guard < 1024 && curPtr; ++guard)
        {
            t_SifRpcServerData *cur = reinterpret_cast<t_SifRpcServerData *>(getMemPtr(rdram, curPtr));
            if (!cur)
                break;
            if (cur->link == sdPtr)
            {
                t_SifRpcServerData *sd = reinterpret_cast<t_SifRpcServerData *>(getMemPtr(rdram, sdPtr));
                ps2_e3::Tap e3t = ps2_e3::tapBegin(rdram, curPtr, sizeof(t_SifRpcServerData)); // E3b R3b B7
                cur->link = sd ? sd->link : 0;
                ps2_e3::tapEnd(std::move(e3t), "sif-rem", rdram, "f=clink");
                if (sd)
                {
                    ps2_e3::Tap e3s = ps2_e3::tapBegin(rdram, sdPtr, sizeof(t_SifRpcServerData));
                    sd->link = 0;
                    ps2_e3::tapEnd(std::move(e3s), "sif-rem", rdram, "f=slink");
                }
                setReturnU32(ctx, sdPtr);
                return;
            }
            curPtr = cur->link;
        }

        setReturnU32(ctx, 0);
    }

    void sceSifCallRpc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        SifCallRpc(rdram, ctx, runtime);
    }

    void sceSifSendCmd(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        // _sceSifSendCmd at 0x426078 uses the seven-register ABI
        // (cid, mode, pkt, size, src, dst, esize), regardless of caller.
        std::array<uint32_t, 11> gpr{};
        for (uint32_t i = 4; i <= 10; ++i)
            gpr[i] = getRegU32(ctx, static_cast<int>(i));
        const ps2_snd_spike::SendCmdArgs args = ps2_snd_spike::decodeSendCmdArgs(gpr.data(), gpr.size());
        const uint32_t cid = args.cid;
        uint32_t packetAddr = args.packet;
        uint32_t packetSize = args.packetSize;
        uint32_t srcExtra = args.srcExtra;
        uint32_t destExtra = args.dstExtra;
        uint32_t sizeExtra = args.extraSize;
        const uint32_t sendCmdRa = getRegU32(ctx, 31);
        if (runtime)
        {
            ps2_snd_spike::onSendCmd(rdram, ps2_e41_trace::lastVsyncTick(), sendCmdRa, cid, packetAddr, packetSize,
                                     [runtime](GuestInvocation invocation)
                                     { runtime->eeScheduler().queueInvocation(std::move(invocation)); });
        }

        if (sizeExtra > 0 && srcExtra && destExtra)
        {
            rpcCopyToRdram(rdram, destExtra, srcExtra, sizeExtra);
        }

        // P1ac: complete the SSX3 EE<->IOP SIF ready-handshake host-side.
        // The game sends SET_SREG(1,1) then spins on sregs[1] (guest
        // 0x52BE04); on HW the IOP's reply arrives via SIF0 DMA and EE
        // set_sreg writes the word, but the HLE has no IOP SIF peer (this
        // send is otherwise a no-op). Any nonzero exits the beqz poll;
        // the value mirrors the sent value and Sony's SetReg(RPCINIT,1).
        // T1: claimed = the host acted on this send (handshake applied).
        bool parkClaimed = false;
        if (g_ssx3SifHandshakeEnabled.load(std::memory_order_relaxed) &&
            cid == 0x80000001u)
        {
            constexpr uint32_t kSregIndexOffset = 4u * sizeof(uint32_t);
            const uint32_t *sregIndex = nullptr;
            if (packetAddr != 0u &&
                packetSize >= 5u * sizeof(uint32_t) &&
                packetAddr <= UINT32_MAX - kSregIndexOffset)
            {
                sregIndex = getEeGuestStruct<uint32_t>(rdram, packetAddr + kSregIndexOffset);
            }
            if (sregIndex != nullptr && *sregIndex == 1u)
            {
                constexpr uint32_t kSsx3Sregs1Addr = 0x52BE04u;
                uint8_t *sregs1 = getMemPtr(rdram, kSsx3Sregs1Addr);
                if (sregs1 != nullptr)
                {
                    const uint32_t one = 1u;
                    std::memcpy(sregs1, &one, sizeof(one));
                    parkClaimed = true;
                    static int handshakeCount = 0;
                    if (handshakeCount < 5)
                    {
                        std::cerr << "[sif-handshake] sregs[1]=1" << std::endl;
                        ++handshakeCount;
                    }
                }
            }
        }

        static int logCount = 0;
        if (logCount < 5)
        {
            RUNTIME_LOG("[sceSifSendCmd] cid=0x" << std::hex << cid
                                                 << " packet=0x" << packetAddr
                                                 << " psize=0x" << packetSize
                                                 << " extra=0x" << destExtra << std::dec << std::endl);
            ++logCount;
        }

        // T1: every SendCmd lands in the snapshot tally (the log line
        // above caps at 5; the tally does not).
        ps2_park::ParkRpcEvent parkSend;
        parkSend.op = "sendcmd";
        parkSend.sid = cid;
        parkSend.sendSize = packetSize;
        parkSend.recvSize = sizeExtra;
        parkSend.tid = runtime ? runtime->eeScheduler().currentThreadId() : 0;
        parkSend.claimed = parkClaimed;
        ps2_park::tallyRpcEvent(std::move(parkSend));

        // Return non-zero on success.
        setReturnS32(ctx, 1);
    }

    void sceRpcGetPacket(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        uint32_t queuePtr = getRegU32(ctx, 4);
        setReturnS32(ctx, static_cast<int32_t>(queuePtr));
    }
}
