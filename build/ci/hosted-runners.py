#!/usr/bin/env python3
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

"""GitLab's hosted runners beside the project's own: the switches that send a class of
jobs there, what a job there is given, and the namespace's compute minutes.

.gitlab-ci.yml's variables: hold one switch for each class of the jobs on the project's
own runners that need nothing of the project's, each a runner tag, linix there, which a
protected project CI/CD variable of the same name sets to one of GitLab's hosted Linux
machines (hmailserver/docs/ContinuousIntegration.md, "GitLab's hosted runners beside
the project's"):

  HM_RUNNER_BUILD     amd64  the builds and checks in the CI image or a distribution's
  HM_RUNNER_PACKAGES  amd64  the package builds and their install jobs, in the lane
                             HM_LANE_PACKAGES (linix-package-build)
  HM_RUNNER_ARM64     arm64  the jobs in an arm64 container
  HM_RUNNER_SUITE     amd64  linux-suite and alpine-suite, in the lane HM_LANE_SUITE
                             (linix-suite)

A lane is the resource group that keeps a class's jobs one at a time on the project's
machines. On GitLab's runners every job has a machine of its own, so a class there is
given a lane of its own per job: the lane's variable set to hosted-$CI_JOB_NAME, which
GitLab expands for each job as it makes the job's resource group.

  python3 build/ci/hosted-runners.py check
      The hosted-runners job (.gitlab/ci/hosted-runners.yml), on linix, in a pipeline
      for which a switch or a lane is not its default. Each switch must name linix or
      one of GitLab's Linux machines of its class's architecture: any other value is a
      tag no runner carries, and the class's jobs would wait for one until GitLab
      failed them. No credential may be in this job's environment: it names no
      environment, as no job a switch moves does, so every variable it is given is
      given to them too - a credential of the project's is scoped to the environment of
      the one job that uses it (CREDENTIALS), and then no other job sees it. The test
      fails closed: it takes for a credential every variable it is given but GitLab's
      own - GITLAB_OWN, and those whose names begin with CI_, written in capitals,
      digits and _ alone as each of GitLab's is (GITLAB_CI_NAME), but one whose name
      says it holds a credential (CREDENTIAL_WORDS) and is none of GitLab's own of such
      names (GITLAB_JOB_CREDENTIALS) -, its runner's (its feature flags, FF_ and a
      flag's name, true or false, and RUNNER_OWN), the container's (PROCESS), those that
      the CI files and the GitLab templates they include set for every job (ci_globals,
      TEMPLATE_GLOBALS), and the project's that hold none (PLAIN); and it accepts by its
      name alone, and says so, the one credential that must reach every job and can
      only write telemetry, the group's Observability ingest token (TELEMETRY_ONLY),
      which scoped to one job's environment would stop GitLab's export. GitLab keeps no
      prefix for its own, so a variable of the project's named CI_, in capitals, that
      says nothing of a credential is taken for GitLab's by its name alone; one with a
      small letter in its name is not; the deploy token GitLab gives every job, while
      the project has one, fails it (GITLAB_CREDENTIALS). Each problem is named, with
      what to do; a value is never printed. A lane that does not go with its class is
      a warning: the jobs run, one at a time on GitLab's machines, or side by side on
      linix.
  python3 build/ci/hosted-runners.py quota
      The compute-quota job, on linix, in the same pipelines, in the environment
      compute-quota, to which HM_QUOTA_TOKEN is scoped: a read_api token of an Owner of
      the group, which alone may read its compute usage. The namespace's minutes this
      month, against its quota (shared_runners_minutes_limit and any purchased), and
      what a pipeline of this kind made at the switches as the job reads them is
      expected to use (EXPECTED). Those are the switches when the job starts, which a
      pipeline made then takes: GitLab gives a job the project's variables as it
      starts, and fixes a job's tags as it makes the pipeline, which a job run again
      keeps, so this pipeline's jobs are where its switches were when it was made.
      While a switch sends a class to GitLab's runners, fewer left than two such
      pipelines, or a twentieth of the quota, is a failure, saying how the switches are
      set back; fewer than ten, or a quarter, is a warning; and so is a missing token,
      said so. With none sent there - the switches set back and the job run again, or a
      lane alone set -, a pipeline made from then on spends no minute, and the usage is
      said, and that this pipeline's jobs keep the runners they were made with. A
      GitLab that cannot be asked is exit 2, which the job tries twice more. The job is
      allowed to fail whatever it answers: a failure of it would save no minute, since
      no job waits for it, and would hold a gate whose jobs had passed until a pipeline
      was made again.
  python3 build/ci/hosted-runners.py --self-test
      Both against environments and answers held in memory, the CI files' variables
      read from the tree's own, and the HTTP reading against a GitLab API served on the
      loopback; and the classes against the tree's .gitlab-ci.yml, whose variables:
      must hold each switch and lane at its default.

Exit codes: 0 nothing to say, 1 a failure (named above it), 2 GitLab could not be asked,
3 a warning.
"""

import os
import sys

# Nothing is imported by name from this script's own directory, which Python puts first
# on sys.path (build/check-release-chain.py, PATH_GUARD).
sys.path[:] = [p for p in sys.path if os.path.normcase(os.path.realpath(p or os.curdir)) !=
               os.path.normcase(os.path.dirname(os.path.realpath(__file__)))]

import http.server
import io
import json
import re
import threading
import urllib.error
import urllib.parse
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
OWN = "linix"
# GitLab's hosted Linux machines, by architecture (docs.gitlab.com, "Hosted runners on
# Linux"; large and above, and medium and large for arm64, need Premium or Ultimate,
# which the group's open-source plan carries).
HOSTED = {
    "amd64": ("saas-linux-small-amd64", "saas-linux-medium-amd64", "saas-linux-large-amd64", "saas-linux-xlarge-amd64",
              "saas-linux-2xlarge-amd64"),
    "arm64": ("saas-linux-small-arm64", "saas-linux-medium-arm64", "saas-linux-large-arm64"),
}
# Each class: its switch, the architecture of its jobs' containers, and its lane's
# variable and default (None for a class without a lane).
CLASSES = (
    ("HM_RUNNER_BUILD", "amd64", None, None),
    ("HM_RUNNER_PACKAGES", "amd64", "HM_LANE_PACKAGES", "linix-package-build"),
    ("HM_RUNNER_ARM64", "arm64", None, None),
    ("HM_RUNNER_SUITE", "amd64", "HM_LANE_SUITE", "linix-suite"),
)
# What a hosted lane is set to: a lane of its own for each job, which GitLab expands
# from CI_JOB_NAME as it makes the job (the resource group's key takes nested variables).
HOSTED_LANE = "hosted-$CI_JOB_NAME"
# The compute minutes a pipeline is expected to use on GitLab's runners for each class
# there, by its kind: the minutes each class's jobs ran on the project's runners in the
# pipelines of 5 to 13 October 2026 (median per job, linix's for an eight-CPU machine,
# the arm64 jobs' at four tenths of their time under qemu-user), with a minute more each
# for its machine and image, at the 0.5 a job-minute GitLab charges a public project of
# its open-source programme on any of its Linux machines (the lower of the runner's cost
# factor and 0.5: GitLab's ee/lib/gitlab/ci/minutes/cost_factor.rb). On master the
# package builds wait for a press, and their arm64 jobs with them; a tag runs one suite.
EXPECTED = {
    "HM_RUNNER_BUILD": {"batch": 95, "master": 90, "tag": 75},
    "HM_RUNNER_PACKAGES": {"batch": 280, "master": 120, "tag": 140},
    "HM_RUNNER_ARM64": {"batch": 15, "master": 0, "tag": 15},
    "HM_RUNNER_SUITE": {"batch": 145, "master": 145, "tag": 75},
}
# The project's credentials, each by the name its job reads it from, with the
# environment that job names - the one each is to be scoped to, so that no other job is
# given it (None: its job names no environment yet). The CI files and their scripts
# read no other secret from a variable of the project's.
CREDENTIALS = {
    "RENOVATE_TOKEN": "renovate",
    "SNAPCRAFT_STORE_CREDENTIALS": "snap-store",
    "GITLAB_AUTH_TOKEN": "scorecard",
    "HM_EXPORT_TOKEN": "forge-export",
    "HM_EXPORT_FULL_TOKEN": "forge-export",
    "HM_LAB_DB_PASSWORD": "test-lab",
    "HM_QUOTA_TOKEN": "compute-quota",
    "PSGALLERY_API_KEY": "psgallery",
    "CHOCO_API_KEY": "chocolatey",
    "AUR_SSH_KEY": "aur",
    "LAUNCHPAD_SIGNING_KEY": "launchpad",
    "LAUNCHPAD_SIGNING_PASSPHRASE": "launchpad",
    "COPR_WEBHOOK_URL": "copr",
    "OBS_RUNSERVICE_TOKEN": "obs",
    "DOCKERHUB_TOKEN": "container-registries",
    "QUAY_ROBOT_TOKEN": "container-registries",
    "AWS_ACCESS_KEY_ID": "container-registries",
    "AWS_SECRET_ACCESS_KEY": "container-registries",
    "AWS_SESSION_TOKEN": "container-registries",
    "HM_REPO_SIGNING_KEY": None,
    "HM_REPO_SIGNING_PASSPHRASE": None,
}
# Every other variable a job that names no environment is given is taken for a
# credential, but for these, which hold none: what GitLab gives every job of its own -
# its predefined variables, whose names begin with CI_ (gitlab_own), and those of
# GITLAB_OWN, its predefined variables of other names (docs.gitlab.com, "Predefined CI/CD
# variables", on 14 October 2026); KUBECONFIG, which GitLab gives every job once an agent
# for Kubernetes lets the project in, holds that agent's token, and is not among them.
GITLAB_OWN = {"CI", "GITLAB_CI", "GITLAB_FEATURES", "GITLAB_USER_EMAIL", "GITLAB_USER_ID", "GITLAB_USER_LOGIN", "GITLAB_USER_NAME",
              "CHAT_CHANNEL", "CHAT_INPUT", "CHAT_USER_ID", "TRIGGER_PAYLOAD"}
