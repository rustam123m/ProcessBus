#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
"""Run the scenarios listed in a config file and report each result as it lands.

Model (do not change without asking):
  * the processor is started first, then the generator START_DELAY seconds later;
  * BOTH applications stop themselves when their own --time expires. The runner
    NEVER signals a healthy run;
  * after the expected end the runner verifies that both really finished. If one
    did not, it is stopped gently (SIGTERM, then SIGKILL after a delay) and the
    WHOLE BATCH IS ABORTED -- a device in an unknown state invalidates the rest;
  * Ctrl+C stops both roles on both devices before exiting.

Every remote call is one script with at most one argument. No command chains, no
redirections, no quoting inside the ssh line: per-run parameters travel as a file
copied with scp, and the roles run inside tmux sessions on the device.

Parsing and the verdict reuse report_gaps.py (steady-state PPS, gap/CRC gates)
and result_parser.sh (summary.json), unchanged.

Usage (the shebang runs it through uv; no virtualenv to manage):
    ./deploy/common/run_tests.py --conf deploy/pairs/pairs.conf --all
    ./deploy/common/run_tests.py --conf deploy/pairs/pairs.conf --scenario cm_b2o_rgoose_gcm
    ./deploy/common/run_tests.py --conf deploy/qotom/scenarios.conf --list
"""

import argparse
import datetime
import glob
import json
import os
import re
import statistics
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from report_gaps import (steady_pps, steady_mbps, bad_streams,  # noqa: E402
                         GAP_OK, CRC_OK, KILO, TOL)

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
PARSER = os.path.join(REPO, "deploy/common/result_parser.sh")

USER = "jarvis"
RDIR = "/home/jarvis/pbus"          # deploy directory on both devices
DEV_OUT = "/tmp/pbus_out"           # capture directory on the device
SENTINEL = "/var/run/pbus_rt"       # tmpfs: absent => the board was rebooted
HOSTS = {                           # name -> (address, NIC BDF on the pair cable)
    "blackbox": ("192.168.100.103", "0000:07:00.0"),
    "opi3b":    ("192.168.100.105", "0000:01:00.0"),
}
# One box running both roles needs two ports: its loopback cable joins these,
# and they are what run_generator.sh / run_processor.sh default to.
LOOPBACK_NIC = {"gen": "0000:0b:00.0", "proc": "0000:0c:00.0"}

START_DELAY = 5       # the processor is started this much before the generator
PROC_PAD = 10         # and keeps running this much after the generator stopped
STARTUP_MARGIN = 90   # EAL init + WaitLink (the app allows up to 60 s) + exit
POLL = 10             # seconds between progress samples
# Per-scenario cost beyond its own duration: START_DELAY, EAL init, the link
# wait, the receiver's tail, teardown and the log fetch. Measured at 43 s median
# across the 2026-08-14 pair and loopback batches, when START_DELAY was 10 s;
# carried down by the 5 s the lead lost, so 40 is a prediction rather than a
# measurement until a batch re-establishes it.
PER_RUN_OVERHEAD = 40

# A receiver taking half the offered rate is already at its ceiling: the point is
# LIMIT whatever happens in the remaining minutes, so the test is cut short and
# the batch moves on. Two consecutive samples, so a single ramp sample cannot
# trigger it.
LOSS_RATIO = 0.5
LOSS_SAMPLES = 2


def losing_badly(tx, rx):
    """True when the receiver is delivering less than LOSS_RATIO of the offer."""
    return tx > 0 and rx < tx * LOSS_RATIO


def is_burst(row):
    """Burst rows are exempt from the early abort.

    A burst run puts a whole second on the wire at the second boundary, so once
    it is past the descriptor ceiling the receiver is *expected* to deliver well
    under half the offer - that is the measurement, not a reason to cut it off
    after two samples.
    """
    return "--burst" in row["gen_args"]


def proc_run_time(duration):
    """Receiver --time: it must cover the lead-in, the whole run, and the tail.

    duration 60 -> 5 + 60 + 10 = 75 s, while the generator gets exactly 60 s.
    """
    return START_DELAY + duration + PROC_PAD

