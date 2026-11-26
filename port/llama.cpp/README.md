# llama.cpp on QNX 8.0

This directory ports [llama.cpp](https://github.com/ggml-org/llama.cpp) (upstream tag `b12778`, commit `51ab45e`) to QNX SDP 8.0. With it you can run local large language models on QNX OS: `llama-server` (OpenAI-compatible HTTP API), `llama-cli`, `llama-completion` and `llama-bench`.

It is a porting layer, not a fork. You apply one small patch to an upstream checkout and build with the QNX toolchain files here.

## Build

```sh
source ~/qnx800/qnxsdp-env.sh
git clone --depth 1 --branch b12778 https://github.com/ggml-org/llama.cpp
./build.sh llama.cpp x86_64       # or: ./build.sh llama.cpp aarch64le   (Raspberry Pi 4/5)
```

The binaries are written to `llama.cpp/build-qnx-<arch>/bin/`. They are statically linked against ggml and llama, so you only need to copy each one to the target with a GGUF model:

```sh
./llama-server -m qwen3-1.7b-q4_k_m.gguf -c 4096 -t 2 --port 8081 --jinja
```

## What had to change

The patch touches five files, with 35 added lines and no deletions. Each change is guarded by `__QNX__` or `CMAKE_SYSTEM_NAME STREQUAL "QNX"`, so other platforms are unaffected.

| File | Problem on QNX | Fix |
|---|---|---|
| `ggml/src/ggml-cpu/ggml-cpu.cpp` | `sysconf(_SC_PHYS_PAGES)` does not exist on QNX, so the CPU backend could not report device memory. | Total RAM is the sum of the `ram` entries in the QNX system page (`SYSPAGE_ENTRY(asinfo)`). Free memory comes from the size QNX reports for `/proc`. |
| `common/download.cpp` | It includes `<sys/syslimits.h>` (BSD/macOS) for `PATH_MAX`. | On QNX, include `<limits.h>`. |
| `vendor/sheredom/subprocess.h` | QNX has `posix_spawn` but no `posix_spawn_file_actions_addchdir_np`. | Report "no working-directory support" on QNX, so the library falls back cleanly. |
| `vendor/cpp-httplib/CMakeLists.txt` | QNX keeps the BSD socket API (`socket`, `bind`, `inet_pton`…) in `libsocket`, not libc. | Link `socket` on QNX. |
| `common/CMakeLists.txt` | The same, for code in `llama-common` that uses sockets directly. | Link `socket` on QNX. |

The configuration choices in `build.sh` are deliberate:
- **OpenMP is off.** ggml then uses its own pthread pool, which works well on QNX.
- **CURL and OpenSSL are off.** Models are provided as local files, which suits an offline device.
- **Libraries are linked statically.** Deploying is a single file copy.

## Measured on QNX 8.0

These were measured on QNX 8.0 x86_64 in QEMU/KVM with 2 vCPUs on a GitHub-hosted runner, using `llama-bench` with 2 threads:

| Model | Size | Prompt processing | Generation |
|---|---|---|---|
| Qwen2.5-1.5B-Instruct Q4_K_M | 1.04 GiB | 39–43 tokens/s | 11.6–20.8 tokens/s |

Generation speed varies with the cloud host's CPU. Qwen3-1.7B Q4_K_M runs at a similar speed. Tool calling through `llama-server --jinja` (the OpenAI `tools` API) works on QNX; the Foreman agent in this repository uses it.

## Status and next steps

- Tested: x86_64 under QEMU/KVM, CPU backend, chat completions, tool calling, `llama-bench`.
- aarch64le (Raspberry Pi 4/5): toolchain file provided; build verification in CI is pending, and hardware reports are welcome.
- Planned: offer the patch upstream to llama.cpp, and the build recipe to [qnx-ports/build-files](https://github.com/qnx-ports/build-files).
