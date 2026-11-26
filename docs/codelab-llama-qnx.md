# Codelab: run a local LLM on QNX 8.0 with llama.cpp

**Duration:** about 60 minutes, most of it downloads and compiling.
**You will build:** a QNX 8.0 virtual machine that runs a 1.7-billion-parameter language model offline, behind an OpenAI-compatible HTTP API, at a scheduling priority that can never delay your real-time work.
**You need:** a Linux x86_64 host with KVM (Ubuntu 22.04 or 24.04 works), about 10 GB of free disk and 4 GB of free RAM. You also need a free QNX non-commercial licence.

---

## 1. Get QNX SDP 8.0 (10 min)

1. Create a myQNX account, then request the free licence at https://www.qnx.com/getqnx.
2. In the myQNX **License Manager**, **Accept** the licence, then **Deploy** it to your own account. Without the deploy step, the QNX Software Center cannot see the licence.
3. Install the QNX Software Center and, with it, *QNX Software Development Platform 8.0* (baseline `com.qnx.qnx800`). Also install the QEMU target package `com.qnx.qnx800.target.qemuvirt`.
4. Load the environment in every shell you use:

```sh
source ~/qnx800/qnxsdp-env.sh
qcc -V        # lists gcc_ntox86_64 and gcc_ntoaarch64le targets
```

## 2. Build a QNX virtual machine with room for a model (5 min)

The default image has a 60 MB data partition. A Q4 model is about 1.1 GB, so make the data partition bigger:

```sh
sudo apt install qemu-system-x86 ninja-build cmake sshpass
mkdir ~/qnxvm && cd ~/qnxvm
mkqnximage --type=qemu --arch=x86_64 --data-size=3000 --data-inodes=20000
```

Boot it headless with user-mode networking, so no bridge is needed. Forward SSH to port 2222 and the model API to port 8081:

```sh
qemu-system-x86_64 -smp 2 --enable-kvm --cpu host -m 3G \
  -kernel output/ifs.bin -drive file=output/disk-qemu.vmdk,if=ide \
  -netdev user,id=n0,hostfwd=tcp::2222-:22,hostfwd=tcp::8081-:8081 -device virtio-net-pci,netdev=n0 \
  -display none -serial file:console.log -daemonize
ssh -p 2222 qnxuser@localhost uname -a       # password: qnxuser
```

> **Tip:** keep `-m` at 3G or less with the default image. In our tests, building the image with `--ram=4G` made the kernel stop during early boot.

## 3. Cross-compile llama.cpp for QNX (15 min)

llama.cpp needs five small changes for QNX. They are in [`port/llama.cpp`](../port/llama.cpp), with an explanation of each:

```sh
git clone https://github.com/sihlangu-mthembu/foreman
git clone --depth 1 --branch b12778 https://github.com/ggml-org/llama.cpp
foreman/port/llama.cpp/build.sh llama.cpp x86_64
ls llama.cpp/build-qnx-x86_64/bin        # llama-server  llama-cli  llama-completion  llama-bench
```

What the patch fixes, in one line each:
- **Physical RAM:** QNX has no `_SC_PHYS_PAGES`, so the patch reads RAM from the system page instead.
- **`PATH_MAX`:** on QNX it is in `<limits.h>`.
- **Subprocesses:** `posix_spawn` has no chdir action on QNX, so that feature is reported as unsupported.
- **Sockets:** they live in `libsocket`, so the patch links it.

## 4. Copy the server and a model to QNX (5 min)

```sh
curl -L -o model.gguf https://huggingface.co/unsloth/Qwen3-1.7B-GGUF/resolve/main/Qwen3-1.7B-Q4_K_M.gguf
scp -P 2222 llama.cpp/build-qnx-x86_64/bin/llama-server llama.cpp/build-qnx-x86_64/bin/llama-bench model.gguf \
    qnxuser@localhost:/data/home/qnxuser/
```

The binaries are statically linked against ggml and llama, so nothing else needs installing on the target.

## 5. Benchmark it (3 min)

```sh
ssh -p 2222 qnxuser@localhost
cd /data/home/qnxuser
./llama-bench -m model.gguf -t 2 -p 64 -n 32
```

On 2 KVM vCPUs we measured about 40 tokens/s for prompt processing and 12–20 tokens/s for generation.

> **Tip:** always pass a context size (`-c 2048` or `-c 4096`). Without one, llama.cpp sizes the context from the model's maximum (32k tokens) and runs out of memory on a small target.

## 6. Serve it at a priority that can't hurt real-time work (5 min)

QNX priorities run from 1 to 255. Priorities 1 to 63 are open to every process. Anything above 63 needs the `PROCMGR_AID_PRIORITY` ability. So:
- run your real-time threads at 64 or higher;
- run the model as an ordinary user at a low priority.

Then the model can never preempt your real-time threads, however busy it gets.

```sh
on -p 10 ./llama-server -m model.gguf -c 4096 -t 2 --host 0.0.0.0 --port 8081 --jinja &
pidin -f anpN | grep llama        # every thread at 10r
on -p 71 sleep 1                  # as qnxuser: "on: Operation not permitted"
```

QNX 8.0 has no adaptive partitioning, so to cap the model's CPU *budget* rather than just its priority, use sporadic scheduling:

```sh
# priority 10 for 50 ms of every 100 ms, then priority 5 until the budget is replenished
on -p 10s:5:4:100:50 ./llama-server -m model.gguf -c 4096 -t 2 --port 8081 --jinja &
```

## 7. Call it like any OpenAI-compatible API, tools included (5 min)

From the host:

```sh
curl -s localhost:8081/v1/chat/completions -H 'Content-Type: application/json' -d '{
  "messages": [{"role": "user", "content": "What is the motor temperature?"}],
  "tools": [{"type": "function", "function": {"name": "get_status",
             "description": "Live machine values", "parameters": {"type": "object", "properties": {}}}}],
  "chat_template_kwargs": {"enable_thinking": false}
}' | python3 -m json.tool
```

The reply contains a `tool_calls` entry for `get_status`. Your application runs the tool, appends the result as a `tool` message and calls the API again. The [Foreman agent](../src/agent/agent.cpp) does this in C++, with read-only tools only.

## 8. Where to go next

- [Foreman](../README.md) shows the whole pattern:
  - a 1 kHz control loop at priority 70
  - a read-only resource manager for machine data
  - an AI agent that gives up its OS abilities
  - measured control-loop timing under AI load
- To run on a Raspberry Pi 4/5, use `foreman/port/llama.cpp/build.sh llama.cpp aarch64le`.
