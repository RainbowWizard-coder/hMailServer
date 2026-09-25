# High availability runbook — active/passive hMailServer

This runbook describes a supported, operationally simple **active/passive**
high-availability topology for hMailServer. It deliberately contains **no
clustering code**: hMailServer runs as a single active node at a time, and
failover is performed by external infrastructure (a shared database, shared
message storage, a floating virtual IP, and a health check). This keeps the mail
server itself simple and avoids split-brain message corruption.

For an active/active or sharded design you would need a different storage and
locking model; that is out of scope here.

---

## 1. Topology

```
                 ┌─────────────────────────────┐
   clients ─────►│  Virtual IP (VIP) / load     │
   (SMTP/IMAP/   │  balancer with health check  │
    POP/REST)    └──────────────┬───────────────┘
                                │  (routes to whichever node is "ready")
                ┌───────────────┴───────────────┐
                │                                │
        ┌───────▼────────┐              ┌────────▼───────┐
        │  Node A (active)│              │ Node B (passive)│
        │  hMailServer    │              │  hMailServer    │
        │  service RUNNING│              │  service STOPPED │
        └───────┬─────────┘              └────────┬────────┘
                │                                  │
                └──────────────┬───────────────────┘
                               │
              ┌──────────────────────────────────────┐
              │  Shared database (MSSQL/MySQL/PG)    │
              │  Shared message store ([Directories] │
              │  DataFolder)                         │
              └──────────────────────────────────────┘
```

Exactly **one** node runs the hMailServer service at any time. Both nodes are
configured identically and point at the **same** database and the **same**
message-store directory.

---

## 2. Shared components

### 2.1 Shared database

* Use an external database server (Microsoft SQL Server, MySQL, or PostgreSQL),
  **not** the built-in SQL Compact / internal database, which is local-only.
* Both nodes use the same `hMailServer.INI` `[Database]` connection settings.
* The database server should itself be made highly available (e.g. SQL Server
  Always On / failover cluster, managed RDS/Cloud SQL with HA), or hosted on the
  same shared-storage layer as the message store.

### 2.2 Shared message store

* hMailServer stores message files under the data directory, which is
  `hMailServer.INI` → `[Directories]` → **`DataFolder`** (there is no setting called
  `DataDirectory`; that is the name of the accessor in the code). Both nodes must see
  the **same** directory on shared storage (SAN/NAS/clustered file system, or a cloud
  file share). Note that this key lives in the per-node ini file, not in the shared
  database, so it is one of the per-node values section 4 leaves you to keep in step.
  The database row for each message references its on-disk path; the database and the
  message store must therefore stay consistent with each other, so keep them on the
  same failover boundary.
* Ensure both nodes' service accounts have identical read/write access to the
  share.

### 2.3 Virtual IP / load balancer

* Clients connect to a floating VIP (or a load balancer / DNS name) rather than a
  node's real address.
* The VIP must point at the node that is **ready** (see health checks below).
* Only ever direct traffic to one node at a time. If you use a load balancer that
  can see both nodes, gate routing strictly on the readiness probe so the passive
  node (service stopped → probe fails/refused) never receives traffic.

---

## 3. Health checks and readiness gating

hMailServer exposes Kubernetes-style probes on the metrics listener. Enable it on
**both** nodes. These are stored settings: set them in the Control Panel, over
`/api/v1/settings/ini`, with `hmctl`, or on a Linux node whose REST listener is
not yet on with `hmailserver --set-setting NAME=VALUE`, then reinitialise the server (the
Control Panel's or the Deck's **Apply now**, or `POST /api/v1/server/reinitialize`) or restart
the service: the listener's port and address are bound when it starts, so they wait for one
of those ([SettingsApply.md](SettingsApply.md)). They are shown
below in the `[Settings]` shape they have always been written in, and that section of
each node's `hMailServer.ini` still holds a copy — but since schema 6042 it is a copy
the server maintains, not the place to change them.

```
[Settings]
MetricsServerPort=8080
MetricsServerBindAddress=0.0.0.0    ; reachable by the load balancer / health checker
MetricsServerAuthToken=<32+ random characters>   ; see below - required for /metrics on a non-loopback bind
ShutdownDrainSeconds=30             ; let in-flight sessions finish on a graceful stop
```

**The three probes never require a credential, and the exposition now does.** On a
non-loopback bind, `/metrics` answers **503** until `MetricsServerAuthToken` (or the
`MetricsServerAuthUsername`/`MetricsServerAuthPassword` pair) is set, and the 503 body
names the settings that would open it. `/livez`, `/readyz` and `/healthz` are served
unauthenticated in every configuration, because a load balancer cannot present a
credential and a health check cannot hold a secret — so **the VIP configuration in this
runbook keeps working exactly as written**, with or without the token.

