### smtp-out: Windows bench, fsync off (default), 12,000 messages, run 3

2026-10-07T06:13:51Z to 2026-10-07T06:14:43Z, host 127.0.0.1, port 25, server hMailServer 6.3.6, Windows, SQL Server Compact, tls none. Generator build/perf/load/loadgen.py.

MaxDeliveryThreads 10, the shipped default (the bench's regression setup keeps 50); logging off; anti-spam and anti-virus off server-wide

| Measure | Value |
|---|---|
| Messages accepted | 12000 of 12000 sent (0 failed) |
| Accepted per second | 471.18 |
| Latency MAIL FROM to 250, p50 / p95 / p99 / max (ms) | 64.32 / 90.37 / 110.21 / 152.07 |
| Relayed messages at the sink | 12000 of 12000 accepted |
| Relayed per second at the sink, first to last arrival | 257.33 |
| Relayed per second, first MAIL FROM to last arrival | 256.61 |
| Latency client MAIL FROM to sink, p50 / p95 / p99 / max (ms) | 20309.18 / 32035.97 / 36713.51 / 39978.98 |
| Busiest second at the sink | 529 messages |
| Server memory, mean (private bytes) | 162.6 MiB |
| Server memory, peak (RSS) | 180.1 MiB |
| Server memory, peak (private bytes) | 163.6 MiB |
| Server CPU used | 2.576 cores |

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
