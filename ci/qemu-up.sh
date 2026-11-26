#!/bin/bash
# Boot the mkqnximage x86_64 image headless with user-mode networking (no bridge needed).
# Usage: scripts/qemu-up.sh <image dir>   -> SSH on localhost:2222, console in <image dir>/console.log
set -u
D=${1:-img}; cd "$D"
KVM="--cpu max"; [ -w /dev/kvm ] && KVM="--enable-kvm --cpu host"
RT=""; [ -n "${QRT:-}" ] && RT="sudo chrt -r ${QRT}"   # host real-time priority for the vCPUs
$RT qemu-system-x86_64 -smp ${QCPU:-2} $KVM -m ${QRAM:-2G} \
  -kernel output/ifs.bin -drive file=output/disk-qemu.vmdk,if=ide,id=drv0 \
  -netdev user,id=net0,hostfwd=tcp::2222-:22,hostfwd=tcp::8080-:8080 -device virtio-net-pci,netdev=net0 \
  -object rng-random,filename=/dev/urandom,id=rng0 -device virtio-rng-pci,rng=rng0 \
  -display none -serial file:console.log -pidfile qemu.pid -daemonize
sudo chmod a+r qemu.pid 2>/dev/null; echo "qemu started (pid $(cat qemu.pid)), kvm: $KVM, host rt: ${QRT:-none}"
for i in $(seq 1 60); do
  if timeout 5 bash -c 'exec 3<>/dev/tcp/127.0.0.1/2222 && head -c 4 <&3' 2>/dev/null | grep -q SSH; then echo "ssh up after ${i}x3s"; exit 0; fi
  sleep 3
done
echo "ssh did not come up"; tail -40 console.log; exit 1