# GitLab keeps no prefix of a variable's name for its own (app/models/concerns/ci/
# has_variable.rb takes any name of letters, digits and _), so a variable of the
# project's or the group's may begin with CI_ as well: a name that does is taken for
# GitLab's only when it is written as each of GitLab's own is, in capitals, digits and _
# alone (GITLAB_CI_NAME: so are all 166 names of GitLab's predefined-variables page, of
# its variable builders and of the runner, 14 October 2026), since CREDENTIAL_WORDS reads
# capitals, and CI_push_token or CI_Deploy_Key would say nothing to it; and only when it
# says nothing of a credential (CREDENTIAL_WORDS), or is one of GitLab's own of such
# names, which are the job's or the runner's, not the project's (GITLAB_JOB_CREDENTIALS)
# - the job's own token, as CI_JOB_TOKEN, CI_REGISTRY_PASSWORD and
# CI_DEPENDENCY_PROXY_PASSWORD, valid while the job runs; CI_TRIGGER_SHORT_TOKEN, the
# first four characters of a pipeline trigger's token, and CI_RUNNER_SHORT_TOKEN, the
# runner's token as the runner shortens it so as not to expose it; and the file of the
# runner's own TLS client key, where its configuration names one (GitLab's
# app/models/ci/build.rb, persisted_variables and dependency_proxy_variables, and
# lib/gitlab/ci/variables/builder.rb; gitlab-runner 19.4's RunnerConfig.GetVariables and
# Build.GetCITLSVariables). A CI_ name in capitals that says nothing of a credential is
# taken for GitLab's by that name alone.
GITLAB_CI_NAME = re.compile(r"CI_[A-Z0-9_]+")
CREDENTIAL_WORDS = re.compile(r"TOKEN|SECRET|PASSWORD|PASSWD|PASSPHRASE|CREDENTIAL|PRIVATE|APIKEY|API_KEY|ACCESS_KEY|"
                              r"WEBHOOK|JWT|COOKIE|(?:^|_)(?:KEYS?|PASS|AUTH|PFX|P12|PEM)(?:_|$)")
GITLAB_JOB_CREDENTIALS = {"CI_JOB_TOKEN", "CI_REGISTRY_PASSWORD", "CI_DEPENDENCY_PROXY_PASSWORD", "CI_TRIGGER_SHORT_TOKEN",
                          "CI_RUNNER_SHORT_TOKEN", "CI_SERVER_TLS_KEY_FILE"}
# The credentials GitLab gives every job under a name that begins with CI_ that are the
# project's: those of its deploy token named gitlab-deploy-token, while it has one (it
# has none).
GITLAB_CREDENTIALS = {"CI_DEPLOY_PASSWORD": "the project's deploy token named gitlab-deploy-token"}
# The runner's feature flags: it gives every job each of them, FF_ and the flag's name,
# true or false, and puts a job's variable of a flag's name in the flag's place
# (gitlab-runner 19.4's Build.getResolvedFeatureFlags, strconv.FormatBool, and
# getNonFeatureFlagJobVariables); it reads a flag's value with strconv.ParseBool, whose
# forms these are, an empty one off. An FF_ variable of another value is none of the
# runner's flags but the project's or the group's, and is taken as any other variable is.
FEATURE_FLAG_VALUES = {"", "1", "t", "T", "TRUE", "true", "True", "0", "f", "F", "FALSE", "false", "False"}
# What the runner gives a job's script beside GitLab's variables (gitlab-runner 19.4's
# Build.GetAllVariables and its shell's exports): the file a job's script may set a
# variable in, the job's temporary directory, git's settings, and the names of the job's
# variables where the runner's own steps run the script.
RUNNER_OWN = {"GITLAB_ENV", "RUNNER_TEMP_PROJECT_DIR", "RUNNER_JOB_VAR_NAMES", "GIT_CONFIG_GLOBAL", "GIT_TERMINAL_PROMPT",
              "GCM_INTERACTIVE", "GIT_LFS_SKIP_SMUDGE"}
# What a container's process is given that is not a variable of GitLab's or the CI files':
# Docker's, the shell's, and Docker Hub's Python image's - PATH, PYTHON_VERSION and
# PYTHON_SHA256 in the image the job runs in, and what its earlier images set (GPG_KEY is
# the id of the key a Python release is signed with, a public one).
PROCESS = {"PATH", "HOME", "HOSTNAME", "LANG", "LC_ALL", "TERM", "SHLVL", "PWD", "OLDPWD", "_", "GPG_KEY",
           "PYTHON_VERSION", "PYTHON_SHA256", "PYTHON_PIP_VERSION", "PYTHON_SETUPTOOLS_VERSION", "PYTHON_GET_PIP_URL",
           "PYTHON_GET_PIP_SHA256"}