SSH_OPTS = ["-o", "StrictHostKeyChecking=no", "-o", "ConnectTimeout=10",
            "-o", "ServerAliveInterval=15", "-o", "ServerAliveCountMax=4"]


# --------------------------------------------------------------------------- #
# remote helpers - one command, plain words, no shell metacharacters
# --------------------------------------------------------------------------- #
def _run(cmd, timeout):
    """Never raise: a timeout is just another failed command, with a message."""
    try:
        return subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return subprocess.CompletedProcess(cmd, 255, "", f"timed out after {timeout}s")


def why(r):
    """The reason a remote command failed, for the message the user sees."""
    return (r.stderr or r.stdout or f"exit status {r.returncode}").strip()


def ssh(host, *args, timeout=120):
    addr = HOSTS[host][0]
    return _run(["ssh", "-n"] + SSH_OPTS + [f"{USER}@{addr}"] + list(args), timeout)


def scp_to(host, local, remote):
    addr = HOSTS[host][0]
    return _run(["scp", "-q"] + SSH_OPTS + [local, f"{USER}@{addr}:{remote}"], 120)


def scp_from(host, remote, local):
    addr = HOSTS[host][0]
    return _run(["scp", "-q"] + SSH_OPTS + [f"{USER}@{addr}:{remote}", local], 120)


def session_alive(host, role):
    return ssh(host, "sudo", "tmux", "has-session", "-t", f"pbus_{role}").returncode == 0


def start_role(host, role, name):
    """tmux new-session -d -s pbus_<role> bash <dir>/device_runner.sh run <role> <name>."""
    return ssh(host, "sudo", "tmux", "new-session", "-d", "-s", f"pbus_{role}",
               "bash", f"{RDIR}/device_runner.sh", "run", role, name)


def stop_role(host, role):
    """Error path only: SIGTERM, then SIGKILL after the runner's own delay."""
    return ssh(host, "sudo", "bash", f"{RDIR}/device_runner.sh", "stop", role)


def preflight(host):
    return ssh(host, "sudo", "bash", f"{RDIR}/device_runner.sh", "preflight")


def prepare(host):
    """Once per host per batch: sync the runner, make sure the board is tuned.

    The applications themselves are deployed by deploy/*/deploy.sh (they need a
    build); only device_runner.sh is refreshed here, because run_tests.py is the
    thing that calls it. setup_platform.sh is guarded by the same sentinel
    deploy.sh uses -- it lives on tmpfs, so a reboot silently undoes hugepages
    and the NIC binding, which is how a whole batch once ran with no receiver.
    """
    r = ssh(host, "true", timeout=20)
    if r.returncode != 0:
        return False, f"unreachable over ssh: {why(r)}"
    r = scp_to(host, os.path.join(REPO, "deploy/common/device_runner.sh"),
               f"{RDIR}/device_runner.sh")
    if r.returncode != 0:
        return False, f"could not copy device_runner.sh: {why(r)}"
    if ssh(host, "test", "-e", SENTINEL).returncode != 0:
        print(f"  {host}: {SENTINEL} missing, running setup_platform.sh")
        r = ssh(host, "sudo", "bash", f"{RDIR}/setup_platform.sh", timeout=300)
        if r.returncode != 0:
            return False, f"setup_platform.sh failed: {why(r)}"
    return True, ""


def nic_for(host, role, loopback):
    """Which port this role binds.

    Across two devices both roles sit on the cable between them, so the host's
    own BDF is the answer. When one box runs both roles the "cable" is a
    loopback between two of its own NICs, and handing the second role the same
    BDF aborts it with "Device or resource busy" - it needs the other port.
    """
    return LOOPBACK_NIC[role] if loopback else HOSTS[host][1]


def write_params(host, name, role, app_args, lcores, run_time, workdir,
                 loopback=False):
    """Per-run parameters travel as a file, never as ssh command arguments."""
    local = os.path.join(workdir, f"{name}_{role}.params")
    with open(local, "w") as fh:
        fh.write(f"APP_ARGS='{app_args}'\n")
        fh.write(f"LCORES='{lcores}'\n")
        fh.write(f"NIC='{nic_for(host, role, loopback)}'\n")
        fh.write(f"TIME={run_time}\n")
    ssh(host, "mkdir", "-p", f"{RDIR}/runs")
    return scp_to(host, local, f"{RDIR}/runs/{name}_{role}.params").returncode == 0


