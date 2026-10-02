#include "MiniTest.h"
#include "runtime/gs/gs_replay_core.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace
{
    // Thin desktop wrapper over the shared core (N8D7M12 Part 1): the record
    // decoding/dispatch loop lives in ps2_runtime now so a future Android app
    // branch can call ps2x_gs_replay_run() directly. This TU keeps the
    // MiniTest registration, the tmpfile framing unit check, and the
    // PS2X_GS_REPLAY_CAPTURE-unset skip.
    void replay(TestCase &t)
    {
        const Ps2xGsReplayResult result = ps2x_gs_replay_run();
        if (result.skipped)
        {
            t.IsTrue(true, "PS2X_GS_REPLAY_CAPTURE unset; GB4 replay skipped");
            return;
        }
        t.IsTrue(result.openOk, "cannot open PS2X_GS_REPLAY_CAPTURE");
        if (!result.openOk)
            return;
        t.IsTrue(result.headerOk, "invalid GB4 capture header");
        if (!result.headerOk)
            return;
        t.IsTrue(result.backendOk, "parallel replay requested without compiled backend");
        if (!result.backendOk)
            return;
        t.IsTrue(result.rtzOk, "PS2X_GS_REPLAY_RTZ must be path1 or all");
        if (!result.rtzOk)
            return;
        t.IsTrue(result.pathFileOk, "invalid PS2X_GS_REPLAY_PATH_FILE");
        if (!result.pathFileOk)
            return;
        // TL1 Part 1b: word-watch removed; result.wordsOk is always true.

        if (std::getenv("PS2X_GS_REPLAY_PACKET_TRACE"))
            t.IsTrue(result.packetTraceOk, "GB4 packet trace written");

        t.IsTrue(result.parseOk, "GB4 capture records parse cleanly");
        t.IsTrue(result.hasStream, "capture has GIF packets and VBlank markers");
        t.IsTrue(result.hasSamples, "capture has sampled hashes");

        if (const char *outPath = std::getenv("PS2X_GS_REPLAY_OUT"))
        {
            (void)outPath;
            t.IsTrue(result.outOk, "replay hashes written");
        }
        if (const char *expectedPath = std::getenv("PS2X_GS_REPLAY_EXPECT"))
        {
            (void)expectedPath;
            t.IsTrue(result.expectOk, result.expectMessage.empty()
                                          ? "GB4 replay hashes match expected stream"
                                          : result.expectMessage.c_str());
        }
    }
}

void register_ps2_gs_replay_tests()
{
    MiniTest::Case("PS2GSReplay", [](TestCase &tc)
    {
        tc.Run("GB4 rejects a partial capture record at EOF", [](TestCase &t)
        {
            FILE *f = std::tmpfile();
            if (!f)
            {
                t.IsTrue(false, "tmpfile opened");
                return;
            }
            uint32_t length = 9u;
            std::fwrite(&length, sizeof(length), 1, f);
            const uint8_t body[3] = {4u, 0u, 0u};
            std::fwrite(body, 1, sizeof(body), f);
            std::rewind(f);
            std::vector<uint8_t> record;
            t.IsTrue(ps2x_gs_replay_read_event(f, length, record) ==
                         Ps2xGsReplayReadResult::Invalid,
                     "truncated body is invalid even when fread sets EOF");
            std::rewind(f);
            t.IsTrue(ps2x_gs_replay_read_event(f, length, record) ==
                         Ps2xGsReplayReadResult::Invalid,
                     "partial body stays invalid on a second read");
            std::fclose(f);
        });
        tc.Run("GB4 replays the captured GS command stream and hashes VBlank checkpoints",
               [](TestCase &t) { replay(t); });
    });
}