# The variables: of the GitLab templates the CI files include (include: - template:),
# which GitLab gives every job as it gives the CI files' own: their names as GitLab's
# templates had them on 14 October 2026 (lib/gitlab/ci/templates/Jobs/ in gitlab-org/
# gitlab; the variables: of the pipeline's merged configuration, CI lint's merged_yaml,
# list them beside the CI files' own). build/check-release-chain.py holds the templates
# named here to those the CI files include. A variable GitLab adds to a template is taken
# for a credential until it is named here.
TEMPLATE_GLOBALS = {
    "Jobs/SAST.gitlab-ci.yml": (
        "SECURE_ANALYZERS_PREFIX", "SAST_IMAGE_SUFFIX", "SAST_EXCLUDED_ANALYZERS", "DEFAULT_SAST_EXCLUDED_PATHS",
        "SAST_EXCLUDED_PATHS", "SCAN_KUBERNETES_MANIFESTS", "GITLAB_ADVANCED_SAST_EXT_INCREMENTAL_ENABLED"),
    "Jobs/Secret-Detection.gitlab-ci.yml": (
        "SECURE_ANALYZERS_PREFIX", "SECRET_DETECTION_IMAGE_SUFFIX", "SECRETS_ANALYZER_VERSION",
        "SECRET_DETECTION_EXCLUDED_PATHS"),
}
# The project's and the group's variables that hold no secret (13 October 2026): the
# Artifact Signing account, endpoint and profile, and the Entra application and tenant
# with which release-authenticode's ID token is exchanged - the identifiers of a federated
# credential, which holds none -, the container image's switch, and GitLab's
# observability export. The switches and the lanes are the CI files' own.
PLAIN = {"ARTIFACT_SIGNING_ACCOUNT", "ARTIFACT_SIGNING_ENDPOINT", "ARTIFACT_SIGNING_PROFILE", "AZURE_CLIENT_ID",
         "AZURE_TENANT_ID", "HM_PUBLISH_IMAGE", "GITLAB_OBSERVABILITY_EXPORT"}
# The credentials a job on GitLab's runners may be given, each by its whole name with
# why: those that can only write telemetry and must reach every job for their reader to
# see them. GITLAB_OBSERVABILITY_TOKEN is the ingest token with which GitLab's
# Observability export sends each pipeline's traces to the group's instance once its
# intake is locked: a protected, masked and hidden group variable of scope *, since the
# export reads it with no environment (scoped_variables with environment nil), which
# keeps only the variables of scope * - scoped to one job's environment, as the advice
# for any other credential goes, it would stop the export -, and the key of an account
# of that instance with no role, which writes traces, metrics and logs and reads
# nothing: what anyone could write while the intake was open. So every job of a
# protected pipeline is given it, those a switch sends to GitLab's runners among them,
# which the owner accepted on 15 October 2026 in having this check learn it, where it
# took the token for any credential (docs/ContinuousIntegration.md, "What a job on
# GitLab's runners is given"). A name is known here whole or not at all: any other
# variable - GITLAB_OBSERVABILITY_TOKEN_2, another *_TOKEN - is taken for a credential
# as before, until it is scoped to its job's environment or named here with why.
TELEMETRY_ONLY = {"GITLAB_OBSERVABILITY_TOKEN": "the group's ingest token for GitLab Observability, of scope * because "
                                                "GitLab's export reads it with no environment, which writes telemetry into "
                                                "the group's instance and reads nothing"}
QUOTA_FAIL_SHARE, QUOTA_WARN_SHARE = 0.95, 0.75
QUOTA_FAIL_PIPELINES, QUOTA_WARN_PIPELINES = 2, 10
DOCUMENT = "docs/ContinuousIntegration.md, \"GitLab's hosted runners beside the project's\""


class Unreachable(Exception):
    """GitLab could not be asked: worth asking again."""


class Refused(Exception):
    """GitLab answered, and not with what was asked for: asking again will not help."""


def switches(environ):
    """[(variable, architecture, value, hosted, lane, lane value, lane default)] for each class."""
    found = []
    for variable, arch, lane, lane_default in CLASSES:
        value = environ.get(variable)
        lane_value = environ.get(lane) if lane else None
        found.append((variable, arch, value, value in HOSTED[arch], lane, lane_value, lane_default))
    return found


def setting_back(environ):
    """What sets every switch back: the variables of the project's that are set, or all of
    them when none is."""
    every = [n for c in CLASSES for n in (c[0], c[2]) if n]
    named = sorted(set(n for c in CLASSES for n in (c[0], c[2]) if n and environ.get(n) not in (None, OWN, c[3]))) or every
    return ("To put every job back on the project's runners, delete the project CI/CD variable%s %s (Settings, CI/CD, "
            "Variables; or glab api -X DELETE projects/$CI_PROJECT_ID/variables/<name>) and make a new pipeline (CI/CD, "
            "Pipelines, Run pipeline, or a push): each class of it is then on the project's runners as before, while a "
            "pipeline already made keeps the runners its jobs were made with, a job of it run again among them (%s)." % (
                "s" if len(named) > 1 else "", ", ".join(named), DOCUMENT))


def defined_variables(text):
    """The names a CI file's top-level variables: define."""
    names, inside = set(), False
    for line in text.replace("\r\n", "\n").split("\n"):
        if re.match(r"^[A-Za-z0-9_.\-]+:", line):
            inside = line.startswith("variables:")
            continue
        m = re.match(r"""^  ["']?([A-Za-z_][A-Za-z0-9_]*)["']?[ \t]*:""", line)
        if inside and m:
            names.add(m.group(1))
    return names


def included(text):
    """[(kind, what)] for each entry of a CI file's top-level include: - local, template,
    or another kind, whose file this cannot read."""
    found, inside = [], False
    for line in text.replace("\r\n", "\n").split("\n"):
        if re.match(r"^[A-Za-z0-9_.\-]+:", line):
            inside = line.startswith("include:")
            continue
        m = re.match(r"""^\s*-\s*([a-z_]+):\s*["']?([^"'\s#]+)""", line)
        if inside and m:
            found.append((m.group(1), m.group(2)))
    return found


def ci_globals(root=ROOT):
    """(the names of the variables the CI files set for every job, [each GitLab template
    they include that TEMPLATE_GLOBALS does not name]): the top-level variables: of
    .gitlab-ci.yml and of every file it includes, at any remove, and those of the GitLab
    templates they include (TEMPLATE_GLOBALS) - which GitLab gives every job, this one
    among them, and which a commit, not a person's setting, makes."""
    names, unknown, seen, pending = set(), [], set(), [".gitlab-ci.yml"]
    while pending:
        path = pending.pop(0)
        if path in seen:
            continue
        seen.add(path)
        try:
            with io.open(os.path.join(root, *path.split("/")), encoding="utf-8") as f:
                text = f.read()
        except (IOError, OSError):
            continue
        names |= defined_variables(text)
        for kind, what in included(text):
            if kind == "local":
                pending.append(what.lstrip("/"))
            elif kind == "template" and what in TEMPLATE_GLOBALS:
                names |= set(TEMPLATE_GLOBALS[what])
            else:
                unknown.append("%s %s" % (kind, what))
    return names, unknown


def gitlab_own(name, value):
    """Whether a variable is GitLab's own or its runner's: by its name, one that begins
    with CI_, is written in capitals, digits and _ alone, as each of GitLab's is
    (GITLAB_CI_NAME), and says nothing of a credential, or is one of GitLab's own that
    does (GITLAB_JOB_CREDENTIALS); and an FF_ one, a feature flag of the runner's, by its
    value."""
    if name.startswith("FF_"):
        return value in FEATURE_FLAG_VALUES
    return bool(GITLAB_CI_NAME.fullmatch(name)) and (name in GITLAB_JOB_CREDENTIALS or not CREDENTIAL_WORDS.search(name))