# --------------------------------------------------------------------------- #
# config
# --------------------------------------------------------------------------- #
def estimate(rows):
    """Wall-clock estimate for a batch, plus the local time it should finish.

    Each scenario costs its own duration plus a fixed overhead, so the sum is
    the floor: an early stop on a ceiling makes it shorter, a retry longer.
    """
    total = sum(r["duration"] + PER_RUN_OVERHEAD for r in rows)
    h, m = divmod(round(total / 60), 60)
    span = f"{h}h {m:02d}m" if h else f"{m}m"
    ends = datetime.datetime.now() + datetime.timedelta(seconds=total)
    same_day = ends.date() == datetime.date.today()
    return f"~{span}, done about {ends.strftime('%H:%M' if same_day else '%a %H:%M')}"


def resolve_conf(path):
    """Accept a path relative to the current directory or to the repository."""
    for candidate in (path, os.path.join(REPO, path)):
        if os.path.isfile(candidate):
            return os.path.abspath(candidate)
    sys.exit(f"config file not found: {path}")


def check_names(path, rows):
    """
    Scenario names are a GLOBAL key, not a per-file one.

    The report tooling (report_capabilities.load_confs) indexes every conf file
    by scenario name alone, so a name used in two files makes one file's stored
    results describe the other file's load and device. That misattribution is
    silent: the run works, the numbers land in the wrong row.

    A name repeated inside ONE file is unambiguous breakage, so it fails here.
    A clash with a DIFFERENT file only spoils reporting, and 60-odd such clashes
    already exist in the tree, so that one warns and carries on.
    """
    names = [r["name"] for r in rows]
    dupes = sorted({n for n in names if names.count(n) > 1})
    if dupes:
        print(f"ERROR: {os.path.basename(path)} defines these scenarios twice: "
              f"{', '.join(dupes)}", file=sys.stderr)
        return False

    mine, clashes = set(names), {}
    here = os.path.abspath(path)
    for other in sorted(glob.glob(os.path.join(REPO, "deploy/*/*.conf"))):
        if os.path.abspath(other) == here:
            continue
        for line in open(other):
            line = line.strip()
            if not line or line.startswith("#") or "|" not in line:
                continue
            name = line.split("|")[0].strip()
            if name in mine:
                clashes.setdefault(os.path.relpath(other, REPO), []).append(name)
    if clashes:
        total = sum(len(v) for v in clashes.values())
        print(f"WARNING: {total} scenario name(s) are also defined in another "
              f"conf file. Results for them will be attributed to whichever "
              f"conf the report tooling loads last:", file=sys.stderr)
        for f, ns in sorted(clashes.items()):
            shown = ", ".join(sorted(ns)[:4]) + (" ..." if len(ns) > 4 else "")
            print(f"    {f}: {shown}", file=sys.stderr)
    return True


def load_conf(path):
    """pairs.conf (9 fields) and scenarios.conf (7 fields, both roles on blackbox)."""
    rows = []
    with open(path) as fh:
        for line in fh:
            # A trailing comment must go before the split, or it ends up inside
            # the last field: result_parser.sh then compares "1000  # note" with
            # -gt and the latency gate silently never runs.
            s = line.split("#")[0].strip()
            if not s:
                continue
            c = [x.strip() for x in s.split("|")]
            if len(c) >= 9:
                rows.append(dict(name=c[0], gen_host=c[1], gen_args=c[2],
                                 proc_host=c[3], proc_args=c[4], gen_lcores=c[5],
                                 proc_lcores=c[6], duration=int(c[7]), max_lat=c[8]))
            elif len(c) >= 7:
                rows.append(dict(name=c[0], gen_host="blackbox", gen_args=c[1],
                                 proc_host="blackbox", proc_args=c[2], gen_lcores=c[3],
                                 proc_lcores=c[4], duration=int(c[5]), max_lat=c[6]))
            else:
                print(f"skipping malformed line: {s}", file=sys.stderr)
    return rows


