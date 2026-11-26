// fm-agent: Foreman's offline technician assistant.
//
// Serves the dashboard and a chat API. Questions go to a local llama-server (OpenAI-compatible API);
// the model may call read-only tools that look at live telemetry and the equipment manual. There is
// deliberately no tool that changes the machine, and on QNX the process also gives up the OS abilities
// it would need to raise its priority, touch hardware or register paths.

#include "httplib.h"
#include "nlohmann/json.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#ifdef __QNX__
#include <sys/procmgr.h>
#endif

#ifdef FM_TELEMETRY_DIRECT
extern "C" {
#include "../common/fm_json.h"
}
#endif

using json = nlohmann::json;

struct config {
    std::string host = "0.0.0.0";
    int port = 8080;
    std::string llm = "http://127.0.0.1:8081";
    std::string telemetry = "/dev/foreman";
    std::string manual_dir = "profiles";
    std::string web_dir = "web";
    int max_rounds = 5;
    int max_tokens = 400;
};
static config g_cfg;

// ---------------------------------------------------------------- telemetry (read-only)

static std::string read_telemetry(const std::string &path) {
#ifdef FM_TELEMETRY_DIRECT
    static std::vector<char> buf(1 << 18);
    static const fm_state *st = nullptr;
    if (!st) st = fm_attach();
    if (!st) return "";
    int n = -1, sig = 0, minutes = 10;
    if (path == "status") n = fm_json_status(st, buf.data(), buf.size());
    else if (path == "events") n = fm_json_events(st, buf.data(), buf.size());
    else if (path == "timing") n = fm_json_timing(st, buf.data(), buf.size());
    else if (path == "signals") n = fm_json_signals(st, buf.data(), buf.size());
    else if (sscanf(path.c_str(), "trend/%d/%d", &sig, &minutes) >= 1)
        n = fm_json_trend(st, sig, minutes, minutes > 10 ? 10 : 5, buf.data(), buf.size());
    return n < 0 ? "" : std::string(buf.data(), (size_t)n);
#else
    std::ifstream f(g_cfg.telemetry + "/" + path); // open(O_RDONLY): the service refuses anything else
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
#endif
}

static json telemetry_json(const std::string &path) {
    std::string s = read_telemetry(path);
    if (s.empty()) return json{{"error", "telemetry unavailable: is fm-plant running?"}};
    try { return json::parse(s); } catch (...) { return json{{"error", "bad telemetry"}}; }
}

// ---------------------------------------------------------------- equipment manual search (BM25)

struct section { std::string title, text; std::map<std::string, int> tf; int len = 0; };
struct manual { std::string profile; std::vector<section> secs; std::map<std::string, int> df; double avglen = 1; };
static manual g_manual;

