---
schema_version: 1
id: ai-ml-runtime
domain: 18-future-research
status: active
title: "TODO-05 -- AI/ML Native Inference Runtime"
---

# TODO-05 -- AI/ML Native Inference Runtime

> **Goal:** Research spike to determine how to run ONNX models and small LLMs
> (Phi-3-mini, LLaMA 7B Q4, Whisper) natively on Impossible OS without a Linux kernel
> -- assessing ggml/llama.cpp portability, designing user-mode AVX2 SIMD enablement,
> specifying GGUF model loading, defining the `SYS_AI_INFER` syscall, and producing
> a bench-backed plan for native on-device inference.

> [!IMPORTANT]
> **XSAVE activation** (`CR4.OSXSAVE`, `XSETBV`, XSAVE area layout) is owned by
> `02-kernel-core/TODO-28 §1` (XSAVE design) and `01-boot-platform/TODO-09 §6`
> (Phase 1 XSAVE & PCID activation window). §3 here adds the **per-thread user-mode
> XSAVE context** (YMM/ZMM save area in TEB, context-switch XSAVE/XRSTOR discipline)
> on top of those foundations -- do not re-specify kernel XSAVE initialization.
>
> **C++ support** (for `ggml` C++ bindings and `llama.cpp`) depends on
> `10-platform-services/TODO-09 §3` (GCC/G++ long-term). The ggml C99 core
> (`src/ggml.c`) does NOT require C++; §2 targets the C99 core only. C++ bindings
> are a post-compiler stretch.
>
> **Compiler/SDK** for building inference apps (`ai_assistant.exe`) depends on
> `10-platform-services/TODO-09` TCC/GCC SDK. This TODO designs the runtime and
> syscall surface; app building follows when the compiler is available.
>
> No production ML code is merged during the research phase. The §7 deliverable
> `ai-ml-runtime-plan.md` and a QEMU prototype (one token from a 2B-parameter model)
> are the outputs.

---

## Inputs

- `02-kernel-core/TODO-09-x86-64-architecture.md §1` (→ XREF) -- XSAVE design; `CR4.OSXSAVE`; `XSETBV(XCR0, AVX_MASK)` -- §4 per-thread XSAVE context builds on this
- `01-boot-platform/TODO-09-cpu-boot-sequencing.md §5` (→ XREF) -- Phase 1 XSAVE & PCID activation window; §4 adds per-thread XSAVE area after §5 finalizes OSXSAVE (XSAVE areas are PMM-backed, not TEB)
- `10-platform-services/TODO-09-compiler-sdk.md §3` (→ XREF) -- GCC/G++ C++ support; ggml C++ bindings depend on this (stretch, post-C99 core)
- `include/kernel/mm/pmm.h` -- `pmm_alloc_contiguous(count)` -- §4 model tensor allocation (128 MB–4 GB contiguous regions)
- `include/kernel/mm/vmm.h` -- `vmm_map_page()` -- §4 user-mode tensor memory mapping
- `include/kernel/sched/task.h` -- `task_t`, `TEB` -- §3 XSAVE area in TEB per thread
- `include/kernel/sched/syscall.h` -- next free syscall number -- §5 `SYS_AI_INFER` addition
- `include/kernel/fs/vfs.h` -- `vfs_open/read/close` -- §4 GGUF model loading from VFS
- `src/kernel/gfx/gfx_simd.c` -- reference for existing SSE2 usage pattern in GFX paths; §3 extends to AVX2 user-mode

---

## Outcome

A `docs/architecture/ai-ml-runtime-plan.md` with: option comparison, SIMD enablement
design (per-thread XSAVE layout), memory budget analysis (Q4 model sizes vs. RAM),
`SYS_AI_INFER` syscall spec, token throughput targets (tokens/s for Phi-3-mini).
A QEMU prototype boots a 2B-parameter GGUF model and generates one token, proving
the ggml port and XSAVE context switch work end-to-end.

