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
Kubernetes, k3s 1.36.1 from Alpine Linux 3.24's own package, with Alpine's Helm
3.19.0, beside a PostgreSQL 18 in the cluster, with the image of 6.3.7
(`registry.gitlab.com/hmailserver/hmailserver:6.3.7`, the index
`sha256:2a5de4ebd04cd1e39f69ebeca3e905dd80217c3178132bd17a07880845ae629f`).

The charts of 6.3.6 and 6.3.7 that the repository serves do not start a server. The
6.3.7 chart, added with `helm repo add` and installed with `--version 6.3.7`, is the
release's file (its SHA-256 is the index's, and `cosign verify-blob` of its bundle
passes), and its pod never started: "container has runAsNonRoot and image has
non-numeric user (hmailserver), cannot verify user is non-root". Given the user by
number, it stopped at "/etc/hmailserver/hMailServer.ini is not writable by
hmailserver": the new, empty configuration volume hid the image's file. Nor did its
first start secure the new installation.

This chart, which the release after 6.3.7 is the first to serve, starts one.
Installed from the project's tree with `--set image.tag=6.3.7`, its pod was ready 16
seconds after `helm install`. Through `kubectl port-forward` and through the
Service's LoadBalancer address, port 25 answered with STARTTLS, under the
certificate the first start made. The REST API answered over TLS: 401 without
credentials, `6.3.7-B46` as Administrator, nothing over plain HTTP. It served the
Control Deck and the webmail. A message sent over SMTP to an account made over the
API was in its inbox. `helm uninstall` left the three PersistentVolumeClaims, as
above. Until that release, the chart of a checkout of the project
(`hmailserver/source/Server/platform/packaging/helm/hmailserver`, installed with
`--set image.tag=6.3.7`) is the one that starts 6.3.7's image.

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

A new release is a new chart and a new image: `helm repo update`, then
`helm upgrade mail hmailserver/hmailserver --reuse-values`. The server does not
update itself inside a container; its update check says that a newer release
exists and that the image is the way to it.
