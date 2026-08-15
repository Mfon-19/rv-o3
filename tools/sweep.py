#!/usr/bin/env python3
"""Configuration sweeps over the benchmark suite.

Runs rvsim across one axis of the design space via -O overrides,
scrapes the stats report, and emits markdown tables: IPC for every
benchmark, plus the metric that axis is expected to move (so the
sweep carries its own evidence, not just a headline number).

Usage: tools/sweep.py AXIS [prog.bin ...]     (axes: rob width l1
       mshr pred; default programs: the six bench/ binaries)
"""
import re
import subprocess
import sys

AXES = {
    "rob": {
        "points": [(f"rob {v}", [f"robSize={v}"]) for v in (16, 32, 64, 128)],
        "aux": ("avg ROB occupancy", r"occupancy: rob ([\d.]+)"),
    },
    "width": {
        "points": [(f"width {v}",
                    [f"width={v}", f"wbPorts={v}", f"aluCount={v}"])
                   for v in (1, 2, 4)],
        "aux": ("avg issue width", r"avg issue ([\d.]+)"),
    },
    "l1": {
        "points": [(f"L1 {v}K", [f"l1i.size={v}k", f"l1d.size={v}k"])
                   for v in (16, 32, 64)],
        "aux": ("L1D miss%", r"L1D \d+ accesses, \d+ misses \(([\d.]+)%"),
    },
    "mshr": {
        "points": [(f"mshrs {v}", [f"l1d.mshrs={v}", f"l2.mshrs={v}"])
                   for v in (1, 2, 4, 8)],
        "aux": ("L1D avg outstanding", r"L1D .*avg outstanding ([\d.]+)"),
    },
    "pred": {
        # 2-bit counters: 2^11 = 512 B ... 2^16 = 16 KiB, BTB scaled along
        "points": [(f"pht 2^{b} ({sz})", [f"phtBits={b}", f"btbEntries={e}"])
                   for b, e, sz in ((11, 32, "512B"), (12, 64, "1K"),
                                    (13, 128, "2K"), (14, 256, "4K"),
                                    (15, 512, "8K"), (16, 1024, "16K"))],
        "aux": ("branch MPKI", r"([\d.]+) MPKI\)"),
    },
}

DEFAULT_PROGS = [f"bench/{b}.bin" for b in
                 ("ptrchase", "mlpbench", "matmul", "mm64", "qsortb", "rle",
                  "branchy")]


def run(prog, overrides):
    cmd = ["./rvsim"]
    for kv in overrides:
        cmd += ["-O", kv]
    cmd.append(prog)
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit(f"sweep: {' '.join(cmd)} exited {r.returncode}")
    return r.stderr


def scrape(text, pattern):
    m = re.search(pattern, text)
    return m.group(1) if m else "?"


def table(title, header, rows):
    print(f"\n**{title}**\n")
    print("| config | " + " | ".join(header) + " |")
    print("|---" * (len(header) + 1) + "|")
    for name, cells in rows:
        print(f"| {name} | " + " | ".join(cells) + " |")


def main():
    if len(sys.argv) < 2 or sys.argv[1] not in AXES:
        sys.exit(f"usage: sweep.py [{'|'.join(AXES)}] [prog.bin ...]")
    axis = AXES[sys.argv[1]]
    progs = sys.argv[2:] or DEFAULT_PROGS
    names = [p.split("/")[-1].removesuffix(".bin") for p in progs]

    ipc_rows, aux_rows = [], []
    aux_name, aux_pat = axis["aux"]
    for point, overrides in axis["points"]:
        outs = [run(p, overrides) for p in progs]
        ipc_rows.append((point, [scrape(o, r"IPC = ([\d.]+)") for o in outs]))
        aux_rows.append((point, [scrape(o, aux_pat) for o in outs]))
    table(f"{sys.argv[1]} sweep: IPC", names, ipc_rows)
    table(f"{sys.argv[1]} sweep: {aux_name}", names, aux_rows)


if __name__ == "__main__":
    main()