---

## Implementation Order

| Step | Section                                  | 💎/⭐ | Dependency                               |
| ---- | ---------------------------------------- | ----- | ---------------------------------------- |
| 1    | Runtime option survey (ggml vs. ONNX vs. custom) | ⭐    | `D10T09 §3` C++ assessment               |
| 2    | SIMD enablement for user mode (XSAVE per-thread) | ⭐    | `D02T09 §1` + `TODO-04 §5` XSAVE foundations |
| 3    | ggml C99 kernel port                     | ⭐    | §2 SIMD; `pmm_alloc_contiguous`; kernel threads |
| 4    | GGUF model format & loading              | ⭐    | §3 ggml infrastructure; `vfs_read`; `vmm_map_page` |
| 5    | OS integration points (`SYS_AI_INFER` + apps) | ⭐    | §3 §4 runtime + loader                   |
| 6    | GPU inference research (stretch, post TODO-03) | ⭐    | `TODO-03` Vulkan driver; §5 ggml Vulkan backend |
| 7    | Research deliverables (`ai-ml-runtime-plan.md`) | ⭐    | §1–§6 complete                           |

---

## 1. Inference Runtime Options Survey `[Sonnet]`

> Compare three options; determine which is achievable within 6 months post-compiler.

- [ ] **Option A -- ggml/llama.cpp** (MIT, ~100 K LOC C/C++):
  - C99 core `src/ggml.c` (~40 K LOC) compiles with `-ffreestanding` after POSIX shims
  - Supports GGUF models (LLaMA, Phi, Mistral, Whisper) with Q4–Q8 quantization
  - CPU backends: pure C scalar + AVX2 SIMD kernels (4×4 tile matrix multiply using `_mm256_*`)
  - POSIX dependencies to replace: `malloc/free` → `kmalloc/kfree` + PMM; `pthread_*` → kernel threads; `mmap` → `vmm_alloc`; `FILE*` I/O → removed (model pre-loaded by §4); `clock_gettime` → `system_get_ticks()`
  - **Feasibility**: ✅ achievable in 6 months; C99 core ports without C++ compiler; AVX2 kernels work after §2 SIMD enablement

- [ ] **Option B -- ONNX Runtime** (MIT, ~500 K LOC C++):
  - Full ONNX graph execution; ONNX operator set ~170 ops; supports vision + NLP models
  - C++ STL dependency (vectors, maps, strings, exceptions, RTTI) -- requires `TODO-09 §3` GCC/G++ first
  - **Feasibility**: ❌ not achievable within 6 months; blocked by C++ compiler + STL port; defer to Phase 2

- [ ] **Option C -- Minimal custom runtime** (~5 K LOC):
  - Implement only: `matmul(A, B, C, m, n, k)` (dense GEMM), `relu(x)`, `gelu(x)`, `softmax(x)`, `rms_norm(x)`, `embed_lookup(idx)`, attention QKV projection
  - Enough for transformer inference (LLaMA/Phi architecture)
  - **Feasibility**: ✅ achievable in 2–3 months; significantly lower throughput than ggml (no AVX2 tiling); good for validating the end-to-end pipeline before full ggml port
  - **Recommended**: implement Option C first as a 2-week prototype to validate §2 and §4; then port ggml proper

- [ ] **Recommended strategy**: Option C prototype → ggml full port → ONNX Runtime (post-GCC); document rationale in §7 deliverable

- [ ] **Option comparison table**:

| Option              | LOC          | C++ required  | AVX2     | Models      | Timeline   |
| ------------------- | ------------ | ------------- | -------- | ----------- | ---------- |
| C -- Custom minimal | ~5 K         | No            | Manually | GGUF subset | 2–3 months |
| A -- ggml           | ~40 K C core | No (C99 core) | Yes      | GGUF full   | 4–6 months |
| B -- ONNX Runtime   | ~500 K       | Yes (STL)     | Via EP   | ONNX full   | 12+ months |

