#!/usr/bin/env python3
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

"""The Kubernetes operator (platform/packaging/helm/operator) holds to the chart it
installs and to itself: run by the chart-and-compose job of .gitlab-ci.yml, beside
the chart's own checks, in its Alpine image with Alpine's helm and PyYAML.

  * the custom resource's spec has one property for each of the chart's top-level
    values (values.yaml) and no other, refuses the two passwords
    (database.password and administrator.password may only be empty) and requires
    existingSecret and database.host, and is not left open to any field;
  * watches.yaml names the CRD's group, version and kind, and the chart at
    helm-charts/hmailserver, where build/ci/operator-context.sh copies it;
  * the ClusterServiceVersion owns that CRD; says it is Progressive Robot's fork of
    hMailServer and names the original's author, Martin Knafve, in its display name,
    its description and its tile's; asks for namespaced permissions alone, in one
    entry for the service account its one deployment runs as, and in them nothing
    but what an allow-list built from the chart gives (ALLOWED_OWN and
    allowed_grants): each kind the chart renders - for the resource of its example
    and for one that gives the passwords in the chart's own Secret - and the Secrets
    Helm keeps a release in, with the seven verbs a client uses; its own resources,
    their status and finalizers to read and update; events to create and patch; and
    a lease - no other resource, no other verb, no wildcard, no resourceNames or
    nonResourceURLs; grants every one of those kinds the seven verbs, and lists
    exactly the chart's kinds as the CRD's resources; supports OwnNamespace and
    AllNamespaces; and its example is the CRD's kind, gives no password, and uses
    only the CRD's properties, each of its spec descriptors naming one;
  * the Dockerfile is held to an allow-list, read as Docker and kaniko read it -
    each instruction's first word in any case and after any indentation, comments
    and blank lines dropped and continued lines joined (dockerfile_instructions,
    which operator-context.sh's reading copies): one FROM, first, the Helm operator
    by tag and digest with no flag or stage name; ARG, LABEL, ENV and WORKDIR; and
    exactly the two COPY lines, with no flag; nothing else, and no parser directive;
  * the Dockerfile's Helm operator, the builder the bundle's annotations and the CSV
    name, and the scorecard's test images are one operator-sdk release, every image
    by its digest;
  * .gitlab/ci/operator.yml holds the three release jobs and nothing else, each
    exactly as OPERATOR_JOBS has it (the images, the needs, the rules, the one script
    line, the artifacts, and operator-image-sign's one ID token, SIGSTORE_ID_TOKEN for
    the audience sigstore), with no key given twice, and no other CI file defines one
    of them; operator-image-sign.sh signs as the release does (SIGN_LINES: the issuer,
    the project's id and path, the identity, Rekor and Fulcio) and names no Sigstore
    or cosign variable but the ID token; and operator-image-push.sh writes its dotenv
    report, which the sign job takes and whose variables outrank the job's own, as
    its three lines alone (PUSH_ENV_BLOCK), once, appending to nothing, with tee
    nowhere, out set once and a cd only in a subshell. These two are scans of the
    scripts' lines, not of sh: a command made another thing, or a line given a
    value built as the script runs, is not seen, the scripts not being held whole;
  * and, when the job runs it without --self-test, operator-context.sh writes the
    image's context with the chart as the tree's and stamped for a version, and
    operator-bundle.sh a bundle whose CSV has that version and the image by digest
    everywhere, and the fork's icon, platform/packaging/hmailserver.png, inlined.

  python3 build/ci/operator-check.py [--self-test]

--self-test breaks each rule in a copy of the tree's files, in memory, and sees it
refused; it needs neither helm nor a shell.
"""
import base64
import copy
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

import yaml

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
PACKAGING = 'hmailserver/source/Server/platform/packaging'
CHART = PACKAGING + '/helm/hmailserver'
OPERATOR = PACKAGING + '/helm/operator'
CRD = OPERATOR + '/bundle/manifests/mail.progressiverobot.com_hmailservers.yaml'
CSV = OPERATOR + '/bundle/manifests/hmailserver-operator.clusterserviceversion.yaml'
ANNOTATIONS = OPERATOR + '/bundle/metadata/annotations.yaml'
SCORECARD = OPERATOR + '/bundle/tests/scorecard/config.yaml'
WATCHES = OPERATOR + '/watches.yaml'
DOCKERFILE = OPERATOR + '/Dockerfile'
VALUES = CHART + '/values.yaml'
ICON = PACKAGING + '/hmailserver.png'
OPERATOR_CI = '.gitlab/ci/operator.yml'
SIGN_SCRIPT = 'build/ci/operator-image-sign.sh'
PUSH_SCRIPT = 'build/ci/operator-image-push.sh'
FILES = (CRD, CSV, ANNOTATIONS, SCORECARD, WATCHES, DOCKERFILE, VALUES, OPERATOR_CI, SIGN_SCRIPT, PUSH_SCRIPT)
# The other CI files, under this key of texts: path -> text.
OTHER_CI = 'other CI files'

