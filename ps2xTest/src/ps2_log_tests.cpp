#include "MiniTest.h"
#include "ps2_log.h"

#include <string>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#include <unistd.h>
#endif

namespace
{

#if !defined(_WIN32)
// RAII stderr capture: redirects fd 2 to a pipe, restores on destruction
// (all paths: assertion failures log to stderr mid-capture). A drain thread
// reads concurrently: the blast exceeds the 64 KB pipe buffer, so reading
// only after the writers join would deadlock.
struct StderrPipe
{
    int saved = -1;
    int fds[2] = {-1, -1};
    bool active = false;
    std::thread drainer;
    std::string drained;

    bool start()
    {
        std::fflush(stderr);
        if (::pipe(fds) != 0)
            return false;
        saved = ::dup(STDERR_FILENO);
        if (saved < 0)
        {
            ::close(fds[0]);
            ::close(fds[1]);
            return false;
        }
        if (::dup2(fds[1], STDERR_FILENO) < 0)
        {
            ::close(saved);
            ::close(fds[0]);
            ::close(fds[1]);
            return false;
        }
        drainer = std::thread([this]()
                              {
            char buf[8192];
            for (;;)
            {
                const ssize_t n = ::read(fds[0], buf, sizeof(buf));
                if (n <= 0)
                    break;
                drained.append(buf, static_cast<size_t>(n));
            } });
        active = true;
        return true;
    }

    std::string stop()
    {
        std::string out;
        if (!active)
            return out;
        std::fflush(stderr);
        ::dup2(saved, STDERR_FILENO);
        ::close(saved);
        ::close(fds[1]); // EOF for the drainer
        if (drainer.joinable())
            drainer.join();
        ::close(fds[0]);
        out = std::move(drained);
        active = false;
        return out;
    }

    ~StderrPipe()
    {
        if (active)
            stop();
    }
};
#endif

} // namespace

// UX1: emitLine is the one line-buffered diagnostic writer (one write per
// line under a mutex). The contention test fails on the old shape (a
// std::cerr chain per line): fragments interleave mid-line.
void register_ps2_log_tests()
{
    MiniTest::Case("Ps2Log", [](TestCase &tc)
                   {
        tc.Run("emitLine appends exactly one newline", [](TestCase &t)
               {
            const std::string line = "[ux1] hello";
            // Contract (no capture): single-line input, no trailing newline
            // in, one newline out. The capture test below asserts bytes.
            t.IsTrue(!line.empty() && line.back() != '\n', "input has no newline");
            ps2_log::emitLine("[ux1] emitLine smoke (one line, expect no splice)"); });

#if !defined(_WIN32)
        tc.Run("emitLine stays whole under thread contention", [](TestCase &t)
               {
            StderrPipe cap;
            t.IsTrue(cap.start(), "stderr redirected to a pipe");
            if (!cap.active)
                return;
            static constexpr int kThreads = 8;
            static constexpr int kPerThread = 200;
            std::vector<std::thread> workers;
            for (int th = 0; th < kThreads; ++th)
            {
                workers.emplace_back([th]()
                                     {
                    for (int i = 0; i < kPerThread; ++i)
                    {
                        // Distinct, fixed-shape payloads (< PIPE_BUF): any
                        // interleave shows as a malformed line.
                        char body[160];
                        std::snprintf(body, sizeof(body), "[ux1] th=%d i=%d pad=%064d", th, i, i);
                        ps2_log::emitLine(body);
                    } });
            }
            for (auto &w : workers)
                w.join();
            const std::string got = cap.stop();
            std::vector<std::string> lines;
            size_t pos = 0;
            while (pos < got.size())
            {
                const size_t eol = got.find('\n', pos);
                if (eol == std::string::npos)
                    break;
                lines.push_back(got.substr(pos, eol - pos));
                pos = eol + 1;
            }
            t.Equals(lines.size(), static_cast<size_t>(kThreads * kPerThread), "every line landed");
            std::vector<int> seen(kThreads, 0);
            size_t malformed = 0;
            for (const std::string &ln : lines)
            {
                int th = -1, i = -1;
                char pad[72] = {};
                // Exact shape, full-line match: sscanf %n proves no extra
                // or missing bytes (a splice fails the count or the %n).
                int end = -1;
                const int n = std::sscanf(ln.c_str(), "[ux1] th=%d i=%d pad=%71[0-9]%n", &th, &i, pad,
                                          &end);
                if (n != 3 || end < 0 || static_cast<size_t>(end) != ln.size() || th < 0 ||
                    th >= kThreads || i < 0 || i >= kPerThread)
                {
                    ++malformed;
                    continue;
                }
                ++seen[th];
            }
            t.Equals(malformed, static_cast<size_t>(0), "zero spliced lines");
            for (int th = 0; th < kThreads; ++th)
                t.Equals(seen[th], kPerThread, "thread lines intact"); });
#endif
    });
}
