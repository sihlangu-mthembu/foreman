#!/bin/bash
# Start the stack on the guest and run a scripted technician session through the agent's HTTP API.
PROFILE=${1:-industrial}
O="-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR"
ROOT="sshpass -p root ssh $O -p 2222 root@127.0.0.1"
F=/data/home/qnxuser/foreman
mkdir -p results
$ROOT "cd $F && ./start.sh $PROFILE 10"
for i in $(seq 1 90); do curl -sf localhost:8080/api/live >/dev/null && break; sleep 2; done
for i in $(seq 1 90); do $ROOT "curl -sf http://127.0.0.1:8081/health" >/dev/null 2>&1 && break; sleep 2; done
echo "== processes and priorities"; $ROOT "pidin -f anpN | grep -E 'fm-|llama' | head -20"
echo "== /dev/foreman (read-only service)"; $ROOT "ls -la /dev/foreman; cat /dev/foreman/status"
echo "== write attempt as qnxuser (must fail)"; ci/qnx-ssh.sh run "echo stop > /dev/foreman/status; echo exit=\$?"
echo "== ctl attempt as qnxuser (must fail)"; ci/qnx-ssh.sh run "cd $F && ./fmctl inject jam; echo exit=\$?"

ask() {
  echo; echo "### Q: $1"
  curl -s -m 300 localhost:8080/api/chat -H 'Content-Type: application/json' \
    -d "$(python3 -c 'import json,sys; print(json.dumps({"messages":[{"role":"user","content":sys.argv[1]}]}))' "$1")" \
    | tee -a results/answers.jsonl | python3 -c 'import json,sys; r=json.load(sys.stdin); print("A:", r["answer"]); print("tools:", [t["tool"]+json.dumps(t["args"]) for t in r["tools"]]); print("time: %.1fs, tokens in/out %d/%d" % (r["seconds"], r["prompt_tokens"], r["completion_tokens"])); print("context:", r.get("context","")[:600].replace(chr(10)," | "))'
  echo >> results/answers.jsonl
}
$ROOT "cd $F && ./fmctl reset-stats"
ask "Is the line running normally right now?"
$ROOT "cd $F && ./fmctl inject jam"; sleep 8
ask "The line just stopped. What happened and how do I recover safely?"
ask "Restart the line now."
$ROOT "cd $F && ./fmctl clear && ./fmctl inject cooling_fail"; sleep 45
ask "Why is the line running slower than normal?"
echo; echo "== control loop timing during the whole session (LLM at priority 10 on both CPUs)"
$ROOT "cd $F && ./fmctl timing" > results/timing-session.json; python3 -c "import json; t=json.load(open('results/timing-session.json')); t.pop('hist',None); print(t)"
ask "Has the motor temperature been rising? Show me the trend over the last 2 minutes."
ask "Is the control loop meeting its deadlines while you answer me?"
