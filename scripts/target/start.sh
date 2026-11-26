#!/bin/sh
# Start the Foreman stack on the QNX target. Run as root from the install directory.
#   fm-plant       root, SCHED_FIFO priority 70   (safety-critical control loop; above 63, the ceiling for
#                                                  unprivileged processes, so no user process can outrank it)
#   fm-telemetryd  root, priority 20              (read-only /dev/foreman service)
#   llama-server   qnxuser, priority 10           (local LLM, both CPUs)
#   fm-agent       qnxuser, priority 10           (assistant + dashboard, abilities dropped)
# Usage: start.sh [profile] [llm-priority]
PROFILE=${1:-industrial}
LLM_PRIO=${2:-10}
D=$(cd "$(dirname "$0")" && pwd)
cd "$D"
slay -f fm-agent llama-server fm-telemetryd fm-plant 2>/dev/null
sleep 1
on -p 70f ./fm-plant -p "$PROFILE" -P 70 > /tmp/fm-plant.log 2>&1 &
sleep 1
on -p 20 ./fm-telemetryd /dev/foreman > /tmp/fm-telemetryd.log 2>&1 &
sleep 1
on -p "$LLM_PRIO" -u qnxuser ./llama-server -m ./model.gguf -c 4096 -t 2 --port 8081 --host 127.0.0.1 \
   --jinja --no-webui -a foreman > /tmp/llama-server.log 2>&1 &
on -p "$LLM_PRIO" -u qnxuser ./fm-agent --port 8080 --llm http://127.0.0.1:8081 \
   --telemetry /dev/foreman --profiles ./profiles --web ./web > /tmp/fm-agent.log 2>&1 &
echo "started profile=$PROFILE llm_priority=$LLM_PRIO"
