#!/usr/bin/env python3
"""Report test results against the two gates.

GATE 1 (zero-gap): a run passes only when every stream shows zero sequence gaps.
A gap subsumes every other failure (ring overflow, imissed, CRC) -- it is the
discontinuity they all produce.

GATE 2 (rate match): steady-state RX must equal steady-state TX. A run that
silently delivers less than it sent is invalid even if few gaps are counted.
Rate is the median of the periodic 'PPS | n |' samples, with startup zeros, the
first ramp sample and the last partial dropped. Never rx_packets/duration -- the
processor starts before and outlives the generator, so that average lies.

Classification (per user rule):
  CLEAN       gaps == 0 and RX == TX (within tol)         -> a real limit-free point
  LINK-NOISE  1..100 gaps AND <=100 CRC AND no kilo loss  -> not a limit; rerun,
              and if load is low, climb to a higher rate
  LIMIT       >100 gaps OR >100 CRC OR >=1000 imissed      -> hard ceiling, stop

Handles both result layouts:
  old:  .../<scenario>/summary.json      + generator.log / processor.log
  new:  .../<ts>/<name>.summary.json     + <name>_gen / <name>_proc

Usage: report_gaps.py [name-substring ...]
"""

import glob
import json
import os
import re
import statistics
import sys

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))

GAP_OK = 100      # >100 gaps => limit
CRC_OK = 100      # >100 crc  => limit
KILO = 1000       # >=1000 imissed => real drops => limit
TOL = 0.01        # RX/TX rate-match tolerance
WIRE_OVERHEAD = 24  # preamble + SFD + FCS + IFG, per frame


def steady_pps(log):
    """Median steady-state PPS from the periodic 'PPS | n |' table."""
    if not os.path.isfile(log):
        return 0
    vals = []
    for line in open(log, errors="ignore"):
        m = re.match(r"\s*PPS\s*\|\s*(\d+)\s*\|", line)
        if m:
            vals.append(int(m.group(1)))
    nz = [v for v in vals if v > 0]
    if not nz:
        return 0
    core = nz[1:-1] if len(nz) > 3 else nz   # drop first ramp + last partial
    return round(statistics.median(core))


def steady_mbps(log):
    """Median steady-state L2 load from the periodic 'Load(Mbps)' table.

    The receiver prints "Load(Mbps) | RX | TX |" and the generator prints a
    single TX column, so column 1 is the interesting direction in both. Same
    median-of-samples treatment as steady_pps: the lifetime average would be
    diluted by the seconds before and after the generator runs.

    This is the L2 frame only. The NIC byte counters behind it exclude the
    preamble, the SFD, the FCS and the interframe gap, so it understates link
    occupancy - see steady_wire_mbps().
    """
    if not os.path.isfile(log):
        return 0.0
    vals = []
    for line in open(log, errors="ignore"):
        m = re.match(r"\s*Load\(Mbps\)\s*\|\s*([\d.]+)\s*\|", line)
        if m:
            vals.append(float(m.group(1)))
    nz = [v for v in vals if v > 0]
    if not nz:
        return 0.0
    core = nz[1:-1] if len(nz) > 3 else nz   # drop first ramp + last partial
    return round(statistics.median(core), 1)


def steady_wire_mbps(log):
    """Median steady-state wire load: what the link actually carried.

    Prefers the receiver's own 'Wire(Mbps)' row. Logs captured before that row
    existed are corrected here instead, from the two counters they do carry:
    every frame costs WIRE_OVERHEAD bytes that no NIC register reports.
    """
    if not os.path.isfile(log):
        return 0.0
    vals = []
    for line in open(log, errors="ignore"):
        m = re.match(r"\s*Wire\(Mbps\)\s*\|\s*([\d.]+)\s*\|", line)
        if m:
            vals.append(float(m.group(1)))
    nz = [v for v in vals if v > 0]
    if nz:
        core = nz[1:-1] if len(nz) > 3 else nz
        return round(statistics.median(core), 1)
    return round(steady_mbps(log) + steady_pps(log) * WIRE_OVERHEAD * 8 / 1e6, 1)