FORK = "Progressive Robot's fork of hMailServer"
ORIGINAL = 'Martin Knafve'
VERBS = {'get', 'list', 'watch', 'create', 'update', 'patch', 'delete'}
# The kinds the chart may render, as RBAC names them.
KINDS = {'StatefulSet': ('apps', 'statefulsets'), 'Service': ('', 'services'), 'Secret': ('', 'secrets')}
# Where Helm keeps each release, whatever the chart renders.
HELM_STORAGE = ('', 'secrets')
# What the operator may do besides the chart's kinds, its group's resources named by
# allowed_grants: events about what it did, and the lease that makes one replica the leader.
ALLOWED_OWN = {('', 'events'): {'create', 'patch'}, ('coordination.k8s.io', 'leases'): VERBS}
OWN_RESOURCES = ('hmailservers', 'hmailservers/status', 'hmailservers/finalizers')
OWN_VERBS = {'get', 'list', 'watch', 'update', 'patch'}
RULE_KEYS = {'apiGroups', 'resources', 'verbs'}
PINNED = re.compile(r'^[a-z0-9.\-/]+:v(\d+\.\d+\.\d+)@sha256:[0-9a-f]{64}$')
BASE_IMAGE = re.compile(r'^quay\.io/operator-framework/helm-operator:v(\d+\.\d+\.\d+)@sha256:[0-9a-f]{64}$')
# The Dockerfile's instructions: the first word of each, and those two COPY lines alone.
DOCKERFILE_WORDS = {'FROM', 'ARG', 'LABEL', 'ENV', 'WORKDIR', 'COPY'}
DOCKERFILE_COPIES = ['watches.yaml ${HOME}/watches.yaml', 'helm-charts ${HOME}/helm-charts']
DIRECTIVE = re.compile(r'^\s*#\s*(?:syntax|escape|check)\s*=', re.IGNORECASE)

# The release jobs of .gitlab/ci/operator.yml, as GitLab reads them (a !reference is a
# tuple). Changing one is a change here too.
RELEASE_RULES = [('!reference', '.release-rules', 'rules')]
OPERATOR_JOBS = {
    'operator-image': {
        'stage': 'release',
        'image': ('!reference', 'linux-image', 'image'),
        'needs': [{'job': 'container-image-sign', 'artifacts': False}],
        'timeout': '20 minutes',
        'interruptible': False,
        'rules': RELEASE_RULES,
        'script': ['sh build/ci/operator-image.sh'],
        'artifacts': {'when': 'always', 'expire_in': '7 days', 'paths': ['operator-image/']},
    },
    'operator-image-push': {
        'stage': 'release',
        'image': ('!reference', 'container-image', 'image'),
        'needs': [{'job': 'operator-image', 'artifacts': True}],
        'timeout': '15 minutes',
        'interruptible': False,
        'rules': RELEASE_RULES,
        'script': ['sh build/ci/operator-image-push.sh'],
        'artifacts': {'when': 'always', 'expire_in': '90 days', 'paths': ['operator-image-push/'],
                      'reports': {'dotenv': 'operator-image-push/operator.env'}},
    },
    'operator-image-sign': {
        'stage': 'release',
        'image': {'name': '$HM_COSIGN_IMAGE', 'entrypoint': ['']},
        'needs': [{'job': 'operator-image-push', 'artifacts': True}],
        'timeout': '15 minutes',
        'interruptible': False,
        'rules': RELEASE_RULES,
        'id_tokens': {'SIGSTORE_ID_TOKEN': {'aud': 'sigstore'}},
        'variables': {'COSIGN_YES': 'true'},
        'script': ['sh build/ci/operator-image-sign.sh'],
        'artifacts': {'when': 'always', 'expire_in': '90 days', 'paths': ['operator-image-signed/', 'operator-bundle/']},
    },
}
JOB_LINE = re.compile(r'''^["']?(operator-image(?:-push|-sign)?)["']?[ \t]*:''', re.MULTILINE)
# operator-image-sign.sh's lines that say as whom and where it signs, each once, and
# the only lines that set those variables.
SIGN_LINES = (
    'issuer=https://gitlab.com',
    'project_id=86730559',
    '  hmailserver/hmailserver) repository=https://gitlab.com/hmailserver/hmailserver ;;',
    'identity="${repository}//.gitlab-ci.yml@refs/tags/${tag}"',
    'sign_flags="--yes --rekor-url https://rekor.sigstore.dev --fulcio-url https://fulcio.sigstore.dev"',
    '  cosign verify $verify_flags --certificate-identity "$identity" --certificate-oidc-issuer "$issuer" \\',
    '    "id=$project_id") ;;',
)
SIGN_APPENDS = ('  *--use-signing-config*) sign_flags="$sign_flags --use-signing-config=false" ;;',
                '  *--new-bundle-format*) sign_flags="$sign_flags --new-bundle-format=false" ;;')
SIGN_SET = re.compile(r'(?:^|[\s;&|(])(?:issuer|project_id|repository|identity|sign_flags)=')
SIGSTORE_NAME = re.compile(r'(?<![A-Za-z0-9_])(?:SIGSTORE|COSIGN)_[A-Z0-9_]+')
# operator-image-push.sh's dotenv report, which operator-image-sign takes.
PUSH_ENV_BLOCK = ('{\n'
                  '  echo "HM_OPERATOR_IMAGE=$repository@$index"\n'
                  '  echo "HM_OPERATOR_IMAGE_AMD64=$repository@$amd64"\n'
                  '  echo "HM_OPERATOR_IMAGE_ARM64=$repository@$arm64"\n'
                  '} > "$out/operator.env"\n')
PUSH_ENV_LINES = ('} > "$out/operator.env"', 'cat "$out/operator.env"')


def read(path):
    with open(os.path.join(ROOT, path), encoding='utf-8') as handle:
        return handle.read()