def check(environ, out, ci=(frozenset(), [])):
    """The switches, the lanes and the credentials, as hosted-runners checks them; ci is
    ci_globals' answer for the tree. The exit code: 1 for a problem, 3 for a warning, 0
    otherwise."""
    ci_names, ci_unknown = ci
    problems, warnings = [], []
    job = environ.get("CI_JOB_NAME", "hosted-runners")
    for variable, arch, value, hosted, lane, lane_value, lane_default in switches(environ):
        if value is None:
            problems.append("%s is not set: .gitlab-ci.yml's variables: give it %s, and its jobs' tags name it" % (variable, OWN))
            continue
        other = [a for a in HOSTED if a != arch and value in HOSTED[a]]
        if value != OWN and not hosted:
            problems.append("%s is %r, %s: its class's jobs would wait for a runner until GitLab failed them. Set it to %s or "
                            "one of %s" % (variable, value, "an %s machine, and its jobs' containers are %s" % (other[0], arch)
                                           if other else "a tag no runner of the project's or GitLab's carries",
                                           OWN, ", ".join(HOSTED[arch])))
            continue
        where = "GitLab's %s" % value if hosted else "the project's runners (%s)" % OWN
        print("%s: %s" % (variable, where), file=out)
        if not lane:
            continue
        if lane_value is None:
            problems.append("%s is not set: .gitlab-ci.yml's variables: give it %s, and its class's jobs' resource group names "
                            "it" % (lane, lane_default))
            continue
        per_job = job in lane_value and lane_value != lane_default
        print("%s: %s" % (lane, "a lane of each job's own" if per_job else repr(lane_value)), file=out)
        if hosted and not per_job:
            warnings.append("%s is %r while %s sends its class to GitLab's runners: its jobs wait for each other there, each "
                            "on a machine of its own. Set %s to %s (expanded, not raw), a lane of each job's own" % (
                                lane, lane_value, variable, lane, HOSTED_LANE))
        elif not hosted and lane_value != lane_default:
            warnings.append("%s is %r while its class is on the project's runners: its jobs run side by side on linix, which the "
                            "lane %s keeps them from. Delete %s, or send the class to GitLab's runners" % (
                                lane, lane_value, lane_default, lane))
    # Every variable is a credential that is not known here to hold none.
    switch_names = set(n for c in CLASSES for n in (c[0], c[2]) if n)
    no_secret = set(ci_names) | GITLAB_OWN | RUNNER_OWN | PROCESS | PLAIN | switch_names
    given = sorted(name for name in environ if not gitlab_own(name, environ[name]))
    accepted = []
    for name in given:
        if name in GITLAB_CREDENTIALS:
            problems.append("%s is given to every job of this pipeline, and so to each job a switch sends to GitLab's runners: "
                            "GitLab gives every job the credentials of %s - revoke it, or keep every class on the project's "
                            "runners" % (name, GITLAB_CREDENTIALS[name]))
        elif name in CREDENTIALS:
            scope = CREDENTIALS[name]
            if scope:
                to = "scope it to the environment %s, which the one job that uses it names" % scope
            else:
                to = ("its job names no environment to scope it to yet: give the job one, and scope the variable to it, before "
                      "a switch is set")
            problems.append("%s is given to every job of this pipeline that names no environment, and so to each job a switch "
                            "sends to GitLab's runners: %s (Settings, CI/CD, Variables, Environment scope)" % (name, to))
        elif name in TELEMETRY_ONLY:
            accepted.append(name)
        elif name.startswith(("CI_", "FF_")) and name not in no_secret:
            ci_name = name.startswith("CI_")
            capitals = bool(GITLAB_CI_NAME.fullmatch(name))
            if not ci_name:
                why = "its value is not true or false, as each flag's is"
            elif not capitals:
                why = "its name is not written in capitals, digits and _ alone, as each of GitLab's own is"
            else:
                why = "its name says it holds a credential, as none of GitLab's own does but those of GITLAB_JOB_CREDENTIALS"
            problems.append("%s is given to every job of this pipeline that names no environment, and so to each job a switch "
                            "sends to GitLab's runners, and it is not %s although its name begins with %s - %s -, but a "
                            "variable of the project's or the group's, which GitLab lets begin so: if it holds a credential, "
                            "scope it to the environment of the one job that uses it (Settings, CI/CD, Variables, Environment "
                            "scope); if it holds none, name it in build/ci/hosted-runners.py's PLAIN%s" % (
                                name, "GitLab's own" if ci_name else "a feature flag of the runner's", name[:3], why,
                                ", or, one GitLab gives every job, in its GITLAB_JOB_CREDENTIALS" if ci_name and capitals else ""))
        elif name not in no_secret:
            problems.append("%s is given to every job of this pipeline that names no environment, and so to each job a switch "
                            "sends to GitLab's runners, and it is none of the variables known to hold no secret - GitLab's "
                            "own, its runner's, the container's, those the CI files and their templates set, and the "
                            "project's in PLAIN: if it holds a credential, scope it to the environment of the one job that "
                            "uses it (Settings, CI/CD, Variables, Environment scope); if it holds none, name it in "
                            "build/ci/hosted-runners.py's PLAIN, or, one of a GitLab template's variables, in its "
                            "TEMPLATE_GLOBALS" % name)
    beyond = [n for n in given if n not in set(ci_names) | GITLAB_OWN | RUNNER_OWN | PROCESS | switch_names]
    print("Given to a job of this pipeline that names no environment, beside GitLab's own variables, its runner's, the "
          "container's and the CI files': %s" % (", ".join(beyond) or "nothing"), file=out)
    if ci_unknown:
        print("The CI files include what this does not read the variables of: %s (TEMPLATE_GLOBALS)" % ", ".join(ci_unknown),
              file=out)
    for name in accepted:
        print("ACCEPTED: %s is given to every job of this pipeline, and so to each job a switch sends to GitLab's runners: "
              "a credential that only writes telemetry, %s (TELEMETRY_ONLY)." % (name, TELEMETRY_ONLY[name]), file=out)
    for p in problems:
        print("FAILED: %s." % p, file=out)
    for w in warnings:
        print("WARNING: %s." % w, file=out)
    if problems:
        print(setting_back(environ), file=out)
        return 1
    if warnings:
        return 3
    print("Every switch names a runner of its kind, every lane goes with its class, and a job that names no environment is "
          "given no variable but those known to hold no secret%s." % (
              " and %s, which only write%s telemetry" % (", ".join(accepted), "s" if len(accepted) == 1 else "")
              if accepted else ""), file=out)
    return 0


def kind_of(environ):
    if environ.get("CI_COMMIT_TAG"):
        return "tag"
    if environ.get("CI_COMMIT_BRANCH") and environ.get("CI_COMMIT_BRANCH") == environ.get("CI_DEFAULT_BRANCH"):
        return "master"
    return "batch"


def fetch_json(url, token, data=None):
    """GitLab's answer to a GET (or a POST of data) of url, read with token."""
    headers = {"PRIVATE-TOKEN": token, "Accept": "application/json"}
    if data is not None:
        headers["Content-Type"] = "application/json"
    request = urllib.request.Request(url, data=data, headers=headers)
    try:
        with urllib.request.urlopen(request, timeout=30) as answer:
            return json.loads(answer.read().decode("utf-8"))
    except urllib.error.HTTPError as error:
        if error.code in (401, 403, 404):
            raise Refused("%s answered %d" % (url.split("?")[0], error.code))
        raise Unreachable("%s answered %d" % (url.split("?")[0], error.code))
    except (urllib.error.URLError, OSError, ValueError) as error:
        raise Unreachable("%s: %s" % (url.split("?")[0], error))


