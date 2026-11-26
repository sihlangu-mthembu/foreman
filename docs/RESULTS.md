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

## Results (run 45889173085)

| Phase | Configuration | Cycles | Deadline misses | p99 wake | p99.9 wake | Worst wake |
|---|---|---|---|---|---|---|
| A | Idle: control loop only | 61,133 | **0** | 20 µs | 150 µs | 0.43 ms |
| **B** | **AI generating on both vCPUs, priority 10, unprivileged user** | **65,099** | **0** | **20 µs** | **50 µs** | **0.64 ms** |
| D | Control: guest idle, host CPUs saturated | 61,114 | 4 | 30 µs | 110 µs | 1.01 ms |
| A2 | Idle, control loop pinned to vCPU 1 | 61,121 | 2 | 30 µs | 210 µs | 1.01 ms |
| B2 | AI pinned to vCPU 0, control loop pinned to vCPU 1 | 61,234 | 30 | 70 µs | 270 µs | 1.06 ms |
| **C** | **Misconfigured: AI at privileged priority 71, above the loop** | **61,243** | **8,578** | n/a | n/a | **8,553 ms** |

During the full interactive technician session (about 109 s, with questions, fault injection and model inference), the loop ran 108,684 cycles with 15 misses (0.014%). The worst wake latency was 2.3 ms.

## Reading the results

- **Priority alone protects the loop.** With the model saturating both vCPUs (B), the loop missed nothing and its 99.9th-percentile wake latency was 50 µs. The QNX scheduler preempts the priority-10 inference threads the instant the priority-70 timer pulse arrives.
- **The measurement detects real interference.** When the model is given a higher priority than the loop (C), the loop doesn't run at all for 8.5 seconds, and 14% of cycles miss. That is the failure Foreman's configuration rules out: the model runs as an unprivileged user, and QNX caps unprivileged processes at priority 63. The workflow checks this cap on every run: `on -p 71` as `qnxuser` fails with "Operation not permitted".
- **The remaining misses come from virtualization.** Phase D loads only the host while QNX is idle, and still produces misses and a 1 ms worst case. So the small number of late cycles in A2, B2 and the session comes from the host and the hypervisor, not from QNX scheduling. On real hardware the worst-case figures would be lower.
- **CPU pinning wasn't needed.** Partitioning the cores (B2) didn't improve on priority-only scheduling (B) in this environment. Pinning is still available (`fm-plant -c <cpu>`, `on -C <cpu>`) for systems that want a dedicated core.
- **On a 2-vCPU host the picture is different, and the control phase explains why.** On GitHub's 2-vCPU private runners, the guest's vCPUs compete with the host for both cores. There, phase B showed about 1–2% misses and phase D (host stress alone) showed 34%. That run is why the experiment includes phase D.

## Reproduce

```sh
# in this repository: Actions → qnx → Run workflow (bench: true)
# or locally, with the stack deployed as in the README and QEMU forwarding SSH on port 2222:
ci/bench.sh 60
```

## Runs

| Run | Host | Notes |
|---|---|---|
| [45889173085](https://github.com/sihlangu-mthembu/foreman/actions/runs/45889173085) | 4 vCPU | first public run: session and timing experiment |
