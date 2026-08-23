#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
"""Tests for the runner: parameter delivery, exit-status capture, empty-run gate.

They cover the three defects that made a whole batch worthless on 2026-08-11:
a run whose parameters never arrived, a run that died without anyone noticing,
and a run with zero traffic that was still scored PASS.

Run:  ./deploy/common/test_run_tests.py
"""

import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import run_tests as rt  # noqa: E402

DEVICE_RUNNER = os.path.join(HERE, "device_runner.sh")


class ConfigTest(unittest.TestCase):
    """Both existing config layouts must keep working - no new config format."""

    def setUp(self):
        self.dir = tempfile.mkdtemp()
        self.path = os.path.join(self.dir, "c.conf")

    def tearDown(self):
        shutil.rmtree(self.dir)

    def test_pairs_layout_nine_fields(self):
        with open(self.path, "w") as fh:
            fh.write("# comment\n\n")
            fh.write("t1 | blackbox | --rgoose 100,1000 | opi3b | --rgoose 100 |  | 1,2,3 | 60 | 1000\n")
        row = rt.load_conf(self.path)[0]
        self.assertEqual(row["name"], "t1")
        self.assertEqual(row["gen_host"], "blackbox")
        self.assertEqual(row["proc_host"], "opi3b")
        self.assertEqual(row["proc_lcores"], "1,2,3")
        self.assertEqual(row["duration"], 60)

    def test_commented_out_scenarios_are_skipped(self):
        """A "#" in front of the name disables that test, with or without a space."""
        with open(self.path, "w") as fh:
            fh.write("keep1 | blackbox | --goose 10 | opi3b | --goose 10 |  | 2 | 60 | 0\n")
            fh.write("#skip1 | blackbox | --goose 10 | opi3b | --goose 10 |  | 2 | 60 | 0\n")
            fh.write("  # skip2 | blackbox | --goose 10 | opi3b | --goose 10 |  | 2 | 60 | 0\n")
            fh.write("keep2 | --goose 10 | --goose 10 |  | 2 | 60 | 0\n")
        self.assertEqual([r["name"] for r in rt.load_conf(self.path)], ["keep1", "keep2"])

    def test_qotom_layout_seven_fields_is_single_box(self):
        with open(self.path, "w") as fh:
            fh.write("t2 | --goose 100,10000 | --goose 100 |  | 4,5,6 | 300 | 100\n")
        row = rt.load_conf(self.path)[0]
        self.assertEqual(row["gen_host"], "blackbox")
        self.assertEqual(row["proc_host"], "blackbox")
        self.assertEqual(row["proc_lcores"], "4,5,6")


class TimingTest(unittest.TestCase):
    """The generator gets exactly the configured time; the receiver brackets it."""

    def test_receiver_covers_lead_in_run_and_tail(self):
        self.assertEqual(rt.START_DELAY, 5)
        self.assertEqual(rt.PROC_PAD, 10)
        self.assertEqual(rt.proc_run_time(60), 75)   # 5 + 60 + 10
        self.assertEqual(rt.proc_run_time(300), 315)


class EarlyStopTest(unittest.TestCase):
    """A receiver at half the offered rate is a ceiling: do not burn the run."""

    def test_half_rate_triggers(self):
        self.assertTrue(rt.losing_badly(700000, 304000))

    def test_full_rate_does_not(self):
        self.assertFalse(rt.losing_badly(700000, 700000))
        self.assertFalse(rt.losing_badly(700000, 699000))

    def test_exactly_at_the_threshold_does_not(self):
        self.assertFalse(rt.losing_badly(100000, 50000))

    def test_no_traffic_yet_does_not(self):
        """Before the generator is up TX is zero - that is startup, not a limit."""
        self.assertFalse(rt.losing_badly(0, 0))

    def test_two_samples_are_required(self):
        self.assertEqual(rt.LOSS_SAMPLES, 2)


