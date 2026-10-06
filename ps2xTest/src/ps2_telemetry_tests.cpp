#include "MiniTest.h"
#include "ps2_telemetry.h"

#include <cstdint>
#include <string>
#include <vector>

namespace
{
size_t countPrefix(const std::string &s, const char *prefix)
{
    size_t n = 0, pos = 0;
    const std::string p = std::string(prefix);
    if (s.compare(0, p.size(), p) == 0)
        ++n;
    while ((pos = s.find("\n" + p, pos)) != std::string::npos)
    {
        ++n;
        ++pos;
    }
    return n;
}
} // namespace

// TEL1: VU1 census + GE1 CSV summary (pure parts of ps2_telemetry.h).
void register_ps2_telemetry_tests()
{
    MiniTest::Case("Ps2Telemetry", [](TestCase &tc)
                   {
        tc.Run("hashBytes: content-sensitive, length-sensitive, stable", [](TestCase &t)
               {
            std::vector<uint8_t> a(16384, 0), b(16384, 0);
            t.Equals(ps2x::telemetry::hashBytes(a.data(), a.size()), ps2x::telemetry::hashBytes(b.data(), b.size()),
                     "same bytes same hash");
            b[9000] = 1;
            t.IsTrue(ps2x::telemetry::hashBytes(a.data(), a.size()) != ps2x::telemetry::hashBytes(b.data(), b.size()),
                     "one byte differs");
            t.IsTrue(ps2x::telemetry::hashBytes(a.data(), 33) != ps2x::telemetry::hashBytes(a.data(), 34),
                     "length matters (tail)"); });

        tc.Run("Vu1Census: images, entries, uploads, first-run proxy", [](TestCase &t)
               {
            ps2x::telemetry::Vu1Census c;
            std::vector<uint8_t> codeA(16384, 0xAA), codeB(16384, 0xBB);
            c.startWindow(0);
            // gen 1 = image A; entry pc 0x10 is new; its first run takes 900 us.
            uint32_t e = c.begin(1, codeA.data(), codeA.size(), 0x10, false, 100, 1);
            c.end(e, 900);
            e = c.begin(1, codeA.data(), codeA.size(), 0x10, false, 101, 2);
            c.end(e, 10);
            e = c.begin(1, codeA.data(), codeA.size(), 0x10, false, 102, 3);
            c.end(e, 30);
            // resume run: counted, no entry.
            e = c.begin(1, codeA.data(), codeA.size(), 0x40, true, 102, 3);
            t.Equals(e, ps2x::telemetry::Vu1Census::kNone, "resume has no entry");
            c.end(e, 5);
            // gen 2 = image B (new), gen 3 = image A again (known bytes: no new image).
            e = c.begin(2, codeB.data(), codeB.size(), 0x10, false, 103, 4);
            c.end(e, 700);
            e = c.begin(3, codeA.data(), codeA.size(), 0x10, false, 104, 5);
            c.end(e, 12);
            t.Equals(c.images(), size_t(2), "2 images");
            t.Equals(c.entries(), size_t(2), "2 entries (A@0x10, B@0x10)");
            t.IsTrue(!c.windowDue(9999), "window not due before 10 s");
            t.IsTrue(c.windowDue(10000), "window due at 10 s");
            std::string out;
            c.takeWindow(10000, 600, out);
            t.Equals(countPrefix(out, "N\t"), size_t(2), "2 N rows");
            t.IsTrue(out.find("\t0x0010\t900\t3\t17.3\t30\n") != std::string::npos, "A: first 900, 3 later runs mean 17.3 max 30");
            t.IsTrue(out.find("\t0x0010\t700\t0\t0.0\t0\n") != std::string::npos, "B: first 700, no later runs");
            t.IsTrue(out.find("W\t10000\t600\t6\t1\t1657\t900\t3\t2\t2\t1600\t2\t2\n") != std::string::npos,
                     "W row: runs 6 resumes 1 run_us 1657 max 900 uploads 3 new_img 2 new_ent 2 new_us 1600");
            // Next window: known entry only -> no N rows, counters reset.
            out.clear();
            e = c.begin(3, codeA.data(), codeA.size(), 0x10, false, 700, 10001);
            c.end(e, 11);
            c.takeWindow(20001, 1200, out);
            t.Equals(countPrefix(out, "N\t"), size_t(0), "no N rows for known entries");
            t.IsTrue(out.find("W\t20001\t1200\t1\t0\t11\t11\t0\t0\t0\t0\t2\t2\n") != std::string::npos, "W reset"); });

        tc.Run("GsCsvSummary: header map, C rows, W aggregate, passthrough", [](TestCase &t)
               {
            ps2x::telemetry::GsCsvSummary s;
            s.startWindow(0);
            std::string out;
            s.feed("vsync,new_tfx,tfx_us,new_spv,spv_us,flush_us,tfx_slow,tfx_max_us,up_kb,uploads,tex_new,"
                   "tex_new_us,wall_us", 0, out);
            s.feed("# prewarm selectors=10 created=9 us=1234", 0, out);
            s.feed("1,0,0,0,0,0,0,0,10,2,1,5,16000", 1, out);
            s.feed("2,3,4500,1,800,0,1,2000,20,4,0,0,25000", 2, out);
            s.feed("3,0,0,0,0,0,0,0,5,1,0,0,60000", 3, out);
            t.IsTrue(out.find("P\t# prewarm selectors=10 created=9 us=1234\n") != std::string::npos, "P passthrough");
            t.Equals(countPrefix(out, "C\t"), size_t(1), "one compile row");
            t.IsTrue(out.find("C\t2\t2\t3\t4500\t1\t800\t0\t1\t2000\t25000\n") != std::string::npos, "C row fields");
            t.IsTrue(!s.windowDue(9999), "not due");
            out.clear();
            s.takeWindow(10000, out);
            t.Equals(out, std::string("W\t10000\t1\t3\t3\t3\t4500\t1\t800\t0\t1\t2000\t35\t7\t1\t5\t60000\t2\t1\t0\n"),
                     "W aggregate");
            out.clear();
            s.takeWindow(20000, out);
            t.Equals(out, std::string(), "empty window writes nothing"); });

        tc.Run("GsCsvSummary: reordered/extra columns, event cap", [](TestCase &t)
               {
            ps2x::telemetry::GsCsvSummary s;
            std::string out;
            s.feed("wall_us,extra,vsync,new_tfx", 0, out);
            for (uint32_t i = 0; i < ps2x::telemetry::kGsMaxEventsPerWindow + 5; ++i)
                s.feed(std::to_string(9000) + ",77," + std::to_string(i + 1) + ",1", 1, out);
            t.Equals(countPrefix(out, "C\t"), size_t(ps2x::telemetry::kGsMaxEventsPerWindow), "C rows capped");
            out.clear();
            s.takeWindow(10000, out);
            t.IsTrue(out.find("\t9000\t0\t0\t5\n") != std::string::npos, "wall max 9000, 5 dropped");
            t.IsTrue(out.rfind("W\t10000\t1\t261\t261\t", 0) == 0, "vsync range from remapped column"); });

        tc.Run("TEL4: replacement snapshot is cumulative and header mapped", [](TestCase &t)
               {
            ps2x::telemetry::GsCsvSummary s;
            std::string out;
            s.feed("vsync,repl_used,repl_indexed,repl_precache_ms1,repl_loaded,repl_cache_bytes,repl_failures,repl_gpu_bytes,hash_cache_bytes", 0, out);
            s.feed("1,3,700,0,400,10000,2,20000,30000", 1, out);
            s.feed("2,5,700,901,700,8000,4,25000,32000", 2, out);
            s.takeWindow(10000, out);
            t.IsTrue(out.find("\t700\t901\t700\t5\t8000\t4\t25000\t32000\n") != std::string::npos,
                     "latest gauges and cumulative totals, not per-frame sums");
            out.clear();
            s.feed("3,6,700,901,700,9000,4,26000,34000", 10001, out);
            s.takeWindow(20000, out);
            t.IsTrue(out.find("\t700\t901\t700\t6\t9000\t4\t26000\t34000\n") != std::string::npos,
                     "snapshot survives next window without summing"); });

        tc.Run("sessionPath joins dir, prefix and stamp", [](TestCase &t)
               {
            const std::string p = ps2x::telemetry::sessionPath("/x/telemetry", "vu1", "tsv");
            t.IsTrue(p.rfind("/x/telemetry/vu1-", 0) == 0, "prefix");
            t.IsTrue(p.size() == std::string("/x/telemetry/vu1-YYYYMMDD-HHMMSS.tsv").size(), "stamp length");
            t.Equals(ps2x::telemetry::sessionPath("/x/", "gs", "tsv").substr(0, 7), std::string("/x/gs-2"), "no double slash"); }); });
}