# --------------------------------------------------------------------------- #
# log inspection
# --------------------------------------------------------------------------- #
def last_pps(path):
    """Latest 'PPS | n |' sample - progress, not a verdict."""
    if not os.path.isfile(path):
        return 0
    val = 0
    for line in open(path, errors="ignore"):
        m = re.match(r"\s*PPS\s*\|\s*(\d+)\s*\|", line)
        if m:
            val = int(m.group(1))
    return val


def last_mbps(path):
    """Latest 'Load(Mbps) | n |' sample - progress, not a verdict."""
    if not os.path.isfile(path):
        return 0.0
    val = 0.0
    for line in open(path, errors="ignore"):
        m = re.match(r"\s*Load\(Mbps\)\s*\|\s*([\d.]+)\s*\|", line)
        if m:
            val = float(m.group(1))
    return val


def last_field(path, field):
    if not os.path.isfile(path):
        return 0
    val = 0
    for line in open(path, errors="ignore"):
        m = re.match(rf"\s*{field}\s*\|\s*(\d+)", line)
        if m:
            val = int(m.group(1))
    return val


def run_clock(path):
    """The application's own run clock ("Time N sec"), i.e. traffic seconds.

    Not wall time: the clock starts when the port is up, so this is the number
    that reaches --time. Displaying wall time instead showed 64/60s for a 60 s
    run, the difference being the generator's EAL init and link wait.
    """
    if not os.path.isfile(path):
        return 0
    val = 0
    for line in open(path, errors="ignore"):
        m = re.match(r"\s*Time\s+(\d+)\s+sec", line)
        if m:
            val = int(m.group(1))
    return val


def last_load(path):
    """RX CPU load: the steady-state Load % of the busiest lcore.

    Rows are "<name> | Min(us) | Max(us) | Load % | Wait % |" and the counters
    are per interval, but the final row is printed at shutdown and covers the
    whole run - including the seconds the processor idles before and after the
    generator, which dilutes it by about a fifth. Take the median of the steady
    samples instead, dropping zeros and then the ramp and the tail, exactly as
    steady_pps() does for rates.
    """
    if not os.path.isfile(path):
        return 0.0
    samples = {}
    for line in open(path, errors="ignore"):
        c = [x.strip() for x in line.split("|")]
        if len(c) < 5 or not re.match(r"^(Main|LCore\d+)$", c[0]):
            continue
        try:
            v = float(c[3])
        except ValueError:
            continue
        if v > 0:
            samples.setdefault(c[0], []).append(v)
    per_lcore = {}
    for name, vals in samples.items():
        core = vals[1:-1] if len(vals) > 3 else vals
        per_lcore[name] = statistics.median(core) if core else 0.0
    return max(per_lcore.values(), default=0.0)


def live_gaps(path):
    """Gap total from the periodic "Gaps | goose | sv |" row of the receiver.

    The per-stream table carrying ErrSeqCnt/ErrSpduCnt is printed once, at the
    end of the run, so it can never feed the progress line. The processor prints
    this row every interval instead, already deduplicated per stream. Falls back
    to the per-stream table for logs from a processor built before that row.
    """
    if not os.path.isfile(path):
        return 0
    total = None
    for line in open(path, errors="ignore"):
        m = re.match(r"\s*Gaps\s*\|\s*(\d+)\s*\|\s*(\d+)", line)
        if m:
            total = int(m.group(1)) + int(m.group(2))
    return total if total is not None else last_gaps(path)


def last_gaps(path):
    """Current sequence gaps, summed over the per-stream tables.

    The column layout differs per table, so the error columns are located from
    the header rather than assumed:
      GOOSE  MAC  | APPID | GOID | StNum  | SqNum     | ErrSeqCnt | ErrSpduCnt
      SV     DMAC | APPID | SVID | SmpCnt | ErrSeqCnt | ErrSpduCnt
    A third table (MAC | APPID | GOID | GOCB | DataSet | CRev) carries no error
    column at all -- reading it by position is what made a clean run report one
    gap per stream. The counters are cumulative, so the latest row per stream is
    the running total.
    """
    if not os.path.isfile(path):
        return 0
    cols, per_stream = None, {}
    for line in open(path, errors="ignore"):
        c = [x.strip() for x in line.split("|")]
        if "ErrSeqCnt" in c or "ErrSpduCnt" in c:
            cols = [i for i, name in enumerate(c) if name in ("ErrSeqCnt", "ErrSpduCnt")]
            continue
        if c and c[0] in ("MAC", "DMAC"):
            cols = None                       # a table without error columns
            continue
        if not cols or not re.match(r"^[0-9A-Fa-f:]{17}$", c[0]):
            continue
        try:
            # max, not sum: one lost frame increments both the inner sequence
            # and the outer R-SPDU sequence, so adding them counts it twice.
            per_stream[c[0]] = max(int(c[i]) for i in cols)
        except (ValueError, IndexError):
            continue
    return sum(per_stream.values())


