### smtp-out: Windows bench, fsync off (default), 12,000 messages, run 0

2026-10-07T05:56:50Z to 2026-10-07T05:57:40Z, host 127.0.0.1, port 25, server hMailServer 6.3.6, Windows, SQL Server Compact, tls none. Generator build/perf/load/loadgen.py.

MaxDeliveryThreads 10, the shipped default (the bench's regression setup keeps 50); logging off; anti-spam and anti-virus off server-wide

| Measure | Value |
|---|---|
| Messages accepted | 12000 of 12000 sent (0 failed) |
| Accepted per second | 471.18 |
| Latency MAIL FROM to 250, p50 / p95 / p99 / max (ms) | 64.56 / 92.81 / 110.75 / 148.1 |
| Relayed messages at the sink | 12000 of 12000 accepted |
| Relayed per second at the sink, first to last arrival | 266.51 |
| Relayed per second, first MAIL FROM to last arrival | 266.19 |
| Latency client MAIL FROM to sink, p50 / p95 / p99 / max (ms) | 19843.08 / 27386.45 / 29901.15 / 32113.66 |
| Busiest second at the sink | 534 messages |
| Server memory, mean (private bytes) | 161.4 MiB |
| Server memory, peak (RSS) | 179.5 MiB |
| Server memory, peak (private bytes) | 162.6 MiB |
| Server CPU used | 2.628 cores |

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
