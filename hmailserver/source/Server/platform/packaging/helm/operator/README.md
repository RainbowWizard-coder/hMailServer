# The Kubernetes operator

A Kubernetes operator for **Progressive Robot's fork of hMailServer**, not the original
hMailServer by Martin Knafve, which is developed separately
([the fork and the original](../../../../../../docs/ForkAndOriginal.md)). Each
`HMailServer` resource is one installation of the project's own Helm chart,
[`../hmailserver`](../hmailserver/README.md), with the resource's spec as the chart's
values. It is operator-sdk's Helm operator, the standard way operator-sdk makes an
operator from a chart, packaged for Operator Lifecycle Manager (OLM) as OperatorHub.io
lists operators.

It sits beside the chart because it is the chart: nothing here templates a resource of
its own, and the chart is not kept a second time in the tree but copied into the image
when it is built (`build/ci/operator-context.sh`), and held there to the tree's chart
file by file.

## What is here

| File | What it is |
| --- | --- |
| `Dockerfile` | The image: `quay.io/operator-framework/helm-operator` (operator-sdk 1.42.3) by digest, with `watches.yaml` and the chart. No `RUN`: kaniko builds every platform without running anything. |
| `watches.yaml` | The one kind the operator watches, `HMailServer` in `mail.progressiverobot.com/v1alpha1`, and the chart it installs for it. The group is a name under progressiverobot.com, the domain the project's owner holds, so that no one else's can be the same. |
| `bundle/manifests/mail.progressiverobot.com_hmailservers.yaml` | The custom resource definition: one property for each of the chart's top-level values and no other. |
| `bundle/manifests/hmailserver-operator.clusterserviceversion.yaml` | What OLM installs and OperatorHub.io shows: the description, the icon, the operator's deployment and its permissions. |
| `bundle/metadata/annotations.yaml` | The bundle's package (`hmailserver-operator`) and its channel (`alpha`). |
| `bundle/tests/scorecard/config.yaml` | operator-sdk scorecard's tests of the bundle. |

The bundle spells no release out, as the chart's `Chart.yaml` does not: the CSV's
version is 0.1.0, its image `:latest`, its date and icon placeholders.
`build/ci/operator-bundle.sh VERSION IMAGE OUT` writes a release's bundle from it, in
the layout OperatorHub.io's repository takes (`operators/hmailserver-operator/<version>/`):
the version, the operator image by its digest, the date, and the fork's icon,
`platform/packaging/hmailserver.png`, inlined.

## The resource

```yaml
apiVersion: mail.progressiverobot.com/v1alpha1
kind: HMailServer
metadata:
  name: mail
spec:
  database:
    type: PostgreSQL
    host: postgres.hmailserver.svc.cluster.local
  existingSecret: mail-passwords
  rest:
    tlsSecretName: mail-tls
```

Every other value is the chart's (its README says what each does). Two rules are the
operator's own:

- **No password in the resource.** `database.password` and `administrator.password` may
  only be empty, since anyone who may read the resource could read them;
  `existingSecret` is required and names a Secret in the resource's namespace holding
  `database-password` and `administrator-password` (or the keys `existingSecretKeys`
  gives).
- **One server.** `replicaCount` is at most 1: a mail server's message store is not
  something two pods may share.

A misspelt value is refused (`kubectl apply` says `unknown field`), as the definition
lists the chart's values and keeps no unknown field.

Before making one: a PostgreSQL or MariaDB the cluster reaches (the database is not in
the chart); the passwords' Secret; a TLS Secret for the REST listener, which off loopback
requires TLS; and port 25 reachable from the internet, which many providers block.
Deleting the resource uninstalls the release; its three PersistentVolumeClaims stay, as
after `helm uninstall`, so that a deletion never loses the message store.

## Permissions

The CSV asks for namespaced permissions alone, and only these: Secrets, Services and
StatefulSets (what the chart makes, and the Secrets Helm keeps each release in); the
operator's own resources and their status; events; and a lease to lead.
`build/ci/operator-check.py` holds them to the kinds the chart renders. Installed for one
namespace (OwnNamespace or SingleNamespace) they are a Role there; installed for all
namespaces OLM makes them a ClusterRole, and the operator may then read every Secret in
the cluster, as any operator that manages Secrets in every namespace must. No metrics
listener is served, so the operator needs neither TokenReview nor SubjectAccessReview.

## How a release builds it

In a `v*` tag's pipeline, after the release's image is signed (`.gitlab/ci/operator.yml`):

1. `operator-image` (kaniko): the context made by `build/ci/operator-context.sh` - the
   tree's chart, its `version` and `appVersion` stamped with the release's, so that it
   installs the release's own image, held to the release's packaged chart
   (`helm-chart-package`) byte for byte but for `Chart.yaml` - built for linux/amd64 and
   linux/arm64 as two tarballs.
2. `operator-image-push` (regctl): both joined under one index and pushed as
   `registry.gitlab.com/hmailserver/hmailserver/operator:<version>`.