def load_texts():
    texts = {path: read(path) for path in FILES}
    others = {}
    for path in ['.gitlab-ci.yml'] + sorted('.gitlab/ci/' + name for name in os.listdir(os.path.join(ROOT, '.gitlab', 'ci'))):
        if path != OPERATOR_CI and path.endswith(('.yml', '.yaml')):
            others[path] = read(path)
    texts[OTHER_CI] = others
    return texts


class CiLoader(yaml.SafeLoader):
    """GitLab's YAML: a !reference as a tuple, and a key given twice refused, where
    PyYAML would keep the last."""

    def construct_mapping(self, node, deep=False):
        seen = set()
        for key_node, _ in node.value:
            key = self.construct_object(key_node, deep=True)
            if key in seen:
                raise yaml.constructor.ConstructorError(None, None, 'the key %r is given twice' % (key,), key_node.start_mark)
            seen.add(key)
        return yaml.SafeLoader.construct_mapping(self, node, deep)


CiLoader.add_constructor('!reference', lambda loader, node: ('!reference',) + tuple(loader.construct_sequence(node)))


def dockerfile_instructions(text):
    """The Dockerfile's instructions as Docker and kaniko read them, (WORD, arguments)
    with the word in upper case and the arguments' white space made single spaces, and
    the problems of reading it: a parser directive, which could change the escape
    character or fetch a frontend, and a comment or a blank line inside a continued
    instruction, which this reads more strictly than Docker does."""
    instructions, problems, pending = [], [], None
    for number, line in enumerate(text.replace('\r\n', '\n').split('\n'), 1):
        stripped = line.strip()
        if stripped.startswith('#'):
            if DIRECTIVE.match(line):
                problems.append("the Dockerfile has a parser directive on its line %d" % number)
            if pending is not None:
                problems.append("the Dockerfile has a comment inside a continued instruction on its line %d" % number)
            continue
        if not stripped:
            if pending is not None:
                problems.append("the Dockerfile has a blank line inside a continued instruction on its line %d" % number)
            continue
        continued = re.search(r'\\[ \t]*$', line)
        pending = (pending or '') + ' ' + (line[:continued.start()] if continued else line)
        if continued:
            continue
        words = pending.split()
        pending = None
        instructions.append((words[0].upper(), ' '.join(words[1:])))
    if pending is not None:
        problems.append("the Dockerfile ends inside a continued instruction")
    return instructions, problems


def dockerfile_problems(text):
    """The Dockerfile's problems, and its operator-sdk release when its FROM is the Helm
    operator by tag and digest."""
    instructions, problems = dockerfile_instructions(text)
    release = None
    froms = [rest for word, rest in instructions if word == 'FROM']
    if len(froms) != 1 or not instructions or instructions[0][0] != 'FROM' or not BASE_IMAGE.match(froms[0]):
        problems.append("the Dockerfile's FROM is not one, first, quay.io/operator-framework/helm-operator by tag and digest "
                        "with no flag or stage name")
    else:
        release = BASE_IMAGE.match(froms[0]).group(1)
    for word in sorted({word for word, _ in instructions} - DOCKERFILE_WORDS):
        problems.append("the Dockerfile has the instruction %s; it may have FROM, ARG, LABEL, ENV, WORKDIR and its two COPY lines alone" % word)
    copies = [rest for word, rest in instructions if word == 'COPY']
    if copies != DOCKERFILE_COPIES:
        problems.append("the Dockerfile's COPY lines are %s, not exactly %s" % (copies, DOCKERFILE_COPIES))
    return problems, release


def allowed_grants(group, kinds):
    """(apiGroup, resource) -> the verbs the CSV may grant it, for the chart's kinds."""
    allowed = {KINDS[name]: VERBS for name in kinds if name in KINDS}
    allowed[HELM_STORAGE] = VERBS
    allowed.update(ALLOWED_OWN)
    for resource in OWN_RESOURCES:
        allowed[(group, resource)] = OWN_VERBS
    return allowed


def ci_problems(texts):
    """The release jobs of operator.yml, and the lines of the sign and push scripts that
    say as whom it signs and what the sign job is handed."""
    problems = []
    try:
        jobs = yaml.load(texts[OPERATOR_CI], Loader=CiLoader)
    except yaml.YAMLError as error:
        return ['%s does not parse as this check reads it: %s' % (OPERATOR_CI, ' '.join(str(error).split()))]
    if not isinstance(jobs, dict) or set(jobs) != set(OPERATOR_JOBS):
        problems.append('%s holds %s, not the three release jobs alone' % (OPERATOR_CI, sorted(jobs or {}) if isinstance(jobs, dict) else jobs))
        jobs = jobs if isinstance(jobs, dict) else {}
    for name in sorted(OPERATOR_JOBS):
        job, held = jobs.get(name), OPERATOR_JOBS[name]
        if job is None or job == held:
            continue
        if not isinstance(job, dict):
            problems.append('%s in %s is not a job' % (name, OPERATOR_CI))
            continue
        differ = sorted(key for key in set(job) | set(held) if job.get(key) != held.get(key))
        problems.append('%s in %s is not as this check holds it (OPERATOR_JOBS): %s' % (name, OPERATOR_CI, ', '.join(differ)))
    for path, text in sorted(texts[OTHER_CI].items()):
        for match in JOB_LINE.finditer(text):
            problems.append('%s defines %s again, which GitLab merges into the release job' % (path, match.group(1)))

    sign = texts[SIGN_SCRIPT]
    code = [line for line in sign.split('\n') if not line.lstrip().startswith('#')]
    for line in SIGN_LINES:
        if code.count(line) != 1:
            problems.append('%s does not have the line %r once' % (SIGN_SCRIPT, line))
    for line in code:
        if SIGN_SET.search(line) and line not in SIGN_LINES + SIGN_APPENDS:
            problems.append('%s sets what it signs as on another line: %r' % (SIGN_SCRIPT, line.strip()))
    for name in sorted(set(SIGSTORE_NAME.findall('\n'.join(code))) - {'SIGSTORE_ID_TOKEN'}):
        problems.append('%s names %s, which cosign reads; it may name the ID token alone' % (SIGN_SCRIPT, name))

    push = texts[PUSH_SCRIPT]
    if push.count(PUSH_ENV_BLOCK) != 1:
        problems.append('%s does not write its dotenv report as its three lines alone, once (PUSH_ENV_BLOCK)' % PUSH_SCRIPT)
    code = [line for line in push.split('\n') if not line.lstrip().startswith('#')]
    for line in code:
        if '.env' in line and line not in PUSH_ENV_LINES:
            problems.append('%s names a .env file on another line: %r' % (PUSH_SCRIPT, line.strip()))
        if '>>' in line or re.search(r'(?<![\w-])tee(?![\w-])', line):
            problems.append('%s appends to a file or runs tee: %r' % (PUSH_SCRIPT, line.strip()))
        if re.search(r'(?<![\w-])cd(?![\w-])', line) and not re.match(r'^\s*\(\s*cd\s', line):
            problems.append('%s changes its directory outside a subshell: %r' % (PUSH_SCRIPT, line.strip()))
    if [line for line in code if re.search(r'(?:^|[\s;&|(])out=', line)] != ['out=operator-image-push']:
        problems.append('%s does not set out once, as out=operator-image-push' % PUSH_SCRIPT)
    return problems


