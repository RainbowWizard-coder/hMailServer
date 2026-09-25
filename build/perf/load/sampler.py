# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

"""The server's memory and CPU, sampled while the load runs. Optional: a run
without a sampler measures rates and latencies and says memory was not sampled.

What it can watch, and how, in order of preference:

  processes by name   --server-process hmailserver  (or dovecot,imap,imap-login,...)
                      with --include-children, the named processes and every
                      descendant. psutil when it is installed (pip install
                      psutil); otherwise /proc on Linux and one PowerShell
                      Get-Process per sample on Windows (slow to start, about a
                      quarter of a second, so the interval is at least 2 s there).
  processes by pid    --server-pid 1234[,5678]
  cgroups (Linux)     --server-cgroup /sys/fs/cgroup/system.slice/docker-<id>.scope[,...]
  containers          --server-container hmlab-bench,hmlab-mariadb-1   (Linux: each
                      container's cgroup, found with docker inspect, the figures
                      added up - a server and the database it needs, or Postfix
                      and Dovecot, are one mail service)

What a sample holds:

  rss_bytes      the sum of the processes' resident sets (Windows: working
                 sets). Pages shared between processes are counted once per
                 process, which overstates a server made of many processes.
  pss_bytes      Linux only, and only where /proc/<pid>/smaps_rollup is readable
                 (the same user, or root): the proportional set, shared pages
                 divided among their sharers. The fair sum for Dovecot's many
                 processes, and what the per-session figure uses when it exists.
  private_bytes  Windows (and psutil): memory committed to the process alone.
  cgroup_anon_bytes, cgroup_current_bytes
                 a cgroup's anonymous memory and its whole charge (which counts
                 page cache: the message files read and written). anon is the
                 per-session figure's input for a container.
  cpu_seconds    the processes' (or the cgroup's) user and system CPU time so
                 far; its change over a window, divided by the window, is how
                 many cores the server used.
"""

import json
import os
import shutil
import subprocess
import sys
import threading
import time

try:
    import psutil  # optional
except ImportError:  # pragma: no cover - the ordinary case
    psutil = None


def _read(path):
    try:
        with open(path, "rb") as handle:
            return handle.read().decode("utf-8", "replace")
    except OSError:
        return None


