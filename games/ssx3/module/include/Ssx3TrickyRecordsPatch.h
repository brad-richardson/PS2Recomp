#pragma once

// tricky-records-1 v3 product guard (stays through the gate): wraps the
// McWrite card-write entry so a Tricky-mapped SET0001 save persists clean
// SSX 3 records. Always armed; fires only on card writes and patches only
// a mapped launch's 3276-B fd-3 write (otherwise chains immediately, so
// stock and all other files are det-identical).

#include <cstddef>
#include <cstdint>

class PS2Runtime;

namespace ps2_ssx3_tricky_records_patch
{

void applyTrickyRecordsPatch(PS2Runtime &runtime);

} // namespace ps2_ssx3_tricky_records_patch