def check(texts, rendered):
    """The problems in texts (path -> text), given rendered: path -> the kinds the
    chart rendered for one set of values."""
    problems = []
    try:
        crd = yaml.safe_load(texts[CRD])
        csv = yaml.safe_load(texts[CSV])
        watches = yaml.safe_load(texts[WATCHES])
        values = yaml.safe_load(texts[VALUES])
        annotations = yaml.safe_load(texts[ANNOTATIONS])['annotations']
        scorecard = yaml.safe_load(texts[SCORECARD])
    except (yaml.YAMLError, KeyError, TypeError) as error:
        return ['a file of the operator does not parse: %s' % error]

    # The CRD.
    group = crd['spec']['group']
    kind = crd['spec']['names']['kind']
    versions = crd['spec']['versions']
    if len(versions) != 1:
        problems.append('the CRD serves %d versions; this holds one' % len(versions))
    version = versions[0]['name']
    spec = versions[0]['schema']['openAPIV3Schema']['properties']['spec']
    properties = spec.get('properties', {})
    if spec.get('x-kubernetes-preserve-unknown-fields'):
        problems.append("the CRD's spec keeps unknown fields, so a misspelt value reaches the chart unread")
    missing = sorted(set(values) - set(properties))
    extra = sorted(set(properties) - set(values))
    if missing:
        problems.append("the CRD's spec has no property for the chart's values %s" % ', '.join(missing))
    if extra:
        problems.append("the CRD's spec has properties the chart has no value for: %s" % ', '.join(extra))
    for top, field in (('database', 'password'), ('administrator', 'password')):
        node = properties.get(top, {}).get('properties', {}).get(field)
        if not node or node.get('maxLength') != 0:
            problems.append('the CRD lets a resource give %s.%s, which anyone who may read the resource could read' % (top, field))
    for field in ('existingSecret', 'database'):
        if field not in spec.get('required', []):
            problems.append('the CRD does not require %s' % field)
    if properties.get('existingSecret', {}).get('minLength') != 1:
        problems.append("the CRD's existingSecret may be empty")
    if 'host' not in properties.get('database', {}).get('required', []):
        problems.append("the CRD does not require database.host, which the chart requires")
    plural = crd['spec']['names']['plural']
    crd_name = '%s.%s' % (plural, group)
    if crd['metadata']['name'] != crd_name:
        problems.append("the CRD is named %s, not %s" % (crd['metadata']['name'], crd_name))

    # watches.yaml.
    if not isinstance(watches, list) or len(watches) != 1:
        problems.append('watches.yaml does not watch exactly one kind')
    else:
        watch = watches[0]
        if (watch.get('group'), watch.get('version'), watch.get('kind')) != (group, version, kind):
            problems.append('watches.yaml watches %s/%s %s, not the CRD\'s %s/%s %s' % (
                watch.get('group'), watch.get('version'), watch.get('kind'), group, version, kind))
        if watch.get('chart') != 'helm-charts/hmailserver':
            problems.append('watches.yaml does not name the chart helm-charts/hmailserver')

    # The CSV.
    owned = csv['spec'].get('customresourcedefinitions', {}).get('owned', [])
    if len(owned) != 1 or (owned[0].get('name'), owned[0].get('kind'), owned[0].get('version')) != (crd_name, kind, version):
        problems.append('the CSV does not own exactly the CRD %s (%s %s)' % (crd_name, kind, version))
    meta = csv['metadata'].get('annotations', {})
    if 'Progressive Robot\'s fork' not in csv['spec'].get('displayName', ''):
        problems.append("the CSV's displayName does not say it is Progressive Robot's fork")
    for where, text in (('spec.description', csv['spec'].get('description', '')),
                        ('description annotation', meta.get('description', ''))):
        flat = ' '.join(text.replace('**', '').split())
        if FORK not in flat:
            problems.append("the CSV's %s does not say %s" % (where, FORK))
    if ORIGINAL not in ' '.join(csv['spec'].get('description', '').split()):
        problems.append("the CSV's description does not name the original's author, %s" % ORIGINAL)
    install = csv['spec']['install']['spec']
    if install.get('clusterPermissions'):
        problems.append('the CSV asks for cluster permissions; the operator needs namespaced ones alone')
    kinds = set()
    for name in sorted(rendered):
        kinds.update(rendered[name])
    allowed = allowed_grants(group, kinds)
    deployments = install.get('deployments', [])
    accounts = [d.get('spec', {}).get('template', {}).get('spec', {}).get('serviceAccountName') for d in deployments]
    entries = install.get('permissions', [])
    if len(deployments) != 1 or not accounts[0]:
        problems.append('the CSV does not install exactly one deployment with a serviceAccountName')
    elif len(entries) != 1 or entries[0].get('serviceAccountName') != accounts[0]:
        problems.append("the CSV's permissions are not one entry for %s, the service account its deployment runs as" % accounts[0])
    granted = {}
    for entry in entries:
        for key in sorted(set(entry) - {'serviceAccountName', 'rules'}):
            problems.append("a permissions entry of the CSV gives %s" % key)
        for rule in entry.get('rules', []):
            for key in sorted(set(rule) - RULE_KEYS):
                problems.append('a rule of the CSV gives %s, which this check does not read' % key)
            for field in ('apiGroups', 'resources', 'verbs'):
                if '*' in rule.get(field, []):
                    problems.append('a rule of the CSV grants * in %s' % field)
            for api_group in rule.get('apiGroups', []):
                for resource in rule.get('resources', []):
                    verbs = set(rule.get('verbs', []))
                    granted.setdefault((api_group, resource), set()).update(verbs)
                    if (api_group, resource) not in allowed:
                        problems.append('the CSV grants %s on %s in the API group %r, which neither the chart nor the operator needs'
                                        % (', '.join(sorted(verbs)), resource, api_group))
                    elif verbs - allowed[(api_group, resource)]:
                        problems.append('the CSV grants %s on %s, beyond its own %s'
                                        % (', '.join(sorted(verbs - allowed[(api_group, resource)])), resource,
                                           ', '.join(sorted(allowed[(api_group, resource)]))))
    for name in sorted(kinds):
        if name not in KINDS:
            problems.append('the chart renders a %s, which this check does not know: give the CSV its permission and add it here' % name)
            continue
        lacking = sorted(VERBS - granted.get(KINDS[name], set()))
        if lacking:
            problems.append('the CSV does not let the operator %s %s' % (', '.join(lacking), KINDS[name][1]))
    for resource in ('hmailservers', 'hmailservers/status', 'hmailservers/finalizers'):
        if not {'get', 'update', 'patch'} <= granted.get((group, resource), set()):
            problems.append('the CSV does not let the operator get, update and patch %s' % resource)
    if not {'get', 'create', 'update'} <= granted.get(('coordination.k8s.io', 'leases'), set()):
        problems.append('the CSV does not let the operator hold its leader lease')
    listed = sorted(r.get('kind') for r in (owned[0].get('resources', []) if owned else []))
    if listed != sorted(kinds):
        problems.append("the CSV lists %s as the CRD's resources; the chart renders %s" % (listed, sorted(kinds)))
    modes = {m['type']: m['supported'] for m in csv['spec'].get('installModes', [])}
    for mode in ('OwnNamespace', 'AllNamespaces'):
        if not modes.get(mode):
            problems.append('the CSV does not support the install mode %s' % mode)
    try:
        examples = json.loads(meta.get('alm-examples', ''))
    except ValueError:
        examples = None
        problems.append("the CSV's alm-examples is not JSON")
    if examples is not None:
        if len(examples) != 1:
            problems.append("the CSV's alm-examples holds %d resources, not one" % len(examples))
        for example in examples:
            if example.get('kind') != kind or example.get('apiVersion') != '%s/%s' % (group, version):
                problems.append("the CSV's example is not a %s of %s/%s" % (kind, group, version))
            example_spec = example.get('spec', {})
            unknown = sorted(set(example_spec) - set(properties))
            if unknown:
                problems.append("the CSV's example gives %s, which the CRD has no property for" % ', '.join(unknown))
            for top in ('database', 'administrator'):
                if example_spec.get(top, {}).get('password'):
                    problems.append("the CSV's example gives %s.password" % top)
    for descriptor in owned[0].get('specDescriptors', []) if owned else []:
        node = {'properties': properties}
        for part in descriptor.get('path', '').split('.'):
            node = node.get('properties', {}).get(part)
            if node is None:
                problems.append("the CSV's spec descriptor %s names no property of the CRD" % descriptor.get('path'))
                break

    # The Dockerfile, and one operator-sdk release, every image by its digest.
    found, release = dockerfile_problems(texts[DOCKERFILE])
    problems.extend(found)
    builders = {annotations.get('operators.operatorframework.io.metrics.builder'),
                meta.get('operators.operatorframework.io/builder')}
    if release and builders != {'operator-sdk-v%s' % release}:
        problems.append('the bundle names the builders %s, not operator-sdk-v%s, the Dockerfile\'s' % (sorted(b or '' for b in builders), release))
    for stage in scorecard.get('stages', []):
        for test in stage.get('tests', []):
            image = test.get('image', '')
            match = PINNED.match(image)
            if not match or not image.startswith('quay.io/operator-framework/scorecard-test:'):
                problems.append('the scorecard test %s is not scorecard-test by tag and digest' % test.get('labels', {}).get('test'))
            elif release and match.group(1) != release:
                problems.append('the scorecard test %s is operator-sdk %s\'s, not %s' % (test.get('labels', {}).get('test'), match.group(1), release))
    return problems + ci_problems(texts)