---

## 2. SIMD Enablement for User Mode `[Opus]`

> Hardware-interface primitives: `CR4.OSXSAVE`, `XSETBV(XCR0, ...)`, per-thread XSAVE
> area in TEB, XSAVE/XRSTOR on context switch. Security-critical: corrupting a thread's
> YMM state is a silent correctness bug. Extends `TODO-19 §1` + `TODO-04 §5` --
> kernel OSXSAVE activation is done there; this section adds the per-thread user layer.

- [ ] **XCR0 AVX enable mask** (in kernel after XSAVE activation from `TODO-19 §1` + `TODO-04 §5`):
  - `XSETBV(0, XCR0_X87 | XCR0_SSE | XCR0_AVX)` -- enables x87, XMM, and YMM state in XSAVE area for user mode
  - AVX-512 (ZMM, opmask): enable `XCR0_ZMM_HI256 | XCR0_HI16_ZMM | XCR0_OPMASK` if `cpuid.AVX512F` present
  - Kernel code continues to NOT use AVX (`-mno-sse -mno-sse2 -mno-avx` flags remain for kernel compilation); only user-mode threads may use AVX2/AVX-512

- [ ] **Per-thread XSAVE area** (extend `struct task_t` and `TEB`):
  - `XSAVE_AREA_SIZE = XSAVEOPT area size from CPUID.0xD.ECX` (typically 832 B for AVX2, 2696 B for AVX-512)
  - Allocate per-thread XSAVE area: `task->xsave_area = pmm_alloc_contiguous(ceil(XSAVE_AREA_SIZE / 4096))` (64-byte aligned, required by XSAVE instruction)
  - Store pointer in `TEB.XSaveArea` for user-mode introspection
  - For kernel threads and threads that never use AVX: `xsave_area = NULL` (use legacy `FXSAVE` 512B area instead)

- [ ] **Context switch XSAVE/XRSTOR** (extend `src/kernel/sched/task.c` context switch path):
  - On switch-out (saving outgoing thread state):
    ```c
    if (outgoing->xsave_area) {
        __asm__ volatile("xsaveopt %0" : "=m"(*outgoing->xsave_area) : "a"(0xFFFFFFFF), "d"(0xFFFFFFFF) : "memory");
    } else {
        __asm__ volatile("fxsave %0" : "=m"(*outgoing->fxsave_area));
    }
    ```
  - On switch-in (restoring incoming thread state):
    ```c
    if (incoming->xsave_area) {
        __asm__ volatile("xrstor %0" : : "m"(*incoming->xsave_area), "a"(0xFFFFFFFF), "d"(0xFFFFFFFF) : "memory");
    } else {
        __asm__ volatile("fxrstor %0" : : "m"(*incoming->fxsave_area));
    }
    ```
  - **XSAVEOPT**: only saves state components that have been modified since last XRSTOR (faster than full XSAVE; requires `cpuid.XSAVEOPT` feature bit from `cpu_data`)
  - On thread creation with `is_ai_thread = 1`: call `XRSTOR` with an initial clean XSAVE image to zero all YMM registers before first context restore

- [ ] **AVX2 availability for user mode** (no kernel change needed in inference code):
  - After `XSETBV` enables YMM, user-mode code compiled with `-mavx2` can use `_mm256_*` intrinsics freely
  - Kernel ensures: VZEROUPPER is NOT called before returning to user mode (would destroy YMM state); this is enforced by the fact that kernel code never executes AVX instructions

- [ ] **Benchmark** (`scripts/benchmark-avx2.sh`): compile a 512×512 matrix multiply with and without `-mavx2`; run in QEMU with `qemu-system-x86_64 -cpu host` (exposes host AVX2); measure cycles via `RDTSC`; target: > 5× speedup with AVX2 vs. scalar for FP32 matmul

---

