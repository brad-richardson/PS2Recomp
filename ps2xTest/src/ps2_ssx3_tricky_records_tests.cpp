// SPDX-License-Identifier: GPL-3.0-or-later OR MIT
#include "MiniTest.h"
#include "ps2_ssx3_tricky_records.h"
#include <vector>

void register_ps2_ssx3_tricky_records_tests()
{
    using namespace ps2_ssx3_tricky_records;
    MiniTest::Case("SSX3 user records quarantine", [](TestCase &tc) {
        tc.Run("all users and donors restore after mapper cleanup and repeated saves", [](TestCase &t) {
            for (unsigned user = 1; user <= 3; ++user)
            {
                std::vector<uint8_t> ram(kRecords + kRecordsSize + 32);
                for (size_t i = 0; i < ram.size(); ++i) ram[i] = uint8_t(i * user + user * 37);
                const auto before = ram;
                State s;
                tick(s, ram.data(), ram.size(), true, true, false);
                t.IsTrue(s.have, "Tricky entry captures this user");
                // Mutate every record/settings byte, not only the ev14 sample.
                for (unsigned save = 0; save < 2; ++save)
                {
                    std::memset(ram.data() + kSettings, 0, kSettingsSize);
                    std::memset(ram.data() + kRecords, 0xff, kRecordsSize);
                    for (size_t i = 0; i < 6; ++i)
                    {
                        ram[kProfiles + i * kProfileStride + 16] ^= 2;
                        ram[kProfiles + i * kProfileStride + 19] ^= 255;
                    }
                    t.IsTrue(restore(s, ram.data(), ram.size()), "serializer restores without a mapper predicate");
                    t.IsTrue(ram == before, "every user's own byte restored; unrelated RAM untouched");
                }
            }
        });
        tc.Run("menu cleanup preserves later settings and subsequent stock results", [](TestCase &t) {
            std::vector<uint8_t> ram(kRecords + kRecordsSize, 0x29);
            const auto before = ram;
            State s;
            tick(s, ram.data(), ram.size(), true, true, false);
            ram[kRecords + 701] = 0;
            tick(s, ram.data(), ram.size(), false, false, false);
            t.IsTrue(ram == before, "exit cleans dirty RAM despite mapper/mode cleared");
            ram[kSettings + 12] = 0x88; // user's later menu setting
            t.IsTrue(!restore(s, ram.data(), ram.size()), "clean menus no longer overwrite new edits");
            tick(s, ram.data(), ram.size(), true, false, false);
            ram[kRecords + 701] = 0x77; // legitimate stock result
            t.IsTrue(!restore(s, ram.data(), ram.size()), "stock race has no quarantine");
            t.Equals(ram[kSettings + 12], uint8_t(0x88), "later setting retained");
            t.Equals(ram[kRecords + 701], uint8_t(0x77), "stock result retained");
            tick(s, ram.data(), ram.size(), false, false, false);
            tick(s, ram.data(), ram.size(), true, true, false);
            ram[kRecords + 701] = 0;
            restore(s, ram.data(), ram.size());
            t.Equals(ram[kRecords + 701], uint8_t(0x77), "next Tricky race snapshots latest stock result");
        });
        tc.Run("stock exact and invalid buffers never write RAM", [](TestCase &t) {
            std::vector<uint8_t> ram(kRecords + kRecordsSize, 0x45);
            const auto before = ram;
            State stock, exact, shortRam;
            tick(stock, ram.data(), ram.size(), true, false, false);
            tick(exact, ram.data(), ram.size(), true, true, true);
            tick(shortRam, ram.data(), kRecords, true, true, false);
            tick(shortRam, nullptr, ram.size(), true, true, false);
            t.IsTrue(!stock.have && !exact.have && !shortRam.have, "no snapshots on bypass paths");
            t.IsTrue(ram == before, "no guest bytes changed");
        });
    });
}