def render_kinds(spec_values):
    """The kinds helm template renders the tree's chart with these values."""
    with tempfile.NamedTemporaryFile('w', suffix='.yaml', delete=False, encoding='utf-8') as handle:
        yaml.safe_dump(spec_values, handle)
        name = handle.name
    try:
        out = subprocess.run(['helm', 'template', 'mail', os.path.join(ROOT, CHART), '-f', name],
                             check=True, capture_output=True, text=True).stdout
    finally:
        os.unlink(name)
    return sorted({doc['kind'] for doc in yaml.safe_load_all(out) if doc})


def run_scripts():
    """operator-context.sh and operator-bundle.sh, run as the release jobs run them."""
    problems = []
    work = tempfile.mkdtemp()
    try:
        for args in ([os.path.join(work, 'plain')], [os.path.join(work, 'stamped'), '9.9.9']):
            done = subprocess.run(['sh', 'build/ci/operator-context.sh'] + args, cwd=ROOT, capture_output=True, text=True)
            print(done.stdout.strip())
            if done.returncode != 0:
                problems.append('operator-context.sh %s failed: %s' % (' '.join(args[1:]) or '(no version)', done.stdout.strip().splitlines()[-1:]))
        chart_yaml = os.path.join(work, 'stamped', 'helm-charts', 'hmailserver', 'Chart.yaml')
        if os.path.exists(chart_yaml):
            stamped = yaml.safe_load(open(chart_yaml, encoding='utf-8'))
            if (stamped.get('version'), stamped.get('appVersion')) != ('9.9.9', '9.9.9'):
                problems.append("operator-context.sh 9.9.9 did not stamp the chart's version and appVersion")
        image = 'registry.gitlab.com/hmailserver/hmailserver/operator@sha256:' + '0' * 64
        done = subprocess.run(['sh', 'build/ci/operator-bundle.sh', '9.9.9', image, os.path.join(work, 'bundle')],
                              cwd=ROOT, capture_output=True, text=True)
        print(done.stdout.strip().splitlines()[-1] if done.stdout.strip() else '')
        if done.returncode != 0:
            problems.append('operator-bundle.sh failed: %s' % done.stdout.strip().splitlines()[-1:])
            return problems
        made = os.path.join(work, 'bundle', 'hmailserver-operator', '9.9.9', 'manifests', 'hmailserver-operator.clusterserviceversion.yaml')
        csv = yaml.safe_load(open(made, encoding='utf-8'))
        if csv['metadata']['name'] != 'hmailserver-operator.v9.9.9' or csv['spec']['version'] != '9.9.9':
            problems.append("operator-bundle.sh did not stamp the CSV's name and version")
        container = csv['spec']['install']['spec']['deployments'][0]['spec']['template']['spec']['containers'][0]
        related = [r['image'] for r in csv['spec'].get('relatedImages', [])]
        if (csv['metadata']['annotations'].get('containerImage'), container.get('image'), related) != (image, image, [image]):
            problems.append("operator-bundle.sh did not name the operator image by its digest everywhere")
        icon = base64.b64decode(csv['spec']['icon'][0]['base64data'])
        with open(os.path.join(ROOT, ICON), 'rb') as handle:
            if icon != handle.read():
                problems.append("the bundle's icon is not platform/packaging/hmailserver.png")
    finally:
        shutil.rmtree(work, ignore_errors=True)
    return problems