def exit_status(path):
    """The line device_runner.sh appends after the application returns."""
    if not os.path.isfile(path):
        return None
    status = None
    for line in open(path, errors="ignore"):
        m = re.search(r"device_runner: exit_status=(-?\d+)", line)
        if m:
            status = int(m.group(1))
    return status


def has_summary(path, role):
    if not os.path.isfile(path):
        return False
    tag = "SUMMARY_GEN" if role == "gen" else "SUMMARY_PROC"
    return any(tag in line for line in open(path, errors="ignore"))


# --------------------------------------------------------------------------- #
# one scenario
# --------------------------------------------------------------------------- #
def finished_cleanly(logs):
    """(ok, reason): every role ended by itself, with a status and a summary.

    Only meaningful for a run that was left alone. A test the runner stopped on
    purpose is not judged here -- we already know why it ended.
    """
    for log, role in logs:
        status = exit_status(log)
        if status is None:
            return False, f"{role} log carries no exit status (killed or truncated)"
        if status != 0:
            return False, f"{role} exit_status={status}"
        if not has_summary(log, role):
            return False, f"{role} finished without a summary block"
    return True, ""


def fetch_logs(row, out):
    gen_log = os.path.join(out, f"{row['name']}_gen")
    proc_log = os.path.join(out, f"{row['name']}_proc")
    scp_from(row["gen_host"], f"{DEV_OUT}/{row['name']}_gen", gen_log)
    scp_from(row["proc_host"], f"{DEV_OUT}/{row['name']}_proc", proc_log)
    return gen_log, proc_log


def stamp_backend(out, proc_log):
    """Record which crypto backend the batch ran on, once, in <out>/backend.

    The processor prints the backend only when routable streams are configured,
    so an L2-only run says nothing and leaves the file for a later row to write.
    Without the file the report tooling falls back to a default, which is right
    for mbedtls and silently wrong for anything else - that is how a whole
    OpenSSL batch came to be filed as mbedtls.
    """
    marker = os.path.join(out, "backend")
    if os.path.exists(marker):
        return
    try:
        with open(proc_log, errors="replace") as fh:
            m = re.search(r"crypto backend:\s*(\w+)", fh.read())
    except OSError:
        return
    if m:
        with open(marker, "w") as fh:
            fh.write(m.group(1) + "\n")
        print(f"    crypto backend: {m.group(1)} (recorded in {marker})")


def verdict(row, gen_log, proc_log, out):
    """Reuse result_parser.sh for the JSON and report_gaps.py for the gates."""
    summary = os.path.join(out, f"{row['name']}.summary.json")
    with open(summary, "w") as fh:
        subprocess.run(["bash", PARSER, gen_log, proc_log, row["name"],
                        str(row["duration"]), row["max_lat"]], stdout=fh)

    with open(summary) as fh:
        js = json.load(fh)
    st, pr = js.get("streams", {}), js.get("processor", {})

    # Same gate inputs as report_gaps.py: gaps are the sequence/SPDU totals, CRC
    # is ierrors, and the per-stream table is only a boolean fallback.
    tx, rx = steady_pps(gen_log), steady_pps(proc_log)
    # Per protocol the two layers count the same loss, so take the larger one.
    gaps = (max(st.get("goose_err_seq_total", 0), st.get("goose_err_spdu_total", 0))
            + max(st.get("sv_err_smp_total", 0), st.get("sv_err_spdu_total", 0)))
    # Totals, cross-checked against the per-stream table and the periodic row.
    gaps = max(gaps, last_gaps(proc_log), live_gaps(proc_log))
    crc = pr.get("ierrors", 0)
    missed = pr.get("imissed", 0)

    if tx == 0 or rx == 0:
        cls = "VOID"                              # nothing was measured
    elif gaps > GAP_OK or crc > CRC_OK or missed >= KILO:
        cls = "LIMIT"
    elif gaps == 0 and abs(rx - tx) / tx <= TOL:
        cls = "CLEAN"
    else:
        cls = "LINK-NOISE"
    load = max((pr.get("lcore_load_pct") or {}).values(), default=last_load(proc_log))
    # Wire load of the received traffic. Prefer the periodic samples: the
    # summary's rx_mbps is a lifetime figure and reads 0 on several runs.
    mbps = steady_mbps(proc_log) or pr.get("rx_mbps", 0.0)
    return dict(tx=tx, rx=rx, gaps=gaps, crc=crc, missed=missed, load=load,
                mbps=mbps, ring_full=pr.get("ring_full", 0), cls=cls)


