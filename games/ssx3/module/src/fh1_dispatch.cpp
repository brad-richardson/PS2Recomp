// full120 slice 3: dispatch cold side moved from ps2_fh1_full120.h: the
// hook-interest table content (which source/target PCs each FIX group
// lists). Body verbatim; the k* PC constants stay runtime-side until
// their last consumers move (later slices). The runtime reaches this body
// through Module.fh1BuildHookInterest (same-name wrapper in
// ps2_fh1_full120.h); config gathering, the table cache and the lean-front
// filter stay runtime-side.
#include "Ssx3Fh1Dispatch.h"

#include "ps2_fh1_full120.h"

#include <algorithm>
#include <utility>
#include <vector>

namespace ps2_ssx3_fh1
{
using namespace ps2_fh1;

void buildHookInterest(const ps2_fh1::HookConfig *cfg, ps2_fh1::HookInterest *out)
{
    const HookConfig &c = *cfg;
    HookInterest r;
    auto addSrc = [&](uint32_t pc) { r.src.push_back(pc); };
    auto addTgt = [&](uint32_t pc) { r.tgt.push_back(pc); };
    const bool on = c.mode == Mode::Always || (c.mode == Mode::Events && c.guestActive);
    if (c.mode == Mode::Always)
        addSrc(kHookSite); // manager-init patch site
    if (c.mode == Mode::Events)
    {
        // eventsOnBranch sites (entry/exit facts + the app-update flip test).
        addSrc(kRaceTickCallSite);
        addSrc(kRaceTick2CallSite);
        addSrc(kAppUpdateSite);
        addTgt(kSelectorDispatch);
        addTgt(kRaceTickCallee);
        addTgt(kRaceTick2Callee);
        addTgt(kSession);
        // FH27 bonusflip: the trick-state recorder in eventsOnBranch (0x1218ac -> 0x117c28) runs
        // in stock windows too, so it is not on-gated (flips rescale states seen while inactive).
        if ((c.main & kFixBonus) != 0u && (c.main & kFixBonusFlip) != 0u)
            addTgt(kTrickPass);
        if (c.replay)
        {
            // RPL1: recorder/player calls and input reset/destroy, in every phase.
            addSrc(ps2_fh1_rpl::kRecordSite);
            addSrc(ps2_fh1_rpl::kPlaySite);
            addTgt(ps2_fh1_rpl::kInputReset);
            addTgt(ps2_fh1_rpl::kInputFree);
        }
    }
    if (c.mode != Mode::Off && (c.main & kFixLife2) != 0u)
    {
        for (uint32_t pc : kLife2Targets) addTgt(pc);
        for (uint32_t pc : kLife2Sources) addSrc(pc);
    }
    if (c.mode != Mode::Off && (c.main & (kFixTrailRing | kFixRng)) == (kFixTrailRing | kFixRng))
        addTgt(kTrailRingAlloc); // allocation happens before the events flip
    if ((c.main & kFixClock) != 0u && on)
    {
        addTgt(kProducer);
        addTgt(kPeriod10s);
    }
    if ((c.main & kFixRaceClock) != 0u && on)
    {
        addSrc(kRaceTick2CallSite);
        addTgt(kRaceTick2Callee);
        addSrc(kRaceTickCallSite);
        addTgt(kRaceTickCallee);
    }
    if ((c.main & kFixLaunch) != 0u && on)
        addTgt(kLaunchHelper);
    if ((c.main & kFixSession) != 0u && on)
    {
        addSrc(kSessionCallSite);
        addTgt(kSession);
    }
    if ((c.main & kFixQuery) != 0u && on)
    {
        addSrc(kCruiseQuerySite);
        addTgt(kCruiseQuery);
    }
    if ((c.main & kFixShadowPub) != 0u && (c.main & kFixQuery) != 0u && on)
    {
        addSrc(kShadowPubBarSite);
        addTgt(kShadowPubBar);
        addSrc(kShadowPubShapeSite);
        addTgt(kShadowPubShape);
    }
    const bool parity = ((c.main & (kFixRng | kFixTrick | kFixAiGate | kFixJcam | kFixPid | kFixC2Cap | kFixInput2 | kFixInputChain | kFixQuery)) != 0u && on) ||
                        ((c.fix12 & (kFix12Particles | kFix12Flare)) != 0u && on);
    if (parity)
        addSrc(kAppUpdateSite); // parityHook counts app-update dispatches
    if ((c.main & kFixRng) != 0u && on)
    {
        // rngHook: odd-update skips of the trail/lookAt calls, the two stream-1
        // Bernoulli rolls, and the flash-countdown hold (any source each).
        addTgt(kTrailPush);
        addTgt(kLookAt);
        addTgt(kRngDraw0);
        addTgt(kFlashSched);
        addSrc(kLensRollSite);
        addSrc(kFlashRollSite);
    }
    if ((c.main & kFixTrick) != 0u && on)
    {
        addSrc(kComboSite);
        addTgt(kComboAccrue);
    }
    if ((c.main & kFixGround2) != 0u && on)
        addTgt(0x113e80u);
    if ((c.main & kFixStuntChg) != 0u && on)
        addTgt(kStuntChgUpdate); // FLP2: stunt-charge hold pre-hook (any source)
    if ((c.main & kFixNormcap) != 0u && on)
        addTgt(kNormcapGround); // NRM1: pre-hook arms the post-map correction record (any source)
    if ((c.main & kFixJcam) != 0u && on)
        addTgt(kJcamJump); // FH28: countdown pre-hook + retention post-hook (any source)
    if ((c.main & kFixJcam2) != 0u && on)
    {
        addTgt(kJcam2Solver); addTgt(kJcam2Launch); addTgt(kJcam2Query);
    }
    if ((c.main & kFixEnvFilt) != 0u && on)
        addTgt(kEnvFiltUpdate); // FH30: conversion only at kEnvFiltSite
    if ((c.main & kFixPid) != 0u && on)
        addTgt(kPidUpdate); // FH29: second-update hold + rollback (any source)
    if ((c.main & kFixC2Cap) != 0u && on)
        addTgt(kC2Update); // FH28: odd-update skip (any source)
    if ((c.main & kFixAiGate) != 0u && on)
    {
        // aiGateHook: odd-update answers of the race-tick read at 4 sites.
        addSrc(0x10bdccu);
        addSrc(0x10da6cu);
        addSrc(0x10dc30u);
        addSrc(0x10b8acu);
        addTgt(kRaceTickGet);
    }
    if ((c.fix12 & kFix12Particles) != 0u && on)
    {
        addSrc(kParticleSiteAll);
        addSrc(kParticleSiteOne);
        addTgt(kParticlePass);
    }
    if ((c.fix12 & kFix12Flare) != 0u && on)
    {
        addSrc(kFlareProbeSite);
        addTgt(kFlareProbeLoop);
    }
    if ((c.main & kFixBonus) != 0u && on)
    {
        addSrc(kBonusSite);
        addTgt(kBonusNext);
    }
    if ((c.main & kFixLift) != 0u && on)
    {
        addSrc(kLiftProbeSite);
        addTgt(kLiftProbe);
    }
    if ((c.main & kFixFlags) != 0u && on)
        addTgt(kFlagUpdate);
    if ((c.main & kFixRclock) != 0u)
        addTgt(kRenderFrameGet); // also answered after exit (frozen offset)
    if ((c.fix12 & (kFix12Spin | kFix12Texanim | kFix12Loops | kFix12FxTimer)) != 0u && on)
    {
        addTgt(kSpinUpdate);
        addTgt(kTexUv);
        addTgt(kTexFlip);
        addTgt(kLoopMode1);
        addTgt(kLoopMode2);
        addTgt(kLoopMode3);
        addTgt(kFxTimerUpdate);
        addTgt(kLoopCtor); // FH27 loops2: the ctor pre-hook (merge MRG2: HK1 predates FH27)
    }
    for (uint32_t t : c.srcTgts)
        addTgt(t); // PS2X_FH1_SRC watch list (diagnostic, runs whatever the mode)
    if (c.fh26s != 0u)
        addTgt(0x133308u); // stick-angle tracker tap
    if (c.fh9r != 0u)
        r.always = true; // FH9 AI tap replicates per-branch state: run the chain
    addTgt(0x111728u); // IN4 rider-action tap (knob-checked inside; one compare)
    if (c.draw && on)
    {
        addSrc(kRenderGateMode1);
        addSrc(kRenderGateMode0);
        addTgt(kRenderTarget);
    }
    if (on && (c.main & (kFixChase2|kFixCamera)) == (kFixChase2|kFixCamera))
        addSrc(0x1626e4u);
    if (c.tapOn)
    {
        addTgt(kUpdateTarget);
        addTgt(kRenderTarget);
        for (uint32_t t : c.tapCountPcs)
            addTgt(t);
        if (c.histRanges)
            r.always = true; // FH10 hist counts every target inside its ranges
    }
    auto dedup = [](std::vector<uint32_t> &v) {
        std::sort(v.begin(), v.end());
        v.erase(std::unique(v.begin(), v.end()), v.end());
    };
    dedup(r.src);
    dedup(r.tgt);
    *out = std::move(r);
}
} // namespace ps2_ssx3_fh1