class Sampler(object):

    def __init__(self, names=None, pids=None, include_children=False, cgroups=None, containers=None, interval=1.0):
        self.names = [n.strip().lower() for n in (names or []) if n.strip()]
        self.pids = [int(p) for p in (pids or [])]
        self.include_children = include_children
        self.cgroups = list(cgroups or [])
        self.interval = interval
        self.method = None
        self.samples = []
        self.problem = None
        self._stop = threading.Event()
        self._thread = None
        for container in containers or []:
            found = self._cgroup_of_container(container.strip())
            if found:
                self.cgroups.append(found)
        if self.cgroups:
            self.method = "cgroup"
        elif self.names or self.pids:
            if psutil is not None:
                self.method = "psutil"
            elif sys.platform.startswith("linux"):
                self.method = "proc"
            elif sys.platform == "win32":
                self.method = "powershell"
                self.interval = max(self.interval, 2.0)
            else:
                self.problem = "no way to read another process's memory here: install psutil"
        if self.method == "proc":
            try:
                self._ticks = os.sysconf("SC_CLK_TCK")
            except (ValueError, OSError, AttributeError):
                self._ticks = 100

    @property
    def enabled(self):
        return self.method is not None

    def describe(self):
        return {
            "method": self.method,
            "names": self.names,
            "pids": self.pids,
            "include_children": self.include_children,
            "cgroups": self.cgroups,
            "interval_seconds": self.interval,
            "problem": self.problem,
        }

    # ------------------------------------------------------------ cgroups

    def _cgroup_of_container(self, container):
        if not sys.platform.startswith("linux") or not shutil.which("docker"):
            self.problem = "--server-container needs Linux and the docker command"
            return None
        try:
            identifier = subprocess.run(["docker", "inspect", "-f", "{{.Id}}", container], stdout=subprocess.PIPE,
                                        stderr=subprocess.DEVNULL, timeout=30).stdout.decode("ascii", "replace").strip()
        except (OSError, subprocess.SubprocessError):
            identifier = ""
        if not identifier:
            self.problem = "no container %s" % container
            return None
        for candidate in ("/sys/fs/cgroup/system.slice/docker-%s.scope" % identifier,
                          "/sys/fs/cgroup/docker/%s" % identifier):
            if os.path.isdir(candidate):
                return candidate
        self.problem = "found container %s but not its cgroup v2 directory under /sys/fs/cgroup" % container
        return None

    def _sample_cgroup(self):
        """Each cgroup's memory, CPU and memory events, added up."""
        sample = {"processes": 0}

        def add(key, value):
            sample[key] = sample.get(key, 0) + value
        for cgroup in self.cgroups:
            current = _read(os.path.join(cgroup, "memory.current"))
            if current and current.strip().isdigit():
                add("cgroup_current_bytes", int(current))
            stat = _read(os.path.join(cgroup, "memory.stat")) or ""
            for line in stat.splitlines():
                key, _, value = line.partition(" ")
                if key == "anon" and value.strip().isdigit():
                    add("cgroup_anon_bytes", int(value))
            events = _read(os.path.join(cgroup, "memory.events")) or ""
            for line in events.splitlines():
                key, _, value = line.partition(" ")
                if key in ("max", "oom", "oom_kill") and value.strip().isdigit():
                    add("cgroup_event_" + key, int(value))
            cpu = _read(os.path.join(cgroup, "cpu.stat")) or ""
            for line in cpu.splitlines():
                key, _, value = line.partition(" ")
                if key == "usage_usec" and value.strip().isdigit():
                    add("cpu_seconds", int(value) / 1e6)
            procs = _read(os.path.join(cgroup, "cgroup.procs")) or ""
            add("processes", len([p for p in procs.split() if p.strip()]))
        return sample

    # ------------------------------------------------------------ /proc

    def _proc_pids(self):
        table = {}
        for entry in os.listdir("/proc"):
            if not entry.isdigit():
                continue
            stat = _read("/proc/%s/stat" % entry)
            if not stat:
                continue
            close = stat.rfind(")")
            comm = stat[stat.find("(") + 1:close].lower()
            rest = stat[close + 2:].split()
            try:
                ppid = int(rest[1])
            except (IndexError, ValueError):
                continue
            argv0 = (_read("/proc/%s/cmdline" % entry) or "").split("\0")[0]
            table[int(entry)] = (comm, os.path.basename(argv0).lower(), ppid)
        chosen = set(p for p in self.pids if p in table)
        for pid, (comm, argv0, _) in table.items():
            if comm in self.names or argv0 in self.names:
                chosen.add(pid)
        if self.include_children and chosen:
            changed = True
            while changed:
                changed = False
                for pid, (_, _, ppid) in table.items():
                    if ppid in chosen and pid not in chosen:
                        chosen.add(pid)
                        changed = True
        return sorted(chosen)

    def _sample_proc(self):
        rss = 0
        pss = 0
        pss_complete = True
        cpu = 0.0
        pids = self._proc_pids()
        for pid in pids:
            status = _read("/proc/%d/status" % pid) or ""
            for line in status.splitlines():
                if line.startswith("VmRSS:"):
                    rss += int(line.split()[1]) * 1024
            rollup = _read("/proc/%d/smaps_rollup" % pid)
            found = False
            if rollup:
                for line in rollup.splitlines():
                    if line.startswith("Pss:"):
                        pss += int(line.split()[1]) * 1024
                        found = True
                        break
            if not found:
                pss_complete = False
            stat = _read("/proc/%d/stat" % pid)
            if stat:
                fields = stat[stat.rfind(")") + 2:].split()
                try:
                    cpu += (int(fields[11]) + int(fields[12])) / float(self._ticks)
                except (IndexError, ValueError):
                    pass
        sample = {"processes": len(pids), "rss_bytes": rss, "cpu_seconds": cpu}
        if pids and pss_complete:
            sample["pss_bytes"] = pss
        return sample

    # ------------------------------------------------------------ psutil

    def _sample_psutil(self):
        chosen = {}
        for process in psutil.process_iter(["pid", "name"]):
            try:
                name = (process.info.get("name") or "").lower()
                base = name[:-4] if name.endswith(".exe") else name
                if process.pid in self.pids or name in self.names or base in self.names:
                    chosen[process.pid] = process
            except (psutil.Error, OSError):
                continue
        if self.include_children:
            for process in list(chosen.values()):
                try:
                    for child in process.children(recursive=True):
                        chosen[child.pid] = child
                except (psutil.Error, OSError):
                    continue
        rss = 0
        private = 0
        has_private = False
        pss = 0
        pss_complete = sys.platform.startswith("linux")
        cpu = 0.0
        for pid, process in chosen.items():
            try:
                memory = process.memory_info()
                rss += memory.rss
                if hasattr(memory, "private"):
                    private += memory.private
                    has_private = True
                times = process.cpu_times()
                cpu += times.user + times.system
            except (psutil.Error, OSError):
                continue
            if pss_complete:
                rollup = _read("/proc/%d/smaps_rollup" % pid)
                found = False
                for line in (rollup or "").splitlines():
                    if line.startswith("Pss:"):
                        pss += int(line.split()[1]) * 1024
                        found = True
                        break
                pss_complete = found
        sample = {"processes": len(chosen), "rss_bytes": rss, "cpu_seconds": cpu}
        if has_private:
            sample["private_bytes"] = private
        if chosen and pss_complete:
            sample["pss_bytes"] = pss
        return sample

    # ------------------------------------------------------------ PowerShell

    def _sample_powershell(self):
        shell = shutil.which("powershell") or shutil.which("pwsh")
        if not shell:
            return {}
        selector = []
        if self.names:
            selector.append("Get-Process -Name %s -ErrorAction SilentlyContinue" % ",".join("'%s'" % n.replace("'", "") for n in self.names))
        if self.pids:
            selector.append("Get-Process -Id %s -ErrorAction SilentlyContinue" % ",".join(str(p) for p in self.pids))
        script = ("$ErrorActionPreference='SilentlyContinue'; @(%s) | ForEach-Object { [pscustomobject]@{ id=$_.Id; "
                  "ws=$_.WorkingSet64; priv=$_.PrivateMemorySize64; cpu=$_.TotalProcessorTime.TotalSeconds } } | "
                  "ConvertTo-Json -Compress" % "; ".join(selector))
        try:
            out = subprocess.run([shell, "-NoProfile", "-NonInteractive", "-Command", script], stdout=subprocess.PIPE,
                                 stderr=subprocess.DEVNULL, timeout=30).stdout.decode("utf-8", "replace").strip()
        except (OSError, subprocess.SubprocessError):
            return {}
        if not out:
            return {"processes": 0, "rss_bytes": 0, "private_bytes": 0, "cpu_seconds": 0.0}
        try:
            data = json.loads(out)
        except ValueError:
            return {}
        if isinstance(data, dict):
            data = [data]
        seen = {}
        for row in data:
            seen[row.get("id")] = row
        return {
            "processes": len(seen),
            "rss_bytes": sum(int(r.get("ws") or 0) for r in seen.values()),
            "private_bytes": sum(int(r.get("priv") or 0) for r in seen.values()),
            "cpu_seconds": sum(float(r.get("cpu") or 0) for r in seen.values()),
        }

    # ------------------------------------------------------------ the loop

    def sample(self):
        if not self.enabled:
            return None
        if self.method == "cgroup":
            data = self._sample_cgroup()
        elif self.method == "psutil":
            data = self._sample_psutil()
        elif self.method == "proc":
            data = self._sample_proc()
        else:
            data = self._sample_powershell()
        data["t"] = time.time()
        self.samples.append(data)
        return data

    def mark(self):
        """A sample now, and its time: the edge of a window. A run shorter
        than the interval still has a sample at each end."""
        moment = time.time()
        if self.enabled:
            try:
                sample = self.sample()
                moment = sample["t"]
            except Exception as error:
                self.problem = "sampling failed: %s" % error
        return moment

    def start(self):
        if not self.enabled or self._thread:
            return
        self._thread = threading.Thread(target=self._loop, name="sampler", daemon=True)
        self._thread.start()

    def _loop(self):
        while not self._stop.is_set():
            started = time.time()
            try:
                self.sample()
            except Exception as error:  # a sampler never ends a run
                self.problem = "sampling failed: %s" % error
            self._stop.wait(max(0.05, self.interval - (time.time() - started)))

    def stop(self):
        self._stop.set()
        if self._thread:
            self._thread.join(timeout=60)
            self._thread = None

    def window(self, start, end):
        """The samples taken between two times: the peak and the mean of each
        memory figure, and the CPU cores used (CPU time's change over the time
        between the first and last sample)."""
        inside = [s for s in self.samples if start <= s["t"] <= end]
        if not inside:
            return None
        result = {"samples": len(inside), "processes": inside[-1].get("processes")}
        for key in ("rss_bytes", "pss_bytes", "private_bytes", "cgroup_anon_bytes", "cgroup_current_bytes"):
            values = [s[key] for s in inside if key in s]
            if values:
                result[key + "_mean"] = int(sum(values) / len(values))
                result[key + "_peak"] = max(values)
        for key in ("cgroup_event_max", "cgroup_event_oom", "cgroup_event_oom_kill"):
            if key in inside[-1]:
                result[key] = inside[-1][key]
        timed = [s for s in inside if "cpu_seconds" in s]
        if len(timed) >= 2 and timed[-1]["t"] > timed[0]["t"]:
            result["cpu_cores_used"] = round((timed[-1]["cpu_seconds"] - timed[0]["cpu_seconds"]) /
                                             (timed[-1]["t"] - timed[0]["t"]), 3)
        return result