def run_one(row, out, workdir):
    """Return (status, info). status: ok | abort."""
    name = row["name"]
    gen_host, proc_host = row["gen_host"], row["proc_host"]
    print(f"\n==== {name}  ({gen_host} -> {proc_host}, {row['duration']}s) ====")

    for host in {gen_host, proc_host}:
        r = preflight(host)
        if r.returncode != 0:
            return "abort", f"preflight failed on {host}: {r.stderr.strip()}"

    loopback = gen_host == proc_host
    ok = write_params(proc_host, name, "proc", row["proc_args"], row["proc_lcores"],
                      proc_run_time(row["duration"]), workdir, loopback)
    ok &= write_params(gen_host, name, "gen", row["gen_args"], row["gen_lcores"],
                       row["duration"], workdir, loopback)
    if not ok:
        return "abort", "could not copy the run parameters"

    if start_role(proc_host, "proc", name).returncode != 0:
        return "abort", "processor did not start"
    time.sleep(START_DELAY)
    if start_role(gen_host, "gen", name).returncode != 0:
        stop_role(proc_host, "proc")
        return "abort", "generator did not start"

    # Both applications end on their own --time. Watch, do not touch. The counter
    # shows the generator's own run (its --time starts only after EAL init and
    # link-up, so it can pass duration by a few seconds); the abort deadline is a
    # separate, much later backstop and is deliberately not displayed.
    started = time.time()
    deadline = started + row["duration"] + STARTUP_MARGIN
    gen_log = proc_log = None
    losing, cut_short = 0, False
    while time.time() < deadline:
        time.sleep(POLL)
        gen_log, proc_log = fetch_logs(row, out)
        gen_running = session_alive(gen_host, "gen")
        if gen_running:
            tx, rx = last_pps(gen_log), last_pps(proc_log)
            print(f"    TX {tx:>9,}  RX {rx:>9,}  "
                  f"gaps {live_gaps(proc_log):>7,}  "
                  f"missed {last_field(proc_log, 'Missed'):>7,}  "
                  f"crc {last_field(proc_log, 'Errors'):>5,}  "
                  f"RX {last_mbps(proc_log):>8,.1f} Mb/s  "
                  f"load {last_load(proc_log):>5.1f}%   "
                  f"({run_clock(gen_log)}/{row['duration']}s)")
            losing = losing + 1 if losing_badly(tx, rx) else 0
            if losing >= LOSS_SAMPLES and not is_burst(row):
                # Deliberate early stop, not a fault: stop both roles and carry
                # on with the batch. The result stands as a ceiling.
                print(f"    RX below {LOSS_RATIO:.0%} of TX for {losing} samples "
                      f"- ceiling reached, stopping this test")
                stop_role(gen_host, "gen")
                stop_role(proc_host, "proc")
                cut_short = True
                break
        else:
            print("    generator finished, waiting for the receiver to close")
        if not gen_running and not session_alive(proc_host, "proc"):
            break
    else:
        # Deadline passed with something still running: stop it and stop the batch.
        for host, role in ((gen_host, "gen"), (proc_host, "proc")):
            if session_alive(host, role):
                stop_role(host, role)
        return "abort", "a run did not finish by itself (stopped by hand)"

    gen_log, proc_log = fetch_logs(row, out)
    if not cut_short:
        ok, reason = finished_cleanly(((gen_log, "gen"), (proc_log, "proc")))
        if not ok:
            return "abort", reason

    stamp_backend(out, proc_log)
    res = verdict(row, gen_log, proc_log, out)
    if cut_short:
        res["cls"] = "LIMIT"          # a ceiling is a ceiling, however short the run
        res["note"] = "stopped early"
    print(f"  RESULT {name}: TX {res['tx']:,}  RX {res['rx']:,}  gaps {res['gaps']:,}  "
          f"crc {res['crc']:,}  missed {res['missed']:,}  ring_full {res['ring_full']:,}  "
          f"RX {res['mbps']:,.1f} Mb/s  RX load {res['load']:.1f}%  -> {res['cls']}"
          f"{' (stopped early)' if cut_short else ''}")
    if res["cls"] == "VOID":
        return "abort", "no traffic was measured (TX or RX is zero)"
    return "ok", res