## 3. ggml C99 Kernel Port `[Opus]`

> Novel: porting a 40 K LOC ML compute library to a freestanding OS environment.
> Replacing POSIX threading, POSIX I/O, and libc allocation with kernel primitives
> requires systematic dependency mapping. Subtle concurrency design: ggml uses
> per-thread workloads dispatched via a thread pool.

**Source:** `src/libs/ggml/ggml.c` (vendored, modified); header `include/libs/ggml.h`

- [ ] **Dependency map** (what to replace):

| ggml POSIX dependency                    | 🚀 Impossible OS replacement             | Notes                      |
| ---------------------------------------- | ---------------------------------------- | -------------------------- |
| `malloc(n)`                              | `kmalloc(n)` for < 4 KB; `pmm_alloc_contiguous(pages)` for tensors | Tensor allocation via PMM  |
| `free(p)`                                | `kfree(p)` / `pmm_free_contiguous(p, pages)` | Match allocator            |
| `realloc(p, n)`                          | alloc new + `memcpy` + free old          | No kernel realloc          |
| `pthread_create`                         | `task_create("ggml_worker", fn, arg)`    | Kernel thread pool         |
| `pthread_mutex_lock/unlock`              | `spinlock_acquire/release` or `mutex_lock/unlock` | Existing kernel sync       |
| `pthread_cond_wait/signal`               | `sched_sleep_wait` / `sched_wake`        | Existing kernel primitives |
| `mmap(NULL, size, ...)`                  | `vmm_alloc_range(size)` + PMM backing    | Large tensor buffers       |
| `FILE*` model I/O                        | **removed** -- model loaded by §4 before ggml init | Caller pre-loads data      |
| `clock_gettime(CLOCK_MONOTONIC)`         | `system_get_ticks()`                     | Timer shim                 |
| `assert(x)`                              | `KERNEL_ASSERT(x, "ggml")`               | Debug mode only            |
| `CPU_FEATURE_AVX2` via `__builtin_cpu_supports` | `cpu_data[0].cpuid_features & CPU_FEATURE_AVX2` | From `cpu_data`            |
| `_mm256_*` intrinsics (AVX2)             | Unchanged -- user-mode inference threads have AVX2 after §2 | ✅ no change               |

- [ ] **Thread pool implementation** (`src/libs/ggml/ggml_threadpool.c`):
  - `ggml_n_threads` → configurable via `HKLM\SYSTEM\AI\InferenceThreads` Registry key (default: `cpu_count / 2`)
  - Worker threads created once at `ggml_init()` via `task_create`; blocked on a `semaphore_wait` work queue
  - Main thread posts work items; workers wake, process their slice of the compute graph node, signal completion semaphore
  - `ggml_set_n_threads(n)`: dynamically adjusts active worker count

- [ ] **AVX2 kernel selection** (keep verbatim from ggml upstream):
  - `ggml_vec_dot_f32_avx2()`, `ggml_vec_mad_f32_avx2()` -- 256-bit YMM dot products; used for Q4×F32 quantized matrix multiply
  - `ggml_vec_dot_q4_0_q8_0_avx2()` -- Q4_0 × Q8_0 mixed-precision; the hot path for 4-bit quantized inference
  - These compile with `-mavx2` flag on the ggml source files only (not all kernel files); `Makefile` adds `GGML_CFLAGS = -mavx2 -mfma` for `src/libs/ggml/*.c`

- [ ] **ggml context** (`ggml_context`): allocate context buffer from `pmm_alloc_contiguous(ceil(ctx_size / 4096))`; context is a simple bump allocator over the PMM buffer -- no free within a context; `ggml_free_context()` returns entire PMM region at once

- [ ] **Porting scope**: target `ggml.c` + `ggml-cpu.c` only; exclude `ggml-cuda.c`, `ggml-metal.c`, `ggml-vulkan.c` (GPU backends -- §6 research), `ggml-opencl.c`; build with `GGML_BACKEND_CPU=1 GGML_NO_ACCELERATE=1 GGML_NO_OPENMP=1`