def memory_figure(window):
    """The one memory number a table shows for a window, and its name: PSS where
    it was read, a cgroup's anonymous memory, private bytes, or the RSS sum."""
    if not window:
        return None, None
    for key, name in (("pss_bytes_mean", "PSS"), ("cgroup_anon_bytes_mean", "cgroup anon"),
                      ("private_bytes_mean", "private bytes"), ("rss_bytes_mean", "RSS")):
        if key in window:
            return window[key], name
    return None, None


def add_arguments(parser):
    group = parser.add_argument_group("server sampling (optional; see sampler.py)")
    group.add_argument("--server-process", default="", help="process names to watch, comma-separated (hmailserver, or dovecot,imap,...)")
    group.add_argument("--server-pid", default="", help="process ids to watch, comma-separated")
    group.add_argument("--include-children", action="store_true", help="the named processes' descendants as well")
    group.add_argument("--server-cgroup", default="", help="cgroup v2 directories to read and add up, comma-separated (Linux)")
    group.add_argument("--server-container", default="", help="docker containers whose cgroups to read and add up, comma-separated (Linux)")
    group.add_argument("--sample-interval", type=float, default=1.0, help="seconds between samples (default 1)")


def from_arguments(args):
    names = [n for n in args.server_process.split(",") if n.strip()]
    pids = [p for p in args.server_pid.split(",") if p.strip()]
    return Sampler(names=names, pids=pids, include_children=args.include_children,
                   cgroups=[c for c in args.server_cgroup.split(",") if c.strip()],
                   containers=[c for c in args.server_container.split(",") if c.strip()],
                   interval=args.sample_interval)
