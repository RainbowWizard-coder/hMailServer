Published numbers
=================

How fast this server is, measured rather than claimed: messages per second in
and out, IMAP sessions per core and memory per session, on named hardware,
against Postfix and Dovecot on the same machine, with the load generator in the
repository so that anyone can run it again. This page is the method, the
fairness rules, how to run it, and the table the numbers go in. **The table is
filled in from runs of the tool and from nowhere else**: a cell that says *to be
measured* has no measurement that may stand there yet - the results say why -
and no number on this page was estimated. Until the table is filled,
Roadmap2.md's first line is a goal, not a claim.

What is measured
----------------

| Measure | Mode | What it is |
|---|---|---|
| Messages in per second | `smtp-in` | N clients deliver M messages of a size distribution to local mailboxes. *Accepted per second* counts the 250 after each message's final dot, the moment the server has taken responsibility for it; *delivered per second* counts until every accepted message is in its mailbox, by IMAP `STATUS`. Both servers queue before they deliver, so the second is the one that compares like with like. |
| Messages out per second | `smtp-out` | The same clients send to a remote domain the server relays to a sink the tool runs, which accepts and discards. *Relayed per second* is counted **at the sink**, from its first arrival to its last, and from the first client's `MAIL FROM` to the sink's last arrival. |
| Latency | `smtp-in`, `smtp-out` | Per message, from `MAIL FROM` to the 250 after the dot (and, out, to the sink's receipt): p50, p90, p95, p99 and the worst. |
| IMAP sessions per core | `imap-sessions` | Sessions are opened in steps (25, 50, 100, ... by default); each logs in, selects INBOX and loops through `FETCH`es, `NOOP` and `IDLE` with a think time. The largest step at which the 95th percentile of every operation's latency stayed under 200 ms, with no error and every session alive, divided by the server machine's logical cores. |
| Memory per session | `imap-sessions` | The server's memory at each step, sampled while the step runs, and the slope of a straight line fitted through them and the idle server: what one more session costs. |
| imaptest | `imaptest` | Dovecot's own IMAP stress tool, in its benchmark profile (`no_tracking`), against either server: commands per second by kind. |

Each mode writes a JSON document (the record) and a Markdown table (what is
pasted below), and both carry the machine: CPU model, physical and logical
cores, RAM, operating system and kernel, the disk under the server's data and
whether the machine is virtual, collected by the tool itself.

The load generator
------------------

`build/perf/load/loadgen.py`, Python 3.9 or later (3.11 for `--tls starttls`),
with nothing but the standard library. `python build/perf/load/loadgen.py
--help` lists the modes; the module's docstring and each file's say more.

| File | What it holds |
|---|---|
| `loadgen.py` | The command line, the modes, the self-test |
| `smtpload.py` | The SMTP clients (asyncio, pipelined where the server offers PIPELINING), the message factory, the sink, the delivery count |
| `imapload.py` | The IMAP sessions and their operations |
| `imaptestrun.py` | imaptest's command line and the reading of its output |
| `sampler.py` | The server's memory and CPU while the load runs |
| `machine.py` | The machine's details |
| `report.py` | Percentiles, the JSON document and the Markdown table |
| `hmprepare.py` | The accounts, route, IP range and fsync setting a run needs on hMailServer, over its REST API |
| `toyserver.py` | A toy SMTP and IMAP server for the self-test and for nothing else |

**Why Python.** Every rate here is bounded by the server long before the
generator: a server that writes each message to disk and to an index reaches
thousands a second, not the hundreds of thousands a benchmark of a web cache
needs. One asyncio process drives several thousand SMTP transactions or IMAP
commands a second on one core, and `--processes` spreads the clients over as
many processes as it is given. Measured on the development laptop against the
tool's own sink: 20,000 messages of 4 KB over 32 connections in four
processes, 8,800 to 13,900 accepted a second - more than either server will
take, and the sink shared the machine. The generator does not assume it was
fast enough: every mode records its own processes' CPU and prints a warning
when one of them was busy for more than 85 per cent of the run, which means
the generator may have been the limit and the number is a floor. A run that
carries that warning is not published.

**Memory.** Sampling the server is optional and needs nothing installed:
psutil when it is there; otherwise `/proc` on Linux (the resident set, and the
proportional set from `smaps_rollup` where it can be read, which divides pages
shared between Dovecot's processes fairly) and `Get-Process` through PowerShell
on Windows (the working set and private bytes, at most one sample every two
seconds); and on Linux, the cgroup of each container named with
`--server-container`, added up - the anonymous memory, the whole charge, the
CPU time and whether a memory limit was reached. The lab's comparison uses the
cgroups, the same figure for both servers.

**Passwords** come from the environment, never the command line:
`HMLOAD_PASSWORD` for the load mailboxes (IMAP, and SMTP AUTH unless
`HMLOAD_SMTP_PASSWORD` is set), `HMLOAD_ADMIN_PASSWORD` for
`prepare-hmailserver`. imaptest takes its passwords from a user file of mode
600 in a directory of mode 700, removed after the run.

**Self-test.** `python build/perf/load/loadgen.py self-test` runs every mode
against the toy server, at low load, on the machine it is run on: 200 messages
in and counted in the mailboxes, 200 relayed and counted at the sink, two steps
of IMAP sessions with every operation exercised and the toy server's memory
sampled, the REST calls `prepare-hmailserver` makes (against a fake listener),
the imaptest command line, the reading of imaptest's output and of cgroup
files. It proves the generator and says nothing about any server.

The fairness rules
------------------

A comparison is only as honest as the two setups are equal where they can be
and stated where they cannot. The published run keeps these rules;
`lab.sh numbers` keeps the ones a script can, and records beside every result
what each side ran with (`postconf -n` and `doveconf -n`; hMailServer's
`MessageStoreFsync`, its logging and the database's flush settings), so that the
rest can be checked afterwards.

1. **One machine, one side at a time.** Both servers run on the same host, the
   generator beside them, each measured with the other stopped and nothing else
   loading the machine: on linix, no GitLab CI job and no regression suite may
   run during a measurement, and the lab's other profiles are down (the
   always-on monitor, about 100 MB, is recorded, not stopped).
2. **The same generator, the same parameters.** The two command lines differ in
   the address, the port and the mail domain and in nothing else; `lab.sh
   numbers` builds both from the same arguments.
3. **The same durability.** A message a server has answered 250 for must
   survive a power cut, on both sides. Postfix fsyncs its queue file before it
   answers, and that cannot be turned off; Dovecot runs with `mail_fsync =
   optimized`, its default, which fsyncs a delivered message and its index.
   hMailServer runs with `MessageStoreFsync` = 1 (a setting in the database; the
   shipped default is 0) and a database that flushes its log at every commit
   (MariaDB's `innodb_flush_log_at_trx_commit` = 1, its default). hMailServer's
   shipped default, fsync off, is measured as a row of its own and labelled as
   not durable; it is never set beside Postfix and Dovecot as if it were.
4. **The same TLS.** The published run is plain on both sides: hMailServer's
   plain ports and Postfix's and Dovecot's with TLS off. A TLS row, when there
   is one, has TLS on both sides (`--tls starttls` for the generator).
5. **No filtering on either side.** No anti-spam, anti-virus, DKIM, SPF or DMARC
   checks: hMailServer's IP range for the generator has spam and virus
   protection off, and `prepare-hmailserver` gives it a priority above every
   range that holds the generator's address - on the server's own machine that
   is the shipped *My computer*, 127.0.0.1 at priority 30, which has both on;
   Postfix has no milter and no policy service. Logging stays at each side's
   default and is stated.
6. **The whole mail service is measured.** hMailServer's memory and CPU are its
   container's and its database's added up; Postfix and Dovecot's are both
   containers'. No CPU limit on any container; memory limits are generous, and
   a run in which any limit was reached (the cgroup's `memory.events`) is void.
7. **The same storage.** Every side's data is on a docker volume on the same
   disk: Postfix's queue, Dovecot's Maildir, and hMailServer's tree
   (`HMLAB_SERVE_STORE`), where it would otherwise be in the container's
   overlay filesystem.
8. **Warm, repeated, the median.** The first run after a server starts is not
   counted; each measure is run three times and the median is published, with
   the lowest and highest beside it.
9. **The generator is not the limit.** No published run carries the
   generator's busy warning. Where one does, it is run again with more
   `--processes`, or from a second machine with `--server-machine`.
10. **Mailboxes seeded the same.** `imap-sessions` runs after `smtp-in` on each
    side, so that both serve the same messages from the same fifty mailboxes;
    imaptest, which appends and expunges, runs last.

What differs, and cannot be made equal:

- **The store.** Dovecot keeps each message as a Maildir file with its
  `dovecot.index` files beside it; hMailServer keeps each message as an `.eml`
  file with its row in a SQL database, here MariaDB with the lab's 128 MB
  buffer pool. The database write is part of what hMailServer costs, and is
  measured as such.
- **The path of a message.** Postfix takes a message into its queue and hands
  it to Dovecot over LMTP, a second process and a second write; hMailServer
  queues and delivers within one process.
- **The process model.** Dovecot runs a process for each IMAP session;
  hMailServer runs every session on a pool of threads in one process. Memory
  per session is therefore a cgroup's anonymous memory on both sides, not a
  process's resident set, which would count Dovecot's shared pages once for
  each process.
- **Concurrency left at each side's default.** Postfix delivers up to 20 at once
  to one destination (`default_destination_concurrency_limit`); hMailServer's
  `MaxDeliveryThreads` is 10. The process caps that would end a run at the
  configuration and not at the server are raised: Postfix's
  `default_process_limit` to 512, Dovecot's process and client limits and
  `mail_max_userip_connections`; hMailServer's `MaxSMTPConnections` and
  `MaxIMAPConnections` are 0, no cap, by default.
- **The builds.** The Postfix image is Alpine (musl); hMailServer's container is
  the project's Ubuntu CI image (glibc). Each allocator is part of its server.

How to run it on linix
----------------------

The comparison lives in the test lab (TestLab.md): profile `numbers` brings up
Postfix and Dovecot configured as above (`build/lab/docker-compose.yml`,
`build/lab/dovecot/numbers.conf`), and `lab.sh numbers` runs the generator on
linix itself against one side, reaching each container at its address on the
lab network rather than through a published port. The sink listens on linix's
port 10365, which the containers reach at the lab network's gateway. On linix,
as a member of the docker group, from a copy of the tree
(`build/lab/push-source.sh`), with nothing else running:

```sh
bash build/lab/lab.sh build
bash build/lab/lab.sh up mariadb
HMLAB_SERVE_STORE=hmlab-numbers-store bash build/lab/lab.sh serve mariadb

# hMailServer, durable: 50 accounts, the route to the sink, the generator's IP
# range, MessageStoreFsync on. Then one warm-up run, then three of each.
bash build/lab/lab.sh numbers hmailserver prepare
bash build/lab/lab.sh numbers hmailserver smtp-in --clients 32 --messages 20000 --processes 4 --label "hMailServer, fsync on"
bash build/lab/lab.sh numbers hmailserver smtp-out --clients 32 --messages 20000 --processes 4 --label "hMailServer, fsync on"
bash build/lab/lab.sh numbers hmailserver imap-sessions --processes 4 --label "hMailServer, fsync on"
bash build/lab/lab.sh numbers hmailserver imaptest --clients 50 --seconds 120 --label "hMailServer"

# hMailServer's shipped default, for its own column: SMTP in and out, the same way
HMLAB_NUMBERS_FSYNC=off bash build/lab/lab.sh numbers hmailserver prepare
bash build/lab/lab.sh numbers hmailserver smtp-in --clients 32 --messages 20000 --processes 4 --label "hMailServer, fsync off (default)"
bash build/lab/lab.sh numbers hmailserver smtp-out --clients 32 --messages 20000 --processes 4 --label "hMailServer, fsync off (default)"

bash build/lab/lab.sh unserve
bash build/lab/lab.sh down mariadb

# Postfix and Dovecot, the same runs
bash build/lab/lab.sh up numbers
bash build/lab/lab.sh numbers postfix-dovecot prepare
bash build/lab/lab.sh numbers postfix-dovecot smtp-in --clients 32 --messages 20000 --processes 4 --label "Postfix + Dovecot"
bash build/lab/lab.sh numbers postfix-dovecot smtp-out --clients 32 --messages 20000 --processes 4 --label "Postfix + Dovecot"
bash build/lab/lab.sh numbers postfix-dovecot imap-sessions --processes 4 --label "Postfix + Dovecot"
bash build/lab/lab.sh numbers postfix-dovecot imaptest --clients 50 --seconds 120 --label "Postfix + Dovecot"
bash build/lab/lab.sh down numbers
```

Each run leaves `~/hmlab/runs/numbers-<time>-<side>-<mode>.json`, `.md` and
`.config.txt`. `HMLAB_NUMBERS_MAILBOXES` changes the fifty mailboxes; every
other `loadgen.py` argument can be added to the line. Where linix's firewall
is on, the lab network must be allowed to reach port 10365 for `smtp-out`.

How to run it on the Windows bench
----------------------------------

There is no Postfix or Dovecot for Windows, so the Windows bench gives
hMailServer's own numbers and no comparison; the comparison is on Linux, above.
(Postfix and Dovecot in WSL2 would run in a virtual machine beside Windows, not
on the same machine as the service, and are not used.) On the bench, from the
root of a checkout of the build to be measured, with the service running that
build, nothing else running on it, and 15 GB free on the data folder's disk (four
runs of 20,000 messages of about 75 KB stay in the mailboxes until the next
`prepare`). Every line runs as written in Windows PowerShell 5.1 or PowerShell 7,
as an administrator. `$py` is a Python outside the PATH, and the logging is off,
as Performance.md, "Running it", sets them up: a gate leaves every log on, and a
`python.exe` on the machine's PATH changes which tests the next gate runs.

```powershell
# The REST listener on 127.0.0.1:9530, as the README shots use.
powershell -NoProfile -ExecutionPolicy Bypass -File build\readme-shots\bench.ps1 -Up
$env:HMLOAD_ADMIN_PASSWORD = 'testar'
$env:HMLOAD_PASSWORD = '<a password for the load accounts>'
$server = 'hMailServer <version>, Windows, <database>'
New-Item -ItemType Directory -Force C:\perf\results | Out-Null

# The shipped delivery threads, 10, which take effect at once: a regression gate's setup
# leaves 50 (TestSetup.cs). What the bench had, and its MessageStoreFsync, are written
# down to be put back.
$app = New-Object -ComObject hMailServer.Application; $null = $app.Authenticate('Administrator', 'testar')
$threadsBefore = $app.Settings.MaxDeliveryThreads; $fsyncBefore = $app.Settings.GetIniSetting('MessageStoreFsync')
"MaxDeliveryThreads was $threadsBefore, MessageStoreFsync '$fsyncBefore'"
$app.Settings.MaxDeliveryThreads = 10

& $py build\perf\load\loadgen.py prepare-hmailserver --rest http://127.0.0.1:9530 --accounts 50 `
    --route sink.test=127.0.0.1:10365 --client-range 127.0.0.1-127.0.0.1 --fsync on

# One warm-up run, not counted, then three (rule 8): runs 0 to 3, a file each.
foreach ($i in 0..3) {
    & $py build\perf\load\loadgen.py smtp-in --port 25 --mailboxes 50 --clients 32 --messages 20000 --processes 4 `
        --verify-imap-port 143 --server-process hMailServer --data-path C:\HMTest\Data --server $server `
        --label "Windows bench, fsync on, run $i" --json "C:\perf\results\in-on-$i.json" --markdown "C:\perf\results\in-on-$i.md"
}
# SMTP out: 12,000 messages a run, not 20,000, each run 130 seconds after the one before
# (why: under the recipe).
foreach ($i in 0..3) {
    Start-Sleep -Seconds 130
    & $py build\perf\load\loadgen.py smtp-out --port 25 --mail-from load1@example.test --clients 32 --messages 12000 `
        --processes 4 --sink-listen 127.0.0.1:10365 --server-process hMailServer --data-path C:\HMTest\Data --server $server `
        --label "Windows bench, fsync on, 12,000 messages, run $i" --json "C:\perf\results\out-on-12k-$i.json" --markdown "C:\perf\results\out-on-12k-$i.md"
}
foreach ($i in 0..3) {
    & $py build\perf\load\loadgen.py imap-sessions --port 143 --users 50 --processes 4 --server-process hMailServer `
        --data-path C:\HMTest\Data --server $server --label "Windows bench, run $i" --json "C:\perf\results\imap-$i.json" --markdown "C:\perf\results\imap-$i.md"
}

# The shipped default, fsync off, for its own column: SMTP in and out, the same way.
& $py build\perf\load\loadgen.py prepare-hmailserver --rest http://127.0.0.1:9530 --accounts 50 `
    --route sink.test=127.0.0.1:10365 --client-range 127.0.0.1-127.0.0.1 --fsync off
foreach ($i in 0..3) {
    & $py build\perf\load\loadgen.py smtp-in --port 25 --mailboxes 50 --clients 32 --messages 20000 --processes 4 `
        --verify-imap-port 143 --server-process hMailServer --data-path C:\HMTest\Data --server $server `
        --label "Windows bench, fsync off (default), run $i" --json "C:\perf\results\in-off-$i.json" --markdown "C:\perf\results\in-off-$i.md"
}
foreach ($i in 0..3) {
    Start-Sleep -Seconds 130
    & $py build\perf\load\loadgen.py smtp-out --port 25 --mail-from load1@example.test --clients 32 --messages 12000 `
        --processes 4 --sink-listen 127.0.0.1:10365 --server-process hMailServer --data-path C:\HMTest\Data --server $server `
        --label "Windows bench, fsync off (default), 12,000 messages, run $i" --json "C:\perf\results\out-off-12k-$i.json" --markdown "C:\perf\results\out-off-12k-$i.md"
}

# Put back: the accounts, the route and the range; then what the bench had for the
# delivery threads and MessageStoreFsync, and the domain prepare made, which --cleanup
# leaves - leave out the Delete() when the first prepare said "kept as it is", the
# domain being the bench's own; then the domain's folder if it is left empty, and the
# listener.
& $py build\perf\load\loadgen.py prepare-hmailserver --rest http://127.0.0.1:9530 --accounts 50 `
    --route sink.test=127.0.0.1:10365 --cleanup
$app = New-Object -ComObject hMailServer.Application; $null = $app.Authenticate('Administrator', 'testar')
$app.Settings.MaxDeliveryThreads = $threadsBefore
if ($fsyncBefore -ne '') { $app.Settings.SetIniSetting('MessageStoreFsync', $fsyncBefore) } else { $app.Settings.DeleteIniSetting('MessageStoreFsync') }
$app.Domains.ItemByName('example.test').Delete()
$left = 'C:\HMTest\Data\example.test'
if ((Test-Path $left) -and -not (Get-ChildItem $left -Force)) { Remove-Item $left }
powershell -NoProfile -ExecutionPolicy Bypass -File build\readme-shots\bench.ps1 -Down
```

`--data-path` names the server's `DataFolder` (from `hMailServer.ini`), so that
the table describes the disk the message store is on. `prepare-hmailserver
--cleanup` removes what it made but the domain, which may have been there before
the run, and leaves `MessageStoreFsync` as the run set it; the lines after it put
back what the bench had: its delivery threads, its `MessageStoreFsync` - a
`DeleteIniSetting` alone leaves no value where the bench may have had one, `0` on
the Windows benches - and its domains. Deleting the load accounts leaves their
domain's folder empty, which the bench's pre-flight takes for an orphan, so an
empty one goes too. Over REST, `PUT /api/v1/settings/ini/MessageStoreFsync`,
`DELETE /api/v1/settings/ini/MessageStoreFsync` and `DELETE
/api/v1/domains/example.test` do the same.

Why 12,000 messages out, and the pause: the sink closes each relayed message's
connection first, and on the loopback each closed connection holds one of
Windows' dynamic ports - 16,384 by default - in TIME_WAIT for two minutes. On the
Windows bench three runs of 20,000 reached the sink with 19,176, 19,060 and
19,085, the rest bounced on connection timeouts as the ports ran out; runs of
12,000 left at most 15,592 ports in TIME_WAIT and arrived whole, every one of
eight, with 130 seconds before each for the ports to come back. Both columns run
the same way. A larger port range or a shorter TIME_WAIT would let 20,000 run,
and change the machine, which this recipe does not. `loadgen.py smtp-out` says so
when it is asked for more than 12,000 on Windows with the sink on the server's
own machine.

`imap-sessions` runs back to back here, the next run starting while the server
is still letting the last one's sessions go: it takes the idle point its memory
figure is fitted through only once the server's memory has stopped falling
(`--idle-wait`, two minutes at most), and its table says the figure is not valid
when the idle point is still above the first step's memory. A run that passes
every step says so: the last step, 1,600 sessions by default, is then the most
it can show, not the server's limit.

The table takes the median of runs 1 to 3 with the lowest and highest beside it;
a run whose JSON carries `generator.warning` is not counted (rule 9), and a run
whose memory figure the tool says is not valid gives no memory cell. On Windows
the service's memory is its working set and private bytes, which are not the
cgroup figure the Linux comparison uses; the two tables are not compared with
each other.

Results
-------

**Partly measured.** The Windows bench's table has six of its seven cells, from
the runs of 7 October 2026; the comparison on linix has none yet. The row in
Roadmap2.md stays open until both are filled from runs of the tool as described
above. Each run's JSON and table are kept in the repository, under
`build/perf/runs/<date>-<machine>/`, and a release note that quotes a number
names the run it came from.

On linix - the machine named by the runs' own tables (CPU, cores, RAM, disk,
virtualisation), to be copied here from the first run:

| Measure | hMailServer, fsync on | hMailServer, fsync off (shipped default) | Postfix + Dovecot |
|---|---|---|---|
| SMTP in: accepted per second (median of 3) | to be measured | to be measured | to be measured |
| SMTP in: delivered per second (median of 3) | to be measured | to be measured | to be measured |
| SMTP in: latency p50 / p95 / p99 (ms) | to be measured | to be measured | to be measured |
| SMTP out: relayed per second at the sink (median of 3) | to be measured | to be measured | to be measured |
| SMTP out: client to sink p50 / p95 (ms) | to be measured | to be measured | to be measured |
| IMAP: most sessions with p95 under 200 ms | to be measured | - | to be measured |
| IMAP: sessions per core | to be measured | - | to be measured |
| IMAP: memory per session (cgroup anon, KiB) | to be measured | - | to be measured |
| imaptest benchmark: FETCH per second | to be measured | - | to be measured |
| imaptest benchmark: SELECT per second | to be measured | - | to be measured |
| imaptest benchmark: APPEND per second | to be measured | - | to be measured |

On the Windows bench - hMailServer alone. **Measured on server1 on 7 October
2026**: an MSI MS-7E48, a physical machine on which Windows reports a hypervisor
present (Hyper-V or virtualisation-based security may be on), with one AMD Ryzen 9
9950X (16 cores, 32 logical), 61.4 GiB of RAM and Windows 11 Pro 10.0.26200, the
data folder on an NVMe SSD (CT2000T705SSD3, NTFS, 1862 GiB), as every run's JSON
records it; the generator on the same machine under CPython 3.14.7. hMailServer
6.3.6 (the tag, e3c448aa6) in a Release build made on the bench, with SQL Server Compact 4.0, logging off,
`MaxDeliveryThreads` 10 (the shipped default; the bench keeps 50, which was put
back after), anti-spam and anti-virus off server-wide, the recipe's 32 clients in
4 processes and its default sizes. Fsync on: SMTP in 04:46 to 04:54 UTC, SMTP out
05:10 to 05:21, IMAP 05:22 to 05:44; fsync off: SMTP in 05:46 to 05:54, SMTP out
05:56 to 06:14. The machine's owner's own programs ran beside them, idle: other
processes used at most 3.26 % of its logical cores during any counted run, and
the generator's busiest process 5.6 % of a run, so no run carries
`generator.warning`. Each cell is the median of runs 1 to 3, run 0 being the
warm-up, with the lowest and highest in brackets. The runs are in
`build/perf/runs/2026-10-07-server1/`: `in-on-*`, `in-off-*`, `out-on-12k-*`,
`out-off-12k-*` and `imap-*`, runs 0 to 3 of each.

| Measure | hMailServer, fsync on | hMailServer, fsync off (shipped default) |
|---|---|---|
| SMTP in: accepted / delivered per second | 442.13 (439.48-444.91) / 188.2 (184.09-190.1) | 444.13 (439.84-447.43) / 185.72 (185.01-187.64) |
| SMTP in: latency p95 (ms) | 96.55 (95.07-97.8) | 96.99 (95.53-97.11) |
| SMTP out: relayed per second at the sink | 252.76 (252.44-253.96), 12,000 messages a run [a] | 257.33 (253.18-258.22), 12,000 messages a run [a] |
| IMAP: sessions per core | 50 (25-50), the last step [b] | - |
| IMAP: memory per session (private bytes, KiB) | to be measured [c] | - |

Every run delivered or relayed every message: 20,000 of 20,000 into the mailboxes
for SMTP in, 12,000 of 12,000 at the sink for SMTP out.

"Fsync on" on Windows is the message files fsynced (`MessageStoreFsync` 1) with
a database that flushes its log every ten seconds, not at every commit: SQL
Server Compact's connection sets `SSCE:Flush Interval=10`
(`SQLCEConnection.cpp`). That is not rule 3's durability. This table is
hMailServer's own and is set beside nothing, but it is not the durable column of
the comparison either.

The generator's connections took the shipped *My computer* range (127.0.0.1,
priority 30, spam and virus protection on), not the one `prepare-hmailserver`
made, which until 8 October 2026 had priority 20. Every anti-spam check and every
virus scanner was off server-wide - SpamAssassin, SPF, MX, HELO, greylisting,
DKIM, DMARC, ARC, DNSBL and SURBL among them, as checked on the bench afterwards -
so nothing filtered the mail, as rule 5 asks.

[a] 20,000 messages out cannot complete on Windows with the sink on the server's
own machine (why: under the recipe). The three runs of 20,000 that were made
reached the sink with 19,176, 19,060 and 19,085; the last, the one at 10 delivery
threads, is kept as `out-on-0`. The counted runs are of 12,000, both columns run
the same way, so this row is published as 12,000 messages a run, and the
recipe's command says 12,000.

[b] Runs 1 and 2 passed every step up to 1,600 sessions, the last of the default
steps, with p95 about 1.6 ms; run 3 had 12 connections refused at connect at
1,600, so its result is 800 sessions, 25 a core. 50 a core is therefore the most
this procedure can show on 32 logical cores, and not a limit it found; nor is it
known to be a floor with room above it, since opening 1,600 sessions at once,
outside the procedure, had 109 to 720 connections refused.

[c] Not valid from these runs. The tool of 6.3.6 took its idle point as each run
began, and the runs went back to back: runs 2 and 3 began about ten seconds
after the run before had ended, the server still holding its 1,600 sessions, and
took 526.4 and 526.5 MiB of private bytes for idle against 238.5 and 270.9 MiB
at their first step, which pulls their fitted slopes down (138.7 and 125.8 KiB;
run 1's, begun 70 seconds after the warm-up, 184.4). The tool now waits for the
server's memory to stop falling before it takes the idle point and says when a
run's figure is not valid; the cell waits for runs made with it.

The default message sizes are 2 KB (25 per cent), 8 KB (35), 32 KB (25), 128 KB
(10) and 1 MB (5), about 75 KB on average; a published row names its sizes if
they are not these.
