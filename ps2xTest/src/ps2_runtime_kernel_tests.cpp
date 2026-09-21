#include "MiniTest.h"
#include "game_overrides.h"
#include "ps2_e3.h"
#include "ps2_log.h"
#include "ps2_park_snapshot.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "ps2_syscalls.h"
#include "ps2_stubs.h"
#include "runtime/ee_scheduler.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace ps2_syscalls;

namespace ps2_syscalls
{
    // Defined in ps2xRuntime Syscalls/RPC.cpp; TU-local forward declaration
    // (same pattern as the resetSifState declaration in ps2_runtime.cpp).
    void resetSsx3SifHandshakeForTesting();
    // Defined in ps2xRuntime Syscalls/Ssx3CopiedPayload.cpp (K1).
    void resetSsx3CopiedPayloadForTesting();
}

namespace
{
    constexpr uint32_t K_PARAM_ADDR = 0x1000u;
    constexpr uint32_t K_STATUS_ADDR = 0x1400u;

    constexpr int KE_OK = 0;
    constexpr int KE_ERROR = -1;
    constexpr int KE_ILLEGAL_THID = -406;
    constexpr int KE_UNKNOWN_THID = -407;
    constexpr int KE_UNKNOWN_SEMID = -408;
    constexpr int KE_DORMANT = -413;
    constexpr int KE_SEMA_OVF = -420;
    constexpr int KE_WAIT_DELETE = -425;
    constexpr int KE_RELEASE_WAIT = -418;
    constexpr uint32_t K_SEMA_WAIT_READY_ADDR = 0x1900u;

    constexpr int THS_WAIT = 0x04;
    constexpr int THS_SUSPEND = 0x08;
    constexpr int THS_WAITSUSPEND = 0x0C;
    constexpr int THS_DORMANT = 0x10;
    constexpr uint32_t TSW_SEMA = 2u;
    constexpr uint32_t TSW_EVENT = 3u;

    struct EeThreadStatusAbi
    {
        int32_t status;
        uint32_t func;
        uint32_t stack;
        int32_t stack_size;
        uint32_t gp_reg;
        int32_t initial_priority;
        int32_t current_priority;
        uint32_t attr;
        uint32_t option;
        uint32_t waitType;
        uint32_t waitId;
        uint32_t wakeupCount;
    };

    struct EeThreadCreateAbi
    {
        int32_t status;
        uint32_t func;
        uint32_t stack;
        int32_t stack_size;
        uint32_t gp_reg;
        int32_t initial_priority;
        int32_t current_priority;
        uint32_t attr;
        uint32_t option;
    };

    struct EeSemaStatus
    {
        int32_t count;
        int32_t max_count;
        int32_t init_count;
        int32_t wait_threads;
        uint32_t attr;
        uint32_t option;
    };

    static_assert(sizeof(EeThreadStatusAbi) == 0x30u, "Unexpected ee_thread_status_t size.");
    static_assert(sizeof(EeThreadCreateAbi) == 0x24u, "Unexpected ee_thread_t size.");
    static_assert(sizeof(EeSemaStatus) == 0x18u, "Unexpected ee_sema_t size.");

    void setRegU32(R5900Context &ctx, int reg, uint32_t value)
    {
        SET_GPR_U32(&ctx, reg, value);
    }

    int32_t getRegS32(const R5900Context &ctx, int reg)
    {
        return static_cast<int32_t>(::getRegU32(&ctx, reg));
    }

    void writeGuestU32(uint8_t *rdram, uint32_t addr, uint32_t value)
    {
        std::memcpy(rdram + addr, &value, sizeof(value));
    }

    void writeGuestWords(uint8_t *rdram, uint32_t addr, const uint32_t *words, size_t count)
    {
        for (size_t i = 0; i < count; ++i)
        {
            writeGuestU32(rdram, addr + static_cast<uint32_t>(i * sizeof(uint32_t)), words[i]);
        }
    }

    uint32_t readGuestU32(const uint8_t *rdram, uint32_t addr)
    {
        uint32_t value = 0;
        std::memcpy(&value, rdram + addr, sizeof(value));
        return value;
    }

    bool callSyscall(uint32_t syscallNumber, uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        return dispatchNumericSyscall(syscallNumber, rdram, ctx, runtime);
    }

    void overrideReturnHandler(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        setReturnU32(ctx, ::getRegU32(ctx, 4) + ::getRegU32(ctx, 5));
        ctx->pc = ::getRegU32(ctx, 31);
    }

    void overrideBrokenHandler(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        setReturnU32(ctx, 0xDEADBEEFu);
        ctx->pc = 0x12345678u;
    }

    void overrideRecursiveFindAddressHandler(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        runtime->handleSyscall(rdram, ctx, 0x83u);
        ctx->pc = ::getRegU32(ctx, 31);
    }

    void overrideKsegCompareHandler(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        auto getLowU64 = [](const R5900Context *cpu, int reg) -> uint64_t
        {
            return (reg == 0) ? 0u : static_cast<uint64_t>(_mm_extract_epi64(cpu->r[reg], 0));
        };
        auto setLowS32 = [](R5900Context *cpu, int reg, uint32_t value)
        {
            SET_GPR_S32(cpu, reg, value);
        };
        auto setLowU64 = [](R5900Context *cpu, int reg, uint64_t value)
        {
            SET_GPR_U64(cpu, reg, value);
        };

        const uint32_t nextA0 = static_cast<uint32_t>(::getRegU32(ctx, 4) + 4u);
        setLowS32(ctx, 4, nextA0);
        setLowU64(ctx, 2, (getLowU64(ctx, 4) < getLowU64(ctx, 5)) ? 1u : 0u);
        if (getLowU64(ctx, 2) == 0u)
        {
            ctx->r[4] = _mm_setzero_si128();
        }
        setLowU64(ctx, 2, getLowU64(ctx, 4));
        ctx->pc = ::getRegU32(ctx, 31);
    }

    constexpr uint64_t K_EXPECTED_UPPER64 = 0x1122334455667788ull;

    void overridePreserveUpper64Handler(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        const uint64_t hi = static_cast<uint64_t>(_mm_extract_epi64(ctx->r[4], 1));
        const uint64_t low = static_cast<uint64_t>(_mm_extract_epi64(ctx->r[4], 0));
        const uint64_t expectedLow = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(0x80000000u)));
        setReturnU32(ctx, (hi == K_EXPECTED_UPPER64 && low == expectedLow) ? 1u : 0u);
        ctx->pc = ::getRegU32(ctx, 31);
    }

    constexpr uint32_t K_OVERRIDE_ENTRY = 0x300500u;
    constexpr uint32_t K_OVERRIDE_RESUME = 0x300504u;
    constexpr uint32_t K_OVERRIDE_BLOCK_ENTRY = 0x300510u;
    constexpr uint32_t K_OVERRIDE_BLOCK_HANDLER = 0x300520u;
    constexpr uint32_t K_OVERRIDE_BLOCK_HANDLER_RESUME = 0x300524u;
    constexpr uint32_t K_OVERRIDE_BLOCK_DRIVER = 0x300530u;
    constexpr uint32_t K_OVERRIDE_BLOCK_BASE_RESUME = 0x300540u;
    constexpr uint32_t K_EXIT_MAIN = 0x300600u;
    constexpr uint32_t K_EXIT_HANDLER_A = 0x300610u;
    constexpr uint32_t K_EXIT_HANDLER_B = 0x300620u;
    constexpr uint32_t K_EXIT_OBSERVER = 0x300630u;
    uint32_t gOverrideSyscall = 0u;
    R5900Context gOverrideResult{};
    std::vector<int> gInvocationTrace;

    void schedulerOverrideEntry(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ctx->pc = K_OVERRIDE_RESUME;
        runtime->handleSyscall(rdram, ctx, gOverrideSyscall);
    }

    void schedulerOverrideResume(uint8_t *, R5900Context *ctx, PS2Runtime *runtime)
    {
        gOverrideResult = *ctx;
        ctx->pc = 0u;
        runtime->requestStop();
    }

    void schedulerBlockingOverrideEntry(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        gInvocationTrace.push_back(1);
        EeThreadCreateParams driver{};
        driver.entry = K_OVERRIDE_BLOCK_DRIVER;
        driver.stack = 0x1E000u;
        driver.stackSize = 0x1000u;
        driver.priority = 10;
        const int driverId = runtime->eeScheduler().createThread(driver);
        runtime->eeScheduler().startThread(driverId, 0u, *ctx, false);
        ctx->pc = K_OVERRIDE_BLOCK_BASE_RESUME;
        runtime->handleSyscall(rdram, ctx, gOverrideSyscall);
    }

    void schedulerBlockingOverrideHandler(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        gInvocationTrace.push_back(2);
        ctx->pc = K_OVERRIDE_BLOCK_HANDLER_RESUME;
        SleepThread(rdram, ctx, runtime);
    }

    void schedulerBlockingOverrideHandlerResume(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        gInvocationTrace.push_back(4);
        setReturnU32(ctx, 0xB10C0EDu);
        ctx->pc = 0u;
    }

    void schedulerBlockingOverrideDriver(uint8_t *, R5900Context *ctx, PS2Runtime *runtime)
    {
        gInvocationTrace.push_back(3);
        ctx->pc = 0u;
        runtime->eeScheduler().wakeupThread(EeScheduler::kMainThreadId, false);
        runtime->eeScheduler().transferIfRequested(false);
    }

    void schedulerBlockingOverrideBaseResume(uint8_t *, R5900Context *ctx, PS2Runtime *runtime)
    {
        gInvocationTrace.push_back(5);
        gOverrideResult = *ctx;
        ctx->pc = 0u;
        runtime->requestStop();
    }

    void schedulerExitHandlerA(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        gInvocationTrace.push_back(getRegS32(*ctx, 4));
        ctx->pc = 0u;
    }

    void schedulerExitHandlerB(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        gInvocationTrace.push_back(getRegS32(*ctx, 4));
        ctx->pc = 0u;
    }

    void schedulerExitObserver(uint8_t *, R5900Context *ctx, PS2Runtime *runtime)
    {
        const GuestThread *main = runtime->eeScheduler().thread(EeScheduler::kMainThreadId);
        gInvocationTrace.push_back(main && main->status == EeThreadStatus::Dormant ? 40 : -40);
        ctx->pc = 0u;
        runtime->requestStop();
    }

    void schedulerExitMain(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        gInvocationTrace.push_back(10);
        EeThreadCreateParams observer{};
        observer.entry = K_EXIT_OBSERVER;
        observer.stack = 0x1F000u;
        observer.stackSize = 0x1000u;
        observer.priority = 10;
        const int observerId = runtime->eeScheduler().createThread(observer);
        runtime->eeScheduler().startThread(observerId, 0u, *ctx, false);
        runtime->addEeExitHandler(EeScheduler::kMainThreadId, K_EXIT_HANDLER_A, 20u);
        runtime->addEeExitHandler(EeScheduler::kMainThreadId, K_EXIT_HANDLER_B, 30u);
        ExitThread(rdram, ctx, runtime);
    }

    void alarmNoopHandler(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        ctx->pc = 0u;
    }

    constexpr uint32_t K_SCHED_MAIN = 0x300000u;
    constexpr uint32_t K_SCHED_A = 0x300100u;
    constexpr uint32_t K_SCHED_A_RESUME = 0x300104u;
    constexpr uint32_t K_SCHED_B = 0x300200u;
    constexpr uint32_t K_SCHED_B_RESUME = 0x300204u;
    constexpr uint32_t K_SCHED_HIGH = 0x300300u;
    constexpr uint32_t K_SCHED_SIGNAL = 0x300400u;
    constexpr uint32_t K_SCHED_SIGNAL_RESUME = 0x300404u;

    std::vector<int> *gSchedulerTrace = nullptr;
    int gSchedulerCreatedId = 0;
    int gSchedulerSemaphoreId = 0;
    int gSchedulerWaitResultA = 0;
    int gSchedulerWaitResultB = 0;
    std::atomic<int> gGuestActive{0};
    std::atomic<int> gGuestMaxActive{0};
    std::atomic<size_t> gGuestExecutorHash{0u};
    std::atomic<bool> gGuestExecutorMismatch{false};
    std::atomic<bool> gGuestExecutingFlagMissing{false};

    struct GuestExecutionProbe
    {
        explicit GuestExecutionProbe(PS2Runtime *runtime)
        {
            const int active = gGuestActive.fetch_add(1, std::memory_order_acq_rel) + 1;
            int maximum = gGuestMaxActive.load(std::memory_order_acquire);
            while (active > maximum &&
                   !gGuestMaxActive.compare_exchange_weak(maximum, active, std::memory_order_acq_rel))
            {
            }
            const size_t hash = std::hash<std::thread::id>{}(std::this_thread::get_id());
            size_t expected = 0u;
            if (!gGuestExecutorHash.compare_exchange_strong(expected, hash, std::memory_order_acq_rel) &&
                expected != hash)
            {
                gGuestExecutorMismatch.store(true, std::memory_order_release);
            }
            if (!runtime->eeScheduler().isExecutingGuest())
            {
                gGuestExecutingFlagMissing.store(true, std::memory_order_release);
            }
        }

        ~GuestExecutionProbe()
        {
            gGuestActive.fetch_sub(1, std::memory_order_acq_rel);
        }
    };

    void schedulerMainExit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        GuestExecutionProbe probe(runtime);
        gSchedulerTrace->push_back(1);
        ExitThread(rdram, ctx, runtime);
    }

    void schedulerTraceA(uint8_t *, R5900Context *ctx, PS2Runtime *runtime)
    {
        GuestExecutionProbe probe(runtime);
        gSchedulerTrace->push_back(10);
        ctx->pc = 0u;
        if (gSchedulerTrace->size() >= 4u)
        {
            runtime->requestStop();
        }
    }

    void schedulerTraceB(uint8_t *, R5900Context *ctx, PS2Runtime *runtime)
    {
        GuestExecutionProbe probe(runtime);
        gSchedulerTrace->push_back(20);
        ctx->pc = 0u;
        if (gSchedulerTrace->size() >= 4u)
        {
            runtime->requestStop();
        }
    }

    void schedulerRotateA(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        gSchedulerTrace->push_back(10);
        ctx->pc = K_SCHED_A_RESUME;
        setRegU32(*ctx, 4, 5u);
        RotateThreadReadyQueue(rdram, ctx, runtime);
    }

    void schedulerRotateAResume(uint8_t *, R5900Context *ctx, PS2Runtime *runtime)
    {
        gSchedulerTrace->push_back(11);
        ctx->pc = 0u;
        runtime->requestStop();
    }

    void schedulerPreemptLow(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        gSchedulerTrace->push_back(30);
        ctx->pc = K_SCHED_A_RESUME;
        setRegU32(*ctx, 4, static_cast<uint32_t>(gSchedulerCreatedId));
        setRegU32(*ctx, 5, 0u);
        StartThread(rdram, ctx, runtime);
    }

    void schedulerPreemptLowResume(uint8_t *, R5900Context *ctx, PS2Runtime *runtime)
    {
        gSchedulerTrace->push_back(31);
        ctx->pc = 0u;
        runtime->requestStop();
    }

    void schedulerHigh(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        gSchedulerTrace->push_back(5);
        ctx->pc = 0u;
    }

    void schedulerWaitA(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc == K_SCHED_A)
        {
            gSchedulerTrace->push_back(10);
            ctx->pc = K_SCHED_A_RESUME;
            setRegU32(*ctx, 4, static_cast<uint32_t>(gSchedulerSemaphoreId));
            WaitSema(rdram, ctx, runtime);
            return;
        }
        gSchedulerWaitResultA = getRegS32(*ctx, 2);
        gSchedulerTrace->push_back(11);
        ctx->pc = 0u;
    }

    void schedulerWaitB(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc == K_SCHED_B)
        {
            gSchedulerTrace->push_back(20);
            ctx->pc = K_SCHED_B_RESUME;
            setRegU32(*ctx, 4, static_cast<uint32_t>(gSchedulerSemaphoreId));
            WaitSema(rdram, ctx, runtime);
            return;
        }
        gSchedulerWaitResultB = getRegS32(*ctx, 2);
        gSchedulerTrace->push_back(21);
        ctx->pc = 0u;
        runtime->requestStop();
    }

    void schedulerSignalTwice(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc == K_SCHED_SIGNAL)
        {
            gSchedulerTrace->push_back(30);
            ctx->pc = K_SCHED_SIGNAL_RESUME;
            setRegU32(*ctx, 4, static_cast<uint32_t>(gSchedulerSemaphoreId));
            SignalSema(rdram, ctx, runtime);
            return;
        }
        gSchedulerTrace->push_back(31);
        setRegU32(*ctx, 4, static_cast<uint32_t>(gSchedulerSemaphoreId));
        SignalSema(rdram, ctx, runtime);
        ctx->pc = 0u;
        runtime->requestStop();
    }

    constexpr uint32_t K_EVENT_FIFO_MAIN = 0x301000u;
    constexpr uint32_t K_EVENT_FIFO_A = 0x301010u;
    constexpr uint32_t K_EVENT_FIFO_A_RESUME = 0x301014u;
    constexpr uint32_t K_EVENT_FIFO_B = 0x301020u;
    constexpr uint32_t K_EVENT_FIFO_B_RESUME = 0x301024u;
    constexpr uint32_t K_EVENT_FIFO_SIGNAL = 0x301030u;
    constexpr uint32_t K_EVENT_FIFO_SIGNAL_AGAIN = 0x301034u;
    constexpr uint32_t K_EVENT_FIFO_DONE = 0x301038u;
    constexpr uint32_t K_EVENT_FIFO_RESULT_A = 0x1A00u;
    constexpr uint32_t K_EVENT_FIFO_RESULT_B = 0x1A04u;
    constexpr uint32_t WEF_OR = 0x01u;
    constexpr uint32_t WEF_CLEAR = 0x10u;
    constexpr uint32_t WEF_CLEAR_ALL = 0x20u;
    int gEventFifoId = 0;
    std::vector<int> gEventFifoTrace;

    void schedulerEventFifoWaitA(uint8_t *, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc == K_EVENT_FIFO_A)
        {
            gEventFifoTrace.push_back(1);
            ctx->pc = K_EVENT_FIFO_A_RESUME;
            runtime->eeScheduler().waitEventFlag(gEventFifoId, 1u, WEF_OR | WEF_CLEAR,
                                                  K_EVENT_FIFO_RESULT_A);
        }
        gEventFifoTrace.push_back(4);
        ctx->pc = 0u;
    }

    void schedulerEventFifoWaitB(uint8_t *, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc == K_EVENT_FIFO_B)
        {
            gEventFifoTrace.push_back(2);
            ctx->pc = K_EVENT_FIFO_B_RESUME;
            runtime->eeScheduler().waitEventFlag(gEventFifoId, 1u, WEF_OR | WEF_CLEAR,
                                                  K_EVENT_FIFO_RESULT_B);
        }
        gEventFifoTrace.push_back(6);
        ctx->pc = 0u;
    }

    void schedulerEventFifoSignal(uint8_t *, R5900Context *ctx, PS2Runtime *runtime)
    {
        gEventFifoTrace.push_back(3);
        ctx->pc = K_EVENT_FIFO_SIGNAL_AGAIN;
        runtime->eeScheduler().setEventFlag(gEventFifoId, 1u, false);
        runtime->eeScheduler().transferIfRequested(false);
    }

    void schedulerEventFifoSignalAgain(uint8_t *, R5900Context *ctx, PS2Runtime *runtime)
    {
        gEventFifoTrace.push_back(5);
        ctx->pc = K_EVENT_FIFO_DONE;
        runtime->eeScheduler().setEventFlag(gEventFifoId, 1u, false);
        runtime->eeScheduler().transferIfRequested(false);
    }

    void schedulerEventFifoDone(uint8_t *, R5900Context *ctx, PS2Runtime *runtime)
    {
        gEventFifoTrace.push_back(7);
        ctx->pc = 0u;
        runtime->requestStop();
    }

    void schedulerEventFifoMain(uint8_t *, R5900Context *ctx, PS2Runtime *runtime)
    {
        EeScheduler &scheduler = runtime->eeScheduler();
        gEventFifoId = scheduler.createEventFlag(0u, 0x02u, 0u);
        const auto create = [&](uint32_t entry, uint32_t stack, int priority)
        {
            EeThreadCreateParams params{};
            params.entry = entry;
            params.stack = stack;
            params.stackSize = 0x800u;
            params.priority = priority;
            const int id = scheduler.createThread(params);
            scheduler.startThread(id, 0u, *ctx, false);
        };
        create(K_EVENT_FIFO_A, 0x21000u, 5);
        create(K_EVENT_FIFO_B, 0x22000u, 5);
        create(K_EVENT_FIFO_SIGNAL, 0x23000u, 10);
        ctx->pc = 0u;
    }

    struct TestEnv
    {
        std::vector<uint8_t> rdram;
        R5900Context ctx{};
        PS2Runtime runtime;

        TestEnv() : rdram(PS2_RAM_SIZE, 0)
        {
            std::memset(&ctx, 0, sizeof(ctx));
        }
    };
}