class ParamsFileTest(unittest.TestCase):
    """Run parameters travel in a file, never on the ssh command line."""

    def setUp(self):
        self.dir = tempfile.mkdtemp()
        self.calls = []
        self._ssh, self._scp = rt.ssh, rt.scp_to
        rt.ssh = lambda *a, **k: self.calls.append(a) or subprocess.CompletedProcess(a, 0, "", "")
        rt.scp_to = lambda *a, **k: subprocess.CompletedProcess(a, 0, "", "")

    def tearDown(self):
        rt.ssh, rt.scp_to = self._ssh, self._scp
        shutil.rmtree(self.dir)

    def test_params_file_content(self):
        rt.write_params("opi3b", "t1", "proc", "--rgoose 100 --r-mode gcm",
                        "1,2,3", 95, self.dir)
        with open(os.path.join(self.dir, "t1_proc.params")) as fh:
            body = fh.read()
        self.assertIn("APP_ARGS='--rgoose 100 --r-mode gcm'", body)
        self.assertIn("LCORES='1,2,3'", body)
        self.assertIn("NIC='0000:01:00.0'", body)   # opi3b BDF, not the blackbox one
        self.assertIn("TIME=95", body)

    def test_ssh_arguments_are_plain_words(self):
        """No shell metacharacters: no chains, quotes or redirections remotely."""
        rt.write_params("opi3b", "t1", "gen", "--goose 100", "", 60, self.dir)
        for call in self.calls:
            for arg in call[1:]:
                self.assertNotRegex(arg, r"[&;|<>\"']")


class PrepareTest(unittest.TestCase):
    """The runner syncs device_runner.sh itself and tunes a rebooted board."""

    def setUp(self):
        self.ssh_calls, self.scp_calls = [], []
        self._ssh, self._scp = rt.ssh, rt.scp_to
        rt.scp_to = lambda *a, **k: self.scp_calls.append(a) or \
            subprocess.CompletedProcess(a, 0, "", "")

    def tearDown(self):
        rt.ssh, rt.scp_to = self._ssh, self._scp

    def _ssh_stub(self, sentinel_present):
        def stub(host, *args, **kw):
            self.ssh_calls.append(args)
            rc = 0 if (args[:2] != ("test", "-e") or sentinel_present) else 1
            return subprocess.CompletedProcess(args, rc, "", "")
        return stub

    def test_device_runner_is_copied(self):
        rt.ssh = self._ssh_stub(sentinel_present=True)
        ok, why = rt.prepare("opi3b")
        self.assertTrue(ok, why)
        self.assertTrue(self.scp_calls[0][1].endswith("device_runner.sh"))

    def test_setup_skipped_when_sentinel_present(self):
        rt.ssh = self._ssh_stub(sentinel_present=True)
        rt.prepare("opi3b")
        self.assertFalse(any("setup_platform.sh" in a for c in self.ssh_calls for a in c))

    def test_failure_reports_the_underlying_error(self):
        """A swallowed scp error once cost a batch: the reason must be visible."""
        rt.ssh = self._ssh_stub(sentinel_present=True)
        rt.scp_to = lambda *a, **k: subprocess.CompletedProcess(
            a, 1, "", "ssh: connect to host 192.168.100.103 port 22: No route to host")
        ok, msg = rt.prepare("blackbox")
        self.assertFalse(ok)
        self.assertIn("No route to host", msg)

    def test_setup_runs_when_board_was_rebooted(self):
        rt.ssh = self._ssh_stub(sentinel_present=False)
        ok, _ = rt.prepare("opi3b")
        self.assertTrue(ok)
        self.assertTrue(any("setup_platform.sh" in a for c in self.ssh_calls for a in c))


class DeviceRunnerTest(unittest.TestCase):
    """device_runner.sh run <role> <name>: reads the params file, records the status."""

    def setUp(self):
        self.dir = tempfile.mkdtemp()
        self.out = os.path.join(self.dir, "out")
        os.makedirs(os.path.join(self.dir, "runs"))
        shutil.copy(DEVICE_RUNNER, self.dir)
        with open(os.path.join(self.dir, "runs", "t1_gen.params"), "w") as fh:
            fh.write("APP_ARGS='--rgoose 100,1000 --r-mode none'\n"
                     "LCORES='1,2,3'\nNIC='0000:07:00.0'\nTIME=60\n")

    def tearDown(self):
        shutil.rmtree(self.dir)
        shutil.rmtree("/tmp/pbus_out", ignore_errors=True)

    def _fake_app(self, body):
        path = os.path.join(self.dir, "run_generator.sh")
        with open(path, "w") as fh:
            fh.write(body)

    def _run(self):
        subprocess.run(["bash", "device_runner.sh", "run", "gen", "t1"],
                       cwd=self.dir, capture_output=True, text=True,
                       env={**os.environ, "OUTDIR": self.out})
        with open("/tmp/pbus_out/t1_gen") as fh:
            return fh.read()

    def test_parameters_reach_the_application(self):
        self._fake_app('#!/bin/bash\necho "lcores=$LCORES nic=$NIC_PCI_ADDR args=$*"\n')
        log = self._run()
        self.assertIn("lcores=1,2,3", log)
        self.assertIn("nic=0000:07:00.0", log)
        self.assertIn("--rgoose 100,1000 --r-mode none --time 60", log)

    def test_exit_status_is_recorded(self):
        self._fake_app("#!/bin/bash\nexit 0\n")
        self.assertIn("device_runner: exit_status=0", self._run())
        self._fake_app("#!/bin/bash\nexit 3\n")
        self.assertIn("device_runner: exit_status=3", self._run())

    def test_missing_params_file_fails_loudly(self):
        r = subprocess.run(["bash", "device_runner.sh", "run", "gen", "nosuch"],
                           cwd=self.dir, capture_output=True, text=True)
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("missing", r.stderr)