---

## 4. GGUF Model Format & Loading `[Sonnet]`

**Source:** `src/libs/ggml/gguf.c` (vendored, VFS-adapted); GGUF format spec v3

- [ ] **GGUF binary format** (`gguf_load(const char *path, gguf_ctx_t *out)`):
  - Header: `magic[4] = "GGUF"`, `version (uint32)` (must be 3), `tensor_count (uint64)`, `kv_count (uint64)`
  - Key-value metadata section: array of `{key_string, value_type, value}` entries; read `model.architecture`, `llm.context_length`, `tokenizer.ggml.model`, `general.quantization_version`
  - Tensor metadata: `{name_string, n_dims (uint32), dims[4] (uint64), type (uint32), offset (uint64)}` per tensor; `offset` is byte offset from start of tensor data section (after metadata)
  - Tensor data section: starts at alignment boundary (default 32 B); raw tensor data
- [ ] **VFS loading** (replaces `FILE*` in upstream ggml):
  ```c
  int gguf_load(const char *path, gguf_ctx_t *out) {
      vfs_file_t *f = vfs_open(path, VFS_FLAG_READ);
      size_t file_size = vfs_file_size(f);
      // Allocate tensor data buffer from PMM
      uintptr_t phys = pmm_alloc_contiguous(ceil(file_size / 4096));
      void *buf = vmm_map_kernel(phys, ceil(file_size / 4096));
      vfs_read(f, buf, file_size);
      vfs_close(f);
      // Parse header + KV + tensor metadata from buf
      // Set out->tensors[] pointing into buf at correct offsets
      out->data_buf = buf; out->data_phys = phys; out->data_size = file_size;
      return 0;
  }
  ```
- [ ] **Quantization type support** (implement dispatch table):

| GGUF type | Bits/weight | 7B model RAM | Notes                           |
| --------- | ----------- | ------------ | ------------------------------- |
| F32       | 32          | ~28 GB       | Unrealistic for most hardware   |
| F16       | 16          | ~14 GB       | High quality; 16+ GB RAM needed |
| Q8_0      | 8           | ~7 GB        | Good quality; 8 GB RAM minimum  |
| Q4_K_M    | ~4.5        | ~4 GB        | Recommended; fits in 4–8 GB     |
| Q4_0      | 4           | ~3.5 GB      | Fast; slightly lower quality    |

- [ ] **PMM large contiguous allocation**: `pmm_alloc_contiguous()` currently supports up to `(free_frames)` pages; a 7B Q4 model needs ~900K pages (3.5 GB); verify `pmm_alloc_contiguous` can serve this on a 4+ GB RAM system; if fragmentation prevents it: add PMM huge-page reservation at boot (reserve top 2–4 GB of RAM as a contiguous ML pool before kernel heap fragments it)
- [ ] **User-mode tensor mapping**: after model load, tensor buffers mapped into inference process virtual address space via `SYS_GPU_MAP` (§5) or a new `SYS_AI_MODEL_MAP` call; user process reads tensors for inference without kernel involvement

- [ ] **Model cache**: models stored at `C:\Users\{name}\AppData\AI\Models\*.gguf`; `gguf_load` path resolves to this directory; cache unloading: `gguf_unload()` → `pmm_free_contiguous()`

---

## 5. OS Integration Points (`SYS_AI_INFER` + Apps) `[Sonnet]`

