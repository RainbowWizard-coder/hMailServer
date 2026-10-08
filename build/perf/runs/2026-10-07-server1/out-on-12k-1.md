### smtp-out: Windows bench, fsync on, 12,000 messages, run 1

2026-10-07T05:14:12Z to 2026-10-07T05:15:05Z, host 127.0.0.1, port 25, server hMailServer 6.3.6, Windows, SQL Server Compact, tls none. Generator build/perf/load/loadgen.py.

MaxDeliveryThreads 10, the shipped default (the bench's regression setup keeps 50); logging off; anti-spam and anti-virus off server-wide

| Measure | Value |
|---|---|
| Messages accepted | 12000 of 12000 sent (0 failed) |
| Accepted per second | 468.44 |
| Latency MAIL FROM to 250, p50 / p95 / p99 / max (ms) | 64.16 / 90.86 / 109.94 / 429.97 |
| Relayed messages at the sink | 12000 of 12000 accepted |
| Relayed per second at the sink, first to last arrival | 252.76 |
| Relayed per second, first MAIL FROM to last arrival | 252.12 |
| Latency client MAIL FROM to sink, p50 / p95 / p99 / max (ms) | 20801.29 / 33508.6 / 38487.83 / 41975.05 |
| Busiest second at the sink | 522 messages |
| Server memory, mean (private bytes) | 139.2 MiB |
| Server memory, peak (RSS) | 167.3 MiB |
| Server memory, peak (private bytes) | 139.9 MiB |
| Server CPU used | 2.537 cores |

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
