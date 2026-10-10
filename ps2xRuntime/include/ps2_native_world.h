// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// NRT1: native static world, phase 1 (L1, ssx3 repo local/research/NRT1/).
// At the VU1 MSCAL dispatch (microVU engine only) a terrain or scenery-instance
// job whose output the host model reproduces bit-exactly (NEON, FPCR RZ+FZ,
// no FMA) is computed on the MTVU thread and its PATH1 packets are submitted
// in place of the VU1 run, as one native record per job (ge1_gs_api.h). The
// EE, DMA and VIF run unchanged; the only guest-visible difference is VU1 data
// memory the skipped job would have written (outputs and scratch nobody reads;
// det keys move on vu1Data only). The record's packets are exactly the GIF
// bytes VU1 would have kicked.
//
// Knobs (read once):
//   PS2X_SSX3_NATIVE_WORLD=terrain (or 1)   route terrain jobs (default off);
//                         =instances         scenery instances;
//                         =terrain,instances both (the play value).
//   PS2X_SSX3_NATIVE_WORLD_CHECK=1          diagnostic: route nothing, run the
//                                           models beside microVU and compare
//                                           the PATH1 bytes (counts on stderr).
// All off: beforeVu1() is one load and a branch per MSCAL.
#include <cstdint>
#include <string>
#include <vector>

class PS2Memory;

namespace ps2_native_world
{
// Any knob set.
bool active();

// MSCAL hook on the MTVU worker, called only when microVU is the engine.
// true: the job was served natively (packets submitted); the caller must not
// run VU1. false: run VU1 as usual, then call afterVu1().
bool beforeVu1(PS2Memory &memory, uint32_t startPC, uint32_t top, uint32_t itop);
void afterVu1();
// MSCNT (resume) seen: counted if it follows a natively served job.
void onResume();
// microVU's path1() sink: check-mode capture and the packet log.
void onVu1Packet(const uint8_t *bytes, uint32_t size);

// Save states defer while a terrain run that skipped VU1 jobs is open (a
// follow-up job needs host state that VU1 memory doesn't hold).
std::string saveReady();
// After a state load: drop host group state.
void resetForLoad();

// One job's packets (concatenated, with their sizes) as a native record
// (ge1_gs_api.h layout), as each natively served job emits them.
void buildRecord(const uint8_t *packets, const uint32_t *sizes, uint32_t count, std::vector<uint8_t> &out);
// NRS1: same packets as buildRecord's, dense (GIF-sized `sizes` in).
void buildCompactRecord(const uint8_t *packets, const uint32_t *sizes, uint32_t count,
                        std::vector<uint8_t> &out);

// Test hooks (synthetic inputs only). Runs the terrain model for one job over
// a 16 KiB VU1 data image; group state persists across calls until
// testResetGroup(). Returns true when the job is servable (no clipper
// polygon, supported sizes); kicks get the PATH1 packets in kick order.
bool testModelTerrain(const uint8_t *vu1Data, uint32_t startPC, uint32_t top,
                      std::vector<std::vector<uint8_t>> &kicks);
// The scenery-instance model (image 2826c443) for one job: header (0x2270
// ITOP=0 / 0x2320), continuation (0x22c8 ITOP=0 / 0x2358) or second pass
// (0x2270 / 0x22c8 with ITOP != 0). packet gets the job's one PATH1 packet.
bool testModelScenery(const uint8_t *vu1Data, uint32_t startPC, uint32_t top, uint32_t itop,
                      std::vector<uint8_t> &packet);
void testResetGroup(); // terrain and scenery state
} // namespace ps2_native_world