def bad_streams(proc_log):
    """Count per-stream rows carrying a nonzero gap (cross-check on the totals).

    The error columns are located from the table header: GOOSE and SV put them at
    different positions, and the stream-configuration table (ending in CRev) has
    none at all. Reading column 5/6 unconditionally counted CRev=1 as a gap and
    flagged every clean GOOSE run.
    """
    if not os.path.isfile(proc_log):
        return 0
    bad, cols = 0, None
    for line in open(proc_log, errors="ignore"):
        c = [x.strip() for x in line.split("|")]
        if "ErrSeqCnt" in c or "ErrSpduCnt" in c:
            cols = [i for i, name in enumerate(c) if name in ("ErrSeqCnt", "ErrSpduCnt")]
            continue
        if c and c[0] in ("MAC", "DMAC"):
            cols = None
            continue
        if not cols or not re.match(r"^[0-9A-Fa-f:]{17}$", c[0]):
            continue
        try:
            if max(int(c[i]) for i in cols):
                bad += 1
        except (ValueError, IndexError):
            pass
    return bad


def discover():
    """Yield (name, summary_path, gen_log, proc_log) for both layouts."""
    for f in glob.glob(os.path.join(REPO, "deploy/*/results/*/*/summary.json")):
        d = os.path.dirname(f)
        yield os.path.basename(d), f, os.path.join(d, "generator.log"), \
            os.path.join(d, "processor.log")
    for f in glob.glob(os.path.join(REPO, "deploy/*/results/*/*.summary.json")):
        d = os.path.dirname(f)
        name = os.path.basename(f)[:-len(".summary.json")]
        yield name, f, os.path.join(d, f"{name}_gen"), os.path.join(d, f"{name}_proc")


def collect(patterns):
    out = []
    for name, sf, gen_log, proc_log in discover():
        if patterns and not any(p in name for p in patterns):
            continue
        try:
            s = json.load(open(sf))
        except (OSError, ValueError):
            continue
        st = s.get("streams", {}) or {}
        p = s.get("processor", {}) or {}
        # Magnitude comes from the aggregate totals (authoritative). The
        # per-stream table is printed every interval, so counting its rows would
        # multiply the true count -- use it only as a boolean fallback when the
        # totals are absent/zero but a stream still shows a discontinuity.
        # One lost frame breaks the inner sequence AND the outer R-SPDU
        # sequence; summing the layers double-counted every R-message loss.
        gaps = (max(st.get("goose_err_seq_total", 0), st.get("goose_err_spdu_total", 0))
                + max(st.get("sv_err_smp_total", 0), st.get("sv_err_spdu_total", 0)))
        if gaps == 0 and bad_streams(proc_log) > 0:
            gaps = 1
        tx, rx = steady_pps(gen_log), steady_pps(proc_log)
        im, crc = p.get("imissed", 0), p.get("ierrors", 0)
        loads = p.get("lcore_load_pct", {}) or {"x": 0}
        match = tx > 0 and abs(rx - tx) / tx <= TOL
        if gaps > GAP_OK or crc > CRC_OK or im >= KILO:
            cls = "LIMIT"
        elif gaps == 0 and match:
            cls = "CLEAN"
        else:
            cls = "LINK-NOISE"
        out.append(dict(name=name, tx=tx, rx=rx, gaps=gaps, crc=crc, imissed=im,
                        match=match, load=max(loads.values()), cls=cls))
    return sorted(out, key=lambda r: r["name"])


def main():
    rows = collect([a for a in sys.argv[1:] if not a.startswith("-")])
    if not rows:
        print("no matching results")
        return
    print(f"{'test':26}{'TXss':>9}{'RXss':>9}{'RX=TX':>6}{'gaps':>12}{'crc':>5}"
          f"{'imissed':>11}{'load%':>7}  {'class':11}")
    n = {"CLEAN": 0, "LINK-NOISE": 0, "LIMIT": 0}
    for r in rows:
        n[r["cls"]] += 1
        print(f"{r['name']:26}{r['tx']:>9}{r['rx']:>9}{'Y' if r['match'] else 'N':>6}"
              f"{r['gaps']:>12,}{r['crc']:>5}{r['imissed']:>11,}{r['load']:>7.1f}  {r['cls']:11}")
    print(f"\nCLEAN {n['CLEAN']}  LINK-NOISE {n['LINK-NOISE']} (rerun/climb)  "
          f"LIMIT {n['LIMIT']} (ceiling)  of {len(rows)}")
    print("Gate: PASS = zero gaps AND steady RX == steady TX. "
          "LINK-NOISE is not a limit; >100 gaps/CRC or kilo imissed is.")


if __name__ == "__main__":
    main()