The reason the exposition is treated differently: on `0.0.0.0` it publishes queue depth,
session counts and authentication-failure counts to anything that can reach the port.
Add the token, and give the scraper an `Authorization: Bearer <token>` header. If you
want the metrics port encrypted as well, set `MetricsServerCertificateFile` and
`MetricsServerPrivateKeyFile`; without them the listener stays plain HTTP and says so in
the application log rather than refusing to start.

Two properties of this listener to design the health check around, both deliberate:

* **`MetricsServerBindAddress` takes an IP literal and nothing else.** It is parsed
  with `inet_pton`, so `0.0.0.0`, `127.0.0.1`, `::` and a specific IPv6 address work,
  while a host name or `localhost` is rejected — the listener logs
  `MetricsServer: Invalid bind address` and does not start, which takes the probes with
  it. `::` serves both families (the listener clears `IPV6_V6ONLY` and says so in the
  log if it cannot); a specific IPv6 literal serves IPv6 only. If your health check gets
  a refused connection on a node whose service is running, this is the first thing to
  check.
* **It is a single accept loop serving one connection at a time.** Probes are cheap and
  answered before anything that can refuse, but a scrape and a probe are still
  serialised behind each other. Keep the probe interval and its timeout comfortably
  apart (the listener bounds a request read at 5s and a response write at 15s), and do
  not point a sub-second health check at it.

Probes (HTTP):

| Path       | Meaning                                                                 | Use for                          |
|------------|-------------------------------------------------------------------------|----------------------------------|
| `/livez`   | Process is alive (200 whenever the listener is up).                     | Liveness restarts.               |
| `/readyz`  | 200 only when the server is `Running` **and** the database is connected. Returns **503** while the server is stopping (`not ready: server not in running state`), while it is **draining** (`not ready: draining` - maintenance mode, entered with `POST /api/v1/server/drain` and by the live update before it stops the service, in which the SMTP, IMAP and POP3 listeners are closed and the delivery queue paused while what is in flight finishes; this listener stays up), or if the database connection is lost. During *startup* it is not 503 but **refused**: this listener is brought up only after the state has already gone to `Running` (it is the first of the optional listeners started, ahead of the REST API and web services), so there is nothing listening until the server is ready. All three read as unhealthy to a load balancer, which is all that matters here. | **VIP / load-balancer routing.** |
| `/api/v1/server/drain` (REST listener) | `GET` answers `{"draining","since","sessions","deliveries","drained"}`; `POST` enters the drain and `DELETE` leaves it, reopening the listeners on the same ports. `drained` is true once nothing is in flight, which is when a stop would be instantaneous. | **Planned failover, section 5.** |
| `/healthz` | JSON: `status` (`ok`/`unavailable`), `state`, `database` (`up`/`down`) and `uptime_seconds`. 200 when running with the database up, 503 otherwise. Per-protocol session counts are not in it — they are `hmailserver_sessions` on `/metrics`, behind its credential. | Dashboards / debugging.          |

**Configure the VIP/load balancer health check against `/readyz`.** Because the
passive node's service is stopped, its `/readyz` connection is refused (unhealthy)
and it will never be routed to. During a graceful stop, the active node flips
`/readyz` to 503 *before* tearing down listeners, so the load balancer drains it
cleanly.

---

## 4. Normal operation

* **Active node (A):** hMailServer service set to *Automatic* and **running**.
* **Passive node (B):** hMailServer service set to *Manual* (or *Disabled*) and
  **stopped**. Keep the binaries in sync with A (same version - the schema pin
  refuses a mismatch in both directions) and point it at the same database and
  data directory.
* Apply configuration changes on the active node. The configuration lives in the
  shared database - since schema 6042 that includes the server settings that
  were the `[Settings]` section of `hMailServer.INI`, which are rows of
  `hm_inisettings` - and B reads it when it starts. The `[Settings]` section of
  B's own INI is only the server's copy: B brings it up to date at that start,
  naming each setting changed since as `HM5804` in its error log, which is the
  copy catching up rather than a fault (a setting added since is written in with
  an application-log note). Do not edit `[Settings]` on B to make
  it match; the edit would be undone, and a line B has for a setting A has
  deleted is adopted back into the shared store when B starts.