def quota(environ, out, fetch=fetch_json):
    """The namespace's compute minutes against what a pipeline of this kind made at the
    switches as the job reads them - when it starts - is expected to use, as
    compute-quota reads them. The exit code: 1, 2, 3 or 0, as the module says."""
    kind = kind_of(environ)
    # The switches as GitLab gave them to this job when it started: those of a pipeline
    # made now. This pipeline's jobs took their tags from the switches when it was made,
    # and keep them, a job run again among them; the two differ once a switch has been set
    # or deleted since.
    hosted = [(variable, value) for variable, _, value, on, _, _, _ in switches(environ) if on]
    expected = sum(EXPECTED[variable][kind] for variable, _ in hosted)
    print("The switches now send %s to GitLab's runners: a %s pipeline made with them is expected to use about %d compute "
          "minutes there." % (", ".join("%s (%s)" % pair for pair in hosted) or "no class", kind, expected), file=out)
    token = environ.get("HM_QUOTA_TOKEN", "")
    if not token:
        print("WARNING: not configured: HM_QUOTA_TOKEN is not set, so the namespace's compute minutes are not read. It is a "
              "read_api token of an Owner of the group, a protected and masked project variable scoped to the environment "
              "compute-quota (%s)." % DOCUMENT, file=out)
        return 3
    api = environ.get("CI_API_V4_URL", "https://gitlab.com/api/v4").rstrip("/")
    namespace = environ.get("CI_PROJECT_ROOT_NAMESPACE", "")
    try:
        found = fetch("%s/namespaces/%s" % (api, urllib.parse.quote(namespace, safe="")), token)
        group = fetch("%s/groups/%s?with_projects=false" % (api, urllib.parse.quote(namespace, safe="")), token)
    except Unreachable as error:
        print("GitLab could not be asked for the namespace's compute minutes: %s." % error, file=out)
        return 2
    except Refused as error:
        print("WARNING: GitLab refused HM_QUOTA_TOKEN (%s): it is to be a read_api token of an Owner of the group %s." % (
            error, namespace), file=out)
        return 3
    usage = (found or {}).get("ci_minutes_usage")
    limit = (group or {}).get("shared_runners_minutes_limit")
    if not isinstance(usage, dict) or not isinstance(limit, int):
        print("WARNING: GitLab gave HM_QUOTA_TOKEN no %s for %s: only an Owner of the group may read it." % (
            "compute usage" if not isinstance(usage, dict) else "quota", namespace), file=out)
        return 3
    total = limit + int((group or {}).get("extra_shared_runners_minutes_limit") or 0)
    used = int(usage.get("total_minutes_used") or 0)
    left = total - used
    if total <= 0:
        print("The namespace %s has no compute quota (%d): nothing runs out." % (namespace, total), file=out)
        return 0
    share = float(used) / total
    print("This month %s has used %d of its %d compute minutes (%.1f%%): %d left, which the 1st of next month makes %d "
          "again." % (namespace, used, total, 100 * share, left, limit), file=out)
    if expected:
        print("That is about %d more such pipeline%s." % (max(0, left // expected), "" if left // expected == 1 else "s"),
              file=out)
    try:
        graph = fetch("%s/api/graphql" % api[:-len("/api/v4")] if api.endswith("/api/v4") else api + "/graphql", token,
                      json.dumps({"query": "query { ciMinutesUsage(namespaceId: \"gid://gitlab/Group/%d\") { nodes { "
                                           "monthIso8601 minutes sharedRunnersDuration } } }" % int(found.get("id") or 0)})
                      .encode("utf-8"))
        nodes = ((graph or {}).get("data") or {}).get("ciMinutesUsage", {}).get("nodes") or []
        month = max(nodes, key=lambda n: n.get("monthIso8601") or "") if nodes else None
        if month and month.get("sharedRunnersDuration"):
            ran = month["sharedRunnersDuration"] / 60.0
            print("Its jobs ran %.0f minutes on GitLab's runners this month for those %d: %.2f a job-minute." % (
                ran, month.get("minutes") or 0, (month.get("minutes") or 0) / ran), file=out)
    except (Unreachable, Refused, ValueError, TypeError, AttributeError):
        pass
    if not hosted:
        # No switch sends a class to GitLab's runners now - the switches were set back and
        # the job is run again, or a lane alone is set: a pipeline made from now on spends
        # no minute. This pipeline's jobs keep the tags they were made with, a job run again
        # among them, so a class of it made while a switch was set is on GitLab's runners
        # still, and can stop for the minutes; which of its jobs are is not read here.
        print("No switch sends a class to GitLab's runners now, so a pipeline made from now on runs every class on the "
              "project's runners. This pipeline's jobs keep the runners they were made with, a job of it run again among "
              "them: if a switch was set when it was made, its class's jobs here run on GitLab's runners still, and stop "
              "once the minutes are spent%s." % ("; so does a merge request's pipeline, on GitLab's small runner"
                                                 if share >= QUOTA_WARN_SHARE else ""), file=out)
        return 0
    if (expected and left < QUOTA_FAIL_PIPELINES * expected) or share >= QUOTA_FAIL_SHARE:
        print("FAILED: %d compute minutes are left this month, fewer than %s: the jobs on GitLab's runners stop with "
              "ci_quota_exceeded once they are spent, a gate's among them. %s" % (
                  left, "%d such pipelines need" % QUOTA_FAIL_PIPELINES if expected and left < QUOTA_FAIL_PIPELINES *
                  expected else "a twentieth of the quota", setting_back(environ)), file=out)
        return 1
    if (expected and left < QUOTA_WARN_PIPELINES * expected) or share >= QUOTA_WARN_SHARE:
        print("WARNING: %d compute minutes are left this month, fewer than %s. %s" % (
            left, "%d such pipelines need" % QUOTA_WARN_PIPELINES if expected and left < QUOTA_WARN_PIPELINES *
            expected else "a quarter of the quota", setting_back(environ)), file=out)
        return 3
    return 0


def self_test():
    failures = []

    def expect(what, got, want):
        if got != want:
            failures.append("%s: got %r, want %r" % (what, got, want))

    # The CI files' variables as the job reads them: from the tree's own files.
    tree_ci = ci_globals(ROOT)
    expect("the tree's CI files: their variables and their templates' are read, every template known",
           ({"HM_RUNNER", "HM_RUNNER_PACKAGES", "HM_LANE_SUITE", "SECRETS_ANALYZER_VERSION", "SAST_EXCLUDED_PATHS"} <=
            tree_ci[0], tree_ci[1]), (True, []))
    # What GitLab gives this job in a batch's pipeline, by name: its predefined variables,
    # its runner's, the Python image's, the CI files' (as the tree has them), their
    # templates' (Jobs/SAST and Jobs/Secret-Detection, as CI lint's merged_yaml listed them
    # on 14 October 2026), and the project's that hold no secret. All but the CI files' are
    # written out here, so that a name this script stops knowing fails a case: GitLab's own
    # whose names say they hold a credential, and others whose names hold such a word in
    # passing (CI_COMMIT_AUTHOR) or a digit (CI_API_V4_URL), and the runner's feature
    # flags, those whose names say so among them - gitlab-runner 19.4.1 has six of its 53
    # so named.
    defaults = dict((name, "x") for name in tree_ci[0])
    defaults.update({
        "CI": "true", "CI_JOB_NAME": "hosted-runners", "CI_JOB_TOKEN": "job-token-value", "CI_REGISTRY_PASSWORD": "registry-value",
        "CI_DEPENDENCY_PROXY_PASSWORD": "registry-value", "CI_RUNNER_SHORT_TOKEN": "x", "CI_TRIGGER_SHORT_TOKEN": "x",
        "CI_SERVER_TLS_KEY_FILE": "/builds/x.tmp/CI_SERVER_TLS_KEY_FILE", "CI_SERVER_TLS_CERT_FILE": "/builds/x.tmp/cert",
        "CI_COMMIT_AUTHOR": "Someone <someone@example.invalid>", "CI_REPOSITORY_URL": "https://gitlab.example/x.git",
        "CI_API_V4_URL": "https://gitlab.example/api/v4",
        "FF_MASK_ALL_DEFAULT_TOKENS": "true", "FF_GIT_URLS_WITHOUT_TOKENS": "false", "FF_DISABLE_AUTOMATIC_TOKEN_ROTATION": "false",
        "FF_SECRET_RESOLVING_FAILS_IF_MISSING": "true", "FF_HASH_CACHE_KEYS": "false", "FF_USE_GIT_PROACTIVE_AUTH": "false",
        "CI_COMMIT_BRANCH": "batch48", "CI_DEFAULT_BRANCH": "master", "GITLAB_CI": "true", "GITLAB_USER_LOGIN": "someone",
        "GITLAB_FEATURES": "", "FF_NETWORK_PER_BUILD": "false", "GITLAB_ENV": "/builds/x.tmp/gitlab_runner_env",
        "RUNNER_TEMP_PROJECT_DIR": "/builds/x.tmp", "PATH": "/usr/local/bin:/usr/bin", "HOME": "/root", "HOSTNAME": "runner-1",
        "PWD": "/builds/x", "SHLVL": "1", "_": "/usr/local/bin/python3", "PYTHON_VERSION": "3.14.7", "PYTHON_SHA256": "0" * 64,
        "HM_RUNNER": "linix", "HM_RUNNER_BUILD": "linix", "HM_RUNNER_PACKAGES": "linix", "HM_LANE_PACKAGES": "linix-package-build",
        "HM_RUNNER_ARM64": "linix", "HM_RUNNER_SUITE": "linix", "HM_LANE_SUITE": "linix-suite",
        "SECURE_ANALYZERS_PREFIX": "registry.gitlab.com/security-products", "SAST_IMAGE_SUFFIX": "", "SAST_EXCLUDED_ANALYZERS": "",
        "DEFAULT_SAST_EXCLUDED_PATHS": "spec, test, tests, tmp", "SAST_EXCLUDED_PATHS": "spec, test, tests, tmp",
        "SCAN_KUBERNETES_MANIFESTS": "false", "GITLAB_ADVANCED_SAST_EXT_INCREMENTAL_ENABLED": "true",
        "SECRET_DETECTION_IMAGE_SUFFIX": "", "SECRETS_ANALYZER_VERSION": "7", "SECRET_DETECTION_EXCLUDED_PATHS": "",
        "ARTIFACT_SIGNING_ACCOUNT": "x", "ARTIFACT_SIGNING_ENDPOINT": "x", "ARTIFACT_SIGNING_PROFILE": "x", "AZURE_CLIENT_ID": "x",
        "AZURE_TENANT_ID": "x", "HM_PUBLISH_IMAGE": "1", "GITLAB_OBSERVABILITY_EXPORT": "traces"})
    # A stand-in no scanner takes for a credential, which no output may hold.
    planted = "Q" * 24

    def run_check(**changes):
        environ = dict(defaults)
        for key, value in changes.items():
            if value is None:
                environ.pop(key, None)
            else:
                environ[key] = value
        out = io.StringIO()
        code = check(environ, out, tree_ci)
        return code, out.getvalue()

    recommended = dict(HM_RUNNER_PACKAGES="saas-linux-large-amd64", HM_LANE_PACKAGES="hosted-hosted-runners",
                       HM_RUNNER_ARM64="saas-linux-medium-arm64")
    code, text = run_check()
    expect("every switch at its default: passed", (code, "FAILED" in text), (0, False))
    expect("every switch at its default: what a job is given beside GitLab's, the runner's, the container's and the CI "
           "files', named", "ARTIFACT_SIGNING_ACCOUNT, ARTIFACT_SIGNING_ENDPOINT, ARTIFACT_SIGNING_PROFILE, AZURE_CLIENT_ID, "
           "AZURE_TENANT_ID, GITLAB_OBSERVABILITY_EXPORT, HM_PUBLISH_IMAGE\n" in text, True)
    expect("GitLab's own credentials are the job's, not the project's", "CI_JOB_TOKEN" in text, False)
    code, text = run_check(**recommended)
    expect("packages and arm64 on GitLab's runners, a lane of each job's own, beside the variables of the CI files and of "
           "their templates (SECRETS_ANALYZER_VERSION and the rest): passed", (code, "FAILED" in text), (0, False))
    expect("the switches are said", "HM_RUNNER_PACKAGES: GitLab's saas-linux-large-amd64" in text and
           "HM_RUNNER_ARM64: GitLab's saas-linux-medium-arm64" in text, True)
    out = io.StringIO()
    expect("the CI files' own variables and their templates', not read: each failed",
           (check(dict(defaults, **recommended), out, (frozenset(), [])), "FAILED: SECRETS_ANALYZER_VERSION is given" in
            out.getvalue() and "FAILED: HM_RUNNER is given" in out.getvalue()), (1, True))
    expect("a CI file's variables, a key quoted or its value below it, are read",
           defined_variables("variables:\n  \"QUOTED\": x\n  'SINGLE' : y\n  EXPANDED:\n    value: z\n    description: d\n"
                             "job:\n  variables:\n    NOT_GLOBAL: w\n"), {"QUOTED", "SINGLE", "EXPANDED"})
    expect("a CI file's includes are read from its include: alone",
           included("include:\n  # a note\n  - local: .gitlab/ci/a.yml\n  - template: 'Jobs/SAST.gitlab-ci.yml'\n"
                    "  - remote: https://example.invalid/x.yml\njob:\n  script:\n    - local: not-an-include\n"),
           [("local", ".gitlab/ci/a.yml"), ("template", "Jobs/SAST.gitlab-ci.yml"), ("remote", "https://example.invalid/x.yml")])
    for name in ("HM_UPDATE_SIGNING_KEY", "COSIGN_KEY", "SSH_DEPLOY_KEY", "NPM_AUTH", "HM_SMTP_PASS", "SIGNING_CERT_PFX",
                 "KUBECONFIG"):
        code, text = run_check(**dict(recommended, **{name: planted}))
        expect("%s, a variable known to hold no secret by none of the lists, whatever its name: failed, named" % name,
               (code, "FAILED: %s is given to every job" % name in text and "none of the variables known to hold no secret"
                in text), (1, True))
    code, text = run_check(**dict(recommended, CI_DEPLOY_USER="gitlab+deploy-token-1", CI_DEPLOY_PASSWORD=planted))
    expect("the deploy token GitLab gives every job: failed, named as such", (code, "FAILED: CI_DEPLOY_PASSWORD is given" in text
                                                                               and "gitlab-deploy-token" in text), (1, True))
    code, text = run_check(HM_RUNNER_PACKAGES="saas-linux-large-amd4")
    expect("a switch no runner carries: failed, naming it", (code, "HM_RUNNER_PACKAGES is 'saas-linux-large-amd4'" in text), (1, True))
    expect("a failure says how to set the switches back", "delete the project CI/CD variable HM_RUNNER_PACKAGES" in text, True)
    expect("and that a new pipeline is on the project's runners, where one made before keeps its runners, a job run again "
           "among them", ("make a new pipeline" in text, "keeps the runners its jobs were made with, a job of it run again "
                                                         "among them" in text, "run the pipeline again" in text),
           (True, True, False))
    code, text = run_check(HM_RUNNER_BUILD="saas-linux-large-arm64")
    expect("an arm64 machine for an amd64 class: failed", (code, "an arm64 machine, and its jobs' containers are amd64" in text),
           (1, True))
    code, text = run_check(HM_RUNNER_ARM64="saas-linux-large-amd64")
    expect("an amd64 machine for the arm64 class: failed", (code, "an amd64 machine" in text), (1, True))
    code, text = run_check(HM_RUNNER_SUITE=None)
    expect("a switch the CI files no longer set: failed", (code, "HM_RUNNER_SUITE is not set" in text), (1, True))
    code, text = run_check(HM_RUNNER_SUITE="saas-linux-xlarge-amd64")
    expect("a class on GitLab's runners in the shared lane: a warning", (code, "HM_LANE_SUITE is 'linix-suite'" in text), (3, True))
    code, text = run_check(HM_RUNNER_PACKAGES="saas-linux-large-amd64", HM_LANE_PACKAGES="hosted")
    expect("a class on GitLab's runners in one lane of another name: a warning", code, 3)
    code, text = run_check(HM_LANE_PACKAGES="hosted-hosted-runners")
    expect("a lane of each job's own while the class is on linix: a warning", (code, "side by side on linix" in text), (3, True))
    code, text = run_check(RENOVATE_TOKEN=planted)
    expect("a credential given to every job: failed, with its environment", (code, "RENOVATE_TOKEN is given to every job" in text
                                                                             and "environment renovate" in text), (1, True))
    expect("a credential's value is never printed", planted in text, False)
    code, text = run_check(SNAPCRAFT_STORE_CREDENTIALS=planted, HM_QUOTA_TOKEN=planted)
    expect("the Snap Store's and the quota's credentials: failed, each named", (code, "snap-store" in text and
                                                                               "compute-quota" in text), (1, True))
    code, text = run_check(HM_REPO_SIGNING_KEY=planted)
    expect("a credential whose job names no environment yet: failed, saying so",
           (code, "its job names no environment to scope it to yet" in text), (1, True))
    code, text = run_check(SOMEONES_API_KEY=planted)
    expect("a variable whose name says it is a credential: failed", (code, "FAILED: SOMEONES_API_KEY is given" in text), (1, True))
    # The group's Observability ingest token, which every job is given once its intake is
    # locked, of scope * as GitLab's export needs it: accepted by its whole name, and
    # said, with a switch set as with none, where it would have failed every pipeline a
    # switch was set for, as a credential to scope to one job's environment, which would
    # stop the export. Its value is never printed, and nothing wider is accepted: a name
    # near its own, or any other token, beside it fails as before.
    for what, switched in (("packages and arm64 on GitLab's runners", recommended), ("every switch at its default", {})):
        code, text = run_check(**dict(switched, GITLAB_OBSERVABILITY_TOKEN=planted))
        expect("GITLAB_OBSERVABILITY_TOKEN, %s: passed, the token accepted and said, its value not printed" % what,
               (code, "FAILED" in text, "ACCEPTED: GITLAB_OBSERVABILITY_TOKEN is given to every job of this pipeline, and so "
                "to each job a switch sends to GitLab's runners: a credential that only writes telemetry" in text,
                "GITLAB_OBSERVABILITY_EXPORT, GITLAB_OBSERVABILITY_TOKEN, HM_PUBLISH_IMAGE\n" in text,
                "and GITLAB_OBSERVABILITY_TOKEN, which only writes telemetry." in text, planted in text),
               (0, False, True, True, True, False))
    for name in ("GITLAB_OBSERVABILITY_TOKEN_2", "GITLAB_OBSERVABILITY_TOKEN_OLD", "XGITLAB_OBSERVABILITY_TOKEN",
                 "OBSERVABILITY_TOKEN", "GITLAB_OBSERVABILITY_KEY", "gitlab_observability_token", "HM_INGEST_TOKEN",
                 "CI_OBSERVABILITY_TOKEN"):
        code, text = run_check(**dict(recommended, GITLAB_OBSERVABILITY_TOKEN=planted, **{name: planted}))
        expect("%s beside the ingest token: failed, named, the token itself still accepted" % name,
               (code, "FAILED: %s is given to every job" % name in text,
                "ACCEPTED: GITLAB_OBSERVABILITY_TOKEN is given" in text, "ACCEPTED: %s" % name in text, planted in text),
               (1, True, True, False, False))
    expect("TELEMETRY_ONLY names no credential with a job of its own, no variable known to hold none and none of GitLab's",
           set(TELEMETRY_ONLY) & (set(CREDENTIALS) | PLAIN | set(GITLAB_CREDENTIALS) | GITLAB_OWN | RUNNER_OWN | PROCESS),
           set())
    # GitLab keeps neither CI_ nor FF_ for its own: a variable of the project's may be named so.
    for name in ("CI_PUSH_TOKEN", "CI_SIGNING_KEY", "CI_NPM_AUTH"):
        code, text = run_check(**dict(recommended, **{name: planted}))
        expect("%s, the project's under a name that begins with CI_ and says it holds a credential: failed, named" % name,
               (code, "FAILED: %s is given to every job" % name in text and "although its name begins with CI_" in text),
               (1, True))
    # Nor does GitLab name one of its own with a small letter, which CREDENTIAL_WORDS, in
    # capitals, does not read: a CI_ name with one is the project's or the group's,
    # whatever it says - a credential's word in small letters, or none.
    for name in ("CI_push_token", "CI_Deploy_Key", "CI_npm_auth", "CI_signing"):
        code, text = run_check(**dict(recommended, **{name: planted}))
        expect("%s, the project's under a name that begins with CI_ and is not in capitals, as each of GitLab's own is: "
               "failed, named, saying so" % name,
               (code, "FAILED: %s is given to every job" % name in text and "its name is not written in capitals" in text,
                "in its GITLAB_JOB_CREDENTIALS" in text), (1, True, False))
    code, text = run_check(**dict(recommended, FF_UPLOAD_TOKEN=planted))
    expect("FF_UPLOAD_TOKEN, the project's under a name that begins with FF_ and of a value no flag of the runner's has: "
           "failed, named", (code, "FAILED: FF_UPLOAD_TOKEN is given to every job" in text and "a feature flag of the "
                             "runner's" in text), (1, True))
    code, text = run_check(**dict(recommended, FF_SOME_NEWER_FLAG="1"))
    expect("a feature flag the runner does not know, set to 1, which it reads as true: passed", (code, "FAILED" in text),
           (0, False))
    expect("no value of any variable is printed", any(v in text for v in (planted, "job-token-value", "registry-value")), False)

    # The quota, against answers held in memory.
    def fake(namespace_answer, group_answer, graph_answer=None, error=None):
        asked = []

        def fetch(url, token, data=None):
            asked.append((url, token))
            if error:
                raise error
            if "/graphql" in url:
                if graph_answer is None:
                    raise Refused("no graph")
                return graph_answer
            return namespace_answer if "/namespaces/" in url else group_answer
        return fetch, asked

    base = dict(defaults, HM_RUNNER_PACKAGES="saas-linux-large-amd64", HM_RUNNER_ARM64="saas-linux-medium-arm64",
                CI_API_V4_URL="https://gitlab.example/api/v4", CI_PROJECT_ROOT_NAMESPACE="hmailserver",
                CI_COMMIT_BRANCH="batch48", CI_DEFAULT_BRANCH="master", HM_QUOTA_TOKEN=planted)

    def run_quota(used, limit=50000, extra=None, environ=None, **kw):
        fetch, asked = fake({"id": 143269842, "ci_minutes_usage": {"total_minutes_used": used, "monthly_minutes_used": used}},
                            {"shared_runners_minutes_limit": limit, "extra_shared_runners_minutes_limit": extra}, **kw)
        out = io.StringIO()
        code = quota(environ or base, out, fetch)
        return code, out.getvalue(), asked

    code, text, asked = run_quota(415)
    expect("415 of 50,000 used: passed", code, 0)
    expect("the expected minutes of a batch's pipeline with packages and arm64 on GitLab's", "about 295 compute minutes" in text, True)
    expect("which are those of a pipeline made at the switches as the job reads them, not of this one",
           ("The switches now send HM_RUNNER_PACKAGES (saas-linux-large-amd64), HM_RUNNER_ARM64 (saas-linux-medium-arm64) to "
            "GitLab's runners: a batch pipeline made with them" in text, "this batch pipeline" in text), (True, False))
    expect("the namespace and the group are asked, by the root namespace", [u for u, _ in asked][:2],
           ["https://gitlab.example/api/v4/namespaces/hmailserver", "https://gitlab.example/api/v4/groups/hmailserver?with_projects=false"])
    expect("the token is never printed", planted in text, False)
    code, text, _ = run_quota(415, graph_answer={"data": {"ciMinutesUsage": {"nodes": [
        {"monthIso8601": "2026-09-01", "minutes": 154, "sharedRunnersDuration": 9210},
        {"monthIso8601": "2026-10-01", "minutes": 415, "sharedRunnersDuration": 24786}]}}})
    expect("this month's minutes against its job-minutes, as GitLab charged them", "1.00 a job-minute" in text, True)
    code, text, _ = run_quota(40000)
    expect("four fifths used: a warning, with the way back", (code, "fewer than a quarter of the quota" in text and
                                                              "delete the project CI/CD variables" in text), (3, True))
    code, text, _ = run_quota(48000)
    expect("nineteen twentieths used: failed", (code, "FAILED" in text), (1, True))
    code, text, _ = run_quota(49000)
    expect("49,000 of 50,000 used: failed", code, 1)
    code, text, _ = run_quota(49000, extra=10000)
    expect("purchased minutes count: 49,000 of 60,000 used is a warning", (code, "of its 60000 compute minutes" in text), (3, True))
    code, text, _ = run_quota(49500)
    expect("500 left, fewer than two batch pipelines' 590: failed", (code, "2 such pipelines need" in text), (1, True))
    code, text, _ = run_quota(46000)
    expect("4,000 left, more than ten pipelines' 2,950 but under a quarter of the quota: a warning", code, 3)
    code, text, _ = run_quota(47000, environ=dict(base, HM_RUNNER_BUILD="saas-linux-large-amd64",
                                                  HM_RUNNER_SUITE="saas-linux-large-amd64"))
    expect("3,000 left with every class on, fewer than ten pipelines' 5,350: a warning saying so",
           (code, "10 such pipelines need" in text), (3, True))
    code, text, _ = run_quota(48000, environ=dict(base, CI_COMMIT_BRANCH="master"))
    expect("master's pipeline expects 120, and 2,000 left is under a twentieth of the quota: failed", code, 1)
    code, text, _ = run_quota(47600, environ=dict(base, CI_COMMIT_BRANCH="master", HM_RUNNER_PACKAGES="linix"))
    expect("master's with the arm64 class alone there, whose jobs wait for a press, 2,400 left: failed, the quota near its end",
           (code, "a twentieth of the quota" in text), (1, True))
    code, text, _ = run_quota(47600, environ=dict(base, HM_RUNNER_PACKAGES="linix", HM_RUNNER_ARM64="linix"))
    # The pipeline's jobs keep the tags it was made with, a job run again among them: what
    # the job knows is the switches as they are now, which a pipeline made now takes.
    expect("the job run again after the switches were set back, 2,400 left: passed, the minutes said, and what is known - "
           "the switches now, a pipeline made now - and that this pipeline's jobs keep their runners, never that none stops",
           (code, "2400 left" in text, "The switches now send no class to GitLab's runners" in text,
            "No switch sends a class to GitLab's runners now, so a pipeline made from now on runs every class on the project's "
            "runners" in text, "This pipeline's jobs keep the runners they were made with, a job of it run again among them: "
            "if a switch was set when it was made, its class's jobs here run on GitLab's runners still" in text,
            any(claim in text for claim in ("none of its jobs stops", "No class of this pipeline", "this batch pipeline")),
            "FAILED" in text), (0, True, True, True, True, False, False))
    code, text, _ = run_quota(49900, environ=dict(base, HM_RUNNER_PACKAGES="linix", HM_RUNNER_ARM64="linix",
                                                  HM_LANE_PACKAGES="hosted-compute-quota"))
    expect("a lane alone set, nothing on GitLab's runners, 100 left: passed, saying what does stop",
           (code, "so does a merge request's pipeline, on GitLab's small runner" in text), (0, True))
    code, text, _ = run_quota(415, environ=dict(base, HM_QUOTA_TOKEN=""))
    expect("no token: a warning, not configured", (code, "not configured" in text), (3, True))
    fetch, _ = fake(None, None, error=Unreachable("connection refused"))
    out = io.StringIO()
    expect("GitLab not answering: exit 2, tried again", quota(base, out, fetch), 2)
    fetch, _ = fake(None, None, error=Refused("401"))
    out = io.StringIO()
    expect("a token GitLab refuses: a warning", (quota(base, out, fetch), "refused HM_QUOTA_TOKEN" in out.getvalue()), (3, True))
    fetch, _ = fake({"id": 1}, {"shared_runners_minutes_limit": 50000})
    out = io.StringIO()
    expect("a token that cannot read the usage: a warning, saying an Owner's is needed",
           (quota(base, out, fetch), "only an Owner" in out.getvalue()), (3, True))
    expect("a tag's pipeline is one of a kind of its own", kind_of({"CI_COMMIT_TAG": "v6.3.7"}), "tag")

    # The HTTP reading, against a GitLab API served on the loopback.
    seen = []

    class Handler(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            seen.append((self.path, self.headers.get("PRIVATE-TOKEN")))
            if self.headers.get("PRIVATE-TOKEN") != planted:
                self.send_response(401)
                self.end_headers()
                return
            body = {"/api/v4/namespaces/hmailserver": {"id": 7, "ci_minutes_usage": {"total_minutes_used": 415}},
                    "/api/v4/groups/hmailserver?with_projects=false": {"shared_runners_minutes_limit": 50000}}.get(self.path)
            if body is None:
                self.send_response(500)
                self.end_headers()
                return
            data = json.dumps(body).encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def do_POST(self):
            self.rfile.read(int(self.headers.get("Content-Length") or 0))
            self.send_response(404)
            self.end_headers()

        def log_message(self, *args):
            pass

    server = http.server.HTTPServer(("127.0.0.1", 0), Handler)
    thread = threading.Thread(target=server.serve_forever)
    thread.daemon = True
    thread.start()
    try:
        api = "http://127.0.0.1:%d/api/v4" % server.server_address[1]
        out = io.StringIO()
        code = quota(dict(base, CI_API_V4_URL=api), out)
        expect("over HTTP: read with the token in its header, and passed", (code, seen[:2]), (0, [
            ("/api/v4/namespaces/hmailserver", planted), ("/api/v4/groups/hmailserver?with_projects=false", planted)]))
        out = io.StringIO()
        expect("over HTTP, a token refused: a warning", quota(dict(base, CI_API_V4_URL=api, HM_QUOTA_TOKEN="wrong"), out), 3)
        out = io.StringIO()
        expect("over HTTP, a GitLab that fails: exit 2", quota(dict(base, CI_API_V4_URL=api, CI_PROJECT_ROOT_NAMESPACE="other"),
                                                               out), 2)
    finally:
        server.shutdown()
        server.server_close()

    # The classes against the tree's own CI file: every switch and lane at its default.
    try:
        with io.open(os.path.join(ROOT, ".gitlab-ci.yml"), encoding="utf-8") as f:
            ci = f.read().replace("\r\n", "\n")
        for variable, _, lane, lane_default in CLASSES:
            for name, default in ((variable, OWN), (lane, lane_default)):
                if name:
                    expect(".gitlab-ci.yml's variables: give %s its default" % name,
                           re.search(r"^  %s: %s\s*$" % (name, re.escape(default)), ci, re.MULTILINE) is not None, True)
        expect("the tree's .gitlab-ci.yml defines the switches", {c[0] for c in CLASSES} <= defined_variables(ci), True)
    except (IOError, OSError) as error:
        failures.append("the tree's .gitlab-ci.yml could not be read: %s" % error)

    for f in failures:
        print("FAIL " + f)
    print("hosted-runners.py self-test: %d failure(s)" % len(failures))
    return 1 if failures else 0


def main(argv):
    if argv == ["--self-test"]:
        return self_test()
    if argv == ["check"]:
        return check(dict(os.environ), sys.stdout, ci_globals(ROOT))
    if argv == ["quota"]:
        return quota(dict(os.environ), sys.stdout)
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
