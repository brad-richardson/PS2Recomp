#include "MiniTest.h"
#include "ps2_fh1_jcam2_scratch.h"

#if PS2X_JCAM2_LAZY_SUPPORTED
#include <pthread.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace {
using namespace ps2_fh1_jcam2;

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
    const bool flags = sig == SIGSEGV && sigismember(&mask, SIGUSR1) == 1 &&
                       sigismember(&mask, SIGSEGV) == 0 && g_resetSegv.load();
    _exit(flags ? 21 : 22);
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
void genuineFault()
{
    prior(&sentinel, SA_SIGINFO | SA_NODEFER | SA_RESETHAND);
    std::vector<uint8_t> live(PS2_RAM_SIZE);
    if (!lazyBegin(live.data())) _exit(1);
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
void ignored()
{
    struct sigaction sa{};
    sa.sa_handler = SIG_IGN;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, nullptr);
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
    std::vector<uint8_t> live(PS2_RAM_SIZE);
    if (!lazyBegin(live.data())) _exit(1);
    void *p = mmap(nullptr, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    volatile uint8_t value = *static_cast<volatile uint8_t *>(p);
    (void)value;
    _exit(2);
}
} // namespace
#endif

void register_ps2_fh1_jcam2_scratch_tests()
{
#if PS2X_JCAM2_LAZY_SUPPORTED
    MiniTest::Case("Ps2Fh1Jcam2Scratch", [](TestCase &tc) {
        tc.Run("first touch and alternate stack", [](TestCase &t) { t.Equals(exited(child(&firstTouch)), 0, "first touch/errno/stack"); });
        tc.Run("saved handler mask NODEFER RESETHAND", [](TestCase &t) { t.Equals(exited(child(&genuineFault)), 21, "saved action flags"); });
        tc.Run("other thread chains", [](TestCase &t) { t.Equals(exited(child(&foreignThread)), 31, "foreign thread"); });
        tc.Run("execute fault chains", [](TestCase &t) { t.Equals(exited(child(&executeFault)), 31, "execute fault"); });
        tc.Run("SIGBUS failure rolls SIGSEGV back", [](TestCase &t) { t.Equals(exited(child(&failedInstall)), 0, "transactional install"); });
        tc.Run("prior IGN survives", [](TestCase &t) { t.Equals(exited(child(&ignored)), 0, "SIG_IGN"); });
        tc.Run("unhandled fault terminates by signal", [](TestCase &t) {
            const int status = child(&defaultCrash);
            t.IsTrue(WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV, "SIG_DFL");
        });
    });
#endif
}