- [ ] **`SYS_AI_INFER = 95` syscall** (add to `include/kernel/sched/syscall.h`):
  ```c
  // SYS_AI_INFER: initiate inference on background thread
  // Args: model_handle (uintptr), input_tokens (uint32*), n_tokens (uint32),
  //       output_buf (uint32*), output_max (uint32), completion_semaphore (sem_t*)
  // Returns: inference_id (uint32) or -EINVAL
  int sys_ai_infer(uintptr_t model_handle, const uint32_t *tokens, uint32_t n,
                   uint32_t *out_tokens, uint32_t max_out, sem_t *done);
  ```
  - Kernel allocates an `ai_infer_request_t`; submits to ggml thread pool; signals `done` semaphore when complete; caller polls or `sched_sleep_wait(done)`
  - `SYS_AI_MODEL_LOAD = 96`: `vfs_read` + `gguf_load`; returns `model_handle`; counts reference; shared across processes
  - `SYS_AI_MODEL_UNLOAD = 97`: decrement ref count; `pmm_free_contiguous` when zero

- [ ] **`ai_assistant.exe`** (user-mode app, post-compiler):
  - Voice input: `portaudio` → PCM buffer → Whisper.cpp `whisper_full()` → transcript text
  - LLM inference: transcript → `SYS_AI_INFER` → response text
  - UI: `CTRL_TEXTBOX` transcript + response; Win+A hotkey to toggle visibility
  - Model: `Phi-3-mini-4k-instruct.Q4_K_M.gguf` (2.2 GB) -- fits in 4 GB RAM

- [ ] **Start Menu semantic search** (embed in `src/desktop/start_menu.c`):
  - Embedding model: `all-MiniLM-L6-v2.Q4_0.gguf` (~25 MB) -- always resident in RAM after first use
  - On keypress in Start Menu search box: if query > 3 words → `SYS_AI_INFER` with embedding model → 384-dim vector → cosine similarity vs. pre-computed app embedding index → boost semantically similar results above exact string matches
  - Pre-compute app embeddings at install time; store in `HKLM\SOFTWARE\Installed\{name}\Embedding` as 384×4 bytes (F32)

- [ ] **Code completion hint in Notepad** (stretch, post `Phi-2` model availability):
  - Detect `.c`, `.py`, `.js` file open in Notepad; on 500 ms idle after typing: `SYS_AI_INFER` with current line context → suggest 1-line completion → display as greyed text; Tab to accept

- [ ] **Token throughput targets** (for §7 benchmark plan):

| Model            | Params | Quantization | RAM     | Target tok/s (8-core QEMU host-passthrough) |
| ---------------- | ------ | ------------ | ------- | ---------------------------------------- |
| TinyLlama-1.1B   | 1.1B   | Q4_0         | ~700 MB | > 30 tok/s                               |
| Phi-3-mini-4K    | 3.8B   | Q4_K_M       | ~2.3 GB | > 10 tok/s                               |
| LLaMA 7B         | 7B     | Q4_K_M       | ~4 GB   | > 5 tok/s                                |
| all-MiniLM-L6-v2 | 22M    | Q4_0         | 25 MB   | > 500 embed/s                            |

---

## 6. GPU Inference Research (Stretch) `[Sonnet]`

> Depends entirely on `13-future-research/TODO-03` Vulkan driver being available.
> Research-only section; no prototype code.

- [ ] **ggml Vulkan backend** (`ggml-vulkan.c` in upstream): uses Vulkan compute shaders for SGEMM; once VirtIO-GPU Vulkan is available (TODO-03 §6), enable by building with `GGML_VULKAN=1`
- [ ] **Potential speedup** (estimated from Linux benchmarks on similar hardware):
  - VirtIO-GPU (host RTX 3080): 7B Q4 model → ~100–200 tok/s (10–40× vs. CPU AVX2)
  - Bare-metal RDNA 3 (future): similar or better; limited by VRAM (24 GB → fits 70B Q4 model)
