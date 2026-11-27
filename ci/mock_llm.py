# Minimal stand-in for llama-server used by ci/test-native.sh: first asks for tools, then answers.
import json, http.server
class H(http.server.BaseHTTPRequestHandler):
    def do_POST(self):
        req = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        msgs = req['messages']
        tool_msgs = [m for m in msgs if m.get('role') == 'tool']
        if len(tool_msgs) == 0:
            msg = {"role":"assistant","content":None,"tool_calls":[
              {"id":"c1","type":"function","function":{"name":"get_status","arguments":"{}"}},
              {"id":"c2","type":"function","function":{"name":"get_trend","arguments":json.dumps({"signal":"motor_temp","minutes":2})}}]}
        elif len(tool_msgs) == 2:
            msg = {"role":"assistant","content":None,"tool_calls":[{"id":"c3","type":"function","function":{"name":"search_manual","arguments":json.dumps({"query":"E42 jam recovery"})}}]}
        else:
            def brief(c):
                try: return json.loads(c).get("in_words") or c[:160]
                except Exception: return c[:160]
            summary = " | ".join(brief(m['content']) for m in tool_msgs)
            msg = {"role":"assistant","content":"MOCK ANSWER based on: " + summary}
        out = {"choices":[{"message":msg}],"usage":{"prompt_tokens":100,"completion_tokens":20}}
        b = json.dumps(out).encode(); self.send_response(200); self.send_header('Content-Type','application/json'); self.send_header('Content-Length',str(len(b))); self.end_headers(); self.wfile.write(b)
    def log_message(self,*a): pass
http.server.HTTPServer(('127.0.0.1',int(__import__('sys').argv[1]) if len(__import__('sys').argv)>1 else 8081),H).serve_forever()