* What *is* per node, and is yours to keep in step by hand or with your
  configuration-management tooling: `[Directories]`, `[Database]` and
  `[Security]` in each node's INI (the paths, the connection, the administrator
  password and its second factor); `PasswordPepper`, the one `[Settings]` key
  that lives in the file only and must be identical on both nodes or no
  account whose password is an Argon2id or scrypt hash signs in on B; the
  **stored-secrets key**, `[Security] StoredSecretsKey`, which every account's
  second factor and the `[Settings]` secrets in the shared database are sealed
  under once the installation has one, and which must be the database's key on
  both nodes or every second factor refuses every code on B and SRS, BATV, HS256
  tokens and `/metrics` stop working there ([SecretsAtRest.md](SecretsAtRest.md)).
  It is made once, by an elevated step - `--create-stored-secrets-key`, or the
  installer on a new installation - never by a service's start, and the database
  records which key it is, so B can neither make one of its own beside A's nor
  have one imported that is not the database's. Carry it with `hMailServer.exe
  --export-stored-secrets-key FILE` on A and `--import-stored-secrets-key FILE`
  on B, never by copying the line, which is sealed under A's own DPAPI, and keep
  the exported file with the database backups; and the certificate and DKIM key
  files at the paths the database names. Do not copy
  A's INI over B's wholesale - on Windows `[Database] Password` is normally
  sealed with the machine's own DPAPI key, and a `[SettingsOverride]` section, if
  A has one, is per node on purpose.

---

## 5. Planned failover (maintenance)

1. **Drain A:** `POST /api/v1/server/drain` on A (`manage_server`; the Deck's
   session - its update view has a **Drain** button - or the administrator
   password). A closes its SMTP, IMAP, POP3 and ManageSieve listeners - a new
   connection is refused at the socket, which every MTA retries on its own -
   pauses its delivery queue, and reports `/readyz` = 503 `draining`, so the
   load balancer stops sending it anything. The sessions and deliveries in
   flight finish at their own pace; A's REST, metrics and web-services listeners
   stay up throughout (the Deck, the webmail, CalDAV and CardDAV keep answering;
   see [Upgrading.md](Upgrading.md#an-upgrade-without-a-window) for what a
   person sees), so you can watch: `GET /api/v1/server/drain` until `drained` is
   true. Nothing is cut, and nothing is accepted only to be cut, which is what a
   plain stop with `ShutdownDrainSeconds` used to do.
2. **Stop A:** stop the hMailServer service on A. It finds the doors already
   closed and, with `ShutdownDrainSeconds` set, waits up to the window for anything
   still in flight - nothing, if `drained` was true - before shutting down. A
   stop after a drain does not wait again for what has already drained.
3. **Verify A is down:** `/readyz` on A is refused.
4. **Move the VIP** to B (or let the load balancer health check do it).
5. **Start B:** start the hMailServer service on B. Wait until `/readyz` on B
   returns 200. If this is an upgrade and B's binaries are newer than the shared
   schema, B refuses to start (`HM5011`) unless its `hMailServer.ini` says
   `[Database] UpgradeOnStart=1`, in which case B applies the chain at its own
   start - see [Upgrading.md](Upgrading.md#an-upgrade-without-a-window). Take the
   database backup before this step, not after. An upgrade to a version that
   seals stored secrets makes no stored-secrets key: sealing stays off until you
   make one on one node (`--create-stored-secrets-key`), export it and import it
   on the other ([Upgrading.md](Upgrading.md)). A node that starts without the
   database's key once there is one says so as `HM7495` at each start, seals
   nothing and makes no key of its own. **One node at a time:** start B
   and let it finish the chain before A's binaries are replaced and A is
   started again; A then reads the new version and does nothing. Two nodes that
   start together are serialised on the version row and the second finds each
   step done, but a warm-standby pair should never need that.
6. **Check the key on B before the VIP is B's for good:** B's error log has no
   `HM7495`, `HM7492` or `HM7493` from this start. `/readyz` is 200 either way -
   a node without the key serves mail, and only its users with a second factor,
   and SRS, BATV and HS256, fail - so this is the check that finds it. If B says
   `HM7495`, stop it, run `hMailServer.exe --import-stored-secrets-key FILE` on B
   with the file exported from A (or kept with the backups), and start it again.
7. **Confirm:** send a test message through the VIP and confirm delivery; check
   `/healthz` reports `database: up` and `state: running`.

Reverse the steps to fail back - step 6 included, on A: after an upgrade the
installer starts A on its own before you can import anything, so when A comes back
check its error log for `HM7495`, `HM7492` and `HM7493` before the VIP moves back,
and import the key there first if it asks for it. To take A off the air for a moment without a
failover at all - a certificate on disk to swap, a disk to grow - step 1, then
`DELETE /api/v1/server/drain` to reopen the same ports: that is maintenance mode,
and nothing was stopped.

---

## 6. Unplanned failover (node A fails)

1. The load balancer health check against `/readyz` on A fails; stop routing to A
   (automatic if the LB is health-check driven).
2. **Fence A** to guarantee it cannot still be writing to the shared store: power
   it off / isolate it from the storage and database network. This prevents
   split-brain (two active nodes writing the same message store).
3. **Move the VIP** to B.
4. **Start** the hMailServer service on B and wait for `/readyz` = 200.
5. When A is repaired, bring it back as the new passive node (service stopped)
   before any future failover.

> **Split-brain safety:** never let both nodes run the service against the shared
> store at the same time. Always fence the failed node before starting the
> standby. The shared database is the source of truth for message metadata; two
> active writers can corrupt mailbox state.

---

## 7. Validation checklist

* [ ] Both nodes use the same external database and the same `[Directories]`
      `DataFolder` on shared storage.
* [ ] `MetricsServerPort` is enabled and reachable by the health checker on both
      nodes; the VIP health check targets `/readyz`.
* [ ] `MetricsServerAuthToken` is set on both nodes, and the scraper sends it. Without
      it `/metrics` answers 503 on a non-loopback bind — the probes and therefore the
      failover still work, so this fails quietly as *missing dashboards*, not as an
      outage.
* [ ] `ShutdownDrainSeconds` is set so planned failovers drain gracefully.
* [ ] `PasswordPepper` is identical in both nodes' `hMailServer.INI` (or absent
      from both), and neither node has a `[SettingsOverride]` section.
* [ ] Both nodes hold the database's stored-secrets key: the key id each prints when
      it exports it is the same, the setup checklist's `stored_secrets_key` item is
      done on both, and neither node's error log has `HM7495`, `HM7492` or `HM7493`.
* [ ] The exported stored-secrets key is kept with the database backups, apart from
      them: without it a database restored after both nodes are lost opens no
      second factor and no `[Settings]` secret.
* [ ] Passive node's service is stopped and set to Manual/Disabled.
* [ ] A documented fencing step exists for unplanned failover.
* [ ] A test message sent through the VIP is delivered after a planned failover in
      both directions.

---

## 8. What hMailServer provides vs. what you provide

| Provided by hMailServer                                  | Provided by your infrastructure          |
|----------------------------------------------------------|-------------------------------------------|
| `/livez` / `/readyz` / `/healthz` readiness gating       | Virtual IP / load balancer + health check |
| Graceful shutdown drain (`ShutdownDrainSeconds`)         | Shared database with its own HA           |
| Shared-database + shared-store single-active design      | Shared message storage (SAN/NAS/cloud)    |
| `/metrics` for alerting on the active node               | Fencing of a failed node                  |

---

## 9. Verified against the code

Checked 13 August 2026. Every setting named on this page exists in
`IniFileSettings::LoadSettings` with the default stated (`MetricsServerPort` 0,
`MetricsServerBindAddress` `127.0.0.1`, `MetricsServerAuthToken` /
`MetricsServerAuthUsername` / `MetricsServerAuthPassword` /
`MetricsServerCertificateFile` / `MetricsServerPrivateKeyFile` all empty,
`ShutdownDrainSeconds` 0). The probe behaviour is `MetricsServer::HandleClient_`,
which answers `/livez`, `/readyz` and `/healthz` **before** any branch that can
refuse — a deliberate invariant recorded at that code, and the reason the VIP
configuration here does not change when a credential is added. `/metrics` closing
with 503 rather than 401 on a non-loopback bind with no credential is
`MetricsServer::Start` plus `BuildMetricsUnavailableResponse_`; the loopback test is
`IsLoopbackAddress_`, which accepts the whole of `127.0.0.0/8`. The drain order —
state to `Stopping` first, so `/readyz` is already 503, then, since 23 September
2026, the doors closed (`IOService::StopAccepting`, `SMTPDeliveryManager::Pause`)
before the bounded wait, which now waits for the deliveries in flight as well as
the sessions, then the listeners come down — is `Application::StopServers`.
Maintenance mode is `Application::EnterDrain` / `LeaveDrain` behind
`RestApiServer::HandleServerDrain_`, and `StateDraining` in `ServerStatus`; the
`draining` reason is `MetricsServer::IsReady_`. The `/healthz` body is
`BuildHealthBody_`. The bind-address parsing and the request/response deadlines are
in `MetricsServer::Start`, `ReadRequest_` and `Send_`.

There is regression coverage for the parts that would fail silently:
`test/RegressionTests/Infrastructure/HealthProbes.cs`,
`test/RegressionTests/Infrastructure/MetricsSecurity.cs`, and for the drain
`test/RegressionTests/Infrastructure/DrainAndUpgrade.cs` beside
`ShutdownDrain.cs`.
