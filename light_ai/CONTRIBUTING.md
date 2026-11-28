# Contributing to `light_ai`

This guide documents the project's coding rules, error model, comment style, and verification gates. Keep changes strictly scoped to `light_ai/` and target `master` via pull requests (during an audit, its integration branch: see `CLAUDE.md`).

## Coding Rules

Each rule is marked **(gate)**, **(review)** or both. **(gate)** means `scripts/tidy_run.sh` enforces it: the `custom-*` checks and `misc-const-correctness` of the `rules` pass ([`scripts/tidy_rules.yaml`](scripts/tidy_rules.yaml)) or [`scripts/rules_lint.sh`](scripts/rules_lint.sh), with self-tests. Reviewers and agents do not re-check (gate) rules by hand. **(review)** means no tool can decide it, so `commit-reviewer` judges it. The allowed exceptions of the gate are the lines of [`scripts/rules_allow.txt`](scripts/rules_allow.txt): every `NOLINT(custom-...)` and `NOLINT(misc-const-correctness)` site, each with its reason in the comment (`NOLINT` or `NOLINTNEXTLINE` only: a `NOLINTBEGIN` / `NOLINTEND` range for either is never allowed).

- **No GStreamer, no OpenCV** (gate: `banned-include`).
- **Zero-copy frame path** (review)**:** Frames exist only as `dma-buf` descriptors; CPU pixel copies are forbidden. The only permitted copies are compressed MJPEG input packets (\~25 KB) into the decoder packet buffer and compressed H.264 packets out of the encoder.
- **I/O & logging** (gate: `custom-no-printf`, `custom-no-std-stream`, not in the test harness; the `Log` injection itself: review)**:** Use `iostream` only: no C stdio output (the printf family, `puts`, `fputc`, `fwrite`, `perror`, the wide and `_unlocked` forms, `err`/`warn`/`error(3)`), and no `std::cout`/`cerr`/`clog` (or their `w` forms) outside `src/main.cpp`. All diagnostics go through `Log` ([`include/core/log.hpp`](include/core/log.hpp)) injected as `const Log&`. Only [`src/main.cpp`](src/main.cpp) owns `Log(std::cout, std::cerr)` and `print_usage(std::cout)`.
- **Include order** (gate: `include-order`)**:** System and SDK headers first (`<...>`), project headers last (`"..."`). Reference: [`src/render/gpu_box_drawer.cpp`](src/render/gpu_box_drawer.cpp).
- **Header layout** (review)**:** Within each visibility section (`public:` / `private:`), member functions and data members live in separate blocks — functions first, then a fresh visibility label for data. Reference: [`include/source/camera_v4l2.hpp`](include/source/camera_v4l2.hpp).
- **Header extensions** (gate: `header-ext`)**:** C++ headers use `.hpp`. The last nine legacy `.h` headers were renamed in Stage E (#112); a new `.h` fails the gate.
- **No manual heap allocation** (gate: `custom-no-new-delete`, which covers `new`/`delete` including placement new, `::operator new`/`delete` and the `malloc` family; `custom-no-smart-ptr`, which covers `unique_ptr`/`shared_ptr`/`weak_ptr` in any declared or written type (template arguments, `sizeof`, aliases, dependent types of templates) and the `make_unique`/`make_shared`/`allocate_shared` calls; no heap growth on recovery and frame paths: review)**:** No `new`/`delete`, no `std::unique_ptr`/`std::shared_ptr`, no heap allocations in recovery or frame paths. Data lives in fixed-size members (`std::vector`/`std::string` are permitted only as startup RAII containers). Reference: [`include/core/pod_list.hpp`](include/core/pod_list.hpp).
- **No statics** (gate: `custom-no-static-local`, `custom-no-static-member`, `custom-no-file-static`, `custom-no-global-var`, `custom-no-thread-local`)**:** No static data members, no static member functions, no function-local statics. Shared state belongs to the owning layer as a regular member; file-local helpers belong in an anonymous namespace inside the `.cpp`. Compile-time class constants use an unscoped `enum`; block- and namespace-scope constants use plain `constexpr` without `static`.
  - *Exceptions* (review)*:* disposable probes in `tools/` and vendored header-only libraries in `externals/`, which must only be included from `.cpp` files, never from a project header.
- **No virtuals or inheritance** (gate: `custom-no-virtual`, `custom-no-inheritance`)**:** Zero vtables and zero dynamic polymorphism. Layer boundaries use composition of concrete members; static polymorphism uses templates (e.g. [`PodList<T, N>`](include/core/pod_list.hpp), [`PodRing<T, N>`](include/core/pod_ring.hpp), [`InferencePool`](include/npu/inference_pool.hpp)). `virtual` functions, `virtual` inheritance and every base class are forbidden, with one exception: a `private`, non-virtual `NonCopyable<T>` or `NonMovable<T>` from [`include/core/non_copyable.hpp`](include/core/non_copyable.hpp), both empty; do not specialize them or define another class of those names (the gate flags both). A class that can be neither copied nor moved takes `private NonMovable<T>`; a non-copyable class that declares its own move constructor takes `private NonCopyable<T>`. Write `private` (a `struct` would default to `public`), and do not derive from a class that uses one of them. With a base a class is no longer an aggregate: give it a constructor if it was brace-initialized.
- **No `const_cast`** (gate: `custom-no-const-cast`)**:** Fix the constness at the source. The one SDK-signature site (`rknn_init` in [`src/npu/rknn_engine.cpp`](src/npu/rknn_engine.cpp)) is allowlisted with its reason. A `#else` branch for another arch is invisible to the gate on this board, so it is (review).
- **Control flow** (gate: `custom-no-else-if`, `custom-no-empty-loop`, `custom-no-infinite-loop`; PR #95)**:** No `if` / `else if` ladders, `if constexpr` included and also the `else { if ... }` wrapper: use a `switch`, a table, early returns / `continue`, or independent `if`s when the conditions exclude each other (an overload set for compile-time type dispatch). No loop with an empty body (`;`, `{}`, `{ ; }`, one nested `{ {} }`; deeper nesting is (review)): the step goes into the body (`do { rc = ioctl( ... ); } while ( rc < 0 && errno == EINTR );`). No loop without a real exit condition (`for (;;)`, `while (true)`, `while (1)`): loop on the condition that ends it, through a helper whose result is that condition when the wait is long (`while ( next_job( job ) )`). The matcher sees literal conditions only; a named always-true constant is (review). `hicpp-multiway-paths-covered` (main pass) also flags a degenerate `switch` (one `case`, or only `default`), a `switch` whose cases do not cover a non-enum condition and has no `default`, and, with `WarnOnMissingElse`, an `if` directly under another `if` without a final `else` (an `else if` rung, or an unbraced nested `if`).
- **`const` by default** (gate: `misc-const-correctness`, #94, for what the check sees; the rest: review)**:** A local that is never changed is `const`: values, references, arrays, `auto` locals, lambda objects, range-`for` variables, also the locals of a non-dependent type in a template. So is the pointee of a local pointer that is only read through (`const T* p`), and a reference or pointer parameter of a free function that is only read (`const T&`, `const T*`; with the header declaration). The check does not see, so the review does: a local of a dependent type in a template, the names of a structured binding (`auto [x, y] = p;`: only `p` is checked), the parameters of member functions, constructors and lambdas, value parameters, `#else` branches for another arch, `const` methods, and `const&` instead of a by-value copy. Not asked for: a top-level `T* const` (`WarnPointersAsValues` off: on a parameter it is not part of the signature and protects no data). A returned local of class type stays non-`const` (the check skips it: `const` would turn the implicit move into a copy); a returned scalar or enum is flagged and takes `const`. The `NOLINT(misc-const-correctness)` sites are types fixed by an API, not by us: `void* user` of `cfg_file_handler` (inih `ini_handler`) in [`src/config/config_file.cpp`](src/config/config_file.cpp), `void* opaque` of `write_packet` (`avio_alloc_context`) in [`src/net/tcp_avio.cpp`](src/net/tcp_avio.cpp), and `char** argv` of `main` in [`tests/test_main.cpp`](tests/test_main.cpp) and [`tests/tsan_main.cpp`](tests/tsan_main.cpp). Non-mutating methods are `const`; read-only parameters are passed by `const&`; hot-path outputs go through out-parameters (e.g. `YoloDecoder::decode` fills `DetList& out`).
- **Branch tables over `if`-chains** (review)**:** Dispatch over fixed sets via `switch` or `constexpr` tables (reference: `BoxMapper::det_color` in [`src/render/box_mapper.cpp`](src/render/box_mapper.cpp)).
- **Preallocated buffer contract** (review)**:** Cross-frame output parameters are never assumed empty — the callee clears them at entry (e.g. `out.clear()` in `YoloDecoder::decode`).
- **SDK-free `core/`** (gate: `core-include`)**:** [`include/core/`](include/core/) and [`src/core/`](src/core/) may include only the C++17 standard library and `core/*`. This keeps host unit tests free of MPP/RGA/RKNN/EGL/FFmpeg dependencies.
- **SDK headers through the vendor paths** (gate: `sdk-include`, #117)**:** RGA as `<im2d.h>` / `<rga.h>` (the vendor librga 1.10.6 headers in `/usr/local/include`), never `<rga/...>` (the distro 2.2.0 headers, against the 1.10.6 library); MPP as `<rockchip/...>`, never a bare `<rk_*.h>` (`<rk_mpi.h>`, `<rk_type.h>`, ...), `<vpu.h>`, `<vpu_api.h>` or `<mpp_*.h>` (the distro 1.5.0 headers through `-I/usr/include/rockchip`). CLAUDE.md, "Board SDK", has the paths.

## Error & Exception Model

- **`Status` ([`include/core/status.hpp`](include/core/status.hpp))** (review)**:** Operations that can fail for a runtime reason return `Status` (`Errc`, string-literal `what`, optional `Sys::Errno` / `Sys::Sdk` code); results go to out-parameters. Outcome enums (e.g. `FrameStatus`, `SourceChannel::Attach`, `HttpHead::Parse`) carry `Status` in their failure arm.
- **Startup-only exceptions** (review)**:** `std::runtime_error` (via [`throw_on`](include/core/throw_on.hpp) when wrapping a `Status`) is permitted only on the startup path (constructors and `Pipeline` initialization).
- **`noexcept` frame path & threads** (`catch (...)`: gate, `custom-no-catch-all`; the rest: review)**:** Frame processing and thread bodies are `noexcept`: no exceptions are thrown and there is no `catch (...)` in the frame path. Exactly one `catch (...)` exists, a startup thread-launch rethrow (`InferencePool::start_workers`), allowlisted with `NOLINT(custom-no-catch-all): <reason>`.

  Legitimate `catch (const std::exception&)` sites outside the frame path:
  - [`src/main.cpp`](src/main.cpp): top-level startup `try`/`catch` logs `Fatal:` and returns exit code `3`.
  - [`include/platform/log_sink.hpp`](include/platform/log_sink.hpp): `AsyncLogSink` flusher thread is `noexcept` and catches `const std::exception&` around stream `write`/`flush`/drop-notice formatting.
  - [`include/platform/pi_mutex.hpp`](include/platform/pi_mutex.hpp): `PiMutex` constructor catches `const std::exception&` around its `PTHREAD_PRIO_INHERIT` fallback warning.
  - [`src/sys/cpu_affinity.cpp`](src/sys/cpu_affinity.cpp): `pin_current_thread_rt`, `lock_process_memory`, and `configure_rt_allocator` are `noexcept` and catch `const std::exception&` from their `Log` stream (the line is dropped, the best-effort RT setup stands).
  `MediaSource` catches `std::bad_variant_access` only on the startup `emplace`.

## Output Side vs. Recognition Invariant (review)

1. **No viewer connected:** The output side sleeps completely (no box drawing, no RGA compose, no `NO SIGNAL` frames, no H.264 encode; `enc` reads `0.0`).
2. **Recognition is viewer-independent:** Capture, decode, NPU inference, and tracking run whenever at least one source is live regardless of viewer state, and sleep (NPU/Re-ID workers parked on condition variables) when all sources are absent.
3. **With a viewer connected:** Online sources take the full draw/compose/encode path; absent sources display the pre-rendered gray `NO SIGNAL` tile.

## Comment Style & Formatting

- Max line length: **140 characters** ([`.clang-format`](.clang-format); gate: `line-length` in `rules_lint.sh`).
- Structured comments (review; header line ending with `:`, one item per line, indented continuations) instead of long prose.
- (review) Do not repeat rules from this file inside code comments; reference local invariants or verified behavior only.

## Pre-PR Verification Gate

Every PR targeting `master` must pass the full board gate:

```bash
./scripts/build.sh > build/gate.log 2>&1; echo rc=$?; tail -n 40 build/gate.log
```

This runs:
1. Release CMake build (`light_ai`).
2. Host unit tests (`light_ai_tests` — 72 suites, 10,293 checks) + ThreadSanitizer suites (`light_ai_tests_tsan` — 11 suites, 6,209 checks).
3. Two-pass `clang-tidy` gate ([`scripts/tidy_run.sh`](scripts/tidy_run.sh)):
   - **self-test + lint:** the rules pass on [`tests/tidy_probe/`](tests/tidy_probe) and [`scripts/rules_lint.sh`](scripts/rules_lint.sh) on its fixture tree must give exactly the expected hits, and `--verify-config` must know every check option key of `scripts/tidy_rules.yaml`; then `rules_lint.sh` over the tree (245 files).
   - **main:** system `clang-tidy-11` against [`.clang-tidy`](.clang-tidy) (`tests/` uses [`tests/.clang-tidy`](tests/.clang-tidy)).
   - **rules:** `venv/bin/clang-tidy` (`23.1.4`) against [`scripts/tidy_rules.yaml`](scripts/tidy_rules.yaml): cognitive complexity (`Threshold: 20`), `misc-const-correctness` and the `custom-*` project rules marked **(gate)** above.
   - A change to a rule check goes together with its fixture and expected file in `tests/tidy_probe/`.
   - **Function-size limits (`src/`):** variables 20 and branches 15 (raised in #93: every `const` local counts, and a flat sequence of checks is cheap to read), lines 90, statements 60, nesting 4. Cognitive complexity 20 is the real limit, for `src/` and `tests/` alike: split a function into helpers instead of relaxing it.
   - While editing: `clangd` diagnostics ([`.clangd`](.clangd)) and `TIDY_FILES="src/a.cpp" bash scripts/tidy_run.sh` on the changed TUs are hints; only the full gate above decides.

For pure-logic or multi-threaded changes, also verify Layer A reference parity (`bash tools/ref_replay.sh misc/ref_evening.mjpeg build/ref_p3.csv 3 reference/ref_evening.csv`) and live hardware execution on the board.

For structural changes that add, move, or remove a folder or a pipeline stage, update [`docs/images/architecture.svg`](docs/images/architecture.svg) by hand and check it with `xmllint --noout docs/images/architecture.svg`.

### Public Hygiene & Teamwork Quota Discipline

- **Public-hygiene check (before commit/PR):** run `grep` over `git diff` and PR/issue drafts to ensure 0 internal hostnames, LAN IPs, or local machine paths leak into the repository.
- **Agent execution discipline:** assign minimal Tier `0`–`3` (`Tier 0`: deterministic gate only; `Tier 1`: <= 1 executor + 1 clean-context adversarial reviewer; `Tier 2`: + fuzzer 50k–100k steps; `Tier 3`: cross-host/A/B), keep flat hierarchy (`max_depth = 1`), track `state.json` (`<= 40` lines) and `ledger.json`, run deterministic gates before LLM review, use `trap cleanup EXIT` in long shell wrappers, enforce context diet (`<= 80` lines stdout, `handoff.md <= 120` lines), and pass Research-Before-Code Gate (Rule 9: primary-source check + anti-pattern list + ablation before any OS/driver/scheduler/network code). All C++17 and soft-RTOS invariants above remain mandatory.
