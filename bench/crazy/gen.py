#!/usr/bin/env python3
"""Generate the event stream both all four implementations read.

Deterministic (xorshift64*), so every language sees byte-identical input.
Columns: ts, uid, kind, region, cents, ms
"""
import sys

MASK = (1 << 64) - 1


class Rng:
    def __init__(self, seed):
        self.s = seed & MASK

    def next(self):
        x = self.s
        x ^= (x >> 12)
        x &= MASK
        x ^= (x << 25) & MASK
        x ^= (x >> 27)
        self.s = x & MASK
        return (self.s * 0x2545F4914F6CDD1D) & MASK

    def below(self, n):
        return self.next() % n


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "data/events.tsv"
    n = int(sys.argv[2]) if len(sys.argv) > 2 else 1_500_000
    rng = Rng(0x9E3779B97F4A7C15)
    kinds, regions = 8, 6
    ts = 1712000000
    with open(out, "w", buffering=1 << 20) as f:
        for i in range(n):
            ts += 0 if i % 3 else 1
            uid = rng.below(50000)
            kind = rng.below(kinds)
            region = rng.below(regions)
            # skew: most rows are small, a few percent are big
            cents = rng.below(2000)
            if rng.below(100) == 0:
                cents += rng.below(18000)
            ms = 5 + rng.below(1200)
            if rng.below(200) == 0:
                ms += rng.below(1800)
            f.write(f"{ts}\t{uid}\t{kind}\t{region}\t{cents}\t{ms}\n")


if __name__ == "__main__":
    main()