class GapsTest(unittest.TestCase):
    """Gaps are read by header: GOOSE, SV and the config table differ."""

    CONFIG = ("MAC                  | APPID      | GOID   | GOCB   | DataSet | CRev       |\n"
              "01:00:5E:40:01:01    | 1          | GOID01 | ref    | ds      | 1          |\n")
    GOOSE = ("MAC                  | APPID | GOID   | StNum | SqNum | ErrSeqCnt | ErrSpduCnt |\n"
             "01:00:5E:40:01:01    | 1     | GOID01 | 5496  | 0     | {a}         | {b}          |\n")
    SV = ("DMAC                 | APPID | SVID   | SmpCnt | ErrSeqCnt | ErrSpduCnt |\n"
          "01:00:5E:40:02:01    | 1     | SVID01 | 4000   | {a}         | {b}          |\n")

    def setUp(self):
        self.dir = tempfile.mkdtemp()

    def tearDown(self):
        shutil.rmtree(self.dir)

    def _log(self, text):
        path = os.path.join(self.dir, "p")
        with open(path, "w") as fh:
            fh.write(text)
        return path

    def test_config_table_is_not_a_gap(self):
        """CRev=1 in the configuration table must not count as a gap."""
        self.assertEqual(rt.last_gaps(self._log(self.CONFIG + self.GOOSE.format(a=0, b=0))), 0)

    def test_goose_gaps_counted(self):
        """max, not sum: one lost frame bumps ErrSeqCnt and ErrSpduCnt together."""
        log = self._log(self.CONFIG + self.GOOSE.format(a=3, b=2))
        self.assertEqual(rt.last_gaps(log), 3)

    def test_one_loss_is_one_gap_not_two(self):
        """The observed case: 4 lost frames reported as 4, never 8."""
        rows = "".join(self.GOOSE.format(a=1, b=1) for _ in range(1))
        self.assertEqual(rt.last_gaps(self._log(rows)), 1)

    def test_sv_column_positions(self):
        """SmpCnt sits where GOOSE keeps ErrSeqCnt - it must not be read as one."""
        self.assertEqual(rt.last_gaps(self._log(self.SV.format(a=0, b=0))), 0)
        self.assertEqual(rt.last_gaps(self._log(self.SV.format(a=7, b=1))), 7)

    def test_live_gaps_read_from_the_periodic_row(self):
        """The per-stream table only exists at the end; the row is per interval."""
        log = self._log("Gaps        | 2          | 0          |\n"
                        "Gaps        | 5          | 3          |\n")
        self.assertEqual(rt.live_gaps(log), 8)

    def test_live_gaps_fall_back_to_the_stream_table(self):
        """Logs from a processor built before the Gaps row must still work."""
        log = self._log(self.CONFIG + self.GOOSE.format(a=4, b=4))
        self.assertEqual(rt.live_gaps(log), 4)

    def test_rx_load_from_the_periodic_lcore_table(self):
        log = self._log(
            "                 | Min(us)    | Max(us)    | Load %     | Wait %     |\n"
            "Main             | 0          | 3          | 12.500     | 87.500     |\n"
            "LCore2           | 0          | 17         | 71.572     | 28.428     |\n")
        self.assertAlmostEqual(rt.last_load(log), 71.572, places=3)
        self.assertEqual(rt.last_load(self._log("nothing here\n")), 0.0)

    def test_run_clock_is_the_application_clock(self):
        """Progress must show traffic seconds, never wall time (64/60s bug)."""
        log = self._log("Time 3 sec\nTime 57 sec\nTime 60 sec\n")
        self.assertEqual(rt.run_clock(log), 60)
        self.assertEqual(rt.run_clock(self._log("no ticks yet\n")), 0)

    def test_counters_are_cumulative_last_block_wins(self):
        log = self._log(self.GOOSE.format(a=1, b=0) + self.GOOSE.format(a=4, b=0))
        self.assertEqual(rt.last_gaps(log), 4)


