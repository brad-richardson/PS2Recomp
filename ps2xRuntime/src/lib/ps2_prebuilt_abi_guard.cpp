// Load-time source-contract guard for prebuilt support archives only.
#include <cstdio>
#include <cstdlib>
#include <cstring>
extern const char g_ps2xRequestedPrebuiltStamp[];
namespace {
struct PrebuiltGuard {
    PrebuiltGuard() {
        if (std::strcmp(g_ps2xRequestedPrebuiltStamp, PS2X_PREBUILT_STAMP) != 0) {
            std::fputs("FATAL: prebuilt runtime contract mismatch\n", stderr);
            std::abort();
        }
    }
};
const PrebuiltGuard guard;
}