3. `operator-image-sign` (cosign): the index and each platform's image signed with
   Sigstore by digest, as this pipeline (the identity the release's files carry), and
   verified; then the release's bundle written for that index, the job's artifact
   `operator-bundle/`.

Like the server's image, nothing is built or pushed while the project's CI/CD variable
`HM_PUBLISH_IMAGE` is anything but 1. The `chart-and-compose` job runs
`build/ci/operator-check.py` (and its `--self-test`) on every change to the chart or the
operator: the definition, `watches.yaml` and the CSV held to the chart and to each other,
and the context and the bundle written as a release writes them.

## Checking an image

```sh
cosign verify \
  --certificate-identity https://gitlab.com/hmailserver/hmailserver//.gitlab-ci.yml@refs/tags/v<version> \
  --certificate-oidc-issuer https://gitlab.com \
  registry.gitlab.com/hmailserver/hmailserver/operator@sha256:<the bundle's digest>
```

## Submitting a release's bundle to OperatorHub.io

OperatorHub.io lists what is merged into `k8s-operatorhub/community-operators` on GitHub.
For each release whose pipeline has made `operator-bundle/`:

1. Check it: `operator-sdk bundle validate operators/hmailserver-operator/<version>
   --select-optional suite=operatorframework` (and `name=operatorhub`), and
   `operator-sdk scorecard` against a cluster.
2. In a fork of `k8s-operatorhub/community-operators`, copy
   `operators/hmailserver-operator/<version>/` in, with `operators/hmailserver-operator/ci.yaml`
   for the first submission (`updateGraph: semver-mode` and the maintainers' GitHub names
   as reviewers).
3. One commit, signed off (`git commit -s`) in the maintainer's real name, as their
   contribution rules require, and a pull request with their checklist filled in.

## What was run

On 25 October 2026, on an x86-64 Ubuntu machine with Docker 29.1.3, from this tree with
the chart that starts a server (the init container and the user 999):

- `build/ci/helm-chart-package.sh` and `build/ci/operator-image.sh` as the tag pipeline
  runs them (`CI_COMMIT_TAG` the tag of the latest release, the tree's own version, whose
  image exists), in Alpine 3.24's and kaniko 1.24.0's pinned
  images: the chart copied in, nine files, each the tree's and the packaged chart's but
  for `Chart.yaml`'s two stamped lines; both platforms built, and built again twenty
  minutes later to the same bytes (`--reproducible`). Given a package whose template
  differs, a chart file the package lacks, a package of another version, a `Chart.yaml`
  without the placeholder version, a `RUN` in the Dockerfile, its base image by tag alone
  and `watches.yaml` naming another chart, `operator-context.sh` refused each; given an
  image by tag, a version that is not SemVer and a CSV without its placeholder image,
  `operator-bundle.sh` refused each. `operator-bundle.sh` ran in cosign's image as well.
- `build/ci/operator-image-push.sh` in regctl's pinned image against a registry inside the
  test cluster (the repository's name and the registry's TLS the only lines changed),
  and `build/ci/operator-bundle.sh` at that version for the index it pushed.
- `operator-sdk bundle validate` (1.42.3) with `suite=operatorframework`,
  `name=operatorhub` and `name=good-practices`: all passed (1.42.3 knows no
  `operatorhub/v2`, which its warning names). `opm validate` (1.74.0) of a file-based
  catalog holding the bundle: valid.
- A one-node Kubernetes, k3s 1.36.1 from Alpine Linux 3.24's own package, with OLM 0.46.0
  from its release manifests (each held to the digest GitHub's release API gives): the
  catalog served the package; a Subscription in `operators` (AllNamespaces), one in a
  namespace of its own (OwnNamespace) and one in a namespace watching another
  (SingleNamespace) each brought the CSV to Succeeded, the last two with a Role alone, the
  operator's service account allowed to list Secrets in the namespace it watches and
  refused them in another; the CSV's own example resource, beside PostgreSQL 18 in the
  cluster, was Deployed and its pod ready 27 seconds after it was made (the image pulled
  first) and 12 seconds in each of the other two namespaces, offered STARTTLS on 25 with
  a completed TLS 1.3 handshake, answered `GET /api/v1/status` with 401 without
  credentials and the release's version and build as Administrator over TLS and nothing
  over plain HTTP, and served the Control Deck and the webmail. The operator put back a
  Service deleted by hand and a StatefulSet changed by hand, and applied a property set
  in the resource (`maxmessagesize` 30720: the pod rolled and EHLO then offered
  `SIZE 31457280`); the definition refused both passwords, a resource without
  `existingSecret`, two replicas and a misspelt value. Deleting the resource removed the
  StatefulSet, the Service, the pod and Helm's release Secrets, and left the three
  claims. The operator's log said `forbidden` nowhere. Uninstalled, the
  operator left the definition, as Operator Lifecycle Manager does for every operator.
- `operator-sdk scorecard` with the bundle's tests: all six passed (`olm-status-descriptors`
  suggesting a status in the example, which a resource only has once it is made).
