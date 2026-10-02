#!/usr/bin/env python3
"""Unit tests for ladder_diff.py (T9 waiter-phase rule). Stdlib only.

Run from the fork root:  python3 tools/test_ladder_diff.py
Run from tools/:         python3 test_ladder_diff.py

The waiter fixtures below COPY the governing thread/sema values from
ssx3's committed local/research/T5/park-snapshot-{base,rule}.json
(t1/t5 sampled phases, sema 29/32 waiter counts, agreeing sema 26
anchor) -- copied, never modified. The fork cannot reference ssx3
paths, so the full-snapshot re-verdicts (D, T1 D1/D2) are tabled in
the T9 report instead of asserted here.
"""

import contextlib
import copy
import io
import json
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ladder_diff

VERDICTS = {
    "EXACT", "KEY_DELTA", "TOL_OK", "COUNT_DELTA", "SAMPLED",
    "SATURATED", "STALE", "BLIND", "TRUNC",
}


def run_diff(snap_a, snap_b, names="base,rule", extra_args=()):
    """Write two snapshots to a temp dir, run ladder_diff.main, parse rows."""
    with tempfile.TemporaryDirectory(prefix="t9-test-") as tmp:
        pa = os.path.join(tmp, "a.json")
        pb = os.path.join(tmp, "b.json")
        with open(pa, "w") as f:
            json.dump(snap_a, f)
        with open(pb, "w") as f:
            json.dump(snap_b, f)
        buf = io.StringIO()
        argv = [pa, pb, "--names", names] + list(extra_args)
        with contextlib.redirect_stdout(buf):
            rc = ladder_diff.main(argv)
    rows = {}
    for line in buf.getvalue().splitlines()[1:]:
        if line.startswith("ladder:"):
            tail = line
            break
        toks = line.split()
        idx = next(i for i, t in enumerate(toks) if t in VERDICTS)
        rows[toks[0]] = (toks[idx], " ".join(toks[idx + 1:]))
    else:
        tail = ""
    return rc, rows, tail


def thread(tid, status, reason, wait_id, pc="0x423de8"):
    return {"id": tid, "status": status, "wait_reason": reason,
            "wait_id": wait_id, "pc": pc}


def sema(sid, count, max_, init, waiters):
    return {"id": sid, "count": count, "max": max_, "init": init,
            "waiters": waiters}


def snap(threads, semaphores, wait_hist=None, signal_hist=None):
    return {
        "schema": "ps2x-park-snapshot/1",
        "source": "emitter",
        "threads": threads,
        "semaphores": semaphores,
        "sema_creates": [],
        "sema_wait_hist": wait_hist or {},
        "sema_signal_hist": signal_hist or {},
        "hot_pc": [],
        "drops": [],
        "sif_rpc": [],
        "gs": {},
        "sched_counts": {},
    }


def t5_pair():
    """Copies of T5's governing rows: t1 WAIT29<->live, t5 live<->WAIT32."""
    snap_a = snap(
        [thread(1, 2, 2, 29), thread(2, 2, 2, 26), thread(5, 1, 0, 0)],
        [sema(26, 0, 32, 0, 1), sema(29, 0, 0, 0, 1), sema(32, 0, 16, 0, 0)],
    )
    snap_b = snap(
        [thread(1, 0, 0, 0, "0x423dc8"), thread(2, 2, 2, 26),
         thread(5, 2, 2, 32)],
        [sema(26, 0, 32, 0, 1), sema(29, 0, 0, 0, 0), sema(32, 0, 16, 0, 1)],
    )
    return snap_a, snap_b


class WaiterPhaseRuleTests(unittest.TestCase):
    def test_waiter_rows_sampled_with_correlation_when_sets_differ(self):
        snap_a, snap_b = t5_pair()
        rc, rows, tail = run_diff(snap_a, snap_b)
        self.assertEqual(rows["sema.29.table"][0], "SAMPLED")
        self.assertEqual(rows["sema.32.table"][0], "SAMPLED")
        # Correlation printed: which thread <-> which sema, both sides.
        self.assertIn("t1", rows["sema.29.table"][1])
        self.assertIn("sema29", rows["sema.29.table"][1])
        self.assertIn("t5", rows["sema.32.table"][1])
        self.assertIn("sema32", rows["sema.32.table"][1])
        # Agreeing waiter row stays exact; sampled phase exits 0.
        self.assertEqual(rows["sema.26.table"][0], "EXACT")
        self.assertEqual(rc, 0)
        self.assertIn("-> OK", tail)

    def test_waiter_rows_exact_when_sets_agree(self):
        snap_a, _ = t5_pair()
        rc, rows, tail = run_diff(snap_a, copy.deepcopy(snap_a))
        for rung, (verdict, _) in rows.items():
            self.assertEqual(verdict, "EXACT", rung)
        self.assertEqual(rc, 0)

    def test_waiter_count_mismatch_with_agreed_set_is_delta(self):
        snap_a, _ = t5_pair()
        snap_b = copy.deepcopy(snap_a)
        # Same sampled wait (t1 WAIT29 both sides) but count contradicts it.
        for s in snap_b["semaphores"]:
            if s["id"] == 29:
                s["waiters"] = 0
        rc, rows, _ = run_diff(snap_a, snap_b)
        self.assertEqual(rows["sema.29.table"][0], "KEY_DELTA")
        self.assertEqual(rc, 1)

    def test_equal_count_with_differing_set_is_sampled(self):
        snap_a = snap(
            [thread(1, 2, 2, 29), thread(2, 1, 0, 0)],
            [sema(29, 0, 0, 0, 1)],
        )
        snap_b = snap(
            [thread(1, 1, 0, 0), thread(2, 2, 2, 29)],
            [sema(29, 0, 0, 0, 1)],
        )
        rc, rows, _ = run_diff(snap_a, snap_b)
        # Count equality is coincidence; the sampled waiter differs.
        self.assertEqual(rows["sema.29.table"][0], "SAMPLED")
        self.assertIn("t1", rows["sema.29.table"][1])
        self.assertIn("t2", rows["sema.29.table"][1])
        self.assertEqual(rc, 0)

    def test_non_waiter_sema_fields_stay_exact(self):
        snap_a, _ = t5_pair()
        snap_b = copy.deepcopy(snap_a)
        for s in snap_b["semaphores"]:
            if s["id"] == 32:
                s["count"] = 1  # deterministic field, not a wall-clock sample
        rc, rows, _ = run_diff(snap_a, snap_b)
        self.assertEqual(rows["sema.32.table"][0], "KEY_DELTA")
        self.assertEqual(rc, 1)

    def test_histories_stay_tol_gated(self):
        snap_a, _ = t5_pair()
        snap_b = copy.deepcopy(snap_a)
        snap_a["sema_wait_hist"] = {"29": {"0x423de8": 5281}}
        snap_b["sema_wait_hist"] = {"29": {"0x423de8": 5290}}
        rc, rows, _ = run_diff(snap_a, snap_b)
        self.assertEqual(rows["sema_wait_hist.29.0x423de8"][0], "TOL_OK")
        self.assertEqual(rc, 0)
        snap_b["sema_wait_hist"] = {"29": {"0x423de8": 9000}}
        rc, rows, _ = run_diff(snap_a, snap_b)
        self.assertEqual(rows["sema_wait_hist.29.0x423de8"][0], "COUNT_DELTA")
        self.assertEqual(rc, 1)


if __name__ == "__main__":
    unittest.main()
