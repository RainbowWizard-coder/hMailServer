#!/usr/bin/env python3
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Turns the TRX file `dotnet test` writes into JUnit XML, for GitLab's test report.

The Linux suite (build/ci/linux-suite.sh) runs under `dotnet test`, whose built-in
loggers write TRX and not JUnit; a JUnit logger would be one more package in
LinuxRegressionTests' lock file for the sake of a file format. This reads the TRX
and writes the shape GitLab's artifacts:reports:junit reads: one <testsuite> per
fixture class, one <testcase> per result, <failure> with the message and the stack
for a failed test, <skipped> with the reason for one that did not run. Output is
kept for failures only, so a report of fifteen hundred tests stays small.

It also prints the one line the job is judged by and exits with it:

  SUMMARY Passed N, Skipped M, Failed F (of T)
  0  when F is 0 and at least one test passed
  1  when any test failed (Failed, Error, Timeout or Aborted)
  2  when nothing passed or the file is not a TRX file

Usage:  python3 build/ci/trx-to-junit.py <run.trx> <junit.xml>
"""

import re
import sys
import xml.etree.ElementTree as ET

NS = {"t": "http://microsoft.com/schemas/VisualStudio/TeamTest/2010"}

FAILED = {"Failed", "Error", "Timeout", "Aborted"}
PASSED = {"Passed", "PassedButRunAborted", "Warning", "Completed"}

# Characters XML 1.0 does not allow, which test output does contain (a raw SMTP
# transcript can carry NUL); ElementTree would write them and GitLab would refuse
# the whole report.
INVALID_XML = re.compile("[\x00-\x08\x0b\x0c\x0e-\x1f\ufffe\uffff]")
LIMIT = 64 * 1024


def clean(text, limit=LIMIT):
    text = INVALID_XML.sub("?", text or "")
    if len(text) > limit:
        text = text[:limit] + "\n[... cut at %d characters]" % limit
    return text


def seconds(duration):
    """hh:mm:ss.fffffff, as TRX writes it."""
    match = re.match(r"^(\d+):(\d+):(\d+(?:\.\d+)?)$", duration or "")
    if not match:
        return 0.0
    hours, minutes, secs = match.groups()
    return int(hours) * 3600 + int(minutes) * 60 + float(secs)


def text_of(element, path):
    found = element.find(path, NS)
    return found.text if found is not None and found.text else ""


def main(argv):
    if len(argv) != 3:
        print(__doc__.strip().splitlines()[-1])
        return 2
    try:
        root = ET.parse(argv[1]).getroot()
    except (OSError, ET.ParseError) as e:
        print("SUMMARY unreadable TRX %s: %s" % (argv[1], e))
        return 2
    if not root.tag.endswith("TestRun"):
        print("SUMMARY %s is not a TRX file (its root is %s)" % (argv[1], root.tag))
        return 2

    # testId -> (class, method) from the definitions; a result names its test by id.
    classes = {}
    for unit in root.iterfind("t:TestDefinitions/t:UnitTest", NS):
        method = unit.find("t:TestMethod", NS)
        class_name = method.get("className", "") if method is not None else ""
        classes[unit.get("id")] = class_name

    suites = {}
    passed = skipped = failed = 0
    for result in root.iterfind("t:Results/t:UnitTestResult", NS):
        outcome = result.get("outcome", "")
        class_name = classes.get(result.get("testId"), "") or "RegressionTests"
        # dotnet test writes the class's full name, possibly with an assembly after a comma.
        class_name = class_name.split(",")[0].strip()
        name = result.get("testName", "?")
        # NUnit's adapter names a test by its full name; the class is the suite already.
        if name.startswith(class_name + "."):
            name = name[len(class_name) + 1:]

        case = ET.Element("testcase", {
            "classname": class_name,
            "name": name,
            "time": "%.3f" % seconds(result.get("duration")),
        })
        message = text_of(result, "t:Output/t:ErrorInfo/t:Message")
        stack = text_of(result, "t:Output/t:ErrorInfo/t:StackTrace")
        stdout = text_of(result, "t:Output/t:StdOut")

        if outcome in PASSED:
            passed += 1
        elif outcome in FAILED:
            failed += 1
            failure = ET.SubElement(case, "failure", {
                "message": clean(message.strip().splitlines()[0] if message.strip() else outcome, 1000),
                "type": outcome,
            })
            failure.text = clean(message + ("\n" + stack if stack else ""))
            if stdout:
                ET.SubElement(case, "system-out").text = clean(stdout)
        else:
            # NotExecuted (a skip, with the shim's reason), Inconclusive, Pending...
            skipped += 1
            reason = message.strip() or stdout.strip() or outcome
            ET.SubElement(case, "skipped", {"message": clean(reason.splitlines()[0] if reason else outcome, 1000)})

        suites.setdefault(class_name, []).append((case, outcome))

    testsuites = ET.Element("testsuites", {"name": "LinuxRegressionTests"})
    for class_name in sorted(suites):
        cases = suites[class_name]
        suite = ET.SubElement(testsuites, "testsuite", {
            "name": class_name,
            "tests": str(len(cases)),
            "failures": str(sum(1 for _, o in cases if o in FAILED)),
            "errors": "0",
            "skipped": str(sum(1 for _, o in cases if o not in FAILED and o not in PASSED)),
            "time": "%.3f" % sum(float(c.get("time")) for c, _ in cases),
        })
        for case, _ in cases:
            suite.append(case)

    total = passed + skipped + failed
    testsuites.set("tests", str(total))
    testsuites.set("failures", str(failed))
    testsuites.set("skipped", str(skipped))
    ET.ElementTree(testsuites).write(argv[2], encoding="utf-8", xml_declaration=True)

    print("SUMMARY Passed %d, Skipped %d, Failed %d (of %d)" % (passed, skipped, failed, total))
    if failed:
        return 1
    if passed == 0:
        print("No test passed: a run of nothing is not a pass.")
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
