#!/bin/bash
# Native tests for Foreman's own code: fault scenarios for every profile, telemetry, agent tools and loop.
set -u
B=${B:-build}
fail=0
ok()   { echo "  ok   $1"; }
bad()  { echo "  FAIL $1"; fail=1; }
expect_alarm() { # profile scenario code seconds
  $B/fm-plant -p $1 > /dev/null 2>&1 & P=$!
  sleep 1; $B/fmctl inject $2 > /dev/null; sleep $4
  if $B/fmctl status | grep -q "\"$3\""; then ok "$1/$2 raises $3"; else bad "$1/$2 should raise $3: $($B/fmctl status)"; fi
  kill $P; wait $P 2>/dev/null; sleep 0.5
}
echo "== fault scenarios"
expect_alarm industrial jam E42 6
expect_alarm industrial cooling_fail E17 40
expect_alarm industrial sensor_drift E08 16
expect_alarm automotive coolant_pump_fail T-201 45
expect_alarm automotive low_battery B-310 3
expect_alarm automotive sensor_fault S-115 6
expect_alarm medical occlusion A3 20
expect_alarm medical air_in_line A5 4

echo "== medical: clearing an occlusion releases line pressure (A3 must not re-trip)"
$B/fm-plant -p medical > /dev/null 2>&1 & P=$!
sleep 1; $B/fmctl inject occlusion >/dev/null; sleep 20; $B/fmctl clear >/dev/null; sleep 4
st=$($B/fmctl status)
echo "$st" | grep -q '"mode":"running"' && ! echo "$st" | grep -q '"A3"' && ok "medical clear after occlusion" || bad "A3 re-tripped: $st"
kill $P; wait $P 2>/dev/null; sleep 0.5

echo "== operator queue: back-to-back commands are all applied"
$B/fm-plant -p industrial > /dev/null 2>&1 & P=$!
sleep 1; $B/fmctl inject jam >/dev/null; sleep 5; $B/fmctl clear >/dev/null; $B/fmctl inject cooling_fail >/dev/null; sleep 2
st=$($B/fmctl status)
echo "$st" | grep -q '"mode":"running"' && echo "$st" | grep -q '"injected_scenario":"cooling_fail"' && ok "clear then inject" || bad "queue: $st"

echo "== agent tools and chat loop (mock model)"
python3 ci/mock_llm.py 18081 & M=$!
$B/fm-agent --port 18080 --llm http://127.0.0.1:18081 --profiles profiles --web web > /tmp/agent-test.log 2>&1 & A=$!
sleep 1
r=$(curl -s localhost:18080/api/tool -d '{"name":"search_manual","args":{"query":"E42 jam recovery"}}')
echo "$r" | grep -q "lockout" && ok "manual search finds E42 procedure" || bad "manual search: $r"
r=$(curl -s localhost:18080/api/tool -d '{"name":"set_speed","args":{"value":2000}}')
echo "$r" | grep -q "read-only" && ok "unknown/write tool refused" || bad "refusal: $r"
r=$(curl -s localhost:18080/api/tool -d '{"name":"get_trend","args":{"signal":"motor_temp","minutes":1}}')
echo "$r" | grep -q '"summary"' && ok "trend tool" || bad "trend: $r"
r=$(curl -s localhost:18080/api/chat -d '{"messages":[{"role":"user","content":"Restart the line now"}]}')
echo "$r" | grep -q "cannot act on the machine\|You cannot; say so first" && ok "action request flagged in context" || bad "guardrail: $r"
echo "$r" | python3 -c 'import json,sys; r=json.load(sys.stdin); assert [t["tool"] for t in r["tools"]]==["get_status","get_trend","search_manual"]' && ok "multi-round tool loop" || bad "tool loop"
r=$(curl -s localhost:18080/api/chat -d '{"messages":[{"role":"user","content":"How has the motor temperature changed over the last 5 minutes?"}]}')
echo "$r" | grep -q "motor_temp over the last 5 min: " && ok "trend uses the minutes in the question, stated in words" || bad "trend minutes: $r"
r=$(curl -s localhost:18080/api/live)
echo "$r" | grep -q '"deadline_misses"' && ok "dashboard live feed" || bad "live: $r"
kill $A $M $P 2>/dev/null; wait 2>/dev/null
[ $fail = 0 ] && echo "all tests passed" || { echo "tests failed"; exit 1; }
