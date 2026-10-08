### smtp-in: Windows bench, fsync on, run 1

2026-10-07T04:48:13Z to 2026-10-07T04:50:07Z, host 127.0.0.1, port 25, server hMailServer 6.3.6, Windows, SQL Server Compact, tls none. Generator build/perf/load/loadgen.py.

MaxDeliveryThreads 10, the shipped default (the bench's regression setup keeps 50); logging off; anti-spam and anti-virus off server-wide

| Measure | Value |
|---|---|
| Messages accepted | 20000 of 20000 sent (0 failed) |
| Accepted per second | 442.13 |
| Accepted MiB per second | 31.72 |
| Latency MAIL FROM to 250, p50 / p95 / p99 / max (ms) | 69.06 / 96.55 / 114.77 / 222.16 |
| Delivered per second (all in the mailboxes, IMAP STATUS) | 190.1 |
| Server memory, mean (private bytes) | 91.5 MiB |
| Server memory, peak (RSS) | 125.9 MiB |
| Server memory, peak (private bytes) | 98.8 MiB |
| Server CPU used | 2.295 cores |

**Machine the generator ran on**

| | |
|---|---|
| Host | Server1 |
| System | Micro-Star International Co., Ltd. MS-7E48 |
| Virtualisation | a hypervisor is present (Hyper-V/VBS may be on; the machine itself: Micro-Star International Co., Ltd. MS-7E48) |
| CPU | AMD Ryzen 9 9950X 16-Core Processor |
| Cores | 16 physical, 32 logical, 1 socket(s) |
| RAM | 61.4 GiB |
| OS | Microsoft Windows 11 Pro 10.0.26200 |
| Disk (C:\HMTest\Data) | CT2000T705SSD3, SSD, NVMe, NTFS, 1862 GiB |
| Python | CPython 3.14.7 |