- [ ] **Memory architecture**: GPU inference requires tensors in GPU-visible memory; `SYS_GPU_MAP` (from `TODO-03 §6`) maps tensor PMM buffers to GPU address space; no copy needed if VirtIO-GPU uses shared memory model
- [ ] **Whisper.cpp GPU acceleration**: Whisper large-v3 runs in real-time on GPU; on CPU it needs ~5× audio duration; GPU backend unlocks real-time transcription in `ai_assistant.exe`
- [ ] **Blocking dependencies for GPU inference**:
  1. VirtIO-GPU Vulkan ICD (TODO-03 §6) -- minimum requirement
  2. `SYS_GPU_MAP` syscall (TODO-03 §6)
  3. ggml Vulkan backend compile with Impossible OS headers (needs C++ or rewritten in C)
  4. Large model VRAM budget: 7B Q4 needs ~4 GB VRAM; VirtIO-GPU shares host GPU VRAM

---

## 7. Research Deliverables `[Sonnet]`

**Source:** `docs/architecture/ai-ml-runtime-plan.md`

- [ ] **`docs/architecture/ai-ml-runtime-plan.md`** -- sections:
  - **Option comparison table** (from §1): ggml vs. ONNX Runtime vs. custom minimal; LOC, C++ need, timeline, feasibility verdict
  - **SIMD enablement design** (from §2): XSAVE area layout diagram (x87 0–511, XMM 512–1023, YMM_Hi 1024–1535, ZMM_Hi 1536–2695 bytes); `CR4.OSXSAVE` → `XSETBV` activation sequence; context switch XSAVEOPT/XRSTOR pseudocode; per-thread `TEB.XSaveArea` pointer
  - **ggml dependency map** (from §3): complete table of POSIX → kernel replacements; which ggml files to exclude (CUDA/Metal/Vulkan backends); `GGML_CFLAGS = -mavx2 -mfma` Makefile pattern
  - **Memory budget analysis** (from §4): table of model sizes vs. Q-level vs. RAM needed; PMM large-allocation strategy (top-of-RAM ML pool reservation at boot)
  - **`SYS_AI_INFER` syscall spec** (from §5): argument layout, return value, background thread model, semaphore signaling
  - **Token throughput benchmark plan**: QEMU commands, benchmark script, expected tok/s per model at each quantization level
  - **GPU inference roadmap** (from §6): VirtIO-GPU dependencies, estimated speedup, blocking items
  - **Blocking dependencies summary**:
    1. `D02T09 §1` + `TODO-04 §5` XSAVE activation before §4 can be implemented
    2. `TODO-09 §3` GCC/G++ before C++ ggml bindings
    3. `TODO-03 §6` Vulkan driver before GPU inference
    4. PMM large-allocation extension (ML pool reservation) before models > 4 GB
- [ ] **QEMU prototype** (in `ai/ggml-spike` branch):
  - Custom minimal Option C runtime (§1): implement 4-function kernel (`matmul`, `rms_norm`, `softmax`, `embed_lookup`) in ~500 LOC
  - Load `TinyLlama-1.1B.Q4_0.gguf` via `gguf_load()`
  - Run one forward pass (1 input token → 1 output token)
  - **Success criterion**: QEMU serial shows `"[AI] Generated token: {id} ({text})"` and process exits cleanly (no panic)
- [ ] **Tracking GitHub Issue**: "AI/ML Native Inference Runtime" → links to `ai-ml-runtime-plan.md`; labels: `P3`, `enhancement`, `future-research`

---

## OS Comparison


