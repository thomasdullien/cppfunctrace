"""Load a .perfetto-trace via trace_processor and print summary stats.

Usage: python3 tests/validate.py path/to/trace.perfetto-trace
"""

import sys

from perfetto.trace_processor import TraceProcessor


def main():
    if len(sys.argv) < 2:
        print("Usage: validate.py <trace.perfetto-trace>", file=sys.stderr)
        sys.exit(1)

    tp = TraceProcessor(trace=sys.argv[1])
    try:
        rows = list(tp.query("SELECT count(*) AS cnt FROM slice"))
        print(f"slices: {rows[0].cnt}")

        rows = list(tp.query("SELECT count(*) AS cnt FROM thread"))
        print(f"threads: {rows[0].cnt}")

        rows = list(tp.query("SELECT count(*) AS cnt FROM process"))
        print(f"processes: {rows[0].cnt}")

        rows = list(tp.query(
            "SELECT count(*) AS cnt FROM slice WHERE dur < 0"))
        print(f"slices with negative dur: {rows[0].cnt}")

        rows = list(tp.query(
            "SELECT COUNT(DISTINCT name) AS cnt FROM slice"))
        print(f"unique slice names: {rows[0].cnt}")

        rows = list(tp.query(
            "SELECT name, count(*) AS cnt FROM slice "
            "GROUP BY name ORDER BY cnt DESC LIMIT 10"))
        print("top 10 slice names by count:")
        for r in rows:
            print(f"  {r.cnt:>8}  {r.name}")

        rows = list(tp.query(
            "SELECT name FROM thread ORDER BY tid LIMIT 10"))
        print("thread names (up to 10):")
        for r in rows:
            print(f"  {r.name}")
    finally:
        tp.close()


if __name__ == "__main__":
    main()
