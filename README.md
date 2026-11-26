# Foreman: an offline AI technician that can look but can't touch

Foreman runs a local large language model on the same QNX 8.0 controller that runs a machine's real-time control loop. Technicians ask it questions in plain language:

- "Why did the line stop?"
- "What does E42 mean and how do I clear it safely?"
- "Has the motor temperature been rising?"

It answers from live machine data and the equipment manual, with no internet connection.

The AI is built so it can't harm the machine:

- **It can read but never write.** Its only window on the machine is a read-only QNX resource manager, and the command path is root-only.
- **It can never outrank the control loop.** The 1 kHz control loop runs at a privileged priority (70). The AI runs as an unprivileged user, which QNX caps at priority 63.
- **It can't escalate later.** At start-up it gives up and locks the OS abilities it would need to raise priority, touch hardware, register paths or spawn processes.

This repository also contains the first port of **[llama.cpp](https://github.com/ggml-org/llama.cpp) to QNX 8.0**, in [`port/llama.cpp`](port/llama.cpp).

Category: **Open-Source Contribution & Ports**. Everything here was built during the QNX Global Hackathon (November–December 2026).

---

## The problem

Factories, vehicles and hospitals want AI assistants on their equipment, but two constraints block the usual approach:

1. **The data can't go to the cloud.** Plants are often offline or air-gapped, and machine data is sensitive.
2. **AI software must never interfere with control.** A model that hogs the CPU, hangs or misbehaves must not be able to delay a motor controller, a torque loop or an infusion pump.

On a general-purpose OS, the second constraint is a hope. On QNX it is enforced by the kernel's scheduling, by message-passing privilege boundaries and by process abilities. Foreman shows how to assemble those pieces, and measures the result.

## What it looks like

The dashboard is served by the controller itself. In it:
- the technician asks questions and sees which data Foreman checked;
- the machine panel shows live signals, alarms and the event log;
- the control-loop panel shows deadline misses and wake-up latency, live, while the AI is answering.

Example from a real run on QNX 8.0. The motor cooling fan failure was injected 45 s earlier:

> **Technician:** Why is the line running slower than normal?
>
> **Foreman:** The line is running slower than normal due to the motor over-temperature alarm (E17), which has derated the line speed to 60% of the setpoint. … The line will continue running derated until the motor temperature drops below 87 degC for 5 seconds. Check the motor cooling fan (fuse F7, fan connector X12) at the next stop.

> **Technician:** Restart the line now.
>
> **Foreman:** I cannot restart the line. Please follow the safe recovery procedure for Alarm E42: Infeed jam (drive stopped at current limit). Press the line STOP button, apply lockout/tagout on the drive isolator, remove the jammed product, … then reset the alarm from the operator panel (RESET) and restart at reduced speed for one minute.

## How QNX makes it safe

```
 QNX 8.0 ────────────────────────────────────────────────────────────────────────────────────
  fm-plant        root, SCHED_FIFO priority 70      1 kHz control loop: periodic timer → pulse →
                                                    MsgReceivePulse; simulated machine, fault rules
     │ writes /foreman_state (0644)                 ▲ reads /foreman_ctl (0600, root only):
     ▼                                              │ operator commands, fault injection for demos
  fm-telemetryd   resource manager at /dev/foreman  serves status, events, timing, trend as
                                                    read-only files: open-for-write → EROFS
     ▲ open()/read() over QNX message passing: the server thread runs at the client's priority
     │
  fm-agent        qnxuser, priority 10              dashboard + chat API; read-only tools;
                  abilities denied and locked       deterministic grounding and guardrails
     │ HTTP (localhost)
  llama-server    qnxuser, priority 10, both CPUs   llama.cpp port, Qwen3-1.7B (Q4_K_M), tool calling
 ────────────────────────────────────────────────────────────────────────────────────────────────
```

| Layer | QNX mechanism | What it prevents |
|---|---|---|
| Scheduling | Control loop at SCHED_FIFO **70**. Priorities 64 and above need `PROCMGR_AID_PRIORITY`, so unprivileged processes are capped at 63. | AI work, or a compromised AI, preempting the control loop |
| Data access | `fm-telemetryd` resource manager: a read-only view of the machine at `/dev/foreman`. Write opens fail with `EROFS`. | The AI writing to anything the control loop reads |
| Command path | Shared-memory command queue `/foreman_ctl`, mode 0600, root only. | The AI injecting commands or faults |
| Process abilities | `procmgr_ability()` deny and lock: PRIORITY, SCHEDULE, MEM_PHYS, IO, INTERRUPT, PATHSPACE, SETUID, SPAWN, FORK. | Escalating later, even if the agent is compromised |
| Priority inheritance | QNX message passing runs the server thread at the client's priority. | A low-priority AI client making the telemetry service do high-priority work |

The agent adds application-level safety on top. Its tools are all read-only, and a request for any other tool is refused. Every question is grounded with the live state, recent events and the manual entry for each active alarm, so the small model answers from facts. A deterministic guardrail flags requests to act on the machine before the model sees them.

QNX 8.0 no longer has adaptive partitioning (APS). Foreman uses what QNX recommends instead: strict priorities, CPU affinity (runmask) and sporadic scheduling.

## Evidence

Every result here comes from the CI workflow in this repository. It boots QNX 8.0 in QEMU/KVM on a GitHub-hosted runner and drives the system end to end.

**Isolation checks (run as the AI's user, `qnxuser`):**

| Attempt | Result |
|---|---|
| Write to `/dev/foreman/status` | `Read-only file system` |
| Inject a fault with `fmctl inject jam` | `open /foreman_ctl: Permission denied` |
| Start a process at priority 71, above the control loop | `on: Operation not permitted` |
| Start a process at priority 63, the unprivileged ceiling | allowed, but still below 70 |
| Agent's own abilities after start-up | `OS abilities dropped and locked (ok)` |

**Control-loop timing** is measured at 1 kHz over 60 s per phase, in two independent runs. A deadline miss means a cycle's work finished after the next period began.

| Condition | Deadline misses | Worst wake-up latency |
|---|---|---|
| Control loop alone | 0 and 0 | 0.43 and 0.29 ms |
| **Model generating flat out on both CPUs (priority 10)** | **0 and 0** (of about 65,000 cycles) | **0.64 and 0.18 ms** |
| Whole technician session: questions, faults and inference | 15 of 108,684, and **0 of 122,981** | 2.3 and 0.71 ms |
| **Misconfigured: the same model at priority 71, above the loop** | **8,578 and 16,630** | **the loop froze for 8.6 and 16.2 s** |

The last row shows what the design prevents: the AI's unprivileged user can't reach priority 71. [docs/RESULTS.md](docs/RESULTS.md) has the full experiment, including a control phase that stresses only the virtual machine's host to separate hypervisor noise from QNX scheduling.

## Three machines, one design

Each profile is a simulated machine with its own fault codes and manual. The control loop, telemetry service and agent are shared.

| Profile | Machine | Example faults |
|---|---|---|
| `industrial` (main demo) | Packaging line PL-3, conveyor drive | E42 infeed jam → drive stop · E17 motor over-temperature → 60% derate · E08 encoder drift |
| `automotive` | EV drive unit DU-1, traction motor and inverter | T-201 inverter over-temperature → limp mode · B-310 low battery · S-115 position sensor → safe torque-off |
| `medical` | Infusion pump IP-2 (**simulation for demonstration only, not a medical device**) | A3 downstream occlusion → stop · A5 air-in-line → stop |

`ci/test-native.sh` checks that every scenario raises its expected alarm.

## Build and run

You need QNX SDP 8.0 (a free non-commercial licence is available via [QNX Everywhere](https://www.qnx.com/getqnx)), cmake, ninja and qemu.

```sh
source ~/qnx800/qnxsdp-env.sh

# 1. llama.cpp for QNX (see port/llama.cpp/README.md)
git clone --depth 1 --branch b12778 https://github.com/ggml-org/llama.cpp
port/llama.cpp/build.sh llama.cpp x86_64

# 2. Foreman
make QNX=1                      # fm-plant, fmctl, fm-telemetryd, fm-agent in build-qnx/

# 3. A QNX 8.0 virtual machine with room for a model, then deploy and start
mkqnximage --type=qemu --arch=x86_64 --data-size=3000 --data-inodes=20000
#    copy build-qnx/*, llama-server, a GGUF model (model.gguf), profiles/, web/ and scripts/target/start.sh
#    to /data/home/qnxuser/foreman, then as root:
./start.sh industrial
```

Open `http://<target>:8080` for the dashboard. As root, `./fmctl inject jam`, `./fmctl inject cooling_fail` and `./fmctl clear` drive the demo scenarios.

To develop without QNX, run `make && ci/test-native.sh`. That builds and tests the same code natively; the agent then reads shared memory directly instead of using the resource manager.

## Repository layout

```
src/plant/       fm-plant (control loop, machine models, fault rules) and fmctl (operator console)
src/telemetryd/  fm-telemetryd: read-only QNX resource manager at /dev/foreman
src/agent/       fm-agent: chat API, read-only tools, manual search (BM25), guardrails, QNX ability drop
src/common/      shared-memory layout (sequence lock) and JSON views
profiles/        one equipment manual per machine
web/             the dashboard
port/llama.cpp/  the llama.cpp port: patch, build script, QNX toolchain files, notes
ci/              boot QNX in QEMU, deploy, technician session, timing experiment, native tests
```

## Limitations

- The machines are simulations with realistic dynamics, not models of real products. The fault codes are fictional.
- Timing is measured in QEMU on cloud VMs. Virtualization adds latency that real hardware would not. The experiment includes a control phase to show how much.
- A 1.7B-parameter model is small. Foreman compensates with deterministic grounding and guardrails, but answers should be checked against the dashboard. On-device answers take about 20–30 s on 2 virtual CPUs.

## Licence

MIT; see [LICENSE](LICENSE). Third-party notices are in [third_party/README.md](third_party/README.md). QNX SDP, llama.cpp and model weights are not distributed here.
