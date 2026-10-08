### smtp-out: Windows bench, fsync off (default), 12,000 messages, run 1

2026-10-07T06:00:09Z to 2026-10-07T06:01:02Z, host 127.0.0.1, port 25, server hMailServer 6.3.6, Windows, SQL Server Compact, tls none. Generator build/perf/load/loadgen.py.

MaxDeliveryThreads 10, the shipped default (the bench's regression setup keeps 50); logging off; anti-spam and anti-virus off server-wide

| Measure | Value |
|---|---|
| Messages accepted | 12000 of 12000 sent (0 failed) |
| Accepted per second | 469.73 |
| Latency MAIL FROM to 250, p50 / p95 / p99 / max (ms) | 64.77 / 89.6 / 110.2 / 158.94 |
| Relayed messages at the sink | 12000 of 12000 accepted |
| Relayed per second at the sink, first to last arrival | 253.18 |
| Relayed per second, first MAIL FROM to last arrival | 252.68 |
| Latency client MAIL FROM to sink, p50 / p95 / p99 / max (ms) | 20103.26 / 33991.81 / 39322.79 / 42740.24 |
| Busiest second at the sink | 523 messages |
| Server memory, mean (private bytes) | 162.5 MiB |
| Server memory, peak (RSS) | 179.9 MiB |
| Server memory, peak (private bytes) | 162.9 MiB |
| Server CPU used | 2.519 cores |

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
