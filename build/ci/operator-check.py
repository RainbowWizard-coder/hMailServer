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
    its description and its tile's; asks for namespaced permissions alone, no
    wildcard among them, each verb one of the seven a client uses; grants every
    kind the chart renders - for the resource of its example and for one that
    gives the passwords in the chart's own Secret - the seven verbs, and lists
    exactly those kinds as the CRD's resources; supports OwnNamespace and
    AllNamespaces; and its example is the CRD's kind, gives no password, and uses
    only the CRD's properties, each of its spec descriptors naming one;
  * the Dockerfile's Helm operator, the builder the bundle's annotations and the CSV
    name, and the scorecard's test images are one operator-sdk release, every image
    by its digest;
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
FILES = (CRD, CSV, ANNOTATIONS, SCORECARD, WATCHES, DOCKERFILE, VALUES)

FORK = "Progressive Robot's fork of hMailServer"
ORIGINAL = 'Martin Knafve'
VERBS = {'get', 'list', 'watch', 'create', 'update', 'patch', 'delete'}
# The kinds the chart may render, as RBAC names them.
KINDS = {'StatefulSet': ('apps', 'statefulsets'), 'Service': ('', 'services'), 'Secret': ('', 'secrets')}
PINNED = re.compile(r'^[a-z0-9.\-/]+:v(\d+\.\d+\.\d+)@sha256:[0-9a-f]{64}$')


def read(path):
    with open(os.path.join(ROOT, path), encoding='utf-8') as handle:
        return handle.read()


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
    granted = {}
    for entry in install.get('permissions', []):
        for rule in entry.get('rules', []):
            for field in ('apiGroups', 'resources', 'verbs'):
                if '*' in rule.get(field, []):
                    problems.append('a rule of the CSV grants * in %s' % field)
            odd = sorted(set(rule.get('verbs', [])) - VERBS - {'*'})
            if odd:
                problems.append('a rule of the CSV grants the verbs %s' % ', '.join(odd))
            for api_group in rule.get('apiGroups', []):
                for resource in rule.get('resources', []):
                    granted.setdefault((api_group, resource), set()).update(rule.get('verbs', []))
    kinds = set()
    for name in sorted(rendered):
        kinds.update(rendered[name])
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

    # One operator-sdk release, every image by its digest.
    froms = [line for line in texts[DOCKERFILE].splitlines() if line.startswith('FROM ')]
    release = None
    if len(froms) != 1 or not PINNED.match(froms[0][5:]) or not froms[0][5:].startswith('quay.io/operator-framework/helm-operator:'):
        problems.append("the Dockerfile's FROM is not quay.io/operator-framework/helm-operator by tag and digest")
    else:
        release = PINNED.match(froms[0][5:]).group(1)
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
    return problems


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
    texts = {path: read(path) for path in FILES}
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
        ('the base image by tag alone', mutate(DOCKERFILE, '@sha256:80e0787ff36e85129d7983dc219736ba8a61e69857fbe46ec7389fddcbbf92f1', ''), rendered),
        ('a scorecard image of another release', mutate(SCORECARD, 'scorecard-test:v1.42.3@', 'scorecard-test:v1.41.1@'), rendered),
        ('AllNamespaces not supported', mutate(CSV, '    - type: AllNamespaces\n      supported: true', '    - type: AllNamespaces\n      supported: false'), rendered),
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
    texts = {path: read(path) for path in FILES}
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
