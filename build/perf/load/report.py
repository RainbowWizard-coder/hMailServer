# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

"""What every mode of the load generator writes: one JSON document and one
Markdown table, both carrying the machine the run was made on.

The JSON document is the record; the Markdown is made from it and is what goes
into hmailserver/docs/PublishedNumbers.md. Both go to standard output when no
file is named, the JSON first.
"""

import datetime
import io
import json
import math
import sys

FORMAT_VERSION = 1


def now_iso():
    return datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def percentile(ordered, fraction):
    """The value at that fraction of an ascending list, interpolated between
    the two nearest ranks (the method numpy calls linear). None for no values."""
    if not ordered:
        return None
    if len(ordered) == 1:
        return ordered[0]
    position = fraction * (len(ordered) - 1)
    below = int(math.floor(position))
    above = min(below + 1, len(ordered) - 1)
    weight = position - below
    return ordered[below] * (1.0 - weight) + ordered[above] * weight


def summary(values):
    """n, mean, min, p50, p90, p95, p99 and max of a list of milliseconds."""
    if not values:
        return {"n": 0}
    ordered = sorted(values)
    result = {
        "n": len(ordered),
        "mean": round(sum(ordered) / len(ordered), 2),
        "min": round(ordered[0], 2),
        "max": round(ordered[-1], 2),
    }
    for name, fraction in (("p50", 0.50), ("p90", 0.90), ("p95", 0.95), ("p99", 0.99)):
        result[name] = round(percentile(ordered, fraction), 2)
    return result


def rate(count, seconds):
    if not seconds or seconds <= 0:
        return None
    return round(count / seconds, 2)


def mib(value):
    if value is None:
        return None
    return round(value / 1048576.0, 1)


def cell(value, unit=""):
    if value is None or value == "":
        return "-"
    if isinstance(value, float):
        text = ("%.2f" % value).rstrip("0").rstrip(".")
    else:
        text = str(value)
    return text + unit


def machine_rows(machine, heading):
    """The machine table: what 'named hardware' means in this project."""
    if not machine:
        return []
    cpu = machine.get("cpu", {})
    disk = machine.get("disk", {})
    os_info = machine.get("os", {})
    memory = machine.get("memory_bytes")
    lines = [
        "",
        "**%s**" % heading,
        "",
        "| | |",
        "|---|---|",
        "| Host | %s |" % cell(machine.get("hostname")),
        "| System | %s |" % cell(machine.get("system_model")),
        "| Virtualisation | %s |" % cell(machine.get("virtualisation")),
        "| CPU | %s |" % cell(cpu.get("model")),
        "| Cores | %s physical, %s logical%s |" % (
            cell(cpu.get("physical_cores")), cell(cpu.get("logical_cores")),
            (", %s socket(s)" % cpu["sockets"]) if cpu.get("sockets") else ""),
        "| RAM | %s |" % (cell(round(memory / 1073741824.0, 1), " GiB") if memory else "-"),
        "| OS | %s |" % cell(" ".join(x for x in (os_info.get("pretty"), os_info.get("kernel")) if x)),
        "| Disk (%s) | %s |" % (cell(disk.get("path")), cell(", ".join(x for x in (
            disk.get("model"), disk.get("kind"), disk.get("bus"), disk.get("filesystem"),
            ("%s GiB" % round(disk["total_bytes"] / 1073741824.0)) if disk.get("total_bytes") else None) if x))),
        "| Python | %s |" % cell(machine.get("python")),
    ]
    return lines


def markdown(document, result_rows):
    """The whole Markdown: a heading, the run's facts, the results, the machine."""
    target = document.get("target", {})
    lines = [
        "### %s: %s" % (document["mode"], document.get("label") or target.get("server") or "unlabelled run"),
        "",
        "%s to %s, %s. Generator %s." % (
            document.get("started", "?"), document.get("finished", "?"),
            ", ".join("%s %s" % (k, v) for k, v in sorted(target.items()) if v not in (None, "")),
            document.get("tool", "")),
    ]
    if document.get("notes"):
        lines += ["", document["notes"]]
    lines += ["", "| Measure | Value |", "|---|---|"]
    for name, value in result_rows:
        lines.append("| %s | %s |" % (name, value))
    generator = document.get("generator") or {}
    if generator.get("warning"):
        lines += ["", "**Warning:** %s" % generator["warning"]]
    lines += machine_rows(document.get("machine"), "Machine the generator ran on")
    if document.get("server_machine"):
        lines += machine_rows(document.get("server_machine"), "Machine the server ran on")
    return "\n".join(lines) + "\n"


def write(document, result_rows, json_path, markdown_path):
    text_json = json.dumps(document, indent=2, sort_keys=False) + "\n"
    text_md = markdown(document, result_rows)
    if json_path:
        with io.open(json_path, "w", encoding="utf-8", newline="\n") as handle:
            handle.write(text_json)
    if markdown_path:
        with io.open(markdown_path, "w", encoding="utf-8", newline="\n") as handle:
            handle.write(text_md)
    if not json_path and not markdown_path:
        sys.stdout.write(text_json)
        sys.stdout.write("\n")
        sys.stdout.write(text_md)
    else:
        sys.stdout.write(text_md)
    sys.stdout.flush()


def generator_verdict(cpu_seconds_by_process, wall_seconds):
    """Whether the generator, rather than the server, was the limit. A worker
    process that was busy for more than 85 per cent of the run's wall time was
    close to one core's worth of Python and could not have gone faster; its
    number is a floor on the server's, not a measure of it."""
    fractions = [round(c / wall_seconds, 3) for c in cpu_seconds_by_process] if wall_seconds else []
    result = {
        "processes": len(cpu_seconds_by_process),
        "cpu_seconds": [round(c, 2) for c in cpu_seconds_by_process],
        "busy_fraction": fractions,
    }
    if fractions and max(fractions) > 0.85:
        result["warning"] = ("a generator process was busy %d%% of the run: the generator may have been the "
                             "limit. Run again with more --processes (or from a second machine) and compare."
                             % int(max(fractions) * 100))
    return result
