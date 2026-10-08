### imap-sessions: Windows bench, run 0

2026-10-07T05:22:01Z to 2026-10-07T05:27:00Z, host 127.0.0.1, port 143, server hMailServer 6.3.6, Windows, SQL Server Compact, tls none. Generator build/perf/load/loadgen.py.

MaxDeliveryThreads 10, the shipped default (the bench's regression setup keeps 50); logging off; anti-spam and anti-virus off server-wide

| Measure | Value |
|---|---|
| Latency limit | p95 of every operation at most 200.0 ms, no errors, every session alive |
| Most sessions under the limit | 1600 |
| Server cores (logical) | 32 |
| Sessions per core | 50 |
| Memory per session (private bytes, slope over the steps) | 247.7 KiB |
| Server CPU at that step | 0.629 cores |
| Step 25 sessions | 16.33 ops/s, p50 0.46, p95 1.61, p99 1.75 ms, 0 errors, passed, memory 153.6 MiB |
| Step 50 sessions | 32.63 ops/s, p50 0.45, p95 1.6, p99 1.93 ms, 0 errors, passed, memory 164.7 MiB |
| Step 100 sessions | 66.23 ops/s, p50 0.43, p95 1.51, p99 1.74 ms, 0 errors, passed, memory 174.1 MiB |
| Step 200 sessions | 133.03 ops/s, p50 0.41, p95 1.52, p99 1.75 ms, 0 errors, passed, memory 196.9 MiB |
| Step 400 sessions | 269.03 ops/s, p50 0.39, p95 1.52, p99 1.8 ms, 0 errors, passed, memory 244.2 MiB |
| Step 800 sessions | 533.2 ops/s, p50 0.38, p95 1.52, p99 1.77 ms, 0 errors, passed, memory 339.7 MiB |
| Step 1600 sessions | 1076.8 ops/s, p50 0.37, p95 1.62, p99 2.04 ms, 0 errors, passed, memory 534.6 MiB |

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
