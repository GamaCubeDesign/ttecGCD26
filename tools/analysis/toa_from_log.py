#!/usr/bin/env python3
"""
Measured time on air against the model, from a ttcd log.

ttcd logs every transmission twice: `tx`, with the frame's modelled time on
air (`toa_us`, from common/gama_lora.c), and `tx_done`, with the time that
actually elapsed until the radio reported the end (`elapsed_ms`). Pairing
them gives the bench evidence for PLANO phase 2.2: measured time on air
within 5% of the model, for every profile and frame length used.

    python3 tools/analysis/toa_from_log.py ttcd.jsonl [--tolerance 0.05]

Exits non-zero if any group of (profile, frame length) is off by more than
the tolerance, so it can gate a bench run.

What `elapsed_ms` includes beyond the air time: the SPI writes that start the
transmission and the interrupt latency until the shell services DIO0 —
normally a few milliseconds. Short frames at FAST are the most sensitive to
it, so read their percentage with that in mind.
"""

import argparse
import json
import statistics
import sys
from collections import defaultdict


def pairs(path):
    last_tx = None
    with open(path, encoding="utf-8") as f:
        for n, line in enumerate(f, 1):
            try:
                e = json.loads(line)
            except json.JSONDecodeError:
                print(f"{path}:{n}: not JSON, skipped", file=sys.stderr)
                continue
            if e.get("event") == "tx":
                last_tx = e
            elif e.get("event") == "tx_done" and last_tx is not None:
                yield last_tx, e
                last_tx = None
            elif e.get("event") == "tx_watchdog":
                last_tx = None       # the end was never reported: no measurement


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("log")
    ap.add_argument("--tolerance", type=float, default=0.05)
    args = ap.parse_args()

    groups = defaultdict(list)
    for tx, done in pairs(args.log):
        groups[(tx["profile"], tx["len"])].append((tx["toa_us"] / 1000.0, done["elapsed_ms"]))

    if not groups:
        print("no tx/tx_done pairs in the log")
        return 1

    worst = 0.0
    print(f"{'profile':<9}{'bytes':>6}{'n':>6}{'model ms':>10}{'measured ms':>13}"
          f"{'error':>8}   verdict")
    for (profile, length), v in sorted(groups.items()):
        model = v[0][0]
        measured = statistics.median(m for _, m in v)
        err = (measured - model) / model
        worst = max(worst, abs(err))
        verdict = "ok" if abs(err) <= args.tolerance else "OUT OF TOLERANCE"
        print(f"{profile:<9}{length:>6}{len(v):>6}{model:>10.1f}{measured:>13.1f}"
              f"{err:>+8.1%}   {verdict}")
    print(f"\nworst error {worst:.1%} (tolerance {args.tolerance:.0%})")
    return 0 if worst <= args.tolerance else 1


if __name__ == "__main__":
    sys.exit(main())
