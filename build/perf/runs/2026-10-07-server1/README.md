The runs of 7 October 2026 on server1
=====================================

The record behind the numbers that `hmailserver/docs/Performance.md` and
`hmailserver/docs/PublishedNumbers.md` publish from server1, a Windows bench, on
7 October 2026: each run's JSON, the record, and the Markdown table the tool
wrote beside it, as the tools wrote them. Nothing here was edited.

The machine and the build
-------------------------

- An MSI MS-7E48, a physical machine with Windows' hypervisor present (Hyper-V
  and virtualisation-based security); an AMD Ryzen 9 9950X, 16 cores and 32
  logical; 61.4 GiB of RAM; Windows 11 Pro 10.0.26200; `C:\HMTest\Data` on an
  NVMe SSD (CT2000T705SSD3, NTFS, 1862 GiB). Every loadgen JSON here carries the
  same machine block.
- The tools on the same machine, under CPython 3.14.7 outside the PATH.
- hMailServer 6.3.6, the tag v6.3.6 (e3c448aa66fc310b74d076884d54fae951296915),
  a Release build made on the bench that morning, `hMailServer.exe` SHA-256
  62748794367F191AB668AD0D3B6724363D61A0569946AAAD009A1AFA6AC53C9E. Its version
  string reads 6.3.6-B45, which the bench's build before it read too; the commit
  is established by the build's log and the hash.
- SQL Server Compact 4.0, whose connection sets `SSCE:Flush Interval=10`. The
  message store on disk. Logging off.
- Beside it, the machine's owner's own programs, idle; no gate or pipeline job,
  the bench's lock held throughout.

What each file is
-----------------

Performance.md, the full-text index off (times in UTC):

- `seed.json`: `seed-folder.py`'s summary, 100,000 messages at seed 49 for
  `self@uicheck.test`.
- `perf-index-off-http.json`, `.md`: `webmail-timing.py --mode http`, 00:58:47
  to 01:01:19, five minutes after a restart. Made with 6.3.6's tool and the one
  change in `webmail-timing-wait429.diff` (against v6.3.6's
  `build/perf/webmail-timing.py`): a 429 waited out after its `Retry-After`, in
  no time; the JSON's `rate_limited` counts the two waits. The tool makes that
  wait itself since 8 October 2026. Its table has no row for
  `folder_tree_first_ms`, the folder's cold read, which Performance.md takes
  from the JSON.
- `perf-index-off-browser.json`, `.md`: `--mode browser --no-bulk`, 01:01:29 to
  01:01:48, with 6.3.6's tool as it was.

PublishedNumbers.md, the Windows table, at `MaxDeliveryThreads` 10, anti-spam and
anti-virus off server-wide; run 0 of each is the warm-up, and each cell the
median of runs 1 to 3:

- `in-on-0` to `in-on-3`, `in-off-0` to `in-off-3`: `smtp-in`, 20,000 messages,
  fsync on and off.
- `out-on-12k-0` to `-3`, `out-off-12k-0` to `-3`: `smtp-out`, 12,000 messages a
  run, 130 seconds before each.
- `imap-0` to `imap-3`: `imap-sessions`, fsync on. Sessions per core is
  published from runs 1 to 3; their memory per session is not, its idle point
  having been taken while the server still held the run before's sessions.
- `out-on-0`: the run of 20,000 messages out at 10 delivery threads that reached
  the sink with 19,085 - the evidence for 12,000. Not counted.

Not here
--------

Kept on server1, under `C:\perf\results`, and not here:

- The full-text index on: `perf-index-on-http` and `perf-index-on-browser`,
  whose searches failed on 6.3.6 and whose other cells came after a probe that
  never passed; the probes `probe-001` to `probe-026`.
- `perf-index-off-browser-discarded-1`, made after an HTTP pass that had ended at
  a 429; the whole pass was made again after a fresh restart.
- `in-on-threads50-*` and `out-on-threads50-*`, at the bench's 50 delivery
  threads rather than the shipped 10; the two runs of 20,000 out among them did
  not complete either.
- `imap-high-*`, sessions opened 1,600 at a time, outside the procedure.