void register_ps2_runtime_kernel_tests()
{
    MiniTest::Case("PS2RuntimeKernel", [](TestCase &tc)
    {
        tc.Run("CreateThread and CreateSema decode the exact PS2SDK EE layouts", [](TestCase &t)
        {
            TestEnv env;
            EeThreadCreateAbi threadParam{};
            threadParam.status = 0x11111111;
            threadParam.func = K_SCHED_HIGH;
            threadParam.stack = 0x00018000u;
            threadParam.stack_size = 0x1000;
            threadParam.gp_reg = 0x00123400u;
            threadParam.initial_priority = 37;
            threadParam.current_priority = 99;
            threadParam.attr = 0xABCDEF01u;
            threadParam.option = 0x10203040u;
            std::memcpy(env.rdram.data() + K_PARAM_ADDR, &threadParam, sizeof(threadParam));

            setRegU32(env.ctx, 4, K_PARAM_ADDR);
            CreateThread(env.rdram.data(), &env.ctx, &env.runtime);
            const int threadId = getRegS32(env.ctx, 2);
            const GuestThread *thread = env.runtime.eeScheduler().thread(threadId);
            t.IsTrue(threadId >= 2 && thread != nullptr, "the EE descriptor should create a guest thread");
            t.Equals(thread->entry, threadParam.func, "func must be decoded from offset 0x04");
            t.Equals(thread->stack, threadParam.stack, "stack must be decoded from offset 0x08");
            t.Equals(thread->stackSize, static_cast<uint32_t>(threadParam.stack_size),
                     "stack_size must be decoded from offset 0x0C");
            t.Equals(thread->gp, threadParam.gp_reg, "gp_reg must be decoded from offset 0x10");
            t.Equals(thread->initialPriority, threadParam.initial_priority,
                     "initial_priority must be decoded from offset 0x14");
            t.Equals(thread->currentPriority, threadParam.initial_priority,
                     "a new thread starts at its initial priority, not the status-only current_priority field");
            t.Equals(thread->attr, threadParam.attr, "attr must be decoded from offset 0x1C");
            t.Equals(thread->option, threadParam.option, "option must be decoded from offset 0x20");
            t.IsTrue(thread->status == EeThreadStatus::Dormant, "a newly created EE thread must be dormant");

            setRegU32(env.ctx, 4, PS2_RAM_SIZE - static_cast<uint32_t>(sizeof(EeThreadCreateAbi)) + 4u);
            CreateThread(env.rdram.data(), &env.ctx, &env.runtime);
            t.Equals(getRegS32(env.ctx, 2), KE_ERROR,
                     "the entire EE descriptor must fit in a valid guest range");

            EeSemaStatus semaParam{};
            semaParam.count = 91;
            semaParam.max_count = 7;
            semaParam.init_count = 3;
            semaParam.wait_threads = 82;
            semaParam.attr = 0x55667788u;
            semaParam.option = 0x99AABBCCu;
            std::memcpy(env.rdram.data() + K_PARAM_ADDR, &semaParam, sizeof(semaParam));
            setRegU32(env.ctx, 4, K_PARAM_ADDR);
            CreateSema(env.rdram.data(), &env.ctx, &env.runtime);
            const int semaId = getRegS32(env.ctx, 2);
            const EeSemaphore *semaphore = env.runtime.eeScheduler().semaphore(semaId);
            t.IsTrue(semaId > 0 && semaphore != nullptr, "the EE semaphore descriptor should create an object");
            t.Equals(semaphore->count, semaParam.init_count,
                     "CreateSema must use init_count at offset 0x08, not the status count field");
            t.Equals(semaphore->maxCount, semaParam.max_count, "max_count must be decoded from offset 0x04");
            t.Equals(semaphore->attr, semaParam.attr, "semaphore attr must be decoded from offset 0x10");
            t.Equals(semaphore->option, semaParam.option, "semaphore option must be decoded from offset 0x14");
        });

        tc.Run("CreateSema stores max_count as-is and waiter-less signals never overflow (P1aa; amends P1v)", [](TestCase &t)
        {
            TestEnv env;
            EeSemaStatus zeroParam{};
            std::memcpy(env.rdram.data() + K_PARAM_ADDR, &zeroParam, sizeof(zeroParam));
            setRegU32(env.ctx, 4, K_PARAM_ADDR);
            CreateSema(env.rdram.data(), &env.ctx, &env.runtime);
            const int zeroId = getRegS32(env.ctx, 2);
            t.IsTrue(zeroId > 0, "zero-max CreateSema must return a usable id, not KE_ERROR");
            const EeSemaphore *zeroSema = env.runtime.eeScheduler().semaphore(zeroId);
            t.IsTrue(zeroSema != nullptr, "zero-max CreateSema must create an object");
            if (zeroSema != nullptr)
            {
                t.Equals(zeroSema->maxCount, 0, "zero max_count is stored as-is (kernel has no clamp, P1z)");
                t.Equals(zeroSema->count, 0, "a zero-max semaphore starts at init_count 0");
            }

            EeSemaStatus validParam{};
            validParam.max_count = 7;
            validParam.init_count = 3;
            std::memcpy(env.rdram.data() + K_PARAM_ADDR, &validParam, sizeof(validParam));
            CreateSema(env.rdram.data(), &env.ctx, &env.runtime);
            const int validId = getRegS32(env.ctx, 2);
            const EeSemaphore *validSema = env.runtime.eeScheduler().semaphore(validId);
            t.IsTrue(validId > 0 && validSema != nullptr, "valid creates still succeed");
            if (validSema != nullptr)
            {
                t.Equals(validSema->maxCount, 7, "a valid max_count is stored unchanged");
                t.Equals(validSema->count, 3, "a valid init_count is stored unchanged");
            }

            EeSemaStatus negParam{};
            negParam.max_count = -2;
            std::memcpy(env.rdram.data() + K_PARAM_ADDR, &negParam, sizeof(negParam));
            CreateSema(env.rdram.data(), &env.ctx, &env.runtime);
            const int negId = getRegS32(env.ctx, 2);
            t.IsTrue(negId > 0, "a negative max_count is accepted (kernel checks only init<0, P1z-2)");
            const EeSemaphore *negSema = env.runtime.eeScheduler().semaphore(negId);
            if (negSema != nullptr)
            {
                t.Equals(negSema->maxCount, -2, "a negative max_count is stored as-is");
            }

            EeSemaStatus overParam{};
            overParam.max_count = 3;
            overParam.init_count = 5;
            std::memcpy(env.rdram.data() + K_PARAM_ADDR, &overParam, sizeof(overParam));
            CreateSema(env.rdram.data(), &env.ctx, &env.runtime);
            const int overId = getRegS32(env.ctx, 2);
            t.IsTrue(overId > 0, "init_count above max_count is accepted (no init<=max check in the kernel)");
            const EeSemaphore *overSema = env.runtime.eeScheduler().semaphore(overId);
            if (overSema != nullptr)
            {
                t.Equals(overSema->maxCount, 3, "an over-init max_count is stored unchanged");
                t.Equals(overSema->count, 5, "an over-init init_count is stored unchanged");
            }

            EeSemaStatus badParam{};
            badParam.max_count = 4;
            badParam.init_count = -1;
            std::memcpy(env.rdram.data() + K_PARAM_ADDR, &badParam, sizeof(badParam));
            CreateSema(env.rdram.data(), &env.ctx, &env.runtime);
            t.Equals(getRegS32(env.ctx, 2), KE_ERROR, "a negative init_count is still rejected");

            // P1v's own race window, kernel-true: two waiter-less signals hold
            // count 2 and both waits succeed; no KE_SEMA_OVF exists (P1z-3).
            TestEnv win;
            EeScheduler &ee = win.runtime.eeScheduler();
            ee.reset(win.rdram.data(), win.ctx);
            ee.bindMainContextForSyscall(win.ctx, win.rdram.data());
            const int winId = ee.createSemaphore(0, 0, 0u, 0u);
            t.IsTrue(winId > 0, "the race-window semaphore must be created");
            const EeSemaphore *winSema = ee.semaphore(winId);
            t.IsTrue(winSema != nullptr, "the race-window semaphore must exist");
            if (winSema != nullptr)
            {
                t.Equals(ee.signalSemaphore(winId, false), winId, "the first waiter-less signal succeeds");
                t.Equals(ee.signalSemaphore(winId, false), winId, "the second waiter-less signal succeeds (no OVF)");
                t.Equals(ee.semaphore(winId)->count, 2, "two waiter-less signals hold count 2");
                ee.waitSemaphore(winId);
                t.Equals(getRegS32(ee.thread(1)->context, 2), winId, "the first wait consumes");
                ee.waitSemaphore(winId);
                t.Equals(getRegS32(ee.thread(1)->context, 2), winId, "the second wait consumes");
                t.Equals(ee.semaphore(winId)->count, 0, "both waits consumed the count back to 0");
            }
        });

        tc.Run("PollSema on a zero-count semaphore returns KE_ERROR like the kernel (P10)", [](TestCase &t)
        {
            TestEnv env;
            EeSemaStatus param{};
            param.max_count = 1;
            param.init_count = 0;
            std::memcpy(env.rdram.data() + K_PARAM_ADDR, &param, sizeof(param));
            setRegU32(env.ctx, 4, K_PARAM_ADDR);
            CreateSema(env.rdram.data(), &env.ctx, &env.runtime);
            const int semaId = getRegS32(env.ctx, 2);
            t.IsTrue(semaId > 0, "the poll test semaphore must be created");

            // Kernel PollSema (0x80004dc0) misses with plain -1: blez at
            // 0x80004de4 falls back to the jr/addiu v0,zero,-1 pair, and no
            // -419 immediate exists anywhere in KERNEL (P10 re-derivation).
            setRegU32(env.ctx, 4, static_cast<uint32_t>(semaId));
            PollSema(env.rdram.data(), &env.ctx, &env.runtime);
            t.Equals(getRegS32(env.ctx, 2), KE_ERROR, "a zero-count poll must miss with KE_ERROR (-1), not KE_SEMA_ZERO");

            setRegU32(env.ctx, 4, static_cast<uint32_t>(semaId));
            SignalSema(env.rdram.data(), &env.ctx, &env.runtime);
            t.Equals(getRegS32(env.ctx, 2), semaId, "a waiter-less signal succeeds");

            setRegU32(env.ctx, 4, static_cast<uint32_t>(semaId));
            PollSema(env.rdram.data(), &env.ctx, &env.runtime);
            t.Equals(getRegS32(env.ctx, 2), semaId, "a poll at count 1 consumes and returns the id");

            setRegU32(env.ctx, 4, static_cast<uint32_t>(semaId));
            PollSema(env.rdram.data(), &env.ctx, &env.runtime);
            t.Equals(getRegS32(env.ctx, 2), KE_ERROR, "the next poll misses again with KE_ERROR");

            setRegU32(env.ctx, 4, static_cast<uint32_t>(semaId));
            iPollSema(env.rdram.data(), &env.ctx, &env.runtime);
            t.Equals(getRegS32(env.ctx, 2), KE_ERROR, "iPollSema shares the kernel -1 miss value");
        });

        tc.Run("unknown semaphore ids return KE_ERROR like the kernel on every sema path (P12)", [](TestCase &t)
        {
            // Stock-kernel unknown-id returns (P12 re-derivation, P1z
            // method): delete 0x80004a94 (b), signal 0x80004c04 (b), wait
            // 0x80004d30 (b), poll 0x80004dcc (jr), refer 0x80004e04 (jr)
            // all land on addiu v0,zero,-1 — for out-of-range AND freed
            // (count<0) ids alike — and no -408 immediate exists anywhere
            // in KERNEL.
            constexpr int kBogus = 9999;
            TestEnv env;
            EeScheduler &ee = env.runtime.eeScheduler();
            ee.reset(env.rdram.data(), env.ctx);
            ee.bindMainContextForSyscall(env.ctx, env.rdram.data());

            t.Equals(ee.deleteSemaphore(kBogus, false), KE_ERROR,
                     "DeleteSema on a never-created id must miss with KE_ERROR (-1)");
            t.Equals(ee.signalSemaphore(kBogus, false), KE_ERROR,
                     "SignalSema on a never-created id must miss with KE_ERROR (-1)");
            t.Equals(ee.pollSemaphore(kBogus), KE_ERROR,
                     "PollSema on a never-created id must miss with KE_ERROR (-1)");
            ee.waitSemaphore(kBogus);
            t.Equals(getRegS32(ee.thread(1)->context, 2), KE_ERROR,
                     "WaitSema on a never-created id must miss with KE_ERROR (-1)");

            setRegU32(env.ctx, 4, static_cast<uint32_t>(kBogus));
            setRegU32(env.ctx, 5, K_STATUS_ADDR);
            ReferSemaStatus(env.rdram.data(), &env.ctx, &env.runtime);
            t.Equals(getRegS32(env.ctx, 2), KE_ERROR,
                     "ReferSemaStatus on a never-created id must miss with KE_ERROR (-1)");

            const int doomed = ee.createSemaphore(0, 1, 0u, 0u);
            t.IsTrue(doomed > 0, "the throwaway semaphore must be created");
            t.Equals(ee.deleteSemaphore(doomed, false), doomed, "deleting the throwaway succeeds");
            t.Equals(ee.signalSemaphore(doomed, false), KE_ERROR,
                     "SignalSema on a deleted id must miss with KE_ERROR (-1)");
            t.Equals(ee.pollSemaphore(doomed), KE_ERROR,
                     "PollSema on a deleted id must miss with KE_ERROR (-1)");
        });

        tc.Run("Drop census lines format exactly and the kill-switch gates emission (P1w)", [](TestCase &t)
        {
            t.Equals(ps2_log::formatDropLine("sched/x", "r", "a=1"), "[drop] sched/x r a=1",
                     "drop lines use the exact [drop] site reason args shape");
            t.Equals(ps2_log::formatDropLine("sched/x", "r", ""), "[drop] sched/x r -",
                     "empty args render as a dash");

            // Save/restore any ambient kill-switch so parallel tests are unaffected.
            const char *prior = std::getenv("PS2X_DROP_SILENCE");
            const std::string saved = prior != nullptr ? prior : "";
            const bool hadPrior = prior != nullptr;

            unsetenv("PS2X_DROP_SILENCE");
            t.IsTrue(!ps2_log::dropsMuted(), "drops are on by default (kill-switch unset)");
            std::ostringstream unmuted;
            ps2_log::emitDropTo(unmuted, "s", "r", "a");
            t.Equals(unmuted.str(), "[drop] s r a\n", "unmuted emission writes one line");

            setenv("PS2X_DROP_SILENCE", "1", 1);
            t.IsTrue(ps2_log::dropsMuted(), "a non-empty kill-switch mutes drops");
            std::ostringstream muted;
            ps2_log::emitDropTo(muted, "s", "r", "a");
            t.Equals(muted.str(), "", "muted emission writes nothing");

            setenv("PS2X_DROP_SILENCE", "", 1);
            t.IsTrue(!ps2_log::dropsMuted(), "an empty kill-switch value still emits");

            if (hadPrior)
            {
                setenv("PS2X_DROP_SILENCE", saved.c_str(), 1);
            }
            else
            {
                unsetenv("PS2X_DROP_SILENCE");
            }
        });

        tc.Run("Park tallies count dispatches, semas, RPC, GS and drops (T1)", [](TestCase &t)
        {
            ps2_park::resetTalliesForTesting();
            ps2_log::resetDropCensusForTesting();

            ps2_park::tallyDispatch(0x41ea18u, 0x41aac0u);
            ps2_park::tallyDispatch(0x41ea18u, 0x41aaccu);
            ps2_park::tallyDispatch(0x326478u, 0x3260ecu);
            const auto &hot = ps2_park::hotPcCounts();
            t.Equals(hot.size(), static_cast<size_t>(2), "two distinct dispatch targets tallied");
            t.Equals(hot.at(0x41ea18u).count, static_cast<uint64_t>(2), "repeat target counts twice");
            t.Equals(hot.at(0x41ea18u).firstRa, static_cast<uint32_t>(0x41aac0u), "first ra kept");
            t.Equals(hot.at(0x41ea18u).lastRa, static_cast<uint32_t>(0x41aaccu), "last ra kept");

            TestEnv env;
            EeScheduler &ee = env.runtime.eeScheduler();
            ee.bindMainContextForSyscall(env.ctx, env.rdram.data());
            const int sema = ee.createSemaphore(1, 5, 0u, 0u);
            t.IsTrue(sema > 0, "throwaway sema created");
            ee.waitSemaphore(sema);
            t.Equals(ee.signalSemaphore(sema, false), sema, "signal returns the id");
            t.Equals(ps2_park::semaCreates().size(), static_cast<size_t>(1), "one create row");
            t.Equals(ps2_park::semaCreates()[0].id, sema, "create row names the sema");
            t.Equals(ps2_park::semaCreates()[0].maxCount, 5, "create row keeps max");
            t.Equals(ps2_park::semaWaitHist()[sema].size(), static_cast<size_t>(1), "one waiter pc");
            t.Equals(ps2_park::semaSignalHist()[sema].size(), static_cast<size_t>(1), "one signaller pc");
            ps2_park::tallySched(1);
            ps2_park::tallySched(3);
            t.Equals(ps2_park::schedCounts()[1], static_cast<uint64_t>(1), "sched count per thread");

            ps2_park::ParkRpcEvent call;
            call.op = "call";
            call.sid = 0x80000211u;
            call.fno = 0x1u;
            call.sendSize = 16u;
            call.recvSize = 144u;
            call.tid = 3;
            call.claimed = false;
            ps2_park::tallyRpcEvent(call);
            t.Equals(ps2_park::rpcEvents().size(), static_cast<size_t>(1), "one rpc row");
            t.Equals(ps2_park::rpcEvents()[0].sid, static_cast<uint32_t>(0x80000211u), "rpc row keeps sid");

            ps2_park::tallyGsKick(true);
            ps2_park::tallyGsKick(false);
            ps2_park::tallyGsGif();
            ps2_park::tallyGsCopyReg();
            t.Equals(ps2_park::gsKickCount().load(), static_cast<uint64_t>(2), "two kicks");
            t.Equals(ps2_park::gsKickDrawingCount().load(), static_cast<uint64_t>(1), "one drawing kick");
            t.Equals(ps2_park::gsGifPacketCount().load(), static_cast<uint64_t>(1), "one gif packet");
            t.Equals(ps2_park::gsCopyRegCount().load(), static_cast<uint64_t>(1), "one copy reg");

            std::ostringstream sink;
            ps2_log::emitDropTo(sink, "sched/x", "KE_ERROR", "id=9");
            ps2_log::emitDropTo(sink, "sched/x", "KE_ERROR", "id=10");
            const auto census = ps2_log::snapshotDropCensus();
            t.Equals(census.size(), static_cast<size_t>(1), "census groups by site+reason");
            t.Equals(census[0].count, static_cast<uint64_t>(2), "both drops counted");
        });

        tc.Run("Park snapshot JSON/table render the exact schema (T1)", [](TestCase &t)
        {
            ps2_park::ParkSnapshotData data;
            data.source = "emitter";
            ps2_park::ParkThreadRow thread;
            thread.id = 3;
            thread.status = 2;
            thread.statusName = "Waiting";
            thread.waitReason = 2;
            thread.waitReasonName = "Semaphore";
            thread.waitId = 30;
            thread.pc = 0x423de8u;
            thread.ra = 0x31aca4u;
            thread.sp = 0x61ff00u;
            thread.entry = 0x31ac60u;
            thread.priority = 101;
            thread.scheduled = 43u;
            thread.chain = {0x31aca4u, 0x31a700u};
            data.threads.push_back(thread);
            ps2_park::ParkSemaRow sema;
            sema.id = 30;
            sema.count = 0;
            sema.maxCount = 1024;
            sema.initCount = 0;
            sema.waiters = 1;
            data.semaphores.push_back(sema);
            ps2_park::ParkRpcEvent load;
            load.op = "load";
            load.sid = 7u;
            load.tid = 1;
            load.claimed = true;
            load.path = "cdrom0:/data/x.\"irx\"\n";
            data.rpc.push_back(load);

            const std::string json = ps2_park::renderParkJson(data);
            t.IsTrue(json.find("\"schema\": \"ps2x-park-snapshot/1\"") != std::string::npos,
                     "json carries the schema tag");
            t.IsTrue(json.find("\"status_name\": \"Waiting\"") != std::string::npos,
                     "json carries thread rows");
            t.IsTrue(json.find("\"chain\": [\"0x31aca4\", \"0x31a700\"]") != std::string::npos,
                     "json renders the ra chain as hex");
            t.IsTrue(json.find("x.\\\"irx\\\"\n") == std::string::npos,
                     "raw quotes must not leak into json");
            t.IsTrue(json.find("x.\\\"irx\\\"\\n") != std::string::npos,
                     "json escapes quotes and newlines in paths");

            const std::string table = ps2_park::renderParkTable(data);
            t.IsTrue(table.find("park snapshot (emitter") != std::string::npos,
                     "table carries the source header");
            t.IsTrue(table.find("id=3 Waiting wait=Semaphore:30 pc=0x423de8") != std::string::npos,
                     "table renders the thread line");
            t.IsTrue(table.find("load claimed n=1") != std::string::npos,
                     "table tallies sif/rpc by op and side");
        });

        tc.Run("EE scheduler selects absolute priority then FIFO", [](TestCase &t)
        {
            TestEnv env;
            std::vector<int> trace;
            gSchedulerTrace = &trace;
            gGuestActive.store(0, std::memory_order_release);
            gGuestMaxActive.store(0, std::memory_order_release);
            gGuestExecutorHash.store(0u, std::memory_order_release);
            gGuestExecutorMismatch.store(false, std::memory_order_release);
            gGuestExecutingFlagMissing.store(false, std::memory_order_release);
            env.runtime.registerFunction(K_SCHED_MAIN, schedulerMainExit);
            env.runtime.registerFunction(K_SCHED_A, schedulerTraceA);
            env.runtime.registerFunction(K_SCHED_B, schedulerTraceB);

            env.ctx.pc = K_SCHED_MAIN;
            EeScheduler &ee = env.runtime.eeScheduler();
            ee.reset(env.rdram.data(), env.ctx);
            const int lowA = ee.createThread(EeThreadCreateParams{0, K_SCHED_A, 0x20000u, 0x800u, 0, 20, 0});
            const int highA = ee.createThread(EeThreadCreateParams{0, K_SCHED_A, 0x21000u, 0x800u, 0, 5, 0});
            const int highB = ee.createThread(EeThreadCreateParams{0, K_SCHED_B, 0x22000u, 0x800u, 0, 5, 0});
            ee.startThread(lowA, 0, env.ctx, false);
            ee.startThread(highA, 0, env.ctx, false);
            ee.startThread(highB, 0, env.ctx, false);
            ee.run();

            t.Equals(trace.size(), size_t{4}, "all runnable contexts should execute once");
            t.Equals(trace[0], 1, "main priority zero executes first");
            t.Equals(trace[1], 10, "first priority-5 thread preserves FIFO order");
            t.Equals(trace[2], 20, "second priority-5 thread follows FIFO order");
            t.Equals(trace[3], 10, "priority-20 thread executes only after priority-5 queue drains");
            t.Equals(gGuestMaxActive.load(std::memory_order_acquire), 1,
                     "there must never be two simultaneous guest executions");
            t.IsFalse(gGuestExecutorMismatch.load(std::memory_order_acquire),
                      "all EE guest functions must execute on the single scheduler host thread");
            t.IsFalse(gGuestExecutingFlagMissing.load(std::memory_order_acquire),
                      "the scheduler must publish guest execution only around the active guest call");
        });

        tc.Run("thread lifecycle, nested suspend, WAIT-SUSPEND, and wakeup count are centralized", [](TestCase &t)
        {
            TestEnv env;
            EeScheduler &ee = env.runtime.eeScheduler();
            ee.reset(env.rdram.data(), env.ctx);
            ee.bindMainContextForSyscall(env.ctx, env.rdram.data());

            const int id = ee.createThread(EeThreadCreateParams{0u, K_SCHED_HIGH, 0x24000u, 0x800u,
                                                                 0u, 20, 0u});
            t.Equals(ee.startThread(id, 0xCAFEu, env.ctx, false), KE_OK, "StartThread should make a dormant thread ready");
            t.IsTrue(ee.thread(id)->status == EeThreadStatus::Ready, "started thread should be in one ready queue");
            t.Equals(ee.suspendThread(id, false), KE_OK, "first suspend should remove the ready thread");
            t.Equals(ee.suspendThread(id, false), KE_OK, "nested suspend should increment suspendCount");
            t.IsTrue(ee.thread(id)->status == EeThreadStatus::Suspended && ee.thread(id)->suspendCount == 2,
                     "nested suspension should have one suspended membership and count two");
            t.Equals(ee.resumeThread(id, false), KE_OK, "first resume should only decrement the nested count");
            t.IsTrue(ee.thread(id)->status == EeThreadStatus::Suspended && ee.thread(id)->suspendCount == 1,
                     "one outstanding suspend keeps the thread suspended");
            t.Equals(ee.resumeThread(id, false), KE_OK, "final resume should restore readiness");
            t.IsTrue(ee.thread(id)->status == EeThreadStatus::Ready && ee.thread(id)->suspendCount == 0,
                     "final resume should enqueue the thread exactly once");

            t.Equals(ee.wakeupThread(id, false), KE_OK, "wakeup against a non-sleeping thread should accumulate");
            t.Equals(ee.wakeupThread(id, false), KE_OK, "a second wakeup should accumulate independently");
            t.Equals(ee.cancelWakeup(id), 2, "CancelWakeupThread should return and clear the exact accumulated count");
            t.Equals(ee.cancelWakeup(id), 0, "the wakeup count should remain cleared");

            uint32_t ownedStack = 0u;
            t.Equals(ee.terminateThread(id, ownedStack, false), KE_OK,
                     "TerminateThread should remove a ready thread and make it dormant");
            t.IsTrue(ee.thread(id)->status == EeThreadStatus::Dormant, "terminated thread should be dormant");
            t.Equals(ee.deleteThread(id, ownedStack), KE_OK, "a dormant thread record should be deletable");
            t.IsTrue(ee.thread(id) == nullptr, "deleted thread must leave every scheduler collection");

            bool slept = false;
            try
            {
                ee.sleepCurrent();
            }
            catch (const EeDispatcherTransfer &)
            {
                slept = true;
            }
            t.IsTrue(slept && ee.thread(EeScheduler::kMainThreadId)->status == EeThreadStatus::Waiting,
                     "SleepThread should transfer the running context into a typed wait");
            t.Equals(ee.suspendThread(EeScheduler::kMainThreadId, false), KE_OK,
                     "suspending a waiter should produce WAIT-SUSPEND");
            t.IsTrue(ee.thread(EeScheduler::kMainThreadId)->status == EeThreadStatus::WaitingSuspended,
                     "the sleeping context should remain in its wait object while suspended");
            t.Equals(ee.wakeupThread(EeScheduler::kMainThreadId, false), KE_OK,
                     "waking WAIT-SUSPEND should complete the wait without enqueueing yet");
            t.IsTrue(ee.thread(EeScheduler::kMainThreadId)->status == EeThreadStatus::Suspended,
                     "completed WAIT-SUSPEND should become plain suspended");
            t.Equals(ee.resumeThread(EeScheduler::kMainThreadId, false), KE_OK,
                     "resuming the final suspend should make the completed waiter ready");
            t.IsTrue(ee.thread(EeScheduler::kMainThreadId)->status == EeThreadStatus::Ready,
                     "the resumed waiter should re-enter its priority queue once");
        });

        tc.Run("semaphore signal, delete, and release complete blocked contexts with exact results", [](TestCase &t)
        {
            const auto blockMain = [](TestEnv &env, int &semaId)
            {
                EeScheduler &ee = env.runtime.eeScheduler();
                ee.reset(env.rdram.data(), env.ctx);
                ee.bindMainContextForSyscall(env.ctx, env.rdram.data());
                semaId = ee.createSemaphore(0, 1, 0u, 0u);
                try
                {
                    ee.waitSemaphore(semaId);
                }
                catch (const EeDispatcherTransfer &)
                {
                }
            };

            TestEnv signaled;
            int signalId = 0;
            blockMain(signaled, signalId);
            t.Equals(signaled.runtime.eeScheduler().signalSemaphore(signalId, false), signalId,
                     "SignalSema should transfer directly to the FIFO waiter");
            t.Equals(getRegS32(signaled.runtime.eeScheduler().thread(1)->context, 2), signalId,
                     "the resumed semaphore waiter should receive the semaphore id");
            t.Equals(signaled.runtime.eeScheduler().semaphore(signalId)->count, 0,
                     "direct semaphore handoff must not increment count");

            TestEnv deleted;
            int deleteId = 0;
            blockMain(deleted, deleteId);
            t.Equals(deleted.runtime.eeScheduler().deleteSemaphore(deleteId, false), deleteId,
                     "DeleteSema should remove the semaphore object");
            t.Equals(getRegS32(deleted.runtime.eeScheduler().thread(1)->context, 2), KE_WAIT_DELETE,
                     "DeleteSema should resume its waiter with KE_WAIT_DELETE");
            t.IsTrue(deleted.runtime.eeScheduler().semaphore(deleteId) == nullptr,
                     "deleted semaphore must no longer own a waiter queue");

            TestEnv released;
            int releaseId = 0;
            blockMain(released, releaseId);
            t.Equals(released.runtime.eeScheduler().releaseWait(EeScheduler::kMainThreadId, false), KE_OK,
                     "ReleaseWaitThread should detach the context from its wait object");
            t.Equals(getRegS32(released.runtime.eeScheduler().thread(1)->context, 2), KE_RELEASE_WAIT,
                     "released waiter should resume with KE_RELEASE_WAIT");
            t.Equals(released.runtime.eeScheduler().semaphore(releaseId)->waiters.size(), size_t{0},
                     "ReleaseWaitThread must remove the exact semaphore waiter");
        });

        tc.Run("event waiters are FIFO and clear modes apply before testing the next waiter", [](TestCase &t)
        {
            TestEnv env;
            env.runtime.registerFunction(K_EVENT_FIFO_MAIN, schedulerEventFifoMain);
            env.runtime.registerFunction(K_EVENT_FIFO_A, schedulerEventFifoWaitA);
            env.runtime.registerFunction(K_EVENT_FIFO_A_RESUME, schedulerEventFifoWaitA);
            env.runtime.registerFunction(K_EVENT_FIFO_B, schedulerEventFifoWaitB);
            env.runtime.registerFunction(K_EVENT_FIFO_B_RESUME, schedulerEventFifoWaitB);
            env.runtime.registerFunction(K_EVENT_FIFO_SIGNAL, schedulerEventFifoSignal);
            env.runtime.registerFunction(K_EVENT_FIFO_SIGNAL_AGAIN, schedulerEventFifoSignalAgain);
            env.runtime.registerFunction(K_EVENT_FIFO_DONE, schedulerEventFifoDone);
            gEventFifoTrace.clear();
            env.ctx.pc = K_EVENT_FIFO_MAIN;
            env.runtime.eeScheduler().reset(env.rdram.data(), env.ctx);
            env.runtime.eeScheduler().run();

            const std::vector<int> expected{1, 2, 3, 4, 5, 6, 7};
            t.IsTrue(gEventFifoTrace == expected,
                     "the first clear waiter must consume the first signal before the second FIFO waiter is tested");
            t.Equals(readGuestU32(env.rdram.data(), K_EVENT_FIFO_RESULT_A), 1u,
                     "first waiter should receive its pre-clear observed bits");
            t.Equals(readGuestU32(env.rdram.data(), K_EVENT_FIFO_RESULT_B), 1u,
                     "second waiter should require and receive the second signal");
            t.Equals(env.runtime.eeScheduler().eventFlag(gEventFifoId)->bits, 0u,
                     "both WEF_CLEAR completions should consume their matched bit");

            const int clearAllId = env.runtime.eeScheduler().createEventFlag(0x7u, 0x02u, 0u);
            uint32_t observed = 0u;
            t.Equals(env.runtime.eeScheduler().pollEventFlag(clearAllId, 0x2u, WEF_OR | WEF_CLEAR_ALL, observed), KE_OK,
                     "WEF_CLEAR_ALL poll should complete when any requested bit is present");
            t.Equals(observed, 0x7u, "event result should contain the bits observed before clearing");
            t.Equals(env.runtime.eeScheduler().eventFlag(clearAllId)->bits, 0u,
                     "WEF_CLEAR_ALL should clear the entire event pattern");
        });

        tc.Run("RotateThreadReadyQueue is the only same-priority rotation", [](TestCase &t)
        {
            TestEnv env;
            std::vector<int> trace;
            gSchedulerTrace = &trace;
            env.runtime.registerFunction(K_SCHED_MAIN, schedulerMainExit);
            env.runtime.registerFunction(K_SCHED_A, schedulerRotateA);
            env.runtime.registerFunction(K_SCHED_A_RESUME, schedulerRotateAResume);
            env.runtime.registerFunction(K_SCHED_B, schedulerTraceB);
            env.ctx.pc = K_SCHED_MAIN;

            EeScheduler &ee = env.runtime.eeScheduler();
            ee.reset(env.rdram.data(), env.ctx);
            const int first = ee.createThread(EeThreadCreateParams{0, K_SCHED_A, 0x20000u, 0x800u, 0, 5, 0});
            const int second = ee.createThread(EeThreadCreateParams{0, K_SCHED_B, 0x21000u, 0x800u, 0, 5, 0});
            ee.startThread(first, 0, env.ctx, false);
            ee.startThread(second, 0, env.ctx, false);
            ee.run();

            const std::vector<int> expected{1, 10, 20, 11};
            t.IsTrue(trace == expected, "explicit rotation should move the current head behind its FIFO peer");
        });

        tc.Run("starting a strictly higher-priority thread preempts immediately", [](TestCase &t)
        {
            TestEnv env;
            std::vector<int> trace;
            gSchedulerTrace = &trace;
            env.runtime.registerFunction(K_SCHED_MAIN, schedulerMainExit);
            env.runtime.registerFunction(K_SCHED_A, schedulerPreemptLow);
            env.runtime.registerFunction(K_SCHED_A_RESUME, schedulerPreemptLowResume);
            env.runtime.registerFunction(K_SCHED_HIGH, schedulerHigh);
            env.ctx.pc = K_SCHED_MAIN;

            EeScheduler &ee = env.runtime.eeScheduler();
            ee.reset(env.rdram.data(), env.ctx);
            const int low = ee.createThread(EeThreadCreateParams{0, K_SCHED_A, 0x20000u, 0x800u, 0, 20, 0});
            gSchedulerCreatedId = ee.createThread(EeThreadCreateParams{0, K_SCHED_HIGH, 0x21000u, 0x800u, 0, 5, 0});
            ee.startThread(low, 0, env.ctx, false);
            ee.run();

            const std::vector<int> expected{1, 30, 5, 31};
            t.IsTrue(trace == expected, "higher priority should run before the starter continues");
        });

        tc.Run("semaphore waiters are FIFO and signal transfers one token directly", [](TestCase &t)
        {
            TestEnv env;
            std::vector<int> trace;
            gSchedulerTrace = &trace;
            gSchedulerWaitResultA = 0;
            gSchedulerWaitResultB = 0;
            env.runtime.registerFunction(K_SCHED_MAIN, schedulerMainExit);
            env.runtime.registerFunction(K_SCHED_A, schedulerWaitA);
            env.runtime.registerFunction(K_SCHED_A_RESUME, schedulerWaitA);
            env.runtime.registerFunction(K_SCHED_B, schedulerWaitB);
            env.runtime.registerFunction(K_SCHED_B_RESUME, schedulerWaitB);
            env.runtime.registerFunction(K_SCHED_SIGNAL, schedulerSignalTwice);
            env.runtime.registerFunction(K_SCHED_SIGNAL_RESUME, schedulerSignalTwice);
            env.ctx.pc = K_SCHED_MAIN;

            EeScheduler &ee = env.runtime.eeScheduler();
            ee.reset(env.rdram.data(), env.ctx);
            gSchedulerSemaphoreId = ee.createSemaphore(0, 1, 0, 0);
            const int waiterA = ee.createThread(EeThreadCreateParams{0, K_SCHED_A, 0x20000u, 0x800u, 0, 5, 0});
            const int waiterB = ee.createThread(EeThreadCreateParams{0, K_SCHED_B, 0x21000u, 0x800u, 0, 5, 0});
            const int signaler = ee.createThread(EeThreadCreateParams{0, K_SCHED_SIGNAL, 0x22000u, 0x800u, 0, 20, 0});
            ee.startThread(waiterA, 0, env.ctx, false);
            ee.startThread(waiterB, 0, env.ctx, false);
            ee.startThread(signaler, 0, env.ctx, false);
            ee.run();

            const std::vector<int> expected{1, 10, 20, 30, 11, 31, 21};
            t.IsTrue(trace == expected, "each signal should wake exactly the FIFO head");
            t.Equals(gSchedulerWaitResultA, gSchedulerSemaphoreId, "first waiter receives sid on resume");
            t.Equals(gSchedulerWaitResultB, gSchedulerSemaphoreId, "second waiter receives sid on resume");
            t.Equals(ee.semaphore(gSchedulerSemaphoreId)->count, 0, "direct handoff must not increment count");
        });

        tc.Run("setup heap and allocator primitives track end-of-heap", [](TestCase &t)
        {
            TestEnv env;

            setRegU32(env.ctx, 4, 0x00180010u);
            setRegU32(env.ctx, 5, 0x00001000u);
            t.IsTrue(callSyscall(0x3Du, env.rdram.data(), &env.ctx, &env.runtime), "SetupHeap syscall should dispatch");
            const uint32_t heapBase = static_cast<uint32_t>(getRegS32(env.ctx, 2));
            t.Equals(heapBase, 0x00180010u, "SetupHeap should return configured base");

            t.IsTrue(callSyscall(0x3Eu, env.rdram.data(), &env.ctx, &env.runtime), "EndOfHeap syscall should dispatch");
            const uint32_t heapLimit = static_cast<uint32_t>(getRegS32(env.ctx, 2));
            t.Equals(heapLimit, 0x00181010u, "EndOfHeap should report the upper limit of the configured heap");

            const uint32_t alignedAlloc = env.runtime.guestMalloc(0x20u, 64u);
            t.IsTrue(alignedAlloc != 0u, "guestMalloc should allocate inside configured heap");
            t.Equals(alignedAlloc & 0x3Fu, 0u, "guestMalloc should honor 64-byte alignment");

            env.runtime.guestFree(alignedAlloc);

            const uint32_t a = env.runtime.guestMalloc(0x100u, 16u);
            const uint32_t b = env.runtime.guestMalloc(0x100u, 16u);
            t.IsTrue(a != 0u && b != 0u, "guestMalloc should provide two adjacent blocks in this heap window");
            env.runtime.guestFree(b);

            const uint32_t grown = env.runtime.guestRealloc(a, 0x180u, 16u);
            t.Equals(grown, a, "guestRealloc should grow in place when adjacent free space is available");

            env.runtime.guestFree(grown);
            const uint32_t reused = env.runtime.guestMalloc(0x80u, 16u);
            t.Equals(reused, heapBase, "guestFree should make the head block reusable");
        });

        tc.Run("memalign stubs allocate aligned guest memory", [](TestCase &t)
        {
            TestEnv env;

            env.runtime.configureGuestHeap(0x00180010u, 0x00182010u);

            setRegU32(env.ctx, 4, 128u);
            setRegU32(env.ctx, 5, 0x40u);
            ps2_stubs::memalign(env.rdram.data(), &env.ctx, &env.runtime);
            const uint32_t direct = ::getRegU32(&env.ctx, 2);
            t.IsTrue(direct != 0u, "memalign should return a guest address");
            t.Equals(direct & 0x7Fu, 0u, "memalign should honor 128-byte alignment");

            setRegU32(env.ctx, 5, 64u);
            setRegU32(env.ctx, 6, 0x40u);
            ps2_stubs::memalign_r(env.rdram.data(), &env.ctx, &env.runtime);
            const uint32_t reent = ::getRegU32(&env.ctx, 2);
            t.IsTrue(reent != 0u, "_memalign_r should return a guest address");
            t.Equals(reent & 0x3Fu, 0u, "_memalign_r should honor 64-byte alignment");
            t.IsTrue(reent != direct, "_memalign_r should allocate a distinct block");
        });

        tc.Run("allocator compatibility stubs use the runtime guest heap", [](TestCase &t)
        {
            TestEnv env;

            env.runtime.configureGuestHeap(0x00180010u, 0x00183010u);

            setRegU32(env.ctx, 5, 0x20u);
            ps2_stubs::malloc_r(env.rdram.data(), &env.ctx, &env.runtime);
            const uint32_t initial = ::getRegU32(&env.ctx, 2);
            t.IsTrue(initial != 0u, "_malloc_r should allocate guest memory");

            writeGuestU32(env.rdram.data(), initial, 0xAABBCCDDu);

            setRegU32(env.ctx, 5, initial);
            setRegU32(env.ctx, 6, 0x80u);
            ps2_stubs::realloc_r(env.rdram.data(), &env.ctx, &env.runtime);
            const uint32_t grown = ::getRegU32(&env.ctx, 2);
            t.IsTrue(grown != 0u, "_realloc_r should return a guest block");
            t.Equals(readGuestU32(env.rdram.data(), grown), 0xAABBCCDDu,
                     "_realloc_r should preserve existing guest bytes");

            setRegU32(env.ctx, 5, grown);
            ps2_stubs::free_r(env.rdram.data(), &env.ctx, &env.runtime);

            setRegU32(env.ctx, 5, 0x100u);
            ps2_stubs::malloc_extend_top(env.rdram.data(), &env.ctx, &env.runtime);
            t.Equals(::getRegU32(&env.ctx, 2), 0u,
                     "malloc_extend_top should be a safe runtime-owned heap no-op");

            ps2_stubs::__malloc_lock(env.rdram.data(), &env.ctx, &env.runtime);
            ps2_stubs::__malloc_unlock(env.rdram.data(), &env.ctx, &env.runtime);
        });

        tc.Run("libc helper stubs cover memclr and libgcc div", [](TestCase &t)
        {
            TestEnv env;

            constexpr uint32_t kBuf = 0x5000u;
            std::memset(env.rdram.data() + kBuf, 0xCD, 16u);
            setRegU32(env.ctx, 4, kBuf);
            setRegU32(env.ctx, 5, 12u);
            ps2_stubs::memclr(env.rdram.data(), &env.ctx, &env.runtime);
            for (uint32_t i = 0; i < 12u; ++i)
            {
                t.Equals(env.rdram[kBuf + i], static_cast<uint8_t>(0),
                         "memclr should zero the requested byte range");
            }
            t.Equals(env.rdram[kBuf + 12u], static_cast<uint8_t>(0xCD),
                     "memclr should not write past the requested byte range");

            SET_GPR_S64(&env.ctx, 4, -9);
            SET_GPR_S64(&env.ctx, 5, 2);
            ps2_stubs::__divdi3(env.rdram.data(), &env.ctx, &env.runtime);
            t.Equals(getRegS32(env.ctx, 2), -4, "__divdi3 should divide signed 64-bit values");
        });

        tc.Run("ReleaseAlarm aliases CancelAlarm and cache toggles succeed", [](TestCase &t)
        {
            TestEnv env;

            constexpr uint32_t kAlarmHandlerAddr = 0x00270000u;
            env.runtime.registerFunction(kAlarmHandlerAddr, &alarmNoopHandler);

            setRegU32(env.ctx, 4, 0xFFFFu);
            setRegU32(env.ctx, 5, kAlarmHandlerAddr);
            setRegU32(env.ctx, 6, 0u);
            SetAlarm(env.rdram.data(), &env.ctx, &env.runtime);
            const int32_t alarmId = getRegS32(env.ctx, 2);
            t.IsTrue(alarmId > 0, "SetAlarm should create a cancellable alarm");

            setRegU32(env.ctx, 4, static_cast<uint32_t>(alarmId));
            ReleaseAlarm(env.rdram.data(), &env.ctx, &env.runtime);
            t.Equals(getRegS32(env.ctx, 2), KE_OK, "ReleaseAlarm should cancel active alarms");

            setRegU32(env.ctx, 4, static_cast<uint32_t>(alarmId));
            CancelAlarm(env.rdram.data(), &env.ctx, &env.runtime);
            t.Equals(getRegS32(env.ctx, 2), KE_ERROR,
                     "CancelAlarm should report missing alarms after ReleaseAlarm consumes them");

            EnableCache(env.rdram.data(), &env.ctx, &env.runtime);
            t.Equals(getRegS32(env.ctx, 2), KE_OK, "EnableCache should succeed as a no-op");

            DisableCache(env.rdram.data(), &env.ctx, &env.runtime);
            t.Equals(getRegS32(env.ctx, 2), KE_OK, "DisableCache should succeed as a no-op");
        });

        tc.Run("setup heap and thread invalid ids use documented kernel errors", [](TestCase &t)
        {
            TestEnv env;

            setRegU32(env.ctx, 4, 0u);
            CreateThread(env.rdram.data(), &env.ctx, &env.runtime);
            t.Equals(getRegS32(env.ctx, 2), KE_ERROR, "CreateThread with null param should fail");

            setRegU32(env.ctx, 4, 0u);
            DeleteThread(env.rdram.data(), &env.ctx, &env.runtime);
            t.Equals(getRegS32(env.ctx, 2), KE_ILLEGAL_THID, "DeleteThread(0) should be KE_ILLEGAL_THID");

            setRegU32(env.ctx, 4, 0x7FFFu);
            StartThread(env.rdram.data(), &env.ctx, &env.runtime);
            t.Equals(getRegS32(env.ctx, 2), KE_UNKNOWN_THID, "StartThread should reject unknown thread ids");

            setRegU32(env.ctx, 4, 0x7FFFu);
            WakeupThread(env.rdram.data(), &env.ctx, &env.runtime);
            t.Equals(getRegS32(env.ctx, 2), KE_UNKNOWN_THID, "WakeupThread should reject unknown thread ids");

            setRegU32(env.ctx, 4, 0x7FFFu);
            PollSema(env.rdram.data(), &env.ctx, &env.runtime);
            t.Equals(getRegS32(env.ctx, 2), KE_ERROR, "PollSema should reject unknown semaphore ids with KE_ERROR like the kernel (P12)");

            setRegU32(env.ctx, 4, 0xFFFFFFFFu);
            t.IsTrue(callSyscall(0x3Du, env.rdram.data(), &env.ctx, &env.runtime), "SetupHeap syscall should dispatch");
            const uint32_t clampedBase = static_cast<uint32_t>(getRegS32(env.ctx, 2));
            t.IsTrue(clampedBase < PS2_RAM_SIZE, "SetupHeap should normalize out-of-range base into guest RAM");

            t.IsTrue(callSyscall(0x3Eu, env.rdram.data(), &env.ctx, &env.runtime), "EndOfHeap syscall should dispatch");
            const uint32_t heapEnd = static_cast<uint32_t>(getRegS32(env.ctx, 2));
            t.IsTrue(heapEnd >= clampedBase, "EndOfHeap should be at or above normalized heap base");

            setRegU32(env.ctx, 4, 1u);
            setRegU32(env.ctx, 5, 0u);
            setRegU32(env.ctx, 6, 0u);
            setRegU32(env.ctx, 29, 0x0010FFF0u);
            t.IsTrue(callSyscall(0x3Cu, env.rdram.data(), &env.ctx, &env.runtime), "SetupThread syscall should dispatch");
            const uint32_t setupSp = static_cast<uint32_t>(getRegS32(env.ctx, 2));
            t.Equals(setupSp & 0xFu, 0u, "SetupThread should always return a 16-byte aligned stack pointer");
        });

        tc.Run("SetupThread exposes stable main stack metadata through ReferThreadStatus", [](TestCase &t)
        {
            TestEnv env;
            constexpr uint32_t kInitialLoaderSp = PS2_RAM_SIZE - 0x10u;
            constexpr uint32_t kMainStackSize = 0x00020000u;
            constexpr uint32_t kExpectedStack = PS2_RAM_SIZE - kMainStackSize;
            constexpr uint32_t kMainGp = 0x0036A7F0u;

            env.ctx.pc = 0x00100000u;
            setRegU32(env.ctx, 29, kInitialLoaderSp);
            setRegU32(env.ctx, 4, kMainGp);
            setRegU32(env.ctx, 5, 0xFFFFFFFFu);
            setRegU32(env.ctx, 6, kMainStackSize);
            t.IsTrue(callSyscall(0x3Cu, env.rdram.data(), &env.ctx, &env.runtime),
                     "SetupThread syscall should dispatch");
            t.Equals(::getRegU32(&env.ctx, 2), kExpectedStack,
                     "automatic main stack should start below the reserved top-of-RDRAM area");

            // ReferThreadStatus can be called after many nested frames have moved $sp.
            // It must report the initial stack recorded by SetupThread, not this live snapshot.
            constexpr uint32_t kTransientSp = kExpectedStack - 0x80u;
            setRegU32(env.ctx, 29, kTransientSp);
            setRegU32(env.ctx, 4, 0u);
            setRegU32(env.ctx, 5, K_STATUS_ADDR);
            t.IsTrue(callSyscall(0x30u, env.rdram.data(), &env.ctx, &env.runtime),
                     "ReferThreadStatus syscall should dispatch");
            t.Equals(getRegS32(env.ctx, 2), KE_OK, "ReferThreadStatus should accept the current-thread id alias");

            EeThreadStatusAbi status{};
            std::memcpy(&status, env.rdram.data() + K_STATUS_ADDR, sizeof(status));
            t.Equals(status.stack, kExpectedStack,
                     "main thread status must expose SetupThread's stable initial stack");
            t.Equals(status.stack_size, static_cast<int32_t>(kMainStackSize),
                     "main thread status must preserve SetupThread's stack size");
            t.Equals(status.gp_reg, kMainGp,
                     "main thread status must preserve SetupThread's global pointer");
            t.IsTrue(status.stack != kInitialLoaderSp && status.stack != kTransientSp,
                     "main thread status must never expose a live stack-pointer snapshot");
        });

        tc.Run("OSD config2 syscalls round-trip extended config", [](TestCase &t)
        {
            TestEnv env;
            constexpr uint32_t kConfig2Addr = 0x00005000u;
            constexpr uint32_t kConfig2OutAddr = 0x00005010u;
            constexpr uint32_t kConfig1OutAddr = 0x00005020u;
            constexpr uint32_t kInitialConfig1 =
                (1u << 0) |  // SPDIF disabled
                (1u << 4) |  // non-Japanese language flag
                (1u << 13) | // OSD2
                (1u << 16);  // English
            constexpr uint32_t kConfig2Raw =
                0xABu |        // format
                (0xB0u << 8) | // daylightSaving=1, timeFormat=1, dateFormat=2
                (2u << 16) |   // extended OSD version
                (10u << 24);   // traditional Chinese

            writeGuestU32(env.rdram.data(), K_PARAM_ADDR, kInitialConfig1);
            setRegU32(env.ctx, 4, K_PARAM_ADDR);
            t.IsTrue(callSyscall(0x4Au, env.rdram.data(), &env.ctx, &env.runtime),
                     "SetOsdConfigParam syscall should dispatch");
            t.Equals(getRegS32(env.ctx, 2), KE_OK, "SetOsdConfigParam should seed base OSD state");

            writeGuestU32(env.rdram.data(), kConfig2Addr, kConfig2Raw);
            setRegU32(env.ctx, 4, kConfig2Addr);
            setRegU32(env.ctx, 5, 4u);
            setRegU32(env.ctx, 6, 0u);
            t.IsTrue(callSyscall(0x6Eu, env.rdram.data(), &env.ctx, &env.runtime),
                     "SetOsdConfigParam2 syscall should dispatch");
            t.Equals(getRegS32(env.ctx, 2), KE_OK, "SetOsdConfigParam2 should succeed");

            writeGuestU32(env.rdram.data(), kConfig2OutAddr, 0xFFFFFFFFu);
            setRegU32(env.ctx, 4, kConfig2OutAddr);
            setRegU32(env.ctx, 5, 4u);
            setRegU32(env.ctx, 6, 0u);
            t.IsTrue(callSyscall(0x6Fu, env.rdram.data(), &env.ctx, &env.runtime),
                     "GetOsdConfigParam2 syscall should dispatch");
            t.Equals(getRegS32(env.ctx, 2), KE_OK, "GetOsdConfigParam2 should succeed");
            const uint32_t readConfig2 = readGuestU32(env.rdram.data(), kConfig2OutAddr);
            t.Equals(readConfig2, kConfig2Raw, "GetOsdConfigParam2 should round-trip the sanitized Config2Param bytes");
            t.Equals((readConfig2 >> 12) & 1u, 1u, "Config2 daylightSaving should live at bit 12 for libosd callers");

            setRegU32(env.ctx, 4, kConfig1OutAddr);
            t.IsTrue(callSyscall(0x4Bu, env.rdram.data(), &env.ctx, &env.runtime),
                     "GetOsdConfigParam syscall should dispatch after Config2 update");
            const uint32_t readConfig1 = readGuestU32(env.rdram.data(), kConfig1OutAddr);
            t.Equals((readConfig1 >> 13) & 0x7u, 2u, "SetOsdConfigParam2 should sync ConfigParam.version");
            t.Equals((readConfig1 >> 16) & 0x1Fu, 10u, "SetOsdConfigParam2 should sync ConfigParam.language");
        });

        tc.Run("numeric syscall 0x83 finds matching table entry", [](TestCase &t)
        {
            TestEnv env;
            constexpr uint32_t kTableBase = 0x00002000u;
            constexpr uint32_t kValues[] = {
                0x11111111u,
                0x11223344u,
                0x55555555u,
                0x89ABCDEFu
            };

            writeGuestWords(env.rdram.data(), kTableBase, kValues, std::size(kValues));
            setRegU32(env.ctx, 4, kTableBase);
            setRegU32(env.ctx, 5, kTableBase + static_cast<uint32_t>(sizeof(kValues)));
            setRegU32(env.ctx, 6, 0x11223344u);

            t.IsTrue(callSyscall(0x83u, env.rdram.data(), &env.ctx, &env.runtime),
                     "syscall 0x83 should dispatch");
            t.Equals(static_cast<uint32_t>(getRegS32(env.ctx, 2)),
                     kTableBase + 4u,
                     "FindAddress should return address of first matching word");
        });

        tc.Run("numeric syscall 0x83 supports KSEG aliases", [](TestCase &t)
        {
            TestEnv env;
            constexpr uint32_t kTableBasePhys = 0x00003000u;
            constexpr uint32_t kTableBaseKseg = 0x80003000u;
            constexpr uint32_t kValues[] = {
                0x00123456u,
                0x8000AAAAu
            };

            writeGuestWords(env.rdram.data(), kTableBasePhys, kValues, std::size(kValues));
            setRegU32(env.ctx, 4, kTableBaseKseg);
            setRegU32(env.ctx, 5, kTableBaseKseg + static_cast<uint32_t>(sizeof(kValues)));
            setRegU32(env.ctx, 6, 0x80123456u); // Alias of first table value

            t.IsTrue(callSyscall(0x83u, env.rdram.data(), &env.ctx, &env.runtime),
                     "syscall 0x83 should dispatch");
            t.Equals(static_cast<uint32_t>(getRegS32(env.ctx, 2)),
                     kTableBaseKseg,
                     "FindAddress should match KSEG aliases and preserve guest segment in return value");
        });

        tc.Run("numeric syscall 0x83 returns 0 when entry is absent", [](TestCase &t)
        {
            TestEnv env;
            constexpr uint32_t kTableBase = 0x00004000u;
            constexpr uint32_t kValues[] = {
                0x00000001u,
                0x00000002u,
                0x00000003u
            };

            writeGuestWords(env.rdram.data(), kTableBase, kValues, std::size(kValues));
            setRegU32(env.ctx, 4, kTableBase);
            setRegU32(env.ctx, 5, kTableBase + static_cast<uint32_t>(sizeof(kValues)));
            setRegU32(env.ctx, 6, 0xDEADBEEFu);

            t.IsTrue(callSyscall(0x83u, env.rdram.data(), &env.ctx, &env.runtime),
                     "syscall 0x83 should dispatch");
            t.Equals(static_cast<uint32_t>(getRegS32(env.ctx, 2)),
                     0u,
                     "FindAddress should return 0 when no matching word exists");
        });

        tc.Run("SetSyscall mirrors guest kernel table entries into low memory", [](TestCase &t)
        {
            TestEnv env;
            initializeGuestKernelState(env.rdram.data(), &env.runtime);

            constexpr uint32_t kGuestSyscallTableGuestBase = 0x80011F80u;
            constexpr uint32_t kSyscallIndex = 0x82u;
            constexpr uint32_t kHandler = 0x00383548u;
            constexpr uint32_t kExpectedGuestAddr = kGuestSyscallTableGuestBase + (kSyscallIndex * 4u);
            constexpr uint32_t kExpectedPhysAddr = kExpectedGuestAddr & 0x1FFFFFFFu;

            setRegU32(env.ctx, 4, kSyscallIndex);
            setRegU32(env.ctx, 5, kHandler);
            t.IsTrue(callSyscall(0x74u, env.rdram.data(), &env.ctx, &env.runtime),
                     "SetSyscall syscall should dispatch");

            uint32_t mirrored = 0u;
            std::memcpy(&mirrored, env.rdram.data() + kExpectedPhysAddr, sizeof(mirrored));
            t.Equals(mirrored,
                     kHandler,
                     "SetSyscall should mirror handler pointers into the guest kernel syscall table");

            setRegU32(env.ctx, 4, 0x80000000u);
            setRegU32(env.ctx, 5, 0x80080000u);
            setRegU32(env.ctx, 6, kHandler);
            t.IsTrue(callSyscall(0x83u, env.rdram.data(), &env.ctx, &env.runtime),
                     "FindAddress syscall should dispatch");
            t.Equals(static_cast<uint32_t>(getRegS32(env.ctx, 2)),
                     kExpectedGuestAddr,
                     "FindAddress should discover mirrored SetSyscall entries in low guest memory");
        });

        tc.Run("SetSyscall honors signed kernel-table offsets", [](TestCase &t)
        {
            TestEnv env;
            initializeGuestKernelState(env.rdram.data(), &env.runtime);

            constexpr uint32_t kPatchIndex = 0xFFFFC402u;
            constexpr uint32_t kHandler = 0xDEADBEEFu;
            constexpr uint32_t kExpectedGuestAddr = 0x80002F88u;
            constexpr uint32_t kExpectedPhysAddr = kExpectedGuestAddr & 0x1FFFFFFFu;

            setRegU32(env.ctx, 4, kPatchIndex);
            setRegU32(env.ctx, 5, kHandler);
            t.IsTrue(callSyscall(0x74u, env.rdram.data(), &env.ctx, &env.runtime),
                     "SetSyscall syscall should dispatch for signed offsets");

            uint32_t mirrored = 0u;
            std::memcpy(&mirrored, env.rdram.data() + kExpectedPhysAddr, sizeof(mirrored));
            t.Equals(mirrored,
                     kHandler,
                     "SetSyscall should treat the syscall index as a signed offset from the kernel table base");
        });

        tc.Run("guest kernel syscall overrides and mirrors are isolated per runtime", [](TestCase &t)
        {
            TestEnv first;
            TestEnv second;
            initializeGuestKernelState(first.rdram.data(), &first.runtime);
            initializeGuestKernelState(second.rdram.data(), &second.runtime);

            constexpr uint32_t kGuestSyscallTableGuestBase = 0x80011F80u;
            constexpr uint32_t kGuestSyscallTableProbeBase = 0x000002F0u;
            constexpr uint32_t kSyscallIndex = 0x5Au;
            constexpr uint32_t kFirstHandler = 0x00383510u;
            constexpr uint32_t kSecondHandler = 0x00383520u;
            constexpr uint32_t kEntryPhysAddr = (kGuestSyscallTableGuestBase + (kSyscallIndex * 4u)) & 0x1FFFFFFFu;

            setRegU32(first.ctx, 4, kSyscallIndex);
            setRegU32(first.ctx, 5, kFirstHandler);
            t.IsTrue(callSyscall(0x74u, first.rdram.data(), &first.ctx, &first.runtime),
                     "SetSyscall should install the first runtime's override");

            uint32_t firstHandler = 0u;
            uint32_t secondHandler = 0u;
            t.IsTrue(first.runtime.findEeSyscallOverride(kSyscallIndex, firstHandler),
                     "the first runtime should own its override");
            t.IsFalse(second.runtime.findEeSyscallOverride(kSyscallIndex, secondHandler),
                      "a different runtime must not observe the first runtime's override");

            setRegU32(second.ctx, 4, kSyscallIndex);
            setRegU32(second.ctx, 5, kSecondHandler);
            t.IsTrue(callSyscall(0x74u, second.rdram.data(), &second.ctx, &second.runtime),
                     "SetSyscall should install an independent second-runtime override");

            uint32_t firstMirrored = 0u;
            uint32_t secondMirrored = 0u;
            std::memcpy(&firstMirrored, first.rdram.data() + kEntryPhysAddr, sizeof(firstMirrored));
            std::memcpy(&secondMirrored, second.rdram.data() + kEntryPhysAddr, sizeof(secondMirrored));
            t.Equals(firstMirrored, kFirstHandler, "the first runtime should retain its own mirror value");
            t.Equals(secondMirrored, kSecondHandler, "the second runtime should publish only its own mirror value");

            initializeGuestKernelState(first.rdram.data(), &first.runtime);
            std::memcpy(&firstMirrored, first.rdram.data() + kEntryPhysAddr, sizeof(firstMirrored));
            t.Equals(firstMirrored, kFirstHandler,
                     "reinitializing one runtime should rebuild its mirror from its instance-owned overrides");

            uint32_t probeHi = 0u;
            uint32_t probeLo = 0u;
            std::memcpy(&probeHi, first.rdram.data() + kGuestSyscallTableProbeBase + 0u, sizeof(probeHi));
            std::memcpy(&probeLo, first.rdram.data() + kGuestSyscallTableProbeBase + 8u, sizeof(probeLo));
            t.Equals(probeHi,
                     kGuestSyscallTableGuestBase >> 16,
                     "Guest kernel initialization should seed the syscall table probe high word");
            t.Equals(probeLo,
                     kGuestSyscallTableGuestBase & 0xFFFFu,
                     "Guest kernel initialization should seed the syscall table probe low word");
        });

        tc.Run("SetSyscall override runs as a scheduler invocation", [](TestCase &t)
        {
            TestEnv env;
            constexpr uint32_t kSyscallIndex = 0x91u;
            constexpr uint32_t kHandler = 0x00200000u;

            env.runtime.registerFunction(kHandler, overrideReturnHandler);
            env.runtime.registerFunction(K_OVERRIDE_ENTRY, schedulerOverrideEntry);
            env.runtime.registerFunction(K_OVERRIDE_RESUME, schedulerOverrideResume);
            env.runtime.setEeSyscallOverride(env.rdram.data(), kSyscallIndex, kHandler);

            gOverrideSyscall = kSyscallIndex;
            R5900Context mainContext{};
            mainContext.pc = K_OVERRIDE_ENTRY;
            setRegU32(mainContext, 4, 7u);
            setRegU32(mainContext, 5, 5u);
            env.runtime.eeScheduler().reset(env.rdram.data(), mainContext);
            env.runtime.eeScheduler().run();

            t.Equals(static_cast<uint32_t>(getRegS32(gOverrideResult, 2)),
                     12u,
                     "the completed invocation should propagate the guest handler return value");
        });

        tc.Run("SetSyscall override preserves KSEG argument sign extension", [](TestCase &t)
        {
            TestEnv env;
            constexpr uint32_t kSyscallIndex = 0x92u;
            constexpr uint32_t kHandler = 0x00200030u;

            env.runtime.registerFunction(kHandler, overrideKsegCompareHandler);
            env.runtime.registerFunction(K_OVERRIDE_ENTRY, schedulerOverrideEntry);
            env.runtime.registerFunction(K_OVERRIDE_RESUME, schedulerOverrideResume);
            env.runtime.setEeSyscallOverride(env.rdram.data(), kSyscallIndex, kHandler);

            gOverrideSyscall = kSyscallIndex;
            R5900Context mainContext{};
            mainContext.pc = K_OVERRIDE_ENTRY;
            setRegU32(mainContext, 4, 0x80000000u);
            setRegU32(mainContext, 5, 0x80080000u);
            env.runtime.eeScheduler().reset(env.rdram.data(), mainContext);
            env.runtime.eeScheduler().run();

            t.Equals(static_cast<uint32_t>(getRegS32(gOverrideResult, 2)),
                     0x80000004u,
                     "Override invocation should preserve KSEG ordering after 32-bit guest writes");
        });

        tc.Run("SetSyscall override preserves upper 64 bits when writing 32-bit args", [](TestCase &t)
        {
            TestEnv env;
            constexpr uint32_t kSyscallIndex = 0x93u;
            constexpr uint32_t kHandler = 0x00200040u;

            env.runtime.registerFunction(kHandler, overridePreserveUpper64Handler);
            env.runtime.registerFunction(K_OVERRIDE_ENTRY, schedulerOverrideEntry);
            env.runtime.registerFunction(K_OVERRIDE_RESUME, schedulerOverrideResume);
            env.runtime.setEeSyscallOverride(env.rdram.data(), kSyscallIndex, kHandler);

            gOverrideSyscall = kSyscallIndex;
            R5900Context mainContext{};
            mainContext.pc = K_OVERRIDE_ENTRY;
            mainContext.r[4] = _mm_set_epi64x(static_cast<int64_t>(K_EXPECTED_UPPER64),
                                               static_cast<int64_t>(static_cast<int32_t>(0x80000000u)));
            env.runtime.eeScheduler().reset(env.rdram.data(), mainContext);
            env.runtime.eeScheduler().run();

            t.Equals(static_cast<uint32_t>(getRegS32(gOverrideResult, 2)),
                     1u,
                     "Override invocation should preserve the upper 64 bits of 128-bit GPRs when setting 32-bit args");
        });

        tc.Run("an override that branches to an invalid PC completes without builtin fallback", [](TestCase &t)
        {
            TestEnv env;
            constexpr uint32_t kHandler = 0x00200010u;

            env.runtime.registerFunction(kHandler, overrideBrokenHandler);
            env.runtime.registerFunction(K_OVERRIDE_ENTRY, schedulerOverrideEntry);
            env.runtime.registerFunction(K_OVERRIDE_RESUME, schedulerOverrideResume);
            env.runtime.setEeSyscallOverride(env.rdram.data(), 0x83u, kHandler);

            gOverrideSyscall = 0x83u;
            R5900Context mainContext{};
            mainContext.pc = K_OVERRIDE_ENTRY;
            env.runtime.eeScheduler().reset(env.rdram.data(), mainContext);
            env.runtime.eeScheduler().run();

            t.Equals(static_cast<uint32_t>(getRegS32(gOverrideResult, 2)),
                     0xDEADBEEFu,
                     "the dispatcher should preserve the invocation result and never call the builtin as a fallback");
        });

        tc.Run("reentrant override invokes the underlying builtin inside its invocation frame", [](TestCase &t)
        {
            TestEnv env;
            constexpr uint32_t kHandler = 0x00200020u;
            constexpr uint32_t kTableBase = 0x00003000u;
            constexpr uint32_t kValues[] = {
                0xCAFEBABEu,
                0x11223344u,
                0x55667788u
            };

            env.runtime.registerFunction(kHandler, overrideRecursiveFindAddressHandler);
            env.runtime.registerFunction(K_OVERRIDE_ENTRY, schedulerOverrideEntry);
            env.runtime.registerFunction(K_OVERRIDE_RESUME, schedulerOverrideResume);
            env.runtime.setEeSyscallOverride(env.rdram.data(), 0x83u, kHandler);

            writeGuestWords(env.rdram.data(), kTableBase, kValues, std::size(kValues));
            gOverrideSyscall = 0x83u;
            R5900Context mainContext{};
            mainContext.pc = K_OVERRIDE_ENTRY;
            setRegU32(mainContext, 4, kTableBase);
            setRegU32(mainContext, 5, kTableBase + static_cast<uint32_t>(sizeof(kValues)));
            setRegU32(mainContext, 6, 0x11223344u);
            env.runtime.eeScheduler().reset(env.rdram.data(), mainContext);
            env.runtime.eeScheduler().run();

            t.Equals(static_cast<uint32_t>(getRegS32(gOverrideResult, 2)),
                     kTableBase + 4u,
                     "only the recursive call should bypass the active override frame and reach the builtin");
        });

        tc.Run("a syscall invocation can block and resume without losing its base context", [](TestCase &t)
        {
            TestEnv env;
            constexpr uint32_t kSyscallIndex = 0x94u;
            env.runtime.registerFunction(K_OVERRIDE_BLOCK_ENTRY, schedulerBlockingOverrideEntry);
            env.runtime.registerFunction(K_OVERRIDE_BLOCK_HANDLER, schedulerBlockingOverrideHandler);
            env.runtime.registerFunction(K_OVERRIDE_BLOCK_HANDLER_RESUME, schedulerBlockingOverrideHandlerResume);
            env.runtime.registerFunction(K_OVERRIDE_BLOCK_DRIVER, schedulerBlockingOverrideDriver);
            env.runtime.registerFunction(K_OVERRIDE_BLOCK_BASE_RESUME, schedulerBlockingOverrideBaseResume);
            env.runtime.setEeSyscallOverride(env.rdram.data(), kSyscallIndex, K_OVERRIDE_BLOCK_HANDLER);

            gOverrideSyscall = kSyscallIndex;
            gInvocationTrace.clear();
            R5900Context mainContext{};
            mainContext.pc = K_OVERRIDE_BLOCK_ENTRY;
            env.runtime.eeScheduler().reset(env.rdram.data(), mainContext);
            env.runtime.eeScheduler().run();

            const std::vector<int> expected{1, 2, 3, 4, 5};
            t.IsTrue(gInvocationTrace == expected,
                     "the sleeping invocation should yield to the driver, resume its own frame, then restore the base frame");
            t.Equals(static_cast<uint32_t>(getRegS32(gOverrideResult, 2)), 0xB10C0EDu,
                     "the invocation result should reach the preserved base context after the wait");
        });

        tc.Run("exit handlers run as ordered invocation frames before the thread becomes dormant", [](TestCase &t)
        {
            TestEnv env;
            env.runtime.registerFunction(K_EXIT_MAIN, schedulerExitMain);
            env.runtime.registerFunction(K_EXIT_HANDLER_A, schedulerExitHandlerA);
            env.runtime.registerFunction(K_EXIT_HANDLER_B, schedulerExitHandlerB);
            env.runtime.registerFunction(K_EXIT_OBSERVER, schedulerExitObserver);

            gInvocationTrace.clear();
            R5900Context mainContext{};
            mainContext.pc = K_EXIT_MAIN;
            env.runtime.eeScheduler().reset(env.rdram.data(), mainContext);
            env.runtime.eeScheduler().run();

            const std::vector<int> expected{10, 20, 30, 40};
            t.IsTrue(gInvocationTrace == expected,
                     "exit handlers should retain registration order and complete before another guest thread observes dormancy");
        });

        tc.Run("Copy syscall (0x5A) performs a memory copy", [](TestCase &t)
        {
            TestEnv env;
            constexpr uint32_t kDestAddr = 0x00005000u;
            constexpr uint32_t kSrcAddr = 0x00006000u;
            constexpr uint32_t kSize = 16u;
            constexpr uint32_t kValues[] = {
                0x11223344u,
                0x55667788u,
                0x99AABBCCu,
                0xDDEEFF00u
            };

            writeGuestWords(env.rdram.data(), kSrcAddr, kValues, std::size(kValues));
            
            setRegU32(env.ctx, 4, kDestAddr);
            setRegU32(env.ctx, 5, kSrcAddr);
            setRegU32(env.ctx, 6, kSize);

            t.IsTrue(callSyscall(0x5Au, env.rdram.data(), &env.ctx, &env.runtime),
                     "Copy syscall should dispatch");
            
            for (size_t i = 0; i < std::size(kValues); ++i)
            {
                uint32_t destVal = readGuestU32(env.rdram.data(), kDestAddr + static_cast<uint32_t>(i * sizeof(uint32_t)));
                t.Equals(destVal, kValues[i], "Copy should correctly transfer bytes");
            }
        });

        tc.Run("GetEntryAddress syscall (0x5B) returns handler from guest table", [](TestCase &t)
        {
            TestEnv env;
            initializeGuestKernelState(env.rdram.data(), &env.runtime);

            constexpr uint32_t kGuestSyscallTableGuestBase = 0x80011F80u;
            constexpr uint32_t kSyscallIndex = 0x5Au;
            constexpr uint32_t kExpectedHandler = 0x00383548u;
            constexpr uint32_t kEntryPhysAddr = (kGuestSyscallTableGuestBase + (kSyscallIndex * 4u)) & 0x1FFFFFFFu;

            writeGuestU32(env.rdram.data(), kEntryPhysAddr, kExpectedHandler);

            setRegU32(env.ctx, 4, kSyscallIndex);

            t.IsTrue(callSyscall(0x5Bu, env.rdram.data(), &env.ctx, &env.runtime),
                     "GetEntryAddress syscall should dispatch");
            
            t.Equals(static_cast<uint32_t>(getRegS32(env.ctx, 2)),
                     kExpectedHandler,
                     "GetEntryAddress should read and return the handler address from the table");
        });

        tc.Run("SIF handshake stays off without the SSX3 override (P1ac gate)", [](TestCase &t)
        {
            TestEnv env;
            resetSsx3SifHandshakeForTesting();

            constexpr uint32_t kPacketAddr = 0x00009000u;
            constexpr uint32_t kSregs1Addr = 0x52BE04u;
            constexpr uint32_t kPacket[] = {0u, 0u, 0u, 0u, 1u, 1u};
            writeGuestWords(env.rdram.data(), kPacketAddr, kPacket, std::size(kPacket));

            setRegU32(env.ctx, 4, 0x80000001u);
            setRegU32(env.ctx, 5, kPacketAddr);
            setRegU32(env.ctx, 6, 0x18u);
            setRegU32(env.ctx, 7, 0u);
            setRegU32(env.ctx, 29, 0x00100000u);
            sceSifSendCmd(env.rdram.data(), &env.ctx, &env.runtime);

            t.Equals(getRegS32(env.ctx, 2), 1, "SendCmd must still return 1 with the gate off");
            t.Equals(readGuestU32(env.rdram.data(), kSregs1Addr), 0u,
                     "sregs[1] must stay 0 when the SSX3 override did not apply");
            resetSsx3SifHandshakeForTesting();
        });

        tc.Run("SIF handshake writes sregs[1]=1 for SSX3 SET_SREG(1,1) (P1ac)", [](TestCase &t)
        {
            TestEnv env;
            resetSsx3SifHandshakeForTesting();
            ps2_game_overrides::applyMatching(env.runtime, "SLUS_207.72", 0x00100008u, 0u, false);

            constexpr uint32_t kPacketAddr = 0x00009000u;
            constexpr uint32_t kSregs1Addr = 0x52BE04u;
            constexpr uint32_t kPacket[] = {0u, 0u, 0u, 0u, 1u, 1u};
            writeGuestWords(env.rdram.data(), kPacketAddr, kPacket, std::size(kPacket));

            setRegU32(env.ctx, 4, 0x80000001u);
            setRegU32(env.ctx, 5, kPacketAddr);
            setRegU32(env.ctx, 6, 0x18u);
            setRegU32(env.ctx, 7, 0u);
            setRegU32(env.ctx, 29, 0x00100000u);
            sceSifSendCmd(env.rdram.data(), &env.ctx, &env.runtime);

            t.Equals(getRegS32(env.ctx, 2), 1, "SendCmd must still return 1");
            t.Equals(readGuestU32(env.rdram.data(), kSregs1Addr), 1u,
                     "SSX3 SET_SREG(1,1) must complete the handshake with sregs[1]=1");
            resetSsx3SifHandshakeForTesting();
        });

        tc.Run("SIF handshake ignores other cids and sreg slots (P1ac)", [](TestCase &t)
        {
            TestEnv env;
            resetSsx3SifHandshakeForTesting();
            ps2_game_overrides::applyMatching(env.runtime, "SLUS_207.72", 0x00100008u, 0u, false);

            constexpr uint32_t kPacketAddr = 0x00009000u;
            constexpr uint32_t kSregs1Addr = 0x52BE04u;

            constexpr uint32_t kBindPacket[] = {0u, 0u, 0u, 0u, 1u, 1u};
            writeGuestWords(env.rdram.data(), kPacketAddr, kBindPacket, std::size(kBindPacket));
            setRegU32(env.ctx, 4, 0x00000019u);
            setRegU32(env.ctx, 5, kPacketAddr);
            setRegU32(env.ctx, 6, 0x18u);
            setRegU32(env.ctx, 7, 0u);
            setRegU32(env.ctx, 29, 0x00100000u);
            sceSifSendCmd(env.rdram.data(), &env.ctx, &env.runtime);
            t.Equals(getRegS32(env.ctx, 2), 1, "SendCmd must still return 1 for other cids");
            t.Equals(readGuestU32(env.rdram.data(), kSregs1Addr), 0u,
                     "a non-SET_SREG cid must not touch sregs[1]");

            constexpr uint32_t kOtherSlot[] = {0u, 0u, 0u, 0u, 2u, 1u};
            writeGuestWords(env.rdram.data(), kPacketAddr, kOtherSlot, std::size(kOtherSlot));
            setRegU32(env.ctx, 4, 0x80000001u);
            sceSifSendCmd(env.rdram.data(), &env.ctx, &env.runtime);
            t.Equals(getRegS32(env.ctx, 2), 1, "SendCmd must still return 1 for other sreg slots");
            t.Equals(readGuestU32(env.rdram.data(), kSregs1Addr), 0u,
                     "SET_SREG for a slot other than 1 must not touch sregs[1]");
            resetSsx3SifHandshakeForTesting();
        });

        tc.Run("SSX3 copied payload lookup serves all six installer keys (K1)", [](TestCase &t)
        {
            TestEnv env;
            resetSsx3CopiedPayloadForTesting();
            ps2_game_overrides::applyMatching(env.runtime, "SLUS_207.72", 0x00100008u, 0u, false);

            // The game's own six pairs (ELF 0x4564C0, destination 0x80075300).
            constexpr uint32_t kPairs[] = {
                0x55u, 0x80075038u, 0x56u, 0x800750C8u, 0x57u, 0x80075108u,
                0x58u, 0x80075158u, 0x59u, 0x800751A8u, 0x03u, 0x80075330u,
            };
            constexpr uint32_t kSrc = 0x4561C0u;
            constexpr uint32_t kDstPhys = 0x75000u;
            constexpr uint32_t kSize = 0x330u;
            for (uint32_t i = 0; i < kSize; ++i)
            {
                env.rdram[kSrc + i] = static_cast<uint8_t>((i * 31u + 7u) & 0xFFu);
            }
            writeGuestWords(env.rdram.data(), kSrc + 0x300u, kPairs, std::size(kPairs));
            std::memcpy(env.rdram.data() + kDstPhys, env.rdram.data() + kSrc, kSize);

            env.runtime.setEeSyscallOverride(env.rdram.data(), 0x5Bu, 0x80075000u);
            for (size_t i = 0; i < std::size(kPairs); i += 2)
            {
                setRegU32(env.ctx, 4, kPairs[i]);
                t.IsTrue(callSyscall(0x5Bu, env.rdram.data(), &env.ctx, &env.runtime),
                         "copied-payload lookup should dispatch");
                t.Equals(static_cast<uint32_t>(getRegS32(env.ctx, 2)), kPairs[i + 1],
                         "copied-payload lookup should return the payload's own value");
            }
            setRegU32(env.ctx, 4, 0x54u);
            t.IsTrue(callSyscall(0x5Bu, env.rdram.data(), &env.ctx, &env.runtime),
                     "a lookup miss should still dispatch");
            t.Equals(static_cast<uint32_t>(getRegS32(env.ctx, 2)), 0u,
                     "a lookup miss should return 0 like the payload");
            resetSsx3CopiedPayloadForTesting();
        });

        tc.Run("SSX3 copied payload helpers validate like the payload (K1)", [](TestCase &t)
        {
            TestEnv env;
            resetSsx3CopiedPayloadForTesting();
            ps2_game_overrides::applyMatching(env.runtime, "SLUS_207.72", 0x00100008u, 0u, false);

            constexpr uint32_t kSrc = 0x4561C0u;
            constexpr uint32_t kDstPhys = 0x75000u;
            constexpr uint32_t kSize = 0x330u;
            for (uint32_t i = 0; i < kSize; ++i)
            {
                env.rdram[kSrc + i] = static_cast<uint8_t>((i * 31u + 7u) & 0xFFu);
            }
            std::memcpy(env.rdram.data() + kDstPhys, env.rdram.data() + kSrc, kSize);

            env.runtime.setEeSyscallOverride(env.rdram.data(), 0x55u, 0x80075038u);
            env.runtime.setEeSyscallOverride(env.rdram.data(), 0x56u, 0x800750C8u);
            env.runtime.setEeSyscallOverride(env.rdram.data(), 0x57u, 0x80075108u);
            env.runtime.setEeSyscallOverride(env.rdram.data(), 0x58u, 0x80075158u);
            env.runtime.setEeSyscallOverride(env.rdram.data(), 0x59u, 0x800751A8u);

            setRegU32(env.ctx, 4, 0x2Fu);
            t.IsTrue(callSyscall(0x56u, env.rdram.data(), &env.ctx, &env.runtime), "0x56 should dispatch");
            t.Equals(getRegS32(env.ctx, 2), 0x2F, "0x56 with a0<0x30 should return the index");
            setRegU32(env.ctx, 4, 0x30u);
            t.IsTrue(callSyscall(0x56u, env.rdram.data(), &env.ctx, &env.runtime), "0x56 should dispatch");
            t.Equals(getRegS32(env.ctx, 2), KE_ERROR, "0x56 with a0>=0x30 should return -1");

            constexpr uint32_t kOut0 = 0x00002000u;
            constexpr uint32_t kOut1 = 0x00002100u;
            constexpr uint32_t kOut2 = 0x00002200u;
            constexpr uint32_t kOut3 = 0x00002300u;
            writeGuestU32(env.rdram.data(), kOut0, 0xDEADBEEFu);
            writeGuestU32(env.rdram.data(), kOut1, 0xDEADBEEFu);
            writeGuestU32(env.rdram.data(), kOut2, 0xDEADBEEFu);
            writeGuestU32(env.rdram.data(), kOut3, 0xDEADBEEFu);
            setRegU32(env.ctx, 4, 0x10u);
            setRegU32(env.ctx, 5, kOut0);
            setRegU32(env.ctx, 6, kOut1);
            setRegU32(env.ctx, 7, kOut2);
            setRegU32(env.ctx, 8, kOut3);
            t.IsTrue(callSyscall(0x57u, env.rdram.data(), &env.ctx, &env.runtime), "0x57 should dispatch");
            t.Equals(getRegS32(env.ctx, 2), 0x10, "0x57 with a0<0x30 should return the index");
            t.Equals(readGuestU32(env.rdram.data(), kOut0), 0u, "0x57 should store empty-TLB zeros");
            t.Equals(readGuestU32(env.rdram.data(), kOut1), 0u, "0x57 should store empty-TLB zeros");
            t.Equals(readGuestU32(env.rdram.data(), kOut2), 0u, "0x57 should store empty-TLB zeros");
            t.Equals(readGuestU32(env.rdram.data(), kOut3), 0u, "0x57 should store empty-TLB zeros");
            setRegU32(env.ctx, 4, 0x30u);
            t.IsTrue(callSyscall(0x57u, env.rdram.data(), &env.ctx, &env.runtime), "0x57 should dispatch");
            t.Equals(getRegS32(env.ctx, 2), KE_ERROR, "0x57 with a0>=0x30 should return -1");

            for (const uint32_t a1 : {0x00123456u, 0x309ABCDEu, 0x40FEDCBAu})
            {
                setRegU32(env.ctx, 5, a1);
                t.IsTrue(callSyscall(0x55u, env.rdram.data(), &env.ctx, &env.runtime),
                         "0x55 should dispatch");
                t.Equals(getRegS32(env.ctx, 2), 0, "0x55 with a valid top nibble should succeed");
            }
            for (const uint32_t a1 : {0x10123456u, 0x20123456u, 0x50123456u})
            {
                setRegU32(env.ctx, 5, a1);
                t.IsTrue(callSyscall(0x55u, env.rdram.data(), &env.ctx, &env.runtime),
                         "0x55 should dispatch");
                t.Equals(getRegS32(env.ctx, 2), KE_ERROR, "0x55 with an invalid top nibble should return -1");
            }

            t.IsTrue(callSyscall(0x58u, env.rdram.data(), &env.ctx, &env.runtime), "0x58 should dispatch");
            t.Equals(getRegS32(env.ctx, 2), KE_ERROR, "0x58 with no HLE mappings should miss with -1");

            setRegU32(env.ctx, 4, 0x1001u);
            t.IsTrue(callSyscall(0x59u, env.rdram.data(), &env.ctx, &env.runtime), "0x59 should dispatch");
            t.Equals(getRegS32(env.ctx, 2), KE_ERROR, "0x59 with a misaligned size should return -1");
            setRegU32(env.ctx, 4, 0x1000u);
            t.IsTrue(callSyscall(0x59u, env.rdram.data(), &env.ctx, &env.runtime), "0x59 should dispatch");
            t.Equals(getRegS32(env.ctx, 2), KE_ERROR, "0x59 with a small size should return -1");
            setRegU32(env.ctx, 4, 0u);
            t.IsTrue(callSyscall(0x59u, env.rdram.data(), &env.ctx, &env.runtime), "0x59 should dispatch");
            t.Equals(getRegS32(env.ctx, 2), 0, "0x59 with size 0 should succeed with 0");
            setRegU32(env.ctx, 4, 0x10000000u);
            t.IsTrue(callSyscall(0x59u, env.rdram.data(), &env.ctx, &env.runtime), "0x59 should dispatch");
            t.Equals(getRegS32(env.ctx, 2), 0, "0x59 should allocate wired index 0 first");
            t.IsTrue(callSyscall(0x59u, env.rdram.data(), &env.ctx, &env.runtime), "0x59 should dispatch");
            t.Equals(getRegS32(env.ctx, 2), 1, "0x59 should allocate wired index 1 next");
            resetSsx3CopiedPayloadForTesting();
        });

        tc.Run("SSX3 copied payload refuses broken copies and other games (K1)", [](TestCase &t)
        {
            TestEnv env;
            resetSsx3CopiedPayloadForTesting();

            constexpr uint32_t kSrc = 0x4561C0u;
            constexpr uint32_t kDstPhys = 0x75000u;
            constexpr uint32_t kSize = 0x330u;
            for (uint32_t i = 0; i < kSize; ++i)
            {
                env.rdram[kSrc + i] = static_cast<uint8_t>((i * 31u + 7u) & 0xFFu);
            }
            std::memcpy(env.rdram.data() + kDstPhys, env.rdram.data() + kSrc, kSize);
            env.runtime.setEeSyscallOverride(env.rdram.data(), 0x5Bu, 0x80075000u);

            setRegU32(env.ctx, 4, 0x55u);
            t.IsTrue(callSyscall(0x5Bu, env.rdram.data(), &env.ctx, &env.runtime),
                     "an unarmed quirk should still dispatch");
            t.Equals(getRegS32(env.ctx, 2), KE_ERROR,
                     "an unarmed quirk should keep the existing KE_ERROR drop path");

            ps2_game_overrides::applyMatching(env.runtime, "SLUS_207.72", 0x00100008u, 0u, false);
            env.rdram[kDstPhys + 0x10u] ^= 0xFFu;
            setRegU32(env.ctx, 4, 0x55u);
            t.IsTrue(callSyscall(0x5Bu, env.rdram.data(), &env.ctx, &env.runtime),
                     "a broken copy should still dispatch");
            t.Equals(getRegS32(env.ctx, 2), KE_ERROR,
                     "a broken copy should keep the existing KE_ERROR drop path");

            env.rdram[kDstPhys + 0x10u] ^= 0xFFu;
            env.runtime.setEeSyscallOverride(env.rdram.data(), 0x54u, 0x80075000u);
            setRegU32(env.ctx, 4, 0x55u);
            t.IsTrue(callSyscall(0x54u, env.rdram.data(), &env.ctx, &env.runtime),
                     "an unmatched pair should still dispatch");
            t.Equals(getRegS32(env.ctx, 2), KE_ERROR,
                     "an unmatched (syscall, handler) pair should keep the KE_ERROR drop path");
            resetSsx3CopiedPayloadForTesting();
        });

        tc.Run("SetSyscall publishes markers the scanner cross-checks (K1)", [](TestCase &t)
        {
            TestEnv env;
            constexpr uint32_t kTableBase = 0x80011F80u;
            setRegU32(env.ctx, 4, 0x83u);
            setRegU32(env.ctx, 5, 0x42C168u);
            t.IsTrue(callSyscall(0x74u, env.rdram.data(), &env.ctx, &env.runtime),
                     "SetSyscall should dispatch for the 0x83 marker");
            setRegU32(env.ctx, 4, 0x5Au);
            setRegU32(env.ctx, 5, 0x42C130u);
            t.IsTrue(callSyscall(0x74u, env.rdram.data(), &env.ctx, &env.runtime),
                     "SetSyscall should dispatch for the 0x5A marker");

            constexpr uint32_t kMarkA = (kTableBase + 0x83u * 4u) & 0x1FFFFFFFu;
            constexpr uint32_t kMarkB = (kTableBase + 0x5Au * 4u) & 0x1FFFFFFFu;
            t.Equals(readGuestU32(env.rdram.data(), kMarkA), 0x42C168u,
                     "the 0x83 marker should be visible in the KSEG0 mirror");
            t.Equals(readGuestU32(env.rdram.data(), kMarkB), 0x42C130u,
                     "the 0x5A marker should be visible in the KSEG0 mirror");
            t.Equals(kMarkA - 0x83u * 4u, kMarkB - 0x5Au * 4u,
                     "both markers should cross-check to the same table base");
        });

        tc.Run("E3 overlap math normalizes aliases and splits wraps (E3b)", [](TestCase &t)
        {
            uint64_t parsed = 0u;
            t.IsTrue(ps2_e3::parseU64("1000", parsed) && parsed == 1000u, "decimal INV should parse");
            t.IsTrue(ps2_e3::parseU64("0x70001C00", parsed) && parsed == 0x70001C00u, "hex base should parse");
            t.IsTrue(!ps2_e3::parseU64("", parsed), "empty should not parse");
            t.IsTrue(!ps2_e3::parseU64("12x", parsed), "trailing junk should not parse");

            const ps2_e3::NormAddr raw = ps2_e3::normAddr(0x501420u);
            t.IsTrue(raw.space == ps2_e3::kRamSpace && raw.off == 0x501420u, "raw RAM should stay put");
            const ps2_e3::NormAddr kseg0 = ps2_e3::normAddr(0x8501420u);
            t.IsTrue(kseg0.space == ps2_e3::kRamSpace && kseg0.off == 0x501420u, "KSEG0 should strip to RAM");
            const ps2_e3::NormAddr kseg1 = ps2_e3::normAddr(0xB001420u);
            t.IsTrue(kseg1.space == ps2_e3::kRamSpace && kseg1.off == 0x1001420u, "KSEG1 should strip to RAM");
            const ps2_e3::NormAddr spr = ps2_e3::normAddr(0x70001C10u);
            t.IsTrue(spr.space == ps2_e3::kSprSpace && spr.off == 0x1C10u, "scratchpad should map to SPR");
            const ps2_e3::NormAddr sprAlias = ps2_e3::normAddr(0xF0000010u);
            t.IsTrue(sprAlias.space == ps2_e3::kSprSpace && sprAlias.off == 0x10u, "SPR alias should map to SPR");

            ps2_e3::Interval ivs[4];
            t.Equals(ps2_e3::splitIntervals(0x501420u, 32u, ivs, 4u), static_cast<size_t>(1u),
                     "plain RAM range should be one interval");
            t.IsTrue(ivs[0].space == ps2_e3::kRamSpace && ivs[0].off == 0x501420u && ivs[0].len == 32u,
                     "plain interval should keep addr/len");
            t.Equals(ps2_e3::splitIntervals(0x1FFFFF8u, 16u, ivs, 4u), static_cast<size_t>(2u),
                     "RAM wrap should split in two");
            t.IsTrue(ivs[0].off == 0x1FFFFF8u && ivs[0].len == 8u && ivs[1].off == 0u && ivs[1].len == 8u,
                     "RAM wrap halves should be [top,8) + [0,8)");
            t.Equals(ps2_e3::splitIntervals(0x70003FF8u, 16u, ivs, 4u), static_cast<size_t>(2u),
                     "SPR wrap should split in two");
            t.IsTrue(ivs[0].space == ps2_e3::kSprSpace && ivs[0].off == 0x3FF8u && ivs[0].len == 8u &&
                         ivs[1].space == ps2_e3::kRamSpace && ivs[1].off == 0u && ivs[1].len == 8u,
                     "generic mapping past 0x70004000 falls to RAM 0 (getMemPtr semantics)");
            t.Equals(ps2_e3::splitSpace(ps2_e3::kSprSpace, 0x3FF8u, 16u, ivs, 4u), static_cast<size_t>(2u),
                     "engine SPR wrap should split in two");
            t.IsTrue(ivs[0].space == ps2_e3::kSprSpace && ivs[0].off == 0x3FF8u && ivs[0].len == 8u &&
                         ivs[1].space == ps2_e3::kSprSpace && ivs[1].off == 0u && ivs[1].len == 8u,
                     "engine SPR wrap halves should be [top,8) + [SPR 0,8)");
            t.Equals(ps2_e3::splitIntervals(0x1000u, 0u, ivs, 4u), static_cast<size_t>(0u),
                     "zero length should split to nothing");

            const ps2_e3::Win wins[2] = {{0x501420u, ps2_e3::kRamSpace, 0x501420u},
                                         {0x70001C10u, ps2_e3::kSprSpace, 0x1C10u}};
            size_t idx[8];
            t.Equals(ps2_e3::overlapWins(ps2_e3::kRamSpace, 0x501424u, 4u, wins, 2u, idx, 8u),
                     static_cast<size_t>(1u), "inner RAM store should hit");
            t.Equals(idx[0], static_cast<size_t>(0u), "inner hit should be window 0");
            t.Equals(ps2_e3::overlapWins(ps2_e3::kRamSpace, 0x501428u, 8u, wins, 2u, idx, 8u),
                     static_cast<size_t>(0u), "abutting store should miss (boundary-exact)");
            t.Equals(ps2_e3::overlapWins(ps2_e3::kSprSpace, 0x1C14u, 4u, wins, 2u, idx, 8u),
                     static_cast<size_t>(1u), "inner SPR store should hit");
            t.Equals(ps2_e3::overlapWins(ps2_e3::kRamSpace, 0x1C10u, 8u, wins, 2u, idx, 8u),
                     static_cast<size_t>(0u), "same offset in the wrong space should miss");
        });

        tc.Run("E3 tap captures before-slices on overlap only (E3b)", [](TestCase &t)
        {
            std::vector<uint8_t> ram(0x2000u, 0xA5u);
            std::vector<uint8_t> spr(0x4000u, 0x5Au);
            uint8_t *savedSpr = ps2GetScratchpadHostPtr();
            ps2SetScratchpadHostPtr(spr.data());

            const std::vector<ps2_e3::Win> wins = {{0x1000u, ps2_e3::kRamSpace, 0x1000u},
                                                   {0x70000100u, ps2_e3::kSprSpace, 0x100u}};
            ps2_e3::Tap hit = ps2_e3::tapBeginImpl(ram.data(), 0x1000u, 4u, wins);
            t.IsTrue(hit.active, "overlapping RAM tap should be active");
            t.Equals(hit.wins.size(), static_cast<size_t>(1u), "one window should overlap");
            t.IsTrue(hit.wins[0].beforeOk && hit.wins[0].before[0] == 0xA5u,
                     "before-slice should carry RAM bytes");

            ps2_e3::Tap miss = ps2_e3::tapBeginImpl(ram.data(), 0x2000u, 4u, wins);
            t.IsTrue(!miss.active, "disjoint tap should stay inactive");

            ps2_e3::Tap sprHit = ps2_e3::tapBeginImpl(ram.data(), 0x70000100u, 4u, wins);
            t.IsTrue(sprHit.active, "overlapping SPR tap should be active");
            t.IsTrue(sprHit.wins[0].beforeOk && sprHit.wins[0].before[0] == 0x5Au,
                     "before-slice should carry SPR bytes");

            uint64_t lo = 0u;
            uint64_t hi = 0u;
            t.IsTrue(ps2_e3::readOld(ram.data(), 0x1000u, 4u, lo, hi), "old-value gather should succeed");
            t.Equals(lo, 0xA5A5A5A5ull, "old lo should assemble little-endian");
            t.Equals(hi, 0ull, "old hi should be zero for width 4");

            ps2SetScratchpadHostPtr(savedSpr);
        });
    });
}
