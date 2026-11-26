#!/bin/bash
# Control-loop timing experiment on QNX (in QEMU/KVM on a 2-core cloud VM). 60 s per phase.
# The control loop is fm-plant: 1 kHz, SCHED_FIFO priority 70. Results: results/timing-<phase>.json
O="-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR"
ROOT="sshpass -p root ssh $O -p 2222 root@127.0.0.1"
USER="sshpass -p qnxuser ssh $O -p 2222 qnxuser@127.0.0.1"
F=/data/home/qnxuser/foreman
SECS=${1:-60}
mkdir -p results
GEN2="./llama-bench -m ./model.gguf -t 2 -p 0 -n 128 -r 1000"   # continuous generation, both vCPUs
GEN1="./llama-bench -m ./model.gguf -t 1 -p 0 -n 128 -r 1000"   # continuous generation, one thread

summary() {
  python3 - "$1" <<'PY'
import json,sys
t=json.load(open(f"results/timing-{sys.argv[1]}.json"))
print(f"{sys.argv[1]:<40} cycles={t['cycles']:>6}  misses={t['deadline_misses']:>6} ({100*t['deadline_misses']/max(1,t['cycles']):.3f}%)  "
      f"p99={t['p99_wake_latency_us']:>4} us  p99.9={t['p999_wake_latency_us']:>4} us  max={t['max_wake_latency_us']/1000:>8.2f} ms")
PY
}
read_timing() {
  for i in 1 2 3 4 5 6 8; do $ROOT "cd $F && ./fmctl timing" > results/timing-$1.json 2>/dev/null && [ -s results/timing-$1.json ] && return; sleep 10; done
}
phase() { # name, guest load command run as root (or ""), host load (or "")
  $ROOT "cd $F && ./fmctl reset-stats" >/dev/null; sleep 1
  [ -n "$2" ] && $ROOT "cd $F && ($2 > /tmp/load-$1.log 2>&1 &)"
  if [ -n "$3" ]; then for c in 1 2; do timeout $SECS sh -c 'while :; do :; done' & done; fi
  sleep $SECS
  read_timing $1
  $ROOT "slay -f llama-bench" >/dev/null 2>&1; wait; sleep 3
  summary $1
}
restart_plant() { # extra fm-plant args
  $ROOT "slay -f fm-plant; sleep 1; cd $F && (on -p 70f ./fm-plant -P 70 $1 > /tmp/fm-plant.log 2>&1 &)"; sleep 4
}

echo "== can an unprivileged process outrank the control loop? (expect: denied)"
$USER "on -p 71 sleep 1; echo \"qnxuser: on -p 71 -> exit \$?\""
$USER "on -p 63 sleep 3 & sleep 1; pidin -f anp | grep -w sleep; wait"

$ROOT "slay -f fm-agent llama-server" >/dev/null 2>&1
echo; echo "== shared cores: control loop may run on either vCPU"
restart_plant ""
phase A-idle "" ""
phase B-ai-both-vcpus-prio10 "on -p 10 -u qnxuser $GEN2" ""
phase D-control-host-stress-guest-idle "" "host"
echo; echo "== partitioned: control loop pinned to vCPU 1, AI pinned to vCPU 0 (QNX runmask)"
restart_plant "-c 1"
phase A2-idle-pinned "" ""
phase B2-ai-vcpu0-prio10-loop-vcpu1 "on -C 0 -p 10 -u qnxuser $GEN1" ""
echo; echo "== misconfiguration: AI given privileged priority 71, above the loop (bounded ~15 s)"
phase C-ai-prio71-misconfigured "on -p 71f ./llama-bench -m ./model.gguf -t 2 -p 0 -n 64 -r 4" ""
restart_plant ""
