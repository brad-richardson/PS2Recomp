#!/usr/bin/env python3
"""ladder_diff.py: rung-table diff of two ps2x-park-snapshot/1 JSON files.

The P9/P11 ladder shape, machine-made: every row carries an EXACT/DELTA
verdict instead of a hand waved "same". Exit 0 iff there are zero
non-throughput deltas (i.e. no KEY_DELTA/COUNT_DELTA verdicts).

Row classes (documented contended rows are NEVER exact):
  EXACT rows:   thread status/wait, semaphore table (count/max/init, plus
                waiter counts only when the sampled thread-wait sets
                agree) + creates, drop census, sif/rpc unhandled-call
                multiset, sendcmd rows (<= cap).
  THROUGHPUT:   sched counts, gs true counts, dma/gif/gsw/vif counters,
                sema-hist counts, and hot-pc counts emitter-vs-emitter.
                Compared with --tolerance (percent, default 10): within ->
                TOL_OK, beyond -> COUNT_DELTA.
  SAMPLED:      thread pc, and thread status while live (Running/Ready
                on either side); parked-thread status/wait stay exact.
                Sema-table waiter counts are wall-clock samples exactly
                like thread status: SAMPLED when the complementary
                thread-wait sets differ (correlation printed), EXACT
                when they agree. Cumulative wait/signal histories stay
                the deterministic signal (throughput-gated).
  SATURATED:    miner-side log caps ([gs:kick]<96, [gs:copy-reg]<64,
                [gs:gif]<48, [sceSifSendCmd]<5, [diag:stub] top-30/block).
  STALE:        tick-derived gs rows (dma/gif/gsw/vif) across mixed
                sources: the miner reads the last [run:tick], the emitter
                reads live counters.
  BLIND/TRUNC:  miner-blind fields (loads, binds, claimed calls, sema
                table, invocation-thread sched, stub-truncated hot-pc
                tails). Informational only.

Usage:
  tools/ladder_diff.py A.json B.json [--tolerance PCT] [--names A,B]
"""

import argparse
import collections
import json
import sys


def pct_diff(a, b):
    if a == b:
        return 0.0
    denom = max(abs(a), abs(b))
    if denom == 0:
        return 0.0
    return abs(a - b) / denom * 100.0


class Ladder:
    def __init__(self, tolerance, name_a, name_b):
        self.tol = tolerance
        self.name_a = name_a
        self.name_b = name_b
        self.rows = []  # (rung, a, b, verdict, note)
        self.counts = collections.Counter()

    def add(self, rung, a, b, verdict, note=""):
        self.rows.append((rung, str(a), str(b), verdict, note))
        self.counts[verdict] += 1

    def exact(self, rung, a, b, note=""):
        self.add(rung, a, b, "EXACT" if a == b else "KEY_DELTA", note)

    def throughput(self, rung, a, b, note=""):
        if a == b:
            self.add(rung, a, b, "EXACT", note)
            return
        d = pct_diff(a, b)
        if d <= self.tol:
            self.add(rung, a, b, "TOL_OK", f"{d:.1f}%<={self.tol}% {note}".strip())
        else:
            self.add(rung, a, b, "COUNT_DELTA", f"{d:.1f}%>{self.tol}% {note}".strip())

    def report(self):
        w_rung = max([len(r[0]) for r in self.rows] + [4])
        w_a = max([len(r[1]) for r in self.rows] + [len(self.name_a)])
        w_b = max([len(r[2]) for r in self.rows] + [len(self.name_b)])
        lines = []
        lines.append(f"{'rung':<{w_rung}}  {self.name_a:<{w_a}}  {self.name_b:<{w_b}}  verdict     note")
        for rung, a, b, verdict, note in self.rows:
            lines.append(f"{rung:<{w_rung}}  {a:<{w_a}}  {b:<{w_b}}  {verdict:<11} {note}")
        deltas = self.counts["KEY_DELTA"] + self.counts["COUNT_DELTA"]
        info = (self.counts["SAMPLED"] + self.counts["SATURATED"] + self.counts["BLIND"] +
                self.counts["TRUNC"] + self.counts["STALE"])
        lines.append(
            f"ladder: {self.counts['EXACT']} exact, {self.counts['TOL_OK']} tol-ok, "
            f"{info} info, "
            f"{deltas} deltas (tolerance {self.tol:g}%) -> {'DELTA' if deltas else 'OK'}"
        )
        return "\n".join(lines), deltas