# --------------------------------------------------------------------------- #
def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--conf", default=os.path.join(REPO, "deploy/pairs/pairs.conf"))
    ap.add_argument("--scenario", action="append", default=[],
                    help="run only this scenario (repeatable)")
    ap.add_argument("--all", action="store_true", help="run every scenario in the config")
    ap.add_argument("--list", action="store_true", help="print scenario names and exit")
    ap.add_argument("--out", default=None, help="results directory (default: <conf dir>/results/<stamp>)")
    args = ap.parse_args()

    args.conf = resolve_conf(args.conf)
    rows = load_conf(args.conf)
    if not check_names(args.conf, rows):
        return 1
    if args.list:
        for r in rows:
            print(r["name"])
        print(f"\n{len(rows)} scenarios, {estimate(rows)}")
        return 0
    if args.scenario:
        rows = [r for r in rows if r["name"] in args.scenario]
    elif not args.all:
        ap.error("give --scenario NAME, --all, or --list")
    if not rows:
        print("no matching scenarios", file=sys.stderr)
        return 1

    stamp = datetime.datetime.now().strftime("%Y_%m_%d-%H_%M")
    out = args.out or os.path.join(os.path.dirname(os.path.abspath(args.conf)),
                                   "results", stamp)
    os.makedirs(out, exist_ok=True)
    workdir = os.path.join(out, "params")
    os.makedirs(workdir, exist_ok=True)
    print(f"config  : {args.conf}")
    print(f"results : {out}")
    print(f"tests   : {len(rows)}")
    print(f"estimate: {estimate(rows)}")

    hosts_used, done, aborted = set(), [], None
    for row in rows:
        hosts_used.update((row["gen_host"], row["proc_host"]))
    print(f"hosts   : {', '.join(sorted(hosts_used))}")
    try:
        for host in sorted(hosts_used):
            ok, why = prepare(host)
            if not ok:
                print(f"ABORT: {host}: {why}")
                return 1

        for row in rows:
            status, info = run_one(row, out, workdir)
            if status == "abort":
                aborted = (row["name"], info)
                print(f"\nABORT after {row['name']}: {info}")
                print("Remaining tests are skipped: the devices are in an unknown state.")
                break
            done.append((row["name"], info))
    except KeyboardInterrupt:
        print("\nCtrl+C - stopping both roles on every device used")
        for host in hosts_used:
            for role in ("gen", "proc"):
                stop_role(host, role)
        print("stopped")
        return 130

    print(f"\n{'test':32}{'TX':>11}{'RX':>11}{'gaps':>11}{'crc':>6}"
          f"{'ring_full':>12}{'RXMb/s':>10}{'RXload%':>9}  class")
    for name, res in done:
        print(f"{name:32}{res['tx']:>11,}{res['rx']:>11,}{res['gaps']:>11,}"
              f"{res['crc']:>6,}{res['ring_full']:>12,}{res['mbps']:>10,.1f}"
              f"{res['load']:>9.1f}  {res['cls']}"
              f"{' (stopped early)' if res.get('note') else ''}")
    print(f"\n{len(done)} of {len(rows)} completed. Results in {out}/")
    return 1 if aborted else 0


if __name__ == "__main__":
    sys.exit(main())
