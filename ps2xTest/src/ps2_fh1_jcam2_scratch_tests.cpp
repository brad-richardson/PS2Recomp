#include "MiniTest.h"
#include "ps2_fh1_jcam2_scratch.h"

#if PS2X_JCAM2_LAZY_SUPPORTED
#include <pthread.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace {
using namespace ps2_fh1_jcam2;
#if defined(__APPLE__)
constexpr int kProtectionSignal = SIGBUS;
constexpr int kProtectionCode = 1; // observed by the child probe
#else
constexpr int kProtectionSignal = SIGSEGV;
constexpr int kProtectionCode = SEGV_ACCERR;
#endif
int probeWriteFd = -1;
void probeHandler(int sig, siginfo_t *info, void *)
{
    const int result[2] = {sig, info ? info->si_code : 0};
    write(probeWriteFd, result, sizeof(result));
    _exit(0);
}
bool probeProtectionFault(int &sig, int &code)
{
    int fds[2]{};
    if (pipe(fds) != 0) return false;
    const pid_t pid = fork();
    if (pid == 0)
    {
        close(fds[0]);
        probeWriteFd = fds[1];
        struct sigaction sa{};
        sa.sa_sigaction = &probeHandler;
        sa.sa_flags = SA_SIGINFO;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGSEGV, &sa, nullptr);
        sigaction(SIGBUS, &sa, nullptr);
        void *p = mmap(nullptr, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (p == MAP_FAILED) _exit(2);
        volatile uint8_t value = *static_cast<volatile uint8_t *>(p);
        (void)value;
        _exit(3);
    }
    close(fds[1]);
    int result[2]{};
    const ssize_t n = read(fds[0], result, sizeof(result));
    close(fds[0]);
    int status = 0;
    const bool okay = pid > 0 && waitpid(pid, &status, 0) == pid &&
        WIFEXITED(status) && WEXITSTATUS(status) == 0 && n == sizeof(result);
    sig = result[0];
    code = result[1];
    return okay;
}