| ⭐  | Feature                                  | 🪟 Win11                                 | 🐧 Linux                                 | 🚀 Impossible OS                         |
| --- | ---------------------------------------- | ---------------------------------------- | ---------------------------------------- | ---------------------------------------- |
| ⭐  | Native on-device LLM inference           | ✅ Copilot+ (NPU/ONNX Runtime); DirectML; llama.cpp | ✅ llama.cpp natively; ONNX Runtime; CPU/GPU | ⬜ §3 -- ggml C99 port; AVX2 user-mode   |
| ⭐  | AVX2 user-mode SIMD with per-thread XSAVE context | ✅ Windows handles XSAVE/XRSTOR automatically (FXSAVE | ✅ Linux XSAVE per-task (`task_struct.thread.fpu`) | ⬜ §2 -- `TEB.XSaveArea`; XSAVEOPT on context switch |
| 💎  | GGUF model loading                       | ✅ llama.cpp + GGUF on Windows           | ✅ llama.cpp native GGUF on Linux        | ⬜ §4 -- `gguf_load()` via VFS; PMM large-alloc |
| ⭐  | `SYS_AI_INFER` background inference syscall | ✅ WinRT `IntelligenceInterface`; ML.NET; no single | ✅ No dedicated kernel AI syscall;       | ⬜ §5 -- `SYS_AI_INFER=95`/`SYS_AI_MODEL_LOAD=96`; semaphore completion |
| ⭐  | Start Menu semantic search               | ✅ Windows Search AI (Copilot+, NPU,     | ✅ GNOME/KDE: no on-device semantic search | ⬜ §5 -- `all-MiniLM-L6-v2` 25 MB resident; cosine |
| 💎  | GPU inference via Vulkan compute         | ✅ DirectML Vulkan; CUDA; ROCm           | ✅ Vulkan compute (`kompute`, `ggml-vulkan`) | ⬜ §6 -- (stretch); `ggml-vulkan.c` backend; blocked by |

Impossible OS's `⭐` advantage: `SYS_AI_INFER` makes inference a first-class kernel
primitive -- the OS schedules inference workloads alongside threads, applies power
management policies to AI background tasks, and allows the scheduler to pre-empt
inference workers in favor of user-interactive tasks. No other OS exposes inference
at the syscall level; Windows Copilot+ and Linux both delegate entirely to user-mode
libraries. The Start Menu semantic search (`all-MiniLM-L6-v2`, 25 MB, always resident)
also sets a standard -- a local-first AI search feature that works offline without
cloud calls or NPU hardware.

---

## Verification

- [ ] **AVX2 context switch**: create two user threads both executing `vaddps ymm0, ymm1, ymm2` in a loop with distinct constant patterns; run 1000 context switches; read `ymm0` value after switch; verify each thread sees its own correct value (no state corruption)
- [ ] **XSAVE area allocated**: `task_create()` with `AI_THREAD` flag → `task->xsave_area != NULL`; `sizeof(xsave_area) >= XSAVE_AREA_SIZE` from `CPUID.0xD.ECX`
- [ ] **ggml matmul**: `ggml_mul_mat()` on 512×512 F32 tensors → result matches reference (tolerance 1e-5); AVX2 path faster than scalar by > 3× (measured via `RDTSC`)
- [ ] **GGUF load**: `gguf_load("TinyLlama-1.1B.Q4_0.gguf")` → metadata fields populated (`model.architecture`, `llm.context_length`); tensor count matches file header; all tensor offsets within bounds
- [ ] **One token forward pass**: `sys_ai_infer(handle, [1], 1, out, 10, &done)` → semaphore signaled; `out[0]` is a valid token ID (> 0, < vocab_size); no panic, no memory leak (PMM free pages unchanged after `SYS_AI_MODEL_UNLOAD`)
- [ ] **Token throughput**: TinyLlama-1.1B Q4_0 → `bench_ai 10 tokens` → ≥ 10 tok/s on QEMU `-cpu host` with 4 threads; logged to `build/benchmark-{version}.json` AI section
- [ ] **Start Menu embedding**: type `"write some code"` in Start Menu → semantic search returns code editor / Notepad before exact-match results (validates embedding cosine similarity path)
- [ ] **GPU inference** (stretch, after TODO-03): `GGML_VULKAN=1` build → ggml routes matmul to Vulkan compute shader via VirtIO-GPU; throughput > 3× CPU path
- [ ] Commit: `"ai: ggml C99 port, AVX2 XSAVE context switch, GGUF loader, SYS_AI_INFER syscall, Start Menu semantic search, ai-ml-runtime-plan.md"`
