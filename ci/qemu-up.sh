#!/bin/bash
# Boot the mkqnximage x86_64 image headless with user-mode networking (no bridge needed).
# Usage: ci/qemu-up.sh <image dir>   -> SSH on localhost:2222, dashboard on localhost:8080
# Retries if the guest stops during early boot; QNX occasionally does so with some host CPU models.
set -u
D=${1:-img}; cd "$D"
echo "host CPU: $(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2 | xargs)"
KVM="--cpu max"; [ -w /dev/kvm ] && KVM="--enable-kvm --cpu host"
# QNX 8.0 stops in early boot ("Too early for per-cpu data") when AMX/AVX-512 features of recent Intel server
# CPUs (seen on Xeon 6975P-C) are passed through; hiding them boots on every runner CPU we have seen (Intel and
# AMD EPYC). Foreman's llama.cpp build uses AVX2, so nothing is lost. Plain passthrough is the fallback.
CPUS=("host,-la57,-amx-tile,-amx-int8,-amx-bf16,-avx512f,-avx512vl,-avx512bw,-avx512dq,-avx512cd" "host" "host,-la57")
for attempt in 1 2 3; do
  [ -w /dev/kvm ] && KVM="--enable-kvm --cpu ${CPUS[$((attempt - 1))]}"
  rm -f console.log qemu.pid
  PIN=""; [ -n "${QPIN:-}" ] && PIN="taskset -c $QPIN"   # optional: keep the VM on dedicated host CPUs
  $PIN qemu-system-x86_64 -smp ${QCPU:-2} $KVM -m ${QRAM:-3G} \
    -kernel output/ifs.bin -drive file=output/disk-qemu.vmdk,if=ide,id=drv0 \
    -netdev user,id=net0,hostfwd=tcp::2222-:22,hostfwd=tcp::8080-:8080 -device virtio-net-pci,netdev=net0 \
    -object rng-random,filename=/dev/urandom,id=rng0 -device virtio-rng-pci,rng=rng0 \
    -display none -serial file:console.log -pidfile qemu.pid -daemonize
  echo "attempt $attempt: qemu started (pid $(cat qemu.pid)), $KVM"
  for i in $(seq 1 40); do
    if grep -q "Shutdown\[" console.log 2>/dev/null; then echo "guest stopped during boot:"; grep -m3 -E "Shutdown|WARNING" console.log; break; fi
    if timeout 5 bash -c 'exec 3<>/dev/tcp/127.0.0.1/2222 && head -c 4 <&3' 2>/dev/null | grep -q SSH; then
      echo "ssh up after ~$((i * 3)) s"; exit 0
    fi
    sleep 3
  done
  echo "--- console (last lines)"; tail -6 console.log 2>/dev/null
  kill "$(cat qemu.pid)" 2>/dev/null; sleep 2
done
echo "QNX did not boot"; tail -30 console.log; exit 1