int child(void (*fn)())
{
    const pid_t pid = fork();
    if (pid == 0) { fn(); _exit(99); }
    if (pid < 0) return -1;
    int status = 0;
    if (waitpid(pid, &status, 0) != pid) return -1;
    return status;
}
int exited(int status) { return WIFEXITED(status) ? WEXITSTATUS(status) : -1; }
void sentinel(int sig, siginfo_t *, void *)
{
    sigset_t mask{};
    sigprocmask(SIG_SETMASK, nullptr, &mask);
    struct sigaction now{};
    sigaction(sig, nullptr, &now);
    const struct sigaction &old = sig == SIGBUS ? g_oldBus : g_oldSegv;
    const int flags = (sig == kProtectionSignal ? 1 : 0) |
                      (sigismember(&mask, SIGUSR1) == 1 ? 2 : 0) |
                      (sigismember(&mask, kProtectionSignal) == 0 ? 4 : 0) |
                      (now.sa_handler == SIG_DFL ? 8 : 0) |
                      ((sig == SIGBUS ? g_resetBus : g_resetSegv).load() ? 16 : 0) |
                      ((old.sa_flags & SA_NODEFER) ? 32 : 0) |
                      ((old.sa_flags & SA_RESETHAND) ? 64 : 0);
    _exit(flags);
}
void plainSentinel(int, siginfo_t *, void *) { _exit(31); }
void prior(void (*handler)(int, siginfo_t *, void *), int flags = SA_SIGINFO)
{
    struct sigaction sa{};
    sa.sa_sigaction = handler;
    sa.sa_flags = flags;
    sigemptyset(&sa.sa_mask);
    sigaddset(&sa.sa_mask, SIGUSR1);
    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGBUS, &sa, nullptr);
}
void firstTouch()
{
    std::vector<uint8_t> live(PS2_RAM_SIZE, 0x41);
    uint8_t *image = lazyBegin(live.data());
    if (!image) _exit(1);
    stack_t stack{};
    if (sigaltstack(nullptr, &stack) != 0 || (stack.ss_flags & SS_DISABLE)) _exit(2);
    errno = EDOM;
    volatile uint8_t value = image[65536u];
    if (value != 0x41 || errno != EDOM) _exit(3);
    lazyEnd();
    lazyShutdown();
    _exit(0);
}
void smallPriorAltStack()
{
    void *memory = mmap(nullptr, 32768u, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (memory == MAP_FAILED) _exit(1);
    stack_t prior{};
    prior.ss_sp = memory;
    prior.ss_size = 32768u;
    if (sigaltstack(&prior, nullptr) != 0) _exit(2);
    std::vector<uint8_t> live(PS2_RAM_SIZE, 0x41);
    uint8_t *image = lazyBegin(live.data());
    if (!image) _exit(3);
    stack_t active{};
    if (sigaltstack(nullptr, &active) != 0 || active.ss_sp == memory || active.ss_size < 65536u) _exit(4);
    volatile uint8_t value = image[65536u];
    if (value != 0x41) _exit(5);
    lazyEnd();
    lazyShutdown();
    stack_t restored{};
    if (sigaltstack(nullptr, &restored) != 0 || restored.ss_sp != memory || restored.ss_size != 32768u) _exit(6);
    _exit(0);
}
void genuineFault()
{
    prior(&sentinel, SA_SIGINFO | SA_NODEFER | SA_RESETHAND);
    std::vector<uint8_t> live(PS2_RAM_SIZE);
    if (!lazyBegin(live.data())) _exit(1);
    // Darwin drops SA_RESETHAND from the action returned by sigaction for
    // SIGBUS (observed bits=39). Supply a saved one-shot action explicitly
    // to test the chain's reset semantics on every supported POSIX host.
    (kProtectionSignal == SIGBUS ? g_oldBus : g_oldSegv).sa_flags |= SA_RESETHAND;
    void *p = mmap(nullptr, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    volatile uint8_t value = *static_cast<volatile uint8_t *>(p);
    (void)value;
    _exit(2);
}
void *otherFault(void *p)
{
    volatile uint8_t value = *static_cast<volatile uint8_t *>(p);
    (void)value;
    return nullptr;
}
void foreignThread()
{
    prior(&plainSentinel);
    std::vector<uint8_t> live(PS2_RAM_SIZE);
    uint8_t *image = lazyBegin(live.data());
    if (!image) _exit(1);
    pthread_t thread{};
    if (pthread_create(&thread, nullptr, &otherFault, image + 4096u) != 0) _exit(2);
    pthread_join(thread, nullptr);
    _exit(3);
}
void executeFault()
{
    prior(&plainSentinel);
    std::vector<uint8_t> live(PS2_RAM_SIZE);
    uint8_t *image = lazyBegin(live.data());
    if (!image) _exit(1);
    reinterpret_cast<void (*)()>(image + 4096u)();
    _exit(2);
}
int rejectBus(int sig, const struct sigaction *sa, struct sigaction *old)
{
    if (sig == SIGBUS) { errno = EINVAL; return -1; }
    return sigaction(sig, sa, old);
}
void failedInstall()
{
    prior(&plainSentinel);
    if (lazyInit(&rejectBus)) _exit(1);
    struct sigaction now{};
    if (sigaction(SIGSEGV, nullptr, &now) != 0 || now.sa_sigaction != &plainSentinel) _exit(2);
    lazyShutdown();
    _exit(0);
}
bool failedInstallLogs()
{
    int fds[2]{};
    if (pipe(fds) != 0) return false;
    const pid_t pid = fork();
    if (pid == 0)
    {
        close(fds[0]);
        if (dup2(fds[1], STDERR_FILENO) < 0) _exit(3);
        close(fds[1]);
        failedInstall();
    }
    close(fds[1]);
    char message[512]{};
    const ssize_t n = read(fds[0], message, sizeof(message) - 1u);
    close(fds[0]);
    int status = 0;
    if (pid < 0 || waitpid(pid, &status, 0) != pid ||
        !WIFEXITED(status) || WEXITSTATUS(status) != 0 || n <= 0) return false;
    return std::strstr(message, "step=sigaction-SIGBUS errno=22") != nullptr;
}
void ignored()
{
    struct sigaction sa{};
    sa.sa_handler = SIG_IGN;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGBUS, &sa, nullptr);
    std::vector<uint8_t> live(PS2_RAM_SIZE);
    if (!lazyBegin(live.data())) _exit(1);
    raise(SIGSEGV);
    lazyShutdown();
    struct sigaction now{};
    sigaction(SIGSEGV, nullptr, &now);
    _exit(now.sa_handler == SIG_IGN ? 0 : 2);
}
void defaultCrash()
{
    struct sigaction sa{};
    sa.sa_handler = SIG_DFL;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGBUS, &sa, nullptr);
    std::vector<uint8_t> live(PS2_RAM_SIZE);
    if (!lazyBegin(live.data())) _exit(1);
    void *p = mmap(nullptr, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    volatile uint8_t value = *static_cast<volatile uint8_t *>(p);
    (void)value;
    _exit(2);
}
} // namespace
#endif