def thread_rows(snap):
    return {t["id"]: t for t in snap.get("threads", [])}


def sema_rows(snap):
    return {s["id"]: s for s in snap.get("semaphores", [])}


def hist_get(hist, sid, pc):
    return hist.get(str(sid), {}).get(pc, 0)


def sema_wait_set(threads, sid):
    """IDs of threads sampled waiting on sema sid (complementary set)."""
    return sorted(tid for tid, t in threads.items()
                  if t["status"] == 2 and t["wait_reason"] == 2 and t["wait_id"] == sid)


def waiter_note(name_a, wa, name_b, wb, sid):
    """Correlation note: which thread <-> which sema, per side."""
    def fmt(tids):
        if not tids:
            return "none"
        return ",".join(f"t{tid}" for tid in tids) + f"->sema{sid}"
    return f"waiters sampled: {name_a} {fmt(wa)} vs {name_b} {fmt(wb)}"


def main(argv=None):
    ap = argparse.ArgumentParser(description="Diff two park snapshots as a ladder table.")
    ap.add_argument("a_json")
    ap.add_argument("b_json")
    ap.add_argument("--tolerance", type=float, default=10.0)
    ap.add_argument("--names", default=None)
    args = ap.parse_args(argv)
    with open(args.a_json) as f:
        ahead = json.load(f)
    with open(args.b_json) as f:
        bhead = json.load(f)
    for tag, doc in (("A", ahead), ("B", bhead)):
        if doc.get("schema") != "ps2x-park-snapshot/1":
            print(f"{tag}: not a ps2x-park-snapshot/1 file", file=sys.stderr)
            return 2
    names = args.names.split(",", 1) if args.names else [args.a_json, args.b_json]
    lad = Ladder(args.tolerance, names[0], names[1])
    miner_involved = "miner" in (ahead.get("source"), bhead.get("source"))

    # Threads: parked-thread status/wait exact; live threads (Running=0,
    # Ready=1 on either side) are wall-clock samples like pc.
    at, bt = thread_rows(ahead), thread_rows(bhead)
    for tid in sorted(set(at) | set(bt)):
        if tid not in at or tid not in bt:
            have = "A" if tid in at else "B"
            lad.add(f"thread.{tid}.present", have, "missing" if have == "A" else "have", "KEY_DELTA")
            continue
        x, y = at[tid], bt[tid]
        sx, sy = (x["status"], x["wait_reason"], x["wait_id"]), (y["status"], y["wait_reason"], y["wait_id"])
        if sx == sy:
            lad.add(f"thread.{tid}.status", sx, sy, "EXACT")
        elif x["status"] in (0, 1) or y["status"] in (0, 1):
            lad.add(f"thread.{tid}.status", sx, sy, "SAMPLED", "live thread sampled")
        else:
            lad.add(f"thread.{tid}.status", sx, sy, "KEY_DELTA")
        if x["pc"] == y["pc"]:
            lad.add(f"thread.{tid}.pc", x["pc"], y["pc"], "EXACT")
        else:
            lad.add(f"thread.{tid}.pc", x["pc"], y["pc"], "SAMPLED", "wall-clock sample")

    # Semaphore table (emitter-only; miner omits it). Waiter counts are
    # SIGTERM wall-clock samples: exact only when the complementary
    # thread-wait sets agree; the cumulative histories stay the signal.
    asema, bsema = sema_rows(ahead), sema_rows(bhead)
    if not asema and not bsema:
        lad.add("semaphores.table", "-", "-", "BLIND", "no table on either side")
    elif not asema or not bsema:
        lad.add("semaphores.table", f"{len(asema)} rows", f"{len(bsema)} rows", "BLIND", "miner-blind")
    else:
        for sid in sorted(set(asema) | set(bsema)):
            if sid not in asema or sid not in bsema:
                lad.add(f"sema.{sid}.present", sid in asema, sid in bsema, "KEY_DELTA")
                continue
            x, y = asema[sid], bsema[sid]
            xa = (x["count"], x["max"], x["init"], x["waiters"])
            xb = (y["count"], y["max"], y["init"], y["waiters"])
            wa, wb = sema_wait_set(at, sid), sema_wait_set(bt, sid)
            if xa == xb and wa == wb:
                lad.add(f"sema.{sid}.table", xa, xb, "EXACT")
            elif (xa[:3] != xb[:3]):
                lad.add(f"sema.{sid}.table", xa, xb, "KEY_DELTA")
            elif wa != wb:
                lad.add(f"sema.{sid}.table", xa, xb, "SAMPLED",
                        waiter_note(lad.name_a, wa, lad.name_b, wb, sid))
            else:
                lad.add(f"sema.{sid}.table", xa, xb, "KEY_DELTA",
                        "waiters disagree despite agreed wait-set")

    # Sema creates: exact multiset.
    def creates(snap):
        return collections.Counter(
            (c["id"], c["tid"], c["pc"], c["init"], c["max"]) for c in snap.get("sema_creates", []))
    ca, cb = creates(ahead), creates(bhead)
    if ca == cb:
        lad.add("sema.creates", f"{sum(ca.values())} rows", f"{sum(cb.values())} rows", "EXACT")
    else:
        lad.add("sema.creates", f"{sum(ca.values())} rows", f"{sum(cb.values())} rows",
                "KEY_DELTA", f"onlyA={sorted(set(ca) - set(cb))} onlyB={sorted(set(cb) - set(ca))}")

    # Sema hists: key-set exact, counts throughput (pump semas are wall-fed).
    for hname in ("sema_wait_hist", "sema_signal_hist"):
        ha, hb = ahead.get(hname, {}), bhead.get(hname, {})
        keys = set()
        for side in (ha, hb):
            for sid, pcs in side.items():
                for pc in pcs:
                    keys.add((sid, pc))
        for sid, pc in sorted(keys, key=lambda k: (int(k[0]), k[1])):
            na, nb = hist_get(ha, sid, pc), hist_get(hb, sid, pc)
            if (na == 0) != (nb == 0):
                lad.add(f"{hname}.{sid}.{pc}", na, nb, "KEY_DELTA", "key membership")
            else:
                lad.throughput(f"{hname}.{sid}.{pc}", na, nb)

    # Hot pc: miner per-pc sums are truncation-corrupted (top-30/block
    # plus wall-clock block boundaries), so counts compare only
    # emitter-vs-emitter; any miner involvement makes them informational.
    def hotpc(snap):
        return {h["pc"]: h["count"] for h in snap.get("hot_pc", [])}
    xa, xb = hotpc(ahead), hotpc(bhead)
    for pc in sorted(set(xa) | set(xb)):
        na, nb = xa.get(pc, 0), xb.get(pc, 0)
        if miner_involved and na != nb:
            lad.add(f"hotpc.{pc}", na, nb, "TRUNC", "stub top-30 truncation")
        elif na == 0 or nb == 0:
            lad.add(f"hotpc.{pc}", na, nb, "KEY_DELTA", "key membership")
        else:
            lad.throughput(f"hotpc.{pc}", na, nb)

    # Drops: exact (deterministic guest-event rows).
    def drops(snap):
        return {(d["site"], d["reason"]): d["count"] for d in snap.get("drops", [])}
    da, db = drops(ahead), drops(bhead)
    for key in sorted(set(da) | set(db)):
        lad.exact(f"drop.{key[0]}.{key[1]}", da.get(key, 0), db.get(key, 0))

    # SIF/RPC: compare per-op; unhandled-call subset exact, claimed side info
    # when a miner is involved (claimed calls never print).
    def rpc_by_op(snap):
        ops = collections.defaultdict(list)
        for e in snap.get("sif_rpc", []):
            ops[e["op"]].append(e)
        return ops
    ra, rb = rpc_by_op(ahead), rpc_by_op(bhead)
    for op in sorted(set(ra) | set(rb)):
        ea, eb = ra.get(op, []), rb.get(op, [])
        if not ea or not eb:
            if miner_involved:
                lad.add(f"rpc.{op}", f"{len(ea)} rows", f"{len(eb)} rows", "BLIND", "miner-blind op")
            else:
                lad.add(f"rpc.{op}", f"{len(ea)} rows", f"{len(eb)} rows", "KEY_DELTA", "op missing")
            continue
        if op == "call":
            # Miner never sees the calling thread (not in the trace line).
            def call_key(e):
                key = (e["sid"], e["fno"], e["send_size"], e["recv_size"])
                return key if miner_involved else key + (e["tid"],)
            ua = collections.Counter(call_key(e) for e in ea if not e["claimed"])
            ub = collections.Counter(call_key(e) for e in eb if not e["claimed"])
            if ua == ub:
                lad.add("rpc.call.unclaimed", f"{sum(ua.values())} rows",
                        f"{sum(ub.values())} rows", "EXACT")
            else:
                lad.add("rpc.call.unclaimed", f"{sum(ua.values())} rows",
                        f"{sum(ub.values())} rows", "KEY_DELTA",
                        f"onlyA={sorted(set(ua) - set(ub))} onlyB={sorted(set(ub) - set(ua))}")
            ca_n = sum(1 for e in ea if e["claimed"])
            cb_n = sum(1 for e in eb if e["claimed"])
            if miner_involved:
                lad.add("rpc.call.claimed", ca_n, cb_n, "BLIND", "claimed calls never print")
            else:
                lad.exact("rpc.call.claimed", ca_n, cb_n)
        elif op == "sendcmd" and miner_involved:
            # Miner sees at most the first 5 [sceSifSendCmd] lines.
            if len(eb) == 5 or len(ea) == 5:
                lad.add("rpc.sendcmd", f"{len(ea)} rows", f"{len(eb)} rows",
                        "SATURATED", "log caps at 5")
            else:
                # Miner sees cid+psize only (extra is an address, tid unlogged).
                def sendcmd_key(e):
                    key = (e["sid"], e["send_size"], e["claimed"])
                    return key if miner_involved else key + (e["recv_size"], e["tid"])
                ma = collections.Counter(sendcmd_key(e) for e in ea)
                mb = collections.Counter(sendcmd_key(e) for e in eb)
                if ma == mb:
                    lad.add("rpc.sendcmd", f"{len(ea)} rows", f"{len(eb)} rows", "EXACT")
                else:
                    lad.add("rpc.sendcmd", f"{len(ea)} rows", f"{len(eb)} rows", "KEY_DELTA",
                            f"onlyA={sorted(set(ma) - set(mb))} onlyB={sorted(set(mb) - set(ma))}")
        else:
            ma = collections.Counter(
                (e["sid"], e["fno"], e["send_size"], e["recv_size"], e["tid"],
                 e["claimed"], e["path"]) for e in ea)
            mb = collections.Counter(
                (e["sid"], e["fno"], e["send_size"], e["recv_size"], e["tid"],
                 e["claimed"], e["path"]) for e in eb)
            if ma == mb:
                lad.add(f"rpc.{op}", f"{len(ea)} rows", f"{len(eb)} rows", "EXACT")
            else:
                lad.add(f"rpc.{op}", f"{len(ea)} rows", f"{len(eb)} rows", "KEY_DELTA",
                        f"onlyA={sorted(set(ma) - set(mb))} onlyB={sorted(set(mb) - set(ma))}")

    # GS: line-count rows saturate at their log caps; tick-derived rows
    # (dma/gif/gsw/vif) are last-tick-vs-live across mixed sources, so
    # they compare only within one source kind.
    ga, gb = ahead.get("gs", {}), bhead.get("gs", {})
    mixed_sources = ahead.get("source") != bhead.get("source")
    caps = {"kicks": 96, "kicks_drawing": 96, "gif_packets": 48, "copy_regs": 64}
    tick_rows = {"dma_starts", "gif_copies", "gs_writes", "vif_writes"}
    for key in ("kicks", "kicks_drawing", "gif_packets", "copy_regs",
                "dma_starts", "gif_copies", "gs_writes", "vif_writes"):
        na, nb = ga.get(key, 0), gb.get(key, 0)
        cap = caps.get(key)
        if miner_involved and cap is not None and (na == cap or nb == cap) and na != nb:
            lad.add(f"gs.{key}", na, nb, "SATURATED", f"log caps at {cap}")
        elif mixed_sources and key in tick_rows and na != nb:
            lad.add(f"gs.{key}", na, nb, "STALE", "miner reads last tick, emitter reads live")
        else:
            lad.throughput(f"gs.{key}", na, nb)

    # Sched counts: throughput (exact on a same-boot emitter/miner pair).
    # Negative tids are invocation threads, which never print [diag:thread].
    sa, sb = ahead.get("sched_counts", {}), bhead.get("sched_counts", {})
    for tid in sorted(set(sa) | set(sb), key=int):
        if miner_involved and int(tid) < 0:
            lad.add(f"sched.{tid}", sa.get(tid, 0), sb.get(tid, 0),
                    "BLIND", "invocation threads never print")
        else:
            lad.throughput(f"sched.{tid}", sa.get(tid, 0), sb.get(tid, 0))

    text, deltas = lad.report()
    print(text)
    return 1 if deltas else 0


if __name__ == "__main__":
    sys.exit(main())
