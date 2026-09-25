# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

"""The machine a run was made on, collected rather than typed: CPU model and
cores, RAM, operating system, the disk under a path, and whether it is a
virtual machine. A number without these is not a published number.

Linux reads /proc and /sys; Windows asks PowerShell's CIM classes once; macOS
asks sysctl. Anything that cannot be read is left out, never guessed.
"""

import json
import os
import platform
import re
import shutil
import socket
import subprocess
import sys


def _read(path):
    try:
        with open(path, "rb") as handle:
            return handle.read().decode("utf-8", "replace")
    except OSError:
        return None


def _run(command, timeout=60):
    """A command's standard output as text, or None. Bytes decoded as UTF-8 by
    hand: text mode would take the console's code page on Windows."""
    try:
        completed = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, timeout=timeout)
    except (OSError, subprocess.SubprocessError):
        return None
    if completed.returncode != 0:
        return None
    return completed.stdout.decode("utf-8", "replace")


# ---------------------------------------------------------------- Linux

def _linux_disk(path):
    mounts = _read("/proc/self/mounts") or ""
    best = None
    real = os.path.realpath(path)
    for line in mounts.splitlines():
        fields = line.split()
        if len(fields) < 3:
            continue
        device, mountpoint, fstype = fields[0], fields[1].replace("\\040", " "), fields[2]
        if real == mountpoint or real.startswith(mountpoint.rstrip("/") + "/") or mountpoint == "/":
            if best is None or len(mountpoint) > len(best[1]):
                best = (device, mountpoint, fstype)
    disk = {"path": path}
    if not best:
        return disk
    device, mountpoint, fstype = best
    disk["filesystem"] = fstype
    disk["mountpoint"] = mountpoint
    disk["device"] = device
    name = os.path.basename(os.path.realpath(device)) if device.startswith("/dev/") else None
    # A partition's disk is its parent in /sys/class/block; a device-mapper
    # volume's is what it sits on (the first of its slaves).
    seen = 0
    while name and seen < 4:
        seen += 1
        base = "/sys/class/block/" + name
        if os.path.exists(base + "/partition"):
            name = os.path.basename(os.path.dirname(os.path.realpath(base)))
            continue
        slaves = base + "/slaves"
        if os.path.isdir(slaves) and os.listdir(slaves):
            name = sorted(os.listdir(slaves))[0]
            continue
        break
    if name and os.path.isdir("/sys/block/" + name):
        disk["block_device"] = name
        model = (_read("/sys/block/%s/device/model" % name) or "").strip()
        vendor = (_read("/sys/block/%s/device/vendor" % name) or "").strip()
        if model:
            disk["model"] = (vendor + " " + model).strip() if vendor and vendor not in model else model
        rotational = (_read("/sys/block/%s/queue/rotational" % name) or "").strip()
        if rotational == "0":
            disk["kind"] = "NVMe SSD" if name.startswith("nvme") else "SSD (non-rotational)"
        elif rotational == "1":
            disk["kind"] = "rotational"
    try:
        disk["total_bytes"] = shutil.disk_usage(path).total
    except OSError:
        pass
    return disk


def _linux(path):
    info = {}
    cpuinfo = _read("/proc/cpuinfo") or ""
    model = None
    cores = set()
    sockets = set()
    physical = None
    core = None
    hypervisor = False
    for line in cpuinfo.splitlines() + [""]:
        if not line.strip():
            if physical is not None and core is not None:
                cores.add((physical, core))
            physical, core = None, None
            continue
        key, _, value = line.partition(":")
        key, value = key.strip(), value.strip()
        if key == "model name" and not model:
            model = value
        elif key == "physical id":
            physical = value
            sockets.add(value)
        elif key == "core id":
            core = value
        elif key == "flags" and " hypervisor" in " " + value:
            hypervisor = True
    info["cpu"] = {"model": model, "logical_cores": os.cpu_count()}
    if cores:
        info["cpu"]["physical_cores"] = len(cores)
    if sockets:
        info["cpu"]["sockets"] = len(sockets)
    meminfo = _read("/proc/meminfo") or ""
    match = re.search(r"^MemTotal:\s+(\d+) kB", meminfo, re.M)
    if match:
        info["memory_bytes"] = int(match.group(1)) * 1024
    release = _read("/etc/os-release") or ""
    match = re.search(r'^PRETTY_NAME="?([^"\n]*)"?', release, re.M)
    info["os"] = {"pretty": match.group(1) if match else "Linux", "kernel": platform.release()}
    vendor = (_read("/sys/class/dmi/id/sys_vendor") or "").strip()
    product = (_read("/sys/class/dmi/id/product_name") or "").strip()
    if vendor or product:
        info["system_model"] = (vendor + " " + product).strip()
    virt = None
    if shutil.which("systemd-detect-virt"):
        virt = (_run(["systemd-detect-virt"]) or "").strip() or None
    if not virt:
        virt = "a hypervisor (cpuinfo)" if hypervisor else None
    if virt == "none":
        virt = "none (bare metal)"
    info["virtualisation"] = virt
    info["disk"] = _linux_disk(path)
    return info


# ---------------------------------------------------------------- Windows

