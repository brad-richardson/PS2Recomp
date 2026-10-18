#pragma once
// F4 (appsec-fix-1, lightweight): public builds ignore env-file knobs whose
// registry class is `diag` unless PS2X_DEV=1 is set.
//
// The list below mirrors tests/det/knob_registry.tsv rows with class `diag`
// at the appsec-fix-1 tip (94 names). When the registry gains a diag knob,
// add it here too (knob_registry.py flags unregistered source literals, and
// the diag-gate test below flags a registry diag knob missing here).
// Lookup is linear: ~94 short compares once per env-file load.
//
// Platform-neutral on purpose so the host unit test compiles this header.
// The Android loader (ps2_android_runtime.cpp) and the iOS loader
// (ps2_ios_runtime.mm) filter file-derived entries through this gate;
// launcher / harness process env is trusted and unfiltered, so det legs
// (which carry no file layers) are unaffected with or without PS2X_DEV=1.
// Lab envs set PS2X_DEV=1 to keep diagnostic files working.

#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace ps2x::diaggate
{

inline bool devModeOpen()
{
    const char *dev = std::getenv("PS2X_DEV");
    return dev != nullptr && dev[0] == '1' && dev[1] == '\0';
}

inline bool devModeOpenIn(const std::vector<std::pair<std::string, std::string>> &entries)
{
    if (devModeOpen())
    {
        return true;
    }
    for (const auto &entry : entries)
    {
        if (entry.first == "PS2X_DEV" && entry.second == "1")
        {
            return true;
        }
    }
    return false;
}

inline const char *const *diagKnobs(size_t *countOut)
{
    static const char *const kDiag[] = {
        "GE1_ADRENO_DSTREAD",
        "GE1_BACKTHREAD",
        "GE1_REPLAY_FIFO_DIR",
        "GE1_TEX_DUMP",
        "GE1_VERTEX_KICK",
        "PS2X_ASPECT",
        "PS2X_AUTOSAVE_TEST_AT",
        "PS2X_CD_READ_LOG",
        "PS2X_CD_READ_TRACE_FROM",
        "PS2X_CD_READ_TRACE_TO",
        "PS2X_ENABLE_AGRESSIVE_LOGS",
        "PS2X_ENABLE_FFMPEG",
        "PS2X_ENABLE_FFMPEG_DEFAULT",
        "PS2X_ENABLE_IOP_RPC_TRACE",
        "PS2X_ENABLE_RUNTIME_LOGS",
        "PS2X_ENABLE_SBR_TRIPWIRE",
        "PS2X_FFMPEG_IOS_ROOT",
        "PS2X_GS_CAPTURE",
        "PS2X_GS_CAPTURE_STOP_TICK",
        "PS2X_GS_EXTERNAL_LOG",
        "PS2X_GS_REPLAY_BACKEND",
        "PS2X_GS_REPLAY_BISECT_TO",
        "PS2X_GS_REPLAY_CAPTURE",
        "PS2X_GS_REPLAY_DROP_PRIV",
        "PS2X_GS_REPLAY_EXPECT",
        "PS2X_GS_REPLAY_MODE",
        "PS2X_GS_REPLAY_OUT",
        "PS2X_GS_REPLAY_PACKET_TRACE",
        "PS2X_GS_REPLAY_PATH_FILE",
        "PS2X_GS_REPLAY_PKTSEQ",
        "PS2X_GS_REPLAY_PPM_DIR",
        "PS2X_GS_REPLAY_PPM_TICKS",
        "PS2X_GS_REPLAY_RAW_PPM_DIR",
        "PS2X_GS_REPLAY_RTZ",
        "PS2X_GS_REPLAY_STEP",
        "PS2X_GS_TFX_RECORD",
        "PS2X_GS_WORKER_CPUS",
        "PS2X_IOS_AUDIO_SESSION",
        "PS2X_LAB_UNCAPPED",
        "PS2X_NET_CAPTURE",
        "PS2X_NET_QUIET_POLLS",
        "PS2X_NET_TEST_HOLD",
        "PS2X_NETPLAY_CANON_DIR",
        "PS2X_NETPLAY_EPOCH",
        "PS2X_NETPLAY_SEED",
        "PS2X_NETSYNC_GHOST",
        "PS2X_NETSYNC_GHOST_EVERY",
        "PS2X_NETSYNC_GHOST_FILE",
        "PS2X_NETSYNC_GHOST_SLOT",
        "PS2X_NETSYNC_LIVE",
        "PS2X_NETSYNC_PEER",
        "PS2X_NETSYNC_SLOT",
        "PS2X_NETSYNC_TOKEN",
        "PS2X_NP_LINK_A_SCRIPT",
        "PS2X_NP_LINK_B_SCRIPT",
        "PS2X_NP_LINK_DELAY",
        "PS2X_NP_LINK_DIR",
        "PS2X_NP_LINK_ENABLE",
        "PS2X_NP_LINK_FIRST_TICK",
        "PS2X_NP_LINK_RELAY",
        "PS2X_NP_LINK_RIDER_SLOT",
        "PS2X_NP_LINK_ROLE",
        "PS2X_NP_RIDER_DROP_TICK",
        "PS2X_NP_RIDER_ENABLE",
        "PS2X_NP_RIDER_SCRIPT",
        "PS2X_NP_RIDER_SLOT",
        "PS2X_OM1_CERT_VERIFY",
        "PS2X_OM1_FORCE_JUMP_MISS",
        "PS2X_OM1_RESUME",
        "PS2X_PRESENT_VULKAN",
        "PS2X_SAVESTATE_ALLOW_FIX_DELTA",
        "PS2X_SET2_SHOT",
        "PS2X_SET2_TEST_PAD",
        "PS2X_SPU_INTERP",
        "PS2X_SSX3_NATIVE_WORLD_CHECK",
        "PS2X_SSX3_TRICKY_HUD_FORCE",
        "PS2X_SSX3_TRICKY_LETTERS_PRESET",
        "PS2X_STRETCH_TRACE",
        "PS2X_TEST_DEFERRED",
        "PS2X_TK_MENU_1_DIAG",
        "PS2X_TK_MENU_1_SHOT",
        "PS2X_TK_MENU_2_SHOT",
        "PS2X_TK_MUSIC_VOICE_LOG",
        "PS2X_TK12_AP_LOG_EVERY",
        "PS2X_TRICKY_HUD_GPU_DIAG",
        "PS2X_VITA_CREATE_PACKAGE",
        "PS2X_VITA_LIB_",
        "PS2X_VU_FLOAT",
        "PS2X_VU_MUSTTAIL",
        "PS2X_VU_NOINLINE",
        "PS2X_VU0_BLOCKS",
        "PS2X_VU1_MUSTTAIL",
        "PS2X_VU1_NOINLINE",
        "PS2X_WIDESCREEN",
    };
    if (countOut)
    {
        *countOut = sizeof(kDiag) / sizeof(kDiag[0]);
    }
    return kDiag;
}

inline bool knobIsDiagGated(const std::string &key)
{
    size_t n = 0;
    const char *const *knobs = diagKnobs(&n);
    for (size_t i = 0; i < n; ++i)
    {
        if (key == knobs[i])
        {
            return true;
        }
    }
    return false;
}

// Split file-derived entries into kept vs ignored-diag. With the dev gate
// open nothing is ignored. Keys only are reported (values never logged).
inline void splitDiagEntries(const std::vector<std::pair<std::string, std::string>> &entries, bool devOpen,
                             std::vector<std::pair<std::string, std::string>> *kept,
                             std::vector<std::string> *ignored)
{
    for (const auto &entry : entries)
    {
        if (!devOpen && knobIsDiagGated(entry.first))
        {
            if (ignored)
            {
                ignored->push_back(entry.first);
            }
            continue;
        }
        if (kept)
        {
            kept->push_back(entry);
        }
    }
}

inline std::string joinIgnored(const std::vector<std::string> &ignored)
{
    std::string out;
    for (size_t i = 0; i < ignored.size(); ++i)
    {
        if (i != 0)
        {
            out += ",";
        }
        out += ignored[i];
    }
    return out;
}

} // namespace ps2x::diaggate