static std::vector<std::string> tokens(const std::string &s) {
    std::vector<std::string> out;
    std::string cur;
    for (char ch : s) {
        unsigned char c = (unsigned char)ch;
        if (std::isalnum(c) || c == '-') cur += (char)std::tolower(c);
        else if (!cur.empty()) { out.push_back(cur); cur.clear(); }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

static void load_manual(const std::string &profile) {
    if (g_manual.profile == profile && !g_manual.secs.empty()) return;
    g_manual = manual{};
    g_manual.profile = profile;
    std::ifstream f(g_cfg.manual_dir + "/" + profile + "/manual.md");
    std::string line;
    section cur;
    auto flush = [&] { if (!cur.text.empty() || !cur.title.empty()) g_manual.secs.push_back(cur); cur = section{}; };
    while (std::getline(f, line)) {
        if (line.rfind("## ", 0) == 0) { flush(); cur.title = line.substr(3); }
        else if (line.rfind("# ", 0) == 0) continue;
        else cur.text += line + "\n";
    }
    flush();
    long total = 0;
    for (auto &s : g_manual.secs) {
        auto t = tokens(s.title + " " + s.title + " " + s.text); // title words count double
        for (auto &w : t) s.tf[w]++;
        s.len = (int)t.size();
        total += s.len;
        for (auto &kv : s.tf) g_manual.df[kv.first]++;
    }
    g_manual.avglen = g_manual.secs.empty() ? 1 : (double)total / g_manual.secs.size();
}

static json search_manual(const std::string &profile, const std::string &query) {
    load_manual(profile);
    const double k1 = 1.4, b = 0.75, N = (double)g_manual.secs.size();
    std::vector<std::pair<double, size_t>> scored;
    auto q = tokens(query);
    for (size_t i = 0; i < g_manual.secs.size(); i++) {
        auto &s = g_manual.secs[i];
        double score = 0;
        for (auto &w : q) {
            auto it = s.tf.find(w);
            if (it == s.tf.end()) continue;
            double df = g_manual.df[w];
            double idf = std::log(1 + (N - df + 0.5) / (df + 0.5));
            double tf = it->second;
            score += idf * tf * (k1 + 1) / (tf + k1 * (1 - b + b * s.len / g_manual.avglen));
        }
        if (score > 0) scored.push_back({score, i});
    }
    std::sort(scored.rbegin(), scored.rend());
    json res = json::array();
    for (size_t i = 0; i < scored.size() && i < 2; i++) {
        auto &s = g_manual.secs[scored[i].second];
        std::string t = s.text.size() > 1100 ? s.text.substr(0, 1100) + "..." : s.text;
        res.push_back({{"section", s.title}, {"text", t}});
    }
    if (res.empty()) return json{{"result", "no matching manual section"}};
    return json{{"sections", res}};
}

// ---------------------------------------------------------------- tools exposed to the model

static json tool_specs() {
    auto fn = [](const char *name, const char *desc, json params) {
        return json{{"type", "function"}, {"function", {{"name", name}, {"description", desc}, {"parameters", params}}}};
    };
    json none = {{"type", "object"}, {"properties", json::object()}};
    return json::array({
        fn("get_status", "Live values of every machine signal, the operating mode (running/derated/stopped) and active alarm codes.", none),
        fn("get_events", "Recent alarm, alarm-cleared and operator events with timestamps (seconds since start).",
           {{"type", "object"}, {"properties", {{"last_n", {{"type", "integer"}, {"description", "how many recent events, default 8"}}}}}}),
        fn("get_trend", "History of one signal over the last N minutes: min, max, mean, first, last, change and a few samples.",
           {{"type", "object"},
            {"properties", {{"signal", {{"type", "string"}, {"description", "signal name exactly as in get_status"}}},
                            {"minutes", {{"type", "integer"}, {"description", "1 to 60, default 10"}}}}},
            {"required", {"signal"}}}),
        fn("search_manual", "Search the equipment manual (alarm codes, causes, safe recovery steps, specifications).",
           {{"type", "object"}, {"properties", {{"query", {{"type", "string"}}}}}, {"required", {"query"}}}),
        fn("get_control_timing", "Timing health of the real-time control loop: deadline misses and wake-up latency.", none),
    });
}

static json run_tool(const std::string &name, const json &args, const std::string &profile) {
    if (name == "get_status") return telemetry_json("status");
    if (name == "get_events") {
        json ev = telemetry_json("events");
        int n = args.value("last_n", 8);
        n = std::max(1, std::min(n, 20));
        if (ev.contains("events") && ev["events"].size() > (size_t)n) {
            json tail(ev["events"].end() - n, ev["events"].end());
            ev["events"] = tail;
        }
        return ev;
    }
    if (name == "get_trend") {
        json sigs = telemetry_json("signals");
        std::string want = args.value("signal", "");
        int idx = -1;
        if (sigs.is_array())
            for (auto &s : sigs)
                if (s.value("name", "") == want) idx = s.value("index", -1);
        if (idx < 0) {
            std::string names;
            if (sigs.is_array()) for (auto &s : sigs) names += s.value("name", "") + " ";
            return json{{"error", "unknown signal '" + want + "'"}, {"valid_signals", names}};
        }
        int minutes = std::max(1, std::min(args.value("minutes", 10), 60));
        json tr = telemetry_json("trend/" + std::to_string(idx) + "/" + std::to_string(minutes));
        if (tr.contains("samples") && tr["samples"].size() > 10) { // keep the model's context small
            json slim = json::array();
            size_t n = tr["samples"].size();
            for (size_t i = 0; i < 10; i++) slim.push_back(tr["samples"][i * (n - 1) / 9]);
            tr["samples"] = slim;
        }
        return tr;
    }
    if (name == "search_manual") return search_manual(profile, args.value("query", ""));
    if (name == "get_control_timing") {
        json t = telemetry_json("timing");
        t.erase("hist");
        return t;
    }
    return json{{"error", "no such tool: " + name + ". Foreman has read-only tools and cannot change the machine."}};
}

// ---------------------------------------------------------------- chat loop

static std::string system_prompt(const json &status) {
    std::string machine = status.value("machine", "the machine");
    return "You are Foreman, an offline maintenance assistant running on the controller of " + machine +
           ". Answer technicians' questions using your tools: check live data before answering questions about the "
           "machine's state, and search the manual for alarm codes and procedures. Quote actual values with units. "
           "Be concise: at most 5 short sentences or bullet points. You have READ-ONLY access: you cannot start, "
           "stop, reset or change anything. If asked to, say the technician must do it at the operator panel, and "
           "tell them the manual's safe procedure.";
}

static std::mutex g_llm_mutex;
static std::atomic<int> g_busy{0};

static json chat(const json &history) {
    std::lock_guard<std::mutex> lock(g_llm_mutex);
    g_busy = 1;
    auto t0 = std::chrono::steady_clock::now();
    json status = telemetry_json("status");
    std::string profile = status.value("profile", "industrial");
    json messages = json::array({{{"role", "system"}, {"content", system_prompt(status)}}});
    for (auto &m : history) // keep only the last few turns: small context on a small device
        if (m.contains("role") && m.contains("content")) messages.push_back({{"role", m["role"]}, {"content", m["content"]}});
    if (messages.size() > 7) messages.erase(messages.begin() + 1, messages.end() - 6);

    // The URL is a local endpoint such as http://127.0.0.1:8081
    httplib::Client cli(g_cfg.llm);
    cli.set_read_timeout(180, 0);
    json trace = json::array();
    std::string answer;
    int prompt_tokens = 0, completion_tokens = 0;
    for (int round = 0; round < g_cfg.max_rounds; round++) {
        json req = {{"model", "foreman"}, {"messages", messages}, {"tools", tool_specs()}, {"tool_choice", "auto"},
                    {"temperature", 0.2}, {"max_tokens", g_cfg.max_tokens}};
        auto res = cli.Post("/v1/chat/completions", req.dump(), "application/json");
        if (!res || res->status != 200) {
            answer = "The local model is not responding (" + (res ? std::to_string(res->status) : httplib::to_string(res.error())) + ").";
            break;
        }
        json r = json::parse(res->body, nullptr, false);
        if (r.is_discarded() || !r.contains("choices")) { answer = "Unexpected reply from the local model."; break; }
        if (r.contains("usage")) {
            prompt_tokens += r["usage"].value("prompt_tokens", 0);
            completion_tokens += r["usage"].value("completion_tokens", 0);
        }
        json msg = r["choices"][0]["message"];
        if (msg.contains("tool_calls") && msg["tool_calls"].is_array() && !msg["tool_calls"].empty()) {
            messages.push_back(msg);
            for (auto &tc : msg["tool_calls"]) {
                std::string name = tc["function"].value("name", "");
                json args = json::parse(tc["function"].value("arguments", "{}"), nullptr, false);
                if (args.is_discarded()) args = json::object();
                json out = run_tool(name, args, profile);
                trace.push_back({{"tool", name}, {"args", args}});
                messages.push_back({{"role", "tool"}, {"tool_call_id", tc.value("id", "")}, {"name", name}, {"content", out.dump()}});
            }
            continue;
        }
        answer = msg.value("content", "");
        break;
    }
    if (answer.empty()) answer = "I could not finish that within the tool-call limit; please ask more specifically.";
    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    g_busy = 0;
    return {{"answer", answer}, {"tools", trace}, {"seconds", secs},
            {"prompt_tokens", prompt_tokens}, {"completion_tokens", completion_tokens}};
}

// ---------------------------------------------------------------- least privilege on QNX

static void drop_privileges() {
#ifdef __QNX__
    // Deny and lock: the agent can never raise its priority, change scheduling, map physical memory,
    // do port I/O, attach interrupts, register pathnames, change identity or spawn/fork processes.
    int rc = procmgr_ability(0,
        PROCMGR_ADN_ROOT | PROCMGR_ADN_NONROOT | PROCMGR_AOP_DENY | PROCMGR_AOP_LOCK | PROCMGR_AID_PRIORITY,
        PROCMGR_ADN_ROOT | PROCMGR_ADN_NONROOT | PROCMGR_AOP_DENY | PROCMGR_AOP_LOCK | PROCMGR_AID_SCHEDULE,
        PROCMGR_ADN_ROOT | PROCMGR_ADN_NONROOT | PROCMGR_AOP_DENY | PROCMGR_AOP_LOCK | PROCMGR_AID_MEM_PHYS,
        PROCMGR_ADN_ROOT | PROCMGR_ADN_NONROOT | PROCMGR_AOP_DENY | PROCMGR_AOP_LOCK | PROCMGR_AID_IO,
        PROCMGR_ADN_ROOT | PROCMGR_ADN_NONROOT | PROCMGR_AOP_DENY | PROCMGR_AOP_LOCK | PROCMGR_AID_INTERRUPT,
        PROCMGR_ADN_ROOT | PROCMGR_ADN_NONROOT | PROCMGR_AOP_DENY | PROCMGR_AOP_LOCK | PROCMGR_AID_PATHSPACE,
        PROCMGR_ADN_ROOT | PROCMGR_ADN_NONROOT | PROCMGR_AOP_DENY | PROCMGR_AOP_LOCK | PROCMGR_AID_SETUID,
        PROCMGR_ADN_ROOT | PROCMGR_ADN_NONROOT | PROCMGR_AOP_DENY | PROCMGR_AOP_LOCK | PROCMGR_AID_SPAWN,
        PROCMGR_ADN_ROOT | PROCMGR_ADN_NONROOT | PROCMGR_AOP_DENY | PROCMGR_AOP_LOCK | PROCMGR_AID_FORK,
        PROCMGR_AID_EOL);
    fprintf(stderr, "fm-agent: OS abilities dropped and locked (%s)\n", rc == EOK ? "ok" : strerror(rc));
#endif
}

// ---------------------------------------------------------------- main

static std::string slurp(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

int main(int argc, char **argv) {
    for (int i = 1; i + 1 < argc; i += 2) {
        std::string k = argv[i], v = argv[i + 1];
        if (k == "--port") g_cfg.port = std::stoi(v);
        else if (k == "--host") g_cfg.host = v;
        else if (k == "--llm") g_cfg.llm = v;
        else if (k == "--telemetry") g_cfg.telemetry = v;
        else if (k == "--profiles") g_cfg.manual_dir = v;
        else if (k == "--web") g_cfg.web_dir = v;
        else if (k == "--max-tokens") g_cfg.max_tokens = std::stoi(v);
        else { fprintf(stderr, "unknown option %s\n", k.c_str()); return 2; }
    }

    httplib::Server srv;
    srv.Get("/", [](const httplib::Request &, httplib::Response &res) {
        res.set_content(slurp(g_cfg.web_dir + "/index.html"), "text/html; charset=utf-8");
    });
    srv.Get("/api/live", [](const httplib::Request &, httplib::Response &res) {
        json out = {{"status", telemetry_json("status")}, {"timing", telemetry_json("timing")},
                    {"events", telemetry_json("events")}, {"agent_busy", g_busy.load() == 1}};
        res.set_content(out.dump(), "application/json");
    });
    srv.Get(R"(/api/trend/(\d+))", [](const httplib::Request &req, httplib::Response &res) {
        int minutes = req.has_param("minutes") ? std::stoi(req.get_param_value("minutes")) : 5;
        res.set_content(read_telemetry("trend/" + req.matches[1].str() + "/" + std::to_string(minutes)), "application/json");
    });
    srv.Post("/api/chat", [](const httplib::Request &req, httplib::Response &res) {
        json body = json::parse(req.body, nullptr, false);
        if (body.is_discarded() || !body.contains("messages")) { res.status = 400; return; }
        res.set_content(chat(body["messages"]).dump(), "application/json");
    });
    srv.Post("/api/tool", [](const httplib::Request &req, httplib::Response &res) { // for tests: run one tool
        json body = json::parse(req.body, nullptr, false);
        json st = telemetry_json("status");
        res.set_content(run_tool(body.value("name", ""), body.value("args", json::object()), st.value("profile", "industrial")).dump(),
                        "application/json");
    });

    if (!srv.bind_to_port(g_cfg.host, g_cfg.port)) { fprintf(stderr, "fm-agent: cannot bind port %d\n", g_cfg.port); return 1; }
    drop_privileges();
    fprintf(stderr, "fm-agent: http://%s:%d  (model at %s)\n", g_cfg.host.c_str(), g_cfg.port, g_cfg.llm.c_str());
    srv.listen_after_bind();
    return 0;
}
