### imap-sessions: Windows bench, run 3

2026-10-07T05:38:44Z to 2026-10-07T05:44:01Z, host 127.0.0.1, port 143, server hMailServer 6.3.6, Windows, SQL Server Compact, tls none. Generator build/perf/load/loadgen.py.

MaxDeliveryThreads 10, the shipped default (the bench's regression setup keeps 50); logging off; anti-spam and anti-virus off server-wide

| Measure | Value |
|---|---|
| Latency limit | p95 of every operation at most 200.0 ms, no errors, every session alive |
| Most sessions under the limit | 800 |
| Server cores (logical) | 32 |
| Sessions per core | 25 |
| Memory per session (private bytes, slope over the steps) | 125.8 KiB |
| Server CPU at that step | 0.302 cores |
| Step 25 sessions | 16.3 ops/s, p50 0.48, p95 1.58, p99 1.69 ms, 0 errors, passed, memory 270.9 MiB |
| Step 50 sessions | 32.83 ops/s, p50 0.46, p95 1.6, p99 1.84 ms, 0 errors, passed, memory 278.3 MiB |
| Step 100 sessions | 66.47 ops/s, p50 0.43, p95 1.53, p99 1.72 ms, 0 errors, passed, memory 284.5 MiB |
| Step 200 sessions | 133.37 ops/s, p50 0.41, p95 1.54, p99 1.76 ms, 0 errors, passed, memory 294.4 MiB |
| Step 400 sessions | 269.2 ops/s, p50 0.4, p95 1.52, p99 1.81 ms, 0 errors, passed, memory 332.4 MiB |
| Step 800 sessions | 534.37 ops/s, p50 0.39, p95 1.54, p99 1.84 ms, 0 errors, passed, memory 406.2 MiB |
| Step 1600 sessions | 1068.97 ops/s, p50 0.38, p95 1.61, p99 2.04 ms, 24 errors, failed, memory 537.6 MiB |

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
