#!/usr/bin/env python3
"""
analyze.py — canned Perfetto trace_processor queries for cppfunctrace output.

Reports: total slices, trace span, top functions by count and by
inclusive time, per-module time, per-thread time, slowest single
calls. Designed to be piped into an agent: output is plain text,
dense, no framing noise.

Usage:
    analyze.py <trace.perfetto-trace> [--top N]
    analyze.py <trace.perfetto-trace> --sql "SELECT ..."

Requires the `perfetto` Python package (pip install perfetto).
"""

import argparse
import sys

try:
    from perfetto.trace_processor import TraceProcessor
except ImportError:
    print("analyze.py: missing `perfetto` package. Install with:",
          file=sys.stderr)
    print("  pip install perfetto", file=sys.stderr)
    sys.exit(2)


def fmt_ns(ns):
    if ns < 1_000:      return f"{ns}ns"
    if ns < 1_000_000:  return f"{ns/1_000:.1f}us"
    if ns < 1_000_000_000: return f"{ns/1_000_000:.2f}ms"
    return f"{ns/1_000_000_000:.3f}s"


def section(title):
    print()
    print(f"── {title} " + "─" * max(1, 60 - len(title)))


def module_of(name):
    """Names come out of ftrc2perfetto as "<symbol> (<module-basename>)"."""
    if "(" in name and name.endswith(")"):
        return name[name.rfind("(") + 1:-1]
    return ""


def run(trace_path, top, raw_sql):
    tp = TraceProcessor(trace=trace_path)
    try:
        if raw_sql:
            for r in tp.query(raw_sql):
                print(dict(r.__dict__))
            return

        # Summary
        rows = list(tp.query(
            "SELECT COUNT(*) AS cnt, MIN(ts) AS mn, MAX(ts+dur) AS mx FROM slice"))
        r = rows[0]
        span_ns = (r.mx - r.mn) if r.cnt else 0
        print(f"slices  : {r.cnt}")
        print(f"span    : {fmt_ns(span_ns)}")
        if r.cnt == 0:
            print("(no slices — did the binary produce .ftrc files?)")
            return

        rows = list(tp.query("SELECT COUNT(*) AS cnt FROM thread"))
        print(f"threads : {rows[0].cnt}")
        rows = list(tp.query("SELECT COUNT(*) AS cnt FROM process"))
        print(f"procs   : {rows[0].cnt}")
        rows = list(tp.query("SELECT COUNT(DISTINCT name) AS cnt FROM slice"))
        print(f"uniques : {rows[0].cnt}")

        section("Top functions by call count")
        rows = list(tp.query(
            f"SELECT name, COUNT(*) AS cnt, SUM(dur) AS tot "
            f"FROM slice GROUP BY name ORDER BY cnt DESC LIMIT {top}"))
        for r in rows:
            print(f"  {r.cnt:>8}  {fmt_ns(r.tot):>10}  {r.name}")

        section("Top functions by inclusive time")
        rows = list(tp.query(
            f"SELECT name, COUNT(*) AS cnt, SUM(dur) AS tot "
            f"FROM slice GROUP BY name ORDER BY tot DESC LIMIT {top}"))
        for r in rows:
            print(f"  {fmt_ns(r.tot):>10}  {r.cnt:>8}  {r.name}")

        section("Time per module")
        rows = list(tp.query(
            "SELECT name, SUM(dur) AS tot, COUNT(*) AS cnt FROM slice "
            "GROUP BY name"))
        mods = {}
        for r in rows:
            m = module_of(r.name) or "<unknown>"
            e = mods.setdefault(m, [0, 0])
            e[0] += r.tot
            e[1] += r.cnt
        for m, (tot, cnt) in sorted(mods.items(), key=lambda x: -x[1][0])[:top]:
            print(f"  {fmt_ns(tot):>10}  {cnt:>8}  {m}")

        section("Time per thread")
        rows = list(tp.query(f"""
            SELECT thread.name AS tn, COUNT(s.id) AS cnt, SUM(s.dur) AS tot
            FROM slice s
            JOIN thread_track tt ON s.track_id = tt.id
            JOIN thread ON tt.utid = thread.utid
            GROUP BY thread.name
            ORDER BY tot DESC LIMIT {top}
        """))
        for r in rows:
            print(f"  {fmt_ns(r.tot):>10}  {r.cnt:>8}  {r.tn}")

        section("Slowest single calls")
        rows = list(tp.query(
            f"SELECT name, dur FROM slice ORDER BY dur DESC LIMIT {top}"))
        for r in rows:
            print(f"  {fmt_ns(r.dur):>10}  {r.name}")
    finally:
        tp.close()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("trace")
    ap.add_argument("--top", type=int, default=10)
    ap.add_argument("--sql", default=None,
                    help="execute a custom SQL statement instead of the summary")
    args = ap.parse_args()
    run(args.trace, args.top, args.sql)


if __name__ == "__main__":
    main()
