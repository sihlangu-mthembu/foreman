#!/bin/bash
# Copy the Foreman stack into the running QNX guest at /data/foreman
set -e
S=ci/qnx-ssh.sh
$S run 'mkdir -p /data/home/qnxuser/foreman'
$S put llama-bin/llama-server llama-bin/llama-bench build-qnx/fm-plant build-qnx/fmctl \
      build-qnx/fm-telemetryd build-qnx/fm-agent scripts/target/start.sh /data/home/qnxuser/foreman/
$S put models/model.gguf /data/home/qnxuser/foreman/model.gguf
tar -cf - profiles web | sshpass -p qnxuser ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -p 2222 qnxuser@127.0.0.1 'cd /data/home/qnxuser/foreman && tar -xf -'
$S run 'ls -la /data/home/qnxuser/foreman'