_WINDOWS_SCRIPT = r"""
$ErrorActionPreference = 'SilentlyContinue'
$p = @(Get-CimInstance Win32_Processor)
$cs = Get-CimInstance Win32_ComputerSystem
$os = Get-CimInstance Win32_OperatingSystem
$letter = '__DRIVE__'
$part = Get-Partition -DriveLetter $letter
$disk = $null; $pd = $null
if ($part) { $disk = Get-Disk -Number $part.DiskNumber }
if ($disk) { $pd = Get-PhysicalDisk | Where-Object { $_.DeviceId -eq [string]$disk.Number } | Select-Object -First 1 }
$vol = Get-Volume -DriveLetter $letter
[pscustomobject]@{
  cpu_model = if ($p.Count) { $p[0].Name.Trim() } else { $null }
  sockets = $p.Count
  cores = ($p | Measure-Object -Property NumberOfCores -Sum).Sum
  logical = ($p | Measure-Object -Property NumberOfLogicalProcessors -Sum).Sum
  memory = $cs.TotalPhysicalMemory
  model = ('' + $cs.Manufacturer + ' ' + $cs.Model).Trim()
  hypervisor = $cs.HypervisorPresent
  os = $os.Caption
  os_version = $os.Version
  disk_model = if ($disk) { $disk.FriendlyName } else { $null }
  disk_bus = if ($disk) { [string]$disk.BusType } else { $null }
  disk_media = if ($pd) { [string]$pd.MediaType } else { $null }
  filesystem = if ($vol) { $vol.FileSystem } else { $null }
  volume_size = if ($vol) { $vol.Size } else { $null }
} | ConvertTo-Json -Compress
"""


def _windows(path):
    drive = os.path.splitdrive(os.path.abspath(path))[0].rstrip(":") or "C"
    script = _WINDOWS_SCRIPT.replace("__DRIVE__", drive[:1])
    shell = shutil.which("powershell") or shutil.which("pwsh")
    data = {}
    if shell:
        text = _run([shell, "-NoProfile", "-NonInteractive", "-Command", script], timeout=120)
        if text:
            try:
                data = json.loads(text.strip().splitlines()[-1])
            except (ValueError, IndexError):
                data = {}
    info = {
        "cpu": {"model": data.get("cpu_model") or platform.processor(), "logical_cores": data.get("logical") or os.cpu_count()},
        "memory_bytes": data.get("memory"),
        "os": {"pretty": data.get("os") or platform.platform(), "kernel": data.get("os_version") or platform.version()},
        "system_model": data.get("model"),
    }
    if data.get("cores"):
        info["cpu"]["physical_cores"] = data["cores"]
    if data.get("sockets"):
        info["cpu"]["sockets"] = data["sockets"]
    model = (data.get("model") or "").lower()
    if any(word in model for word in ("vmware", "virtual", "kvm", "qemu", "xen", "hyper-v")):
        info["virtualisation"] = data.get("model")
    elif data.get("hypervisor"):
        info["virtualisation"] = "a hypervisor is present (Hyper-V/VBS may be on; the machine itself: %s)" % data.get("model")
    else:
        info["virtualisation"] = "none reported"
    disk = {"path": path}
    for key, name in (("disk_model", "model"), ("disk_media", "kind"), ("disk_bus", "bus"),
                      ("filesystem", "filesystem"), ("volume_size", "total_bytes")):
        if data.get(key) not in (None, "", "Unspecified"):
            disk[name] = data[key]
    info["disk"] = disk
    return info


# ---------------------------------------------------------------- macOS

def _mac(path):
    def sysctl(name):
        return (_run(["sysctl", "-n", name]) or "").strip() or None
    memory = sysctl("hw.memsize")
    info = {
        "cpu": {"model": sysctl("machdep.cpu.brand_string"), "logical_cores": os.cpu_count()},
        "memory_bytes": int(memory) if memory and memory.isdigit() else None,
        "os": {"pretty": "macOS " + platform.mac_ver()[0], "kernel": platform.release()},
        "system_model": sysctl("hw.model"),
        "virtualisation": None,
    }
    physical = sysctl("hw.physicalcpu")
    if physical and physical.isdigit():
        info["cpu"]["physical_cores"] = int(physical)
    disk = {"path": path}
    try:
        disk["total_bytes"] = shutil.disk_usage(path).total
    except OSError:
        pass
    info["disk"] = disk
    return info


def collect(path=None):
    """The machine this process runs on. `path` names the disk to describe:
    where the server keeps its data when the generator runs beside it."""
    path = path or os.getcwd()
    if sys.platform.startswith("linux"):
        info = _linux(path)
    elif sys.platform == "win32":
        info = _windows(path)
    elif sys.platform == "darwin":
        info = _mac(path)
    else:
        info = {"cpu": {"logical_cores": os.cpu_count()}, "os": {"pretty": platform.platform()}}
    info["hostname"] = socket.gethostname()
    info["python"] = "%s %s" % (platform.python_implementation(), platform.python_version())
    return info


def load(path):
    """A machine document written by `loadgen.py machine --json` on the
    server's machine, when the generator ran on another."""
    with open(path, "rb") as handle:
        data = json.loads(handle.read().decode("utf-8"))
    return data.get("machine", data)
