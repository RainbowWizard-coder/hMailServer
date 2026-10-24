# hMailServer

hMailServer is a mail server: SMTP, submission, IMAP, POP3 and ManageSieve, with a
REST API, the Control Deck (its web administration) and the webmail on one HTTPS
listener. This chart runs it on Kubernetes as a StatefulSet of one, with its
message store, its logs and its configuration on three persistent volumes, from the
project's own container image, `registry.gitlab.com/hmailserver/hmailserver`.

This is Progressive Robot's fork of hMailServer, not the original. The original
hMailServer is Martin Knafve's; it is developed separately and has releases of its
own. The fork's home is <https://gitlab.com/hmailserver/hmailserver>.

The database is not in the chart and never will be: the server keeps its accounts,
its settings and its message index in a PostgreSQL or MariaDB server you run or
rent, and `database.host` names it.

The project: <https://gitlab.com/hmailserver/hmailserver>. The container image and
this chart, and how both are made and signed:
[ContainerImage.md](https://gitlab.com/hmailserver/hmailserver/-/blob/master/hmailserver/docs/ContainerImage.md).

## Install

```
helm repo add hmailserver https://updates.progressiverobot.com/helm/
helm repo update

kubectl create secret tls mail-tls --cert=mail.pem --key=mail.key
helm install mail hmailserver/hmailserver \
  --set database.host=postgres.example.internal \
  --set database.password='the database password' \
  --set administrator.password='the administrator password' \
  --set rest.tlsSecretName=mail-tls
```

Each chart the repository serves is a release's: its version is the release's, and
it installs that release's image unless `image.tag` names another. A pre-release's
chart is listed only to `helm search repo --devel` and installed only with
`--version` or `--devel`.

What that makes:

- a StatefulSet of one pod, running as the image's own user and group, 999, with
  only the capability to bind the mail ports below 1024;
- before the server, a first container that puts the image's `hMailServer.ini` on
  the configuration volume while the volume holds none, and makes the data volume's
  `temp`, `events` and `database` folders, since Kubernetes copies nothing of an
  image's into a new volume;
- a first start that secures the new installation as the packaged service's first
  start does: a self-signed certificate, STARTTLS on 25, 587, 110 and 143, and no
  password taken in the clear from another address until TLS is up;
- a Service of type LoadBalancer with SMTP 25, submission 587 and 465, IMAP 143 and
  993, POP3 110 and 995, ManageSieve 4190, and the REST listener, 8045;
- a Secret with the two passwords, unless `existingSecret` names one you made;
- three PersistentVolumeClaims, which `helm uninstall` leaves in place.

Then `https://<the service's address>:8045/` is the Control Deck (Administrator and
the administrator password) and `/portal` the webmail.

## What it needs

- **A database.** PostgreSQL (`database.type: PostgreSQL`, the default) or MariaDB
  (`MySQL`), reachable from the cluster, on the type's own port (5432 or 3306)
  unless `database.port` names another. On its first start the server creates the
  database's schema if it is empty, and upgrades it when it is behind
  (`database.create`, `database.upgrade`). The image of the release the chart comes
  with carries the client for each, libpq and MariaDB Connector/C; an older image,
  named in `image.tag`, may carry PostgreSQL's alone and reach MariaDB not at all
  (ContainerImage.md says which images carry both).
- **A certificate for the REST listener.** The server requires TLS on any address
  but loopback, so the listener stays off until `rest.tlsSecretName` names a Secret
  of type `kubernetes.io/tls` (cert-manager's, or one you made). The mail ports
  have the self-signed certificate the first start makes until another is set in
  the Control Deck.
- **Two passwords.** `database.password` and `administrator.password`, or a Secret of
  your own with the keys `existingSecretKeys` names. The administrator password is
  stored as a PBKDF2 hash, never as it is.

## Values

Every value is in `values.yaml`, with what it does. The ones most installations set:

| Value | Default | What it is |
|---|---|---|
| `database.type`, `database.host`, `database.port`, `database.name`, `database.user` | `PostgreSQL`, none, the type's own (5432, or 3306 for `MySQL`), `hmailserver`, `hmailserver` | where the database is |
| `database.password`, `administrator.password` | none | the two secrets, unless `existingSecret` is set |
| `existingSecret` | none | a Secret you made, with the two keys of `existingSecretKeys` |
| `rest.tlsSecretName` | none | the REST listener's certificate; the listener waits for it |
| `image.repository`, `image.tag` | the project's registry, the chart's appVersion | the image |
| `settings`, `properties` | none | any other setting, stored in the database at every start |
| `settingsFrom` | none | a setting whose value is a secret, from a key of a Secret you made |
| `service.type`, `service.loadBalancerIP` | `LoadBalancer` | how the mail ports reach the world |
| `persistence.*.size`, `persistence.storageClassName` | 50Gi, 5Gi, 100Mi | the three volumes |

A setting named in `settings`, `properties` or `settingsFrom` is the environment's:
the server stores it at every start and refuses a change to it in the Control Deck
and over the REST API, which the next start would undo. Take it out of the values
to manage it there again.

## Installed into a cluster

On 23 October 2026 the chart was installed as this page says into a one-node
Kubernetes, k3s from Alpine Linux 3.24's own package with Alpine's Helm, beside a
PostgreSQL 18 in the cluster, with the image of the release then published. The
record, with the versions and the image's digest, is in
`hmailserver/docs/ContainerImage.md`, "What was run by hand".

The charts the repository served that day, those published before the chart gave
the pod its user and group by number, do not start a server. Such a chart, added
with `helm repo add` and installed with its `--version`, is the release's file (its
SHA-256 is the index's, and `cosign verify-blob` of its bundle passes), and its pod
never started: "container has runAsNonRoot and image has non-numeric user
(hmailserver), cannot verify user is non-root". Given the user and group by number
(`helm upgrade --reuse-values --set securityContext.runAsUser=999 --set
securityContext.runAsGroup=999`), the pod stayed as it was, in
`CreateContainerConfigError`, until it was deleted (Updates, below, says why). Made
again, it stopped in `CrashLoopBackOff` at "/etc/hmailserver/hMailServer.ini is not
writable by hmailserver": the new, empty configuration volume hid the image's file.
Nor does such a chart run `hmailserver --apply-new-install-defaults --if-marked`
before the server, so a server it started would not secure a new installation.

This chart, which the first release made after 23 October 2026 is the first to
serve, starts one. Installed from the project's tree with `--set image.tag=` the
published release's version, its pod was ready 16 seconds after `helm install`.
Through `kubectl port-forward` and through the Service's LoadBalancer address, port
25 answered with STARTTLS, under the certificate the first start made. The REST API
answered over TLS: 401 without credentials, the release's version and build as
Administrator, nothing over plain HTTP. It served the Control Deck and the webmail.
A message sent over SMTP to an account made over the API was in its inbox. `helm
uninstall` left the three PersistentVolumeClaims, as above. Until that release, the
chart of a checkout of the project
(`hmailserver/source/Server/platform/packaging/helm/hmailserver`, installed with
`--set image.tag=` the published release's version) is the one that starts the
published image.

Releases of both published charts were then moved to this one as Updates, below,
says, with that `--set image.tag=` added. Installed with either chart's
`--version`, each pod stopped at `CreateContainerConfigError`. After `helm upgrade
--reset-then-reuse-values` the StatefulSet's template ran the pod as 999:999 with
fsGroup 999 and had the init container, but the pod was the same one, in the same
state, 60 and 120 seconds later, and the StatefulSet's current revision was still
the old chart's. Deleted with `kubectl delete pod`, the pod was made again from this
chart and was ready in 8 or 9 seconds; it answered as above, and a message reached
an account's inbox. A pod that had stopped in `CrashLoopBackOff`, given the user and
group by number as above, waited in the same way for 75 seconds and started in the
same way once deleted. Upgraded with `--reuse-values` instead, the release kept the
old chart's `fsGroup` 1000 and `HM_DB_PORT` 5432, and its pod, once deleted, ran as
999:999 (and the group 1000) and answered the same. The message stayed in the inbox
when a later `helm upgrade` replaced the pod by itself (ready in 6 to 8 seconds),
when the pod was deleted again, and when an upgrade moved `fsGroup` from 1000 to
999, which Kubernetes then gave the volumes' files.

## Checking the chart

The chart is a file of the release, signed with Sigstore by the release's own
pipeline like every other file, with its bundle beside it on the release page. With
cosign 2.4 or later:

```
cosign verify-blob \
  --bundle hmailserver-chart-<version>.tgz.cosign.bundle \
  --certificate-identity https://gitlab.com/hmailserver/hmailserver//.gitlab-ci.yml@refs/tags/v<version> \
  --certificate-oidc-issuer https://gitlab.com \
  hmailserver-chart-<version>.tgz
jq -r .verificationMaterial.certificate.rawBytes hmailserver-chart-<version>.tgz.cosign.bundle \
  | base64 -d | openssl x509 -inform DER -noout -text | grep -A1 1.3.6.1.4.1.57264.1.15
helm install mail ./hmailserver-chart-<version>.tgz --set database.host=...
```

The second command must print `..86730559` under `1.3.6.1.4.1.57264.1.15`: the
project's own id, which GitLab gives no other project, read by OpenSSL from the
certificate's extension (Fulcio's source repository identifier). The identity is
a path, and a path could pass to another project were the group or the project
renamed or deleted; the id cannot, and a server requires it.

The identity is the project's path when the release was signed: a release tagged
before the project moved into the group `hmailserver` on 29 September 2026 (one
release, the last before the move, was signed by its pipeline there) was signed as
`https://gitlab.com/Progressiverobot/hmailserver//.gitlab-ci.yml@refs/tags/v<version>`,
and the release page's verification section says which.

The image is signed the same way, by its digest (`cosign verify`); ContainerImage.md
says how, and how to read the software bill of materials attested to it.

## Updates

A new release is a new chart and a new image:

```
helm repo update
helm upgrade mail hmailserver/hmailserver --reset-then-reuse-values
```

`--reset-then-reuse-values` (Helm 3.14 or later) renders the new chart with its own
defaults and the values the release was given. `--reuse-values` renders it with the
defaults of the chart the release was installed with instead, so a default that the
new chart changes does not reach the release. The server does not update itself
inside a container; its update check says that a newer release exists and that the
image is the way to it.

A release installed from a chart published before the chart gave the pod its user
and group has a pod that never started (Installed into a cluster, above). The same `helm upgrade` moves it to a chart that
starts one, and then its pod has to be deleted by hand:

```
kubectl delete pod mail-hmailserver-0
```

(the pod of a release named `mail`; `kubectl get pods` shows another's). The
StatefulSet's `podManagementPolicy` is `OrderedReady`, Kubernetes' default, under
which the StatefulSet changes none of its pods while one of them is not Running and
Ready. So it never replaces the stuck pod, whether it is in
`CreateContainerConfigError` or `CrashLoopBackOff`, and the pod stays as the old
chart made it; deleted, it is made again from the new chart. Kubernetes'
documentation of StatefulSets describes this under "Forced rollback".

Upgraded with `--reuse-values`, such a release's pod runs as 999:999 as well, since
the chart gives the pod the image's user and group wherever the values name none.
But the release keeps those charts' other defaults: `fsGroup` 1000, the base image's
`ubuntu` group, and `database.port` 5432, whatever `database.type` says.