def self_test():
    texts = load_texts()
    rendered = {'example': ['Service', 'StatefulSet'], 'passwords': ['Secret', 'Service', 'StatefulSet']}
    failures = 0
    if check(texts, rendered):
        print('self-test: the tree itself is refused:', check(texts, rendered))
        failures += 1

    def mutate(path, old, new):
        changed = dict(texts)
        if old not in changed[path]:
            raise SystemExit('self-test: %r is not in %s' % (old, path))
        changed[path] = changed[path].replace(old, new, 1)
        return changed

    def defined_in(path, text):
        changed = dict(texts)
        changed[OTHER_CI] = dict(texts[OTHER_CI])
        changed[OTHER_CI][path] = changed[OTHER_CI][path] + text
        return changed

    # The pins as the tree has them, so that a move of them, as Renovate makes one, is
    # no change to this test.
    base = re.search(r'^FROM (quay\.io/operator-framework/helm-operator:v[0-9.]+)(@sha256:[0-9a-f]{64})$', texts[DOCKERFILE], re.MULTILINE)
    if not base:
        raise SystemExit("self-test: the Dockerfile's FROM is not the Helm operator by tag and digest")
    scorecard_tag = re.search(r'scorecard-test:(v[0-9.]+)@', texts[SCORECARD]).group(1)
    rules_end = '              resources: [leases]\n              verbs: [get, list, watch, create, update, patch, delete]\n'
    cases = [
        ('a chart value without a CRD property', mutate(VALUES, '\nextraEnv: []', '\nextraEnv: []\nnewValue: 1'), rendered),
        ('the database password allowed', mutate(CRD, '                      maxLength: 0\n', '', ), rendered),
        ('the spec left open', mutate(CRD, '              required: [database, existingSecret]',
                                      '              x-kubernetes-preserve-unknown-fields: true\n              required: [database, existingSecret]'), rendered),
        ('existingSecret not required', mutate(CRD, 'required: [database, existingSecret]', 'required: [database]'), rendered),
        ('watches.yaml another group', mutate(WATCHES, '- group: mail.progressiverobot.com', '- group: mail.example.com'), rendered),
        ('a cluster permission', mutate(CSV, '      permissions:\n', '      clusterPermissions:\n        - serviceAccountName: x\n          rules: []\n      permissions:\n'), rendered),
        ('a wildcard verb', mutate(CSV, 'resources: [statefulsets]\n              verbs: [get, list, watch, create, update, patch, delete]',
                                   'resources: [statefulsets]\n              verbs: ["*"]'), rendered),
        ('services not granted', mutate(CSV, 'resources: [secrets, services]', 'resources: [secrets]'), rendered),
        ('a kind the chart renders that this does not know', texts, dict(rendered, extra=['ConfigMap'])),
        ('the description without the fork', mutate(CSV, "**Progressive Robot's fork of hMailServer**", '**hMailServer**'), rendered),
        ('the tile without the fork', mutate(CSV, "      Progressive Robot's fork of hMailServer, not the original:", '      hMailServer:'), rendered),
        ('a password in the example', mutate(CSV, '"host": "postgres.hmailserver.svc.cluster.local"',
                                            '"host": "postgres.hmailserver.svc.cluster.local", "password": "x"'), rendered),
        ('a descriptor of no property', mutate(CSV, '- path: rest.tlsSecretName', '- path: rest.certificate'), rendered),
        ('the CRD resources not the chart\'s', mutate(CSV, '          - kind: Secret\n            version: v1\n', ''), rendered),
        ('the base image by tag alone', mutate(DOCKERFILE, base.group(0), 'FROM ' + base.group(1)), rendered),
        ('a scorecard image of another release', mutate(SCORECARD, 'scorecard-test:%s@' % scorecard_tag, 'scorecard-test:v1.41.1@'), rendered),
        ('AllNamespaces not supported', mutate(CSV, '    - type: AllNamespaces\n      supported: true', '    - type: AllNamespaces\n      supported: false'), rendered),
        # The permissions: nothing beyond what the chart and the operator need, for the
        # deployment's own service account.
        ('serviceaccounts/token granted', mutate(CSV, rules_end, rules_end + '            - apiGroups: [""]\n              resources: [serviceaccounts/token]\n              verbs: [create]\n'), rendered),
        ('pods/exec granted', mutate(CSV, rules_end, rules_end + '            - apiGroups: [""]\n              resources: [pods/exec]\n              verbs: [create]\n'), rendered),
        ('roles and role bindings granted', mutate(CSV, rules_end, rules_end + '            - apiGroups: [rbac.authorization.k8s.io]\n              resources: [roles, rolebindings]\n              verbs: [get, create]\n'), rendered),
        ('a verb beyond its own on events', mutate(CSV, 'resources: [events]\n              verbs: [create, patch]', 'resources: [events]\n              verbs: [create, patch, delete]'), rendered),
        ('a verb no client uses', mutate(CSV, 'resources: [events]\n              verbs: [create, patch]', 'resources: [events]\n              verbs: [create, patch, escalate]'), rendered),
        ('the permissions for another service account', mutate(CSV, '        - serviceAccountName: hmailserver-operator-controller-manager\n          rules:',
                                                               '        - serviceAccountName: default\n          rules:'), rendered),
        ('a second permissions entry', mutate(CSV, rules_end, rules_end + '        - serviceAccountName: default\n          rules:\n            - apiGroups: [""]\n              resources: [events]\n              verbs: [create]\n'), rendered),
        ('a grant narrowed by resourceNames', mutate(CSV, 'resources: [events]\n              verbs: [create, patch]', 'resources: [events]\n              resourceNames: [x]\n              verbs: [create, patch]'), rendered),
        # The Dockerfile, read as Docker and kaniko read it.
        ('a run in lower case', mutate(DOCKERFILE, 'WORKDIR ${HOME}\n', 'WORKDIR ${HOME}\nrun echo x\n'), rendered),
        ('a RUN indented', mutate(DOCKERFILE, 'WORKDIR ${HOME}\n', 'WORKDIR ${HOME}\n  RUN echo x\n'), rendered),
        ('a second stage in lower case', mutate(DOCKERFILE, 'WORKDIR ${HOME}\n', 'WORKDIR ${HOME}\nfrom alpine:3\n'), rendered),
        ('a COPY from another image', mutate(DOCKERFILE, 'COPY watches.yaml ${HOME}/watches.yaml', 'COPY --from=alpine:latest /bin/busybox ${HOME}/watches.yaml'), rendered),
        ('an ONBUILD', mutate(DOCKERFILE, 'WORKDIR ${HOME}\n', 'WORKDIR ${HOME}\nOnBuild RUN echo x\n'), rendered),
        ('an ADD of a URL', mutate(DOCKERFILE, 'WORKDIR ${HOME}\n', 'WORKDIR ${HOME}\nadd https://example.com/x /x\n'), rendered),
        ('a parser directive', mutate(DOCKERFILE, '# https://www.progressiverobot.com\n', '# syntax=docker/dockerfile:1\n# https://www.progressiverobot.com\n'), rendered),
        ('a USER', mutate(DOCKERFILE, 'WORKDIR ${HOME}\n', 'WORKDIR ${HOME}\nUSER root\n'), rendered),
        ('the FROM with a stage name', mutate(DOCKERFILE, base.group(0), base.group(0) + ' AS operator'), rendered),
        # The release jobs and what their scripts are handed.
        ('the sign job in another image', mutate(OPERATOR_CI, '    name: $HM_COSIGN_IMAGE\n', '    name: alpine:3.24\n'), rendered),
        ('the ID token for another audience', mutate(OPERATOR_CI, '      aud: sigstore', '      aud: fulcio'), rendered),
        ('a second ID token', mutate(OPERATOR_CI, '      aud: sigstore\n', '      aud: sigstore\n    PROBE_ID_TOKEN:\n      aud: probe\n'), rendered),
        ('a second script line', mutate(OPERATOR_CI, '    - sh build/ci/operator-image-sign.sh\n', '    - sh build/ci/operator-image-sign.sh\n    - env\n'), rendered),
        ('an after_script', mutate(OPERATOR_CI, '    - sh build/ci/operator-image-push.sh\n', '    - sh build/ci/operator-image-push.sh\n  after_script:\n    - echo X=1 >> operator-image-push/operator.env\n'), rendered),
        ('a key given twice', mutate(OPERATOR_CI, '    - sh build/ci/operator-image-sign.sh\n', '    - sh build/ci/operator-image-sign.sh\n  script:\n    - sh build/ci/operator-image-sign.sh\n'), rendered),
        ('the chart taken from an artifact again', mutate(OPERATOR_CI, '    - job: container-image-sign\n      artifacts: false\n',
                                                         '    - job: helm-chart-package\n      artifacts: true\n    - job: container-image-sign\n      artifacts: false\n'), rendered),
        ('a fourth job', mutate(OPERATOR_CI, '\noperator-image-push:', '\noperator-probe:\n  script: [env]\n\noperator-image-push:'), rendered),
        ('the sign job defined again', defined_in('.gitlab-ci.yml', '\noperator-image-sign:\n  variables:\n    SIGSTORE_REKOR_URL: https://rekor.example\n'), rendered),
        ('another issuer', mutate(SIGN_SCRIPT, 'issuer=https://gitlab.com\n', 'issuer=https://gitlab.example\n'), rendered),
        ('another project id', mutate(SIGN_SCRIPT, 'project_id=86730559\n', 'project_id=1\n'), rendered),
        ('the identity set again', mutate(SIGN_SCRIPT, 'operator_repository="${CI_REGISTRY_IMAGE:-}/operator"\n',
                                          'operator_repository="${CI_REGISTRY_IMAGE:-}/operator"\nidentity="https://gitlab.example//x"\n'), rendered),
        ('another Rekor', mutate(SIGN_SCRIPT, '--rekor-url https://rekor.sigstore.dev', '--rekor-url https://rekor.example'), rendered),
        ('a cosign variable set', mutate(SIGN_SCRIPT, 'sign_flags="--yes', 'export SIGSTORE_REKOR_URL=https://rekor.example\nsign_flags="--yes'), rendered),
        ('a fourth line in the dotenv report', mutate(PUSH_SCRIPT, '  echo "HM_OPERATOR_IMAGE_ARM64=$repository@$arm64"\n',
                                                      '  echo "HM_OPERATOR_IMAGE_ARM64=$repository@$arm64"\n  echo "SIGSTORE_REKOR_URL=https://rekor.example"\n'), rendered),
        ('the dotenv report appended to', mutate(PUSH_SCRIPT, 'cat "$out/operator.env"\n', 'echo X=1 >> "$out/operator.env"\ncat "$out/operator.env"\n'), rendered),
        ('the dotenv report written by tee', mutate(PUSH_SCRIPT, 'cat "$out/operator.env"\n', 'echo X=1 | tee -a "$out/$name"\ncat "$out/operator.env"\n'), rendered),
        ('out set again', mutate(PUSH_SCRIPT, 'mkdir -p "$out"\n', 'mkdir -p "$out"\nout=elsewhere\n'), rendered),
        ('a cd before the report', mutate(PUSH_SCRIPT, '{\n  echo "HM_OPERATOR_IMAGE=', 'cd /tmp\n{\n  echo "HM_OPERATOR_IMAGE='), rendered),
    ]
    for name, changed, given in cases:
        found = check(changed, given)
        if found:
            print('self-test: %s: refused (%s)' % (name, found[0]))
        else:
            print('self-test: %s: NOT refused' % name)
            failures += 1
    print('self-test: %d failure(s)' % failures)
    return 1 if failures else 0


def main():
    if sys.argv[1:] == ['--self-test']:
        return self_test()
    if sys.argv[1:]:
        print(__doc__)
        return 2
    texts = load_texts()
    csv = yaml.safe_load(texts[CSV])
    example = json.loads(csv['metadata']['annotations']['alm-examples'])[0]['spec']
    with_passwords = copy.deepcopy(example)
    with_passwords.pop('existingSecret', None)
    with_passwords.setdefault('database', {})['password'] = 'x'
    with_passwords.setdefault('administrator', {})['password'] = 'y'
    rendered = {'example': render_kinds(example), 'passwords': render_kinds(with_passwords)}
    print('the chart renders %s for the CSV\'s example and %s with the passwords in its own Secret'
          % (', '.join(rendered['example']), ', '.join(rendered['passwords'])))
    problems = check(texts, rendered) + run_scripts()
    for problem in problems:
        print('OPERATOR CHECK FAILED: ' + problem)
    if problems:
        return 1
    print('The operator holds to the chart and to itself, and its context and bundle are written as a release writes them.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
