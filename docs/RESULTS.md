# Results: control-loop timing under AI load on QNX 8.0

## Question

Can a local large language model run flat out on the same QNX controller as a 1 kHz control loop, without making that loop miss deadlines? And if the system is misconfigured, does the measurement show it?

## Setup

- **Target:** QNX OS 8.0 x86_64, in QEMU with KVM, 2 vCPUs, 3 GB RAM. The image was built by `mkqnximage`.
- **Host:** a GitHub-hosted `ubuntu-24.04` runner with 4 vCPUs and 16 GB RAM. The guest's 2 vCPUs therefore don't compete with the host for every core.
- **Control loop:** `fm-plant` runs at SCHED_FIFO priority 70. A 1 ms periodic timer delivers a pulse, which the loop receives with `MsgReceivePulse`. Each cycle runs the industrial machine model and its fault rules, then publishes to shared memory.
- **Measures:**
  - **Wake latency:** the time from the scheduled expiry to the loop thread running. The schedule is anchored to the timer's first expiry, because QNX timers expire on clock-tick boundaries.
  - **Deadline miss:** a cycle whose work finished after the next period had started.
- **AI load:** `llama-bench` generates tokens continuously with Qwen3-1.7B Q4_K_M and 2 threads. That's harder on the CPU than interactive chat.
- **Duration:** 60 s per phase, about 61,000 cycles. Statistics reset at the start of each phase.

All numbers below come from `ci/bench.sh` in the public workflow runs listed at the end. Each run's log shows the full output.

## Results

Two independent runs, on different host CPUs:
- **Run 1** ([45889173085](https://github.com/sihlangu-mthembu/foreman/actions/runs/45889173085)): AMD EPYC host.
- **Run 2** ([45895830849](https://github.com/sihlangu-mthembu/foreman/actions/runs/45895830849)): Intel Xeon 6975P-C host.

Each phase runs about 61,000–65,000 cycles.

| Phase | Configuration | Misses, run 1 | Misses, run 2 | p99.9 wake (1 / 2) | Worst wake (1 / 2) |
|---|---|---|---|---|---|
| A | Idle: control loop only | **0** | **0** | 150 / 50 µs | 0.43 / 0.29 ms |
| **B** | **AI generating on both vCPUs, priority 10, unprivileged user** | **0** | **0** | **50 / 90 µs** | **0.64 / 0.18 ms** |
| D | Control: guest idle, host CPUs saturated | 4 | 57 | 110 / 990 µs | 1.01 / 3.76 ms |
| A2 | Idle, control loop pinned to vCPU 1 | 2 | 0 | 210 / 70 µs | 1.01 / 0.43 ms |
| B2 | AI pinned to vCPU 0, control loop pinned to vCPU 1 | 30 | 2 | 270 / 80 µs | 1.06 / 1.04 ms |
| **C** | **Misconfigured: AI at privileged priority 71, above the loop** | **8,578** | **16,630** | n/a | **8,553 / 16,201 ms** |

**Whole interactive technician session.** This covers questions, fault injection and on-device inference, with the model at priority 10:
- Run 1: 15 misses in 108,684 cycles (0.014%), worst wake 2.3 ms.
- Run 2: **0 misses in 122,981 cycles**, worst wake 0.71 ms.

## Reading the results

- **Priority alone protects the loop.** With the model saturating both vCPUs (B), the loop missed nothing and its 99.9th-percentile wake latency was 50 µs. The QNX scheduler preempts the priority-10 inference threads the instant the priority-70 timer pulse arrives.
- **The measurement detects real interference.** When the model is given a higher priority than the loop (C), the loop doesn't run at all for 8.6 seconds (run 1) or 16.2 seconds (run 2), and 14–27% of cycles miss. That is the failure Foreman's configuration rules out: the model runs as an unprivileged user, and QNX caps unprivileged processes at priority 63. The workflow checks this cap on every run: `on -p 71` as `qnxuser` fails with "Operation not permitted".
- **The remaining misses come from virtualization.** Phase D loads only the host while QNX is idle, and still produces misses and a 1 ms worst case. So the small number of late cycles in A2, B2 and the session comes from the host and the hypervisor, not from QNX scheduling. On real hardware the worst-case figures would be lower.
- **CPU pinning wasn't needed.** Partitioning the cores (B2) didn't improve on priority-only scheduling (B) in this environment. Pinning is still available (`fm-plant -c <cpu>`, `on -C <cpu>`) for systems that want a dedicated core.
- **On a 2-vCPU host the picture is different, and the control phase explains why.** On GitHub's 2-vCPU private runners, the guest's vCPUs compete with the host for both cores. There, phase B showed about 1–2% misses and phase D (host stress alone) showed 34%. That run is why the experiment includes phase D.

## A note on the demo recording

The demo video is recorded on the CI host, and a browser on the host competes with the virtual machine for CPU, the same effect as phase D. The workflow keeps that small in three ways:
- QEMU runs on host CPUs 2–3 and the browser on CPUs 0–1 (CPU affinity only).
- Chrome's screencast frames are captured during the scenario instead of encoding video live.
- The MP4 is encoded only after the QNX workload has stopped.

The loop's timing during each recording is saved next to the video (`timing-during-recording.json`, on the `media` branch). For the current recordings:

| Recording | Run | Misses | p99 wake-up latency |
|---|---|---|---|
| industrial | [45985812464](https://github.com/sihlangu-mthembu/foreman/actions/runs/45985812464) | 49 of 206,840 (0.024%) | 50 µs |
| automotive | [45982834112](https://github.com/sihlangu-mthembu/foreman/actions/runs/45982834112) | 48 of 108,308 (0.044%) | 60 µs |
| medical | [45982837636](https://github.com/sihlangu-mthembu/foreman/actions/runs/45982837636) | 64 of 80,789 (0.079%) | 70 µs | Earlier attempts show how much the recording setup matters:
- Live encoding with no affinity split: 0.2–0.4% misses.
- Giving QEMU's threads real-time priority starved QEMU's own I/O thread and made things far worse.

A real controller has no screen recorder running on it. The timing experiment above runs without a browser and without any pinning.

## Reproduce

```sh
# in this repository: Actions → qnx → Run workflow (bench: true)
# or locally, with the stack deployed as in the README and QEMU forwarding SSH on port 2222:
ci/bench.sh 60
```

## Runs

| Run | Host | Notes |
|---|---|---|
| [45889173085](https://github.com/sihlangu-mthembu/foreman/actions/runs/45889173085) | 4 vCPU, AMD EPYC | session and timing experiment |
| [45895830849](https://github.com/sihlangu-mthembu/foreman/actions/runs/45895830849) | 4 vCPU, Intel Xeon 6975P-C | session and timing experiment (guest CPU with AMX/AVX-512 hidden, see below) |

## Note: booting QNX 8.0 under KVM on recent Intel server CPUs

On runners with an Intel Xeon 6975P-C, QNX 8.0 stops during early boot ("Too early for per-cpu data") when QEMU passes the host CPU through (`-cpu host`). Hiding LA57 alone does not help. Hiding the AMX and AVX-512 feature flags lets it boot. We checked this with `boot-probe.yml`, which tries every CPU model on 4 runners. On AMD EPYC runners, plain passthrough works. `ci/qemu-up.sh` therefore boots with `-cpu host,-la57,-amx-tile,-amx-int8,-amx-bf16,-avx512f,-avx512vl,-avx512bw,-avx512dq,-avx512cd`, which works on every runner seen so far.