int ps2_fh1_jcam2_protection_probe()
{
#if PS2X_JCAM2_LAZY_SUPPORTED
    int sig = 0, code = 0;
    const bool okay = probeProtectionFault(sig, code);
    std::printf("jcam2 protection probe: signal=%d si_code=%d status=%s\n",
                sig, code, okay ? "ok" : "failed");
    return okay ? 0 : 1;
#else
    return 2;
#endif
}

void register_ps2_fh1_jcam2_scratch_tests()
{
#if PS2X_JCAM2_LAZY_SUPPORTED
    MiniTest::Case("Ps2Fh1Jcam2Scratch", [](TestCase &tc) {
        tc.Run("protection fault probe", [](TestCase &t) {
            int sig = 0, code = 0;
            t.IsTrue(probeProtectionFault(sig, code), "child reports signal/code");
            t.Equals(sig, kProtectionSignal, "protection signal");
            t.Equals(code, kProtectionCode, "protection si_code");
        });
        tc.Run("first touch and alternate stack", [](TestCase &t) { t.Equals(exited(child(&firstTouch)), 0, "first touch/errno/stack"); });
        tc.Run("small prior alternate stack is restored", [](TestCase &t) {
            t.Equals(exited(child(&smallPriorAltStack)), 0, "larger stack and prior restore");
        });
        tc.Run("saved one-shot handler mask NODEFER RESETHAND", [](TestCase &t) {
            const int observed = exited(child(&genuineFault));
            std::fprintf(stderr, "[jcam2-chain-probe] bits=%d expected=127\n", observed);
            t.Equals(observed, 127, "saved action flags");
        });
        tc.Run("other thread chains", [](TestCase &t) { t.Equals(exited(child(&foreignThread)), 31, "foreign thread"); });
        tc.Run("execute fault chains", [](TestCase &t) { t.Equals(exited(child(&executeFault)), 31, "execute fault"); });
        tc.Run("SIGBUS failure rolls SIGSEGV back", [](TestCase &t) { t.Equals(exited(child(&failedInstall)), 0, "transactional install"); });
        tc.Run("SIGBUS failure logs step and errno", [](TestCase &t) { t.IsTrue(failedInstallLogs(), "failure diagnostic"); });
        tc.Run("prior IGN survives", [](TestCase &t) { t.Equals(exited(child(&ignored)), 0, "SIG_IGN"); });
        tc.Run("unhandled fault terminates by signal", [](TestCase &t) {
            const int status = child(&defaultCrash);
            t.IsTrue(WIFSIGNALED(status) && WTERMSIG(status) == kProtectionSignal, "SIG_DFL");
        });
    });
#endif
}
