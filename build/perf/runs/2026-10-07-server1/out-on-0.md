### smtp-out: Windows bench, fsync on, run 0

2026-10-07T04:54:30Z to 2026-10-07T05:05:18Z, host 127.0.0.1, port 25, server hMailServer 6.3.6, Windows, SQL Server Compact, tls none. Generator build/perf/load/loadgen.py.

MaxDeliveryThreads 10, the shipped default (the bench's regression setup keeps 50); logging off; anti-spam and anti-virus off server-wide

| Measure | Value |
|---|---|
| Messages accepted | 20000 of 20000 sent (0 failed) |
| Accepted per second | 469.03 |
| Latency MAIL FROM to 250, p50 / p95 / p99 / max (ms) | 64.96 / 90.49 / 109.55 / 151.78 |
| Relayed messages at the sink | 19085 of 20000 accepted - INCOMPLETE |
| Relayed per second at the sink, first to last arrival | 117.1 |
| Relayed per second, first MAIL FROM to last arrival | 117.03 |
| Latency client MAIL FROM to sink, p50 / p95 / p99 / max (ms) | 34869.66 / 109510.96 / 122440.78 / 128409.75 |
| Busiest second at the sink | 528 messages |
| Server memory, mean (private bytes) | 137.0 MiB |
| Server memory, peak (RSS) | 164.8 MiB |
| Server memory, peak (private bytes) | 138.3 MiB |
| Server CPU used | 0.355 cores |

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