class FinishedCleanlyTest(unittest.TestCase):
    """Only a run left alone is judged on how it ended."""

    def setUp(self):
        self.dir = tempfile.mkdtemp()

    def tearDown(self):
        shutil.rmtree(self.dir)

    def _log(self, text, name="l"):
        path = os.path.join(self.dir, name)
        with open(path, "w") as fh:
            fh.write(text)
        return path

    def test_clean_pair(self):
        g = self._log("SUMMARY_GEN\ndevice_runner: exit_status=0\n", "g")
        p = self._log("SUMMARY_PROC\ndevice_runner: exit_status=0\n", "p")
        self.assertEqual(rt.finished_cleanly(((g, "gen"), (p, "proc"))), (True, ""))

    def test_missing_status_is_reported(self):
        g = self._log("SUMMARY_GEN\n", "g")
        p = self._log("SUMMARY_PROC\ndevice_runner: exit_status=0\n", "p")
        ok, reason = rt.finished_cleanly(((g, "gen"), (p, "proc")))
        self.assertFalse(ok)
        self.assertIn("no exit status", reason)

    def test_nonzero_status_is_reported(self):
        g = self._log("device_runner: exit_status=139\n", "g")
        ok, reason = rt.finished_cleanly(((g, "gen"),))
        self.assertFalse(ok)
        self.assertIn("139", reason)


class VerdictTest(unittest.TestCase):
    """A run with no traffic is VOID. It must never be reported as a pass."""

    def setUp(self):
        self.dir = tempfile.mkdtemp()

    def tearDown(self):
        shutil.rmtree(self.dir)

    def _logs(self, gen_body, proc_body):
        gen = os.path.join(self.dir, "t_gen")
        proc = os.path.join(self.dir, "t_proc")
        with open(gen, "w") as fh:
            fh.write(gen_body)
        with open(proc, "w") as fh:
            fh.write(proc_body)
        return gen, proc

    def test_empty_run_is_void(self):
        """The 2026-08-11 shape: app died early, every counter absent."""
        gen, proc = self._logs("Start main loop\ndevice_runner: exit_status=0\n",
                               "Time 24 sec\ndevice_runner: exit_status=0\n")
        row = dict(name="t", duration=60, max_lat="1000")
        res = rt.verdict(row, gen, proc, self.dir)
        self.assertEqual(res["cls"], "VOID")
        self.assertEqual(res["tx"], 0)
        self.assertEqual(res["rx"], 0)
        # A log with no SUMMARY block parses as all zeros, which used to read as
        # a flawless run. Absence of the block is itself the failure.
        with open(os.path.join(self.dir, "t.summary.json")) as fh:
            js = json.load(fh)
        self.assertFalse(js["pass"])
        self.assertIn("summary missing", js["fail_reason"])

    def test_clean_run(self):
        table = "PPS         | {v}          |\n"
        gen = "".join(table.format(v=100000) for _ in range(6))
        proc = "".join(table.format(v=100000) for _ in range(6))
        gen, proc = self._logs(gen, proc)
        res = rt.verdict(dict(name="t", duration=60, max_lat="0"), gen, proc, self.dir)
        self.assertEqual(res["cls"], "CLEAN")
        self.assertEqual(res["tx"], 100000)
        self.assertEqual(res["rx"], 100000)


class BackendMarkerTest(unittest.TestCase):
    """A batch must record its crypto backend, so reports never have to guess."""

    def setUp(self):
        self.dir = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.dir, ignore_errors=True)
        self.marker = os.path.join(self.dir, "backend")

    def _log(self, text):
        path = os.path.join(self.dir, "p")
        with open(path, "w") as fh:
            fh.write(text)
        return path

    def test_banner_is_recorded(self):
        rt.stamp_backend(self.dir, self._log(
            "Routable streams: 3 group(s), security hmac (crypto backend: openssl)\n"))
        with open(self.marker) as fh:
            self.assertEqual(fh.read().strip(), "openssl")

    def test_l2_only_run_leaves_it_for_a_later_row(self):
        # L2 never prints the banner, and guessing there is what mislabelled a
        # whole OpenSSL batch as mbedtls.
        rt.stamp_backend(self.dir, self._log("GOOSE L2, no routable streams\n"))
        self.assertFalse(os.path.exists(self.marker))

    def test_first_writer_wins(self):
        rt.stamp_backend(self.dir, self._log("(crypto backend: mbedtls)\n"))
        rt.stamp_backend(self.dir, self._log("(crypto backend: openssl)\n"))
        with open(self.marker) as fh:
            self.assertEqual(fh.read().strip(), "mbedtls")


if __name__ == "__main__":
    unittest.main(verbosity=2)
