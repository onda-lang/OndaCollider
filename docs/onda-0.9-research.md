# Onda 0.9 integration research

Research date: 2026-11-07. This records the local upstream checkout at
`/home/francesco/Sources/onda-shader2`, commit `2a5f8e2` (`0.8.40-59-g2a5f8e2`),
against OndaCollider's initial 0.8.38 pin (the [current pin](../onda-version) has
since been migrated). Upstream's workspace and
changelog name 0.9.0, but the local repository has no 0.9.0 tag. These are candidate
changes observed in source, not a guarantee about the eventual published release.
The upstream working tree was clean during inspection. [Version][version],
[candidate changelog][changelog].

Evidence comes from local first-party documentation, implementation, tests, and
`git diff 0.8.38..HEAD`; `git diff 0.8.40..HEAD` separates the candidate work from
the two releases skipped by the current pin. No production code was changed for
this research. Binding-specific checks and proposed scope follow the upstream
findings below.

## Implementation status

The binding now targets the 0.9.0 descriptors, unified constructor, readiness
check, and payload layout API. Scalar event validation uses typed tensor
descriptors; no JSON parser or compatibility layer was needed. Packed scalar
event dispatch normalizes enum domains, and ranged params use the plain-value
setter to clamp and snap before generated execution. Unranged params retain raw
typed storage. Delegate decoding uses preallocated region storage and logs
structured leaves by dotted path.

Per the user's decision, the binding does not call Onda's FP-mode helper and
preserves SC's thread policy. Linux ARM64 plugin support remains out of scope.
Enum controls use numeric ordinals and stepped specs; exporting choice labels
is deferred. The Linux workflow now uses a pinned Debian 12 container on an Ubuntu
runner. While 0.9.0 is unreleased, build against the local checkout or select an
unpublished SDK run and its full source commit in the manual workflow.

Validation: both plugin targets built against local `onda-shader2` and SC 3.14.1
headers. `pw-jack ctest --test-dir build --output-on-failure` passed both scsynth
and supernova suites, including Main, smoothed inputs, enum bounds and singleton
specs, output arrays, structured/empty delegate payloads with exact i64 values,
event-shape rejection, and invalid-buffer recovery. macOS and Windows were not
executed locally. All 38 called Onda API names are declared in the candidate
header. The observations below record the original investigation.

## Required hosted C API migration

The executable processor ABI remaining at version 7 does **not** imply that the
hosted `libonda` API is source compatible. Current `include/onda.h` removes many
entry points and changes signatures. MIR schema is now 9 and processor descriptor
schema is 7. OndaCollider uses the hosted API, so its immediate migration boundary
is `onda.h`. [Version distinctions][format-versions], [hosted API][api-intro].

| Area | Candidate API and integration consequence |
| --- | --- |
| Instance creation | Use `onda_instance_create(program, allocator, diag)`. Channel counts derive from the program; the allocator is optional. The separate allocator and initialized constructor variants are removed. The returned instance is uninitialized. [Lifecycle declaration][create] |
| Initialization | Set controls and bind buffers, then call `onda_init_checked(instance, ONDA_INIT_FULL, output)` and check its numeric status. This preserves OndaCollider's existing create/configure/init ordering. The unchecked init helper is removed. [Initialization][init], [runtime implementation][runtime-init] |
| Full blocks | `onda_process_checked(instance, output)` loses its `frames` argument and processes exactly the compiled block size. `onda_process_unchecked(instance, output)` already had this signature in 0.8.38; OndaCollider uses that call, so no process-call signature change is needed here. Partial or offset blocks use the existing segment calls. [Processing][process], [unchecked processing][prepare], [current processing](../plugins/Onda/Onda.cpp:2376) |
| Readiness | `onda_prepare_process` replaces `onda_prepare_unchecked_process`. The standalone validation helpers are removed. Binding validates and updates tables immediately; successful rebinds preserve readiness. [Readiness][prepare], [binding implementation][bindings] |
| Value metadata | Replace individual name/type/default/range/array/offset/control-domain getters with borrowed `onda_input_info`, `onda_output_info`, `onda_control_output_info`, `onda_param_info`, and `onda_state_info`. Counts and name-to-index functions remain. [Value records][value-info] |
| Buffer metadata | Use `onda_buffer_info` and `onda_buffer_array_info`; records contain element type/width, channel constraints, write permission, and array `first`/`len`. [Buffer records][message-info] |
| Event/delegate metadata | Use `onda_event_info`/`onda_delegate_info`, recursive `schema_json`, and their borrowed `payload_plan`. Old per-parameter scalar/array getters and separate event/delegate sizing getters are removed. [Message records][message-info], [recursive metadata][messages] |
| Payload layout | `onda_payload_tensor_info`, `onda_payload_layout`, and `onda_payload_layout_from_bytes` work for events **and** delegates. Layout reports all packed offsets, tensor lengths, and aligned workspace sizes without allocation. [Layout contract][payload-layout] |
| Source/project metadata | Property getters consolidate into `*_document_info`, `*_resolution_info`, unresolved-source info, project-buffer info, and materialization-file info records. This matters only for any current or future use of those query families. [Source records][source-info] |
| Occurrences | Indexed `*_batch_occurrence_at` helpers are removed; cursor-based `*_batch_next` remains. OndaCollider already uses cursors, so this removal requires no behavioral redesign. [Batch API][batches] |
| Errors | `ONDA_API_ERROR_PARAMETER_REJECTED` is removed. Runtime validation has specific negative codes for uninitialized/invalidated state, missing binding, unknown index, size, alignment, shape, sample rate, type, segments, flags, and domains. Generated statuses remain 0 success, 1 runtime safety failure, 2 rejected event input. [Errors][errors], [execution statuses][process] |

Descriptors and their strings, exact default bytes, schema JSON, and plans are
program-owned borrows. They may be cached while that program is retained; no
descriptor disposal is required. Optional strings/bytes can be null, and absent
numeric metadata uses flags and NaN. Parameter default bytes use native byte order;
event/delegate payloads remain packed little endian. Avoid replacing exact defaults
with the convenience `double` representation for full-width `i64` values.
[Descriptor contract][value-info], [encoding][encoding], [value-cache implementation][value-cache].

## Runtime consequences

Binding now commits only the affected audio or buffer descriptor slots after
validation. Rejection preserves the old binding. An unbound buffer remains a
neutral, processable resource; readiness requires initialized state and complete
audio bindings, not every external buffer. OndaCollider's documented policy of
silencing when a requested SC buffer is missing remains a separate host policy,
not something that can be delegated to Onda's readiness check. Rebinding does not
rerun authored initialization. [Binding implementation][bindings],
[neutral buffers and readiness][neutral], [OndaCollider constraints](../README.md).

Generated initialization, process, and event failures invalidate state; full init
or snapshot restore is required for recovery. Rejected event input does not
invalidate the processor. Runtime validation/execution errors no longer construct
diagnostic strings; rich compiler diagnostics remain separate. Initialization,
processing, events, binding validation and snapshot operations are documented as
allocation-free even on their rejection/failure paths. [Runtime error contract][init-doc],
[invalidation implementation][runtime-process], [event invalidation][event-status].

The host now owns floating-point thread policy. Runtime calls no longer establish
it implicitly. `onda_configure_current_thread_audio_fp_mode()` explicitly applies
x86 FTZ/DAZ or ARM64 FZ, preserves other control bits, and does not restore the
previous mode. Every explicit call reapplies the policy; it is not a once-per-thread
registration API. [FP implementation][fp].

Follow-up inspection of `../supercollider` at `97e4952` (3.15.0-dev); both server
denormal-setup function bodies also match the pinned `Version-3.14.1` tag:

- On x86, scsynth's `sc_SetDenormalFlags` enables FTZ and DAZ, and JACK,
  PortAudio, and CoreAudio callbacks invoke it before DSP. Supernova enables
  the same bits for its main audio thread and each DSP worker through its
  thread initialization functors. [scsynth setup][sc-fp],
  [JACK callback][sc-jack-fp], [PortAudio callback][sc-pa-fp],
  [CoreAudio callback][sc-ca-fp], [supernova setup][nova-fp],
  [worker initialization][nova-worker-fp], [main initialization][nova-main-fp].
- On ARM64, neither server has an explicit FPCR FZ setup in these paths.
  scsynth has a legacy `__VFP_FP__` FPSCR branch for 32-bit ARM; supernova's
  helper is x86-only. Clang preprocessing for both AArch64 Linux and Apple
  arm64 targets confirms `__VFP_FP__` is not defined there. This does not prove
  a particular audio backend or inherited thread mode has FZ disabled, but
  there is no equivalent explicit server guarantee to rely on.
- The same Onda helper can be used in both builds. If we choose explicit flushing,
  call it before initialization and at the start of each processing block, before
  parameter/event execution. It is allocation-free and writes only when the
  desired bits are absent. A plugin-load or UGen-constructor-only call cannot
  cover supernova's worker threads. The policy persists on the whole current
  thread; save and restore the previous mode around Onda execution if isolation
  from other UGens is required. Avoid a once-per-UGen or once-per-thread cache,
  which cannot detect another component resetting the mode.
- Revised recommendation after investigating performance: absence of ARM64 FZ
  setup does not itself demonstrate a performance problem. A published small
  [Rust benchmark](https://github.com/im-0/rust-denormals-bench#aarch64-arm64)
  reports equivalent timing for normal, subnormal, and flushed `f32` arithmetic
  on an Apple M2 Max running Linux. Its source measures a division/addition/
  multiplication expression on selected inputs; it does not establish the cost
  of all instructions, `f64`, library functions, dependency chains, or real DSP
  programs on every Apple chip. ARM64 specifies configurable denormal handling,
  not a universal timing guarantee. [Arm's FZ description](https://support.arm.com/documentation/102807/0001/AArch64-System-registers/AArch64-Generic-System-Control-registers-summary/FPCR--Floating-point-Control-Register).
  For our supported platforms, leaving SC's ARM64 mode unchanged is a reasonable
  default pending denormal-heavy Onda measurements. Explicit flushing remains a
  deliberate option for measured speed benefits or matching x86 underflow policy.
  With FZ disabled, tiny nonzero values can remain in decaying DSP state; this
  preserves gradual underflow rather than rounding those operations to zero.

Creation/destruction are documented as not realtime-safe. This is **not evidence
of a new regression**: the 0.8.38 header already explicitly marked destruction
not realtime-safe, and its custom-allocator creation allocated the same principal
runtime storage and instance handle. Current creation still routes parameter,
state, event-workspace, pointer-table, and handle allocations through the supplied
allocator. Failure paths still construct diagnostics, and dropping the last
program reference can release program/JIT ownership. A custom SC allocator alone
does not establish an unconditional realtime guarantee. This is a preexisting
lifecycle design decision to audit separately, not a newly introduced reason to
abandon the hosted API. [Current allocation][create-impl], [handle allocation][handle],
[runtime allocation][runtime-create], [destruction][destroy]. Historical evidence:
`git show 0.8.38:include/onda.h` lines 751–820 and
`git show 0.8.38:crates/onda_api/src/lib.rs` lines 1549–1585, 4263–4380.

## Events and logging

Structured messages are **not new in 0.9**: 0.8.38 already exposed recursive
schemas and structured SoA payloads alongside the scalar property getters.
The candidate removes that projection, making the recursive schema and generic
payload plan the metadata surface. Historical evidence:
`git show 0.8.38:docs/api.md` lines 427–465 and 663–689;
[current message contract][messages].

For OndaCollider's existing single-scalar input-event restriction, tensor count
and byte size alone do not distinguish a scalar from a one-field struct or a
length-one array. Inspection of the canonical planner established that every
valid parameter contributes at least one tensor: empty structs/tuples and zero-
length fixed arrays are rejected. One tensor/region, parameter index zero, an
unqualified leaf path, no fixed axes, and no slice marker therefore establish
one scalar parameter without parsing JSON. Enum scalars use `i32` storage with
additional domain metadata. [Shape validation][payload-shape],
[parameter planning][payload-plan], [enum C API tests][enum-tests].

For delegate logging, cache schema/tensor metadata outside processing and use
`onda_payload_layout_from_bytes` with preallocated region storage when decoding
each occurrence. This handles nested tensors and dynamic slices without copying
Onda's layout algorithm into the plugin. The query validates the entire payload's
shape before writing output, but does not validate/normalize primitive values.
Preserve the existing bounded log policy. [Layout contract][payload-layout].

Print and delegate batches remain separate caller-owned storage with call-local
sequence numbers for merging. Entries reset batches on entry; generated failure
retains diagnostic prints but clears delegates. Cursor decoding is linear and
should be consumed through the end to validate the complete batch. These are
contracts to preserve, not features absent from today's binding. [Output contract][output].

## Language and compiler changes visible to users

- `proc Main` is an optional ordinary, non-generic entry processor instantiated
  once per host instance. Its public names and shapes project onto the existing
  host interface; no separate host constructor is needed. It cannot coexist with
  bare top-level host/executable sections. Host params refresh before block-pre
  and exported events, with ordinary proc writes/hooks even for equal values;
  intermediate process segments do not refresh them. Main's own writes do not
  update host controls. [Entry semantics][main], [adapter implementation][main-impl].
- Enums have dense `i32` ordinals, external-domain clamping, nominal language types,
  and cached choice JSON on value descriptors and recursive event/delegate schemas.
  Enum `config const` values also use `i32` host inputs and export choice metadata.
  Existing numeric SC controls can carry ordinals; exposing labels is a separate
  binding/UI choice. [Enum tests][enum-tests], [candidate additions][changelog].
- Input `ramp` and `lag` attributes provide sample-rate-aware linear/exponential
  smoothing for float scalars and fixed arrays, including proc inputs. They work
  in generated DSP without a host-side smoothing implementation; zero disables
  state, segments do not restart smoothing, and snapshots include it. These apply
  to `ins`, not `params`; parameter smoothing remains host-owned. [Input attributes][smoothing].
- Buffer `.len()` now counts total scalar elements across channels; `.samples()`
  is the per-channel length. Buffer-collection `.len()` still counts buffers.
  `std::data::Data.samples()` replaces `.frames()`. Audit multichannel source code
  and position/length calculations when upgrading. The hosted `onda_bind_buffer`
  argument remains named `frames`, so this is a language/stdlib change rather
  than a C binding shape change. [Buffer semantics][buffer-length], [bind declaration][bind-header].
- Stdlib filter modes, ramp shapes, envelope stages, and FFT window choices now
  use enums. Existing Onda source that supplied numeric mode constants may need
  corresponding enum members. [Migration-relevant changelog][format-versions].
- Nested processor arrays/references and slices now preserve state, current
  buffers, callback environments, lifecycle hooks, and selection effects through
  helpers and conditional joins. Large arrays no longer require an implementation
  per element; reported 4–5× voice-bank benchmark gains are workload-specific,
  not an OndaCollider performance promise. [Compiler changes][compiler-changes],
  [implementation architecture][views].
- Since the pin also skips 0.8.39/0.8.40, the upgrade includes automatic native
  SLEEF vector math and broader compile/runtime optimizations, `Pan3db`, exact
  i64 control fixes, and the f64 sample-player position change. SLEEF is embedded
  bitcode, not a new runtime shared-library dependency. [Intervening releases][intervening],
  [LLVM dependency strategy][llvm].

## Projects, SDK, and scope

The project crate has no changes in `git diff 0.8.38..HEAD -- crates/onda_project`.
Current project behavior still includes manifest-relative assets, compile-constant
precedence, immutable shared project defaults, read-only enforcement, neutral
unbound slots, and explicit reset to a project default. `Main` projects use authored
public buffer names through the compiler's entry adapter. Source/project compile
entry points retain their models; the main integration delta is descriptor query
consolidation. [Projects][projects], [entry adapter][main-impl], [source API][api-source].

The candidate SDK states a Debian 12 / glibc 2.36 Linux baseline,
and uses VS2022 for Windows; macOS ARM64 minimum remains 11.0 and Windows static
libraries use `/MT`. Review OndaCollider's Linux build runner and supported host
baseline before pinning a published SDK. Linux ARM64 plugin support is explicitly
out of scope at the user's request. [Release platforms][sdk],
[candidate platform changes][changelog].

The new shared GPU run UI and native UI SDK are optional capabilities. They do not
require replacing OndaCollider's SuperCollider interface. `onda bench` can help
measure native DSP in isolation, but the SC integration still needs its own host
tests. [UI and bench additions][changelog], [architecture ownership][ui].

## Validation to perform during implementation

Compile against the candidate header first, then exercise actual SC integration:
descriptor defaults/names/order, per-UGen ownership, configured-before-init values,
full and partial processing, invalidation/recovery, scalar event edge behavior,
enum ordinal bounds, print/delegate order and bounded overflow, project defaults,
SC buffer replacement/unbinding/recovery, and a `proc Main` source and project.
Include a multichannel buffer example that distinguishes `.samples()` from
`.len()`. These checks target the changed contracts above; upstream test existence
alone does not demonstrate binding compatibility.

## Checks performed against OndaCollider

- Compared the candidate header with actual `onda_*` calls in
  [Onda.cpp](../plugins/Onda/Onda.cpp:269): 45 distinct called function names are
  absent, mostly metadata getters plus the allocator constructor and readiness
  helper. This counts missing declarations, not 45 independent migration tasks.
- Ran a C++17 syntax-only compile with the candidate header and installed
  SuperCollider development headers. It fails immediately on removed parameter
  getters in `readParamSpec`, confirming that changing only the version pin
  cannot build. This was not a plugin link or runtime test.
- Loaded the existing local `target/release/libonda.so`, which reports version
  `0.9.0`, and compiled every checked-in `.onda`, `.on`, and `.ondaproject` under
  `examples` and `tests/fixtures` at 48 kHz, block size 64: 20 inputs succeeded;
  the intentionally invalid fixture failed with `expected program`. This checks
  compiler acceptance, not SC binding acceptance or audio behavior. The local
  binary's exact source commit was not independently established.
- Compiled small enum and `proc Main`/input-`lag` programs through that library.
  Enum descriptors expose `i32`, ordinal ranges, unit steps, and member labels;
  a single-member enum has range `[0, 0]`. Main exposes its public parameter under
  its ordinary name. Test the zero-width enum domain when adapting ControlSpecs.

The current binding already configures controls/buffers before full init, merges
print/delegate output with cursors, and processes complete SC blocks. Preserve
those behaviors. Its input-event constraint and delegate formatter do depend on
removed metadata APIs, so those paths need deliberate adaptation.
[Initialization](../plugins/Onda/Onda.cpp:1253),
[event inspection](../plugins/Onda/Onda.cpp:571),
[delegate formatting](../plugins/Onda/Onda.cpp:1692),
[output merging](../plugins/Onda/Onda.cpp:1881).

## Proposed scope for discussion

1. **Required migration:** target the 0.9 API directly; replace getter families
   with descriptors, adapt both instance creation sites, use `onda_prepare_process`,
   retain configured-before-init ordering, and report the returned runtime error
   code when an operation fails. Inspect typed payload descriptors and use
   Onda's layout plans for bounded delegate logging. Keep the present SC
   endpoint restrictions explicit. Preserve SC's FP policy by default; add an
   explicit helper only for a demonstrated need or chosen flushing semantics,
   as discussed above.
2. **Recommended accompanying feature:** expose enum names, members, and labels
   through `OndaDef` metadata while keeping UGen controls numeric ordinals. This
   needs a metadata protocol change and a decision about convenient symbol-to-
   ordinal conversion. Enum events should remain subject to the same scalar
   event policy. Account for the existing 4,096-byte compile reply limit rather
   than blindly appending arbitrary label JSON.
   [Reply format and limit](../plugins/Onda/Onda.cpp:770),
   [sclang metadata decoding](../plugins/Onda/Onda.sc:143).
3. **Release work:** the Linux workflow currently uses `ubuntu-22.04`, below the
   SDK's stated glibc 2.36 baseline. Select a compatible build environment and
   document the resulting supported Linux baseline; building on a newer runner
   may increase it further. Keep testing against the local checkout until release
   artifacts exist; update `onda-version` as part of release readiness.
   [Current workflow](../.github/workflows/pluginbuild.yml:24),
   [current download platforms](../scripts/fetch-onda.sh:32).

Broader typed audio conversion, input/parameter arrays, aggregate event transport,
buffer-array overrides, and a GPU UI would expand existing host capabilities;
they are not prerequisites for this upgrade. The recommended first scope is the
required migration plus enum metadata, with `Main` and smoothing examples and
targeted integration checks. The examples' current `.len()` calculations concern
mono buffers and still compile; use `.samples()` where per-channel position or
length is intended and add a multichannel check to make the distinction visible.
[Current constraints](../README.md:29),
[granulator length](../examples/syncGranulator.onda:15),
[project length](../examples/project/main.onda:11).

[version]: /home/francesco/Sources/onda-shader2/Cargo.toml:33
[changelog]: /home/francesco/Sources/onda-shader2/CHANGELOG.md:8
[format-versions]: /home/francesco/Sources/onda-shader2/CHANGELOG.md:65
[api-intro]: /home/francesco/Sources/onda-shader2/docs/api.md:11
[create]: /home/francesco/Sources/onda-shader2/include/onda.h:659
[init]: /home/francesco/Sources/onda-shader2/include/onda.h:871
[runtime-init]: /home/francesco/Sources/onda-shader2/crates/onda_runtime/src/lib.rs:1342
[process]: /home/francesco/Sources/onda-shader2/include/onda.h:839
[prepare]: /home/francesco/Sources/onda-shader2/include/onda.h:918
[bindings]: /home/francesco/Sources/onda-shader2/crates/onda_runtime/src/bindings.rs:5
[value-info]: /home/francesco/Sources/onda-shader2/include/onda.h:941
[message-info]: /home/francesco/Sources/onda-shader2/include/onda.h:967
[messages]: /home/francesco/Sources/onda-shader2/docs/api.md:403
[payload-layout]: /home/francesco/Sources/onda-shader2/include/onda.h:1015
[payload-shape]: /home/francesco/Sources/onda-shader2/crates/onda_processor_abi/src/payload/shape.rs:20
[payload-plan]: /home/francesco/Sources/onda-shader2/crates/onda_processor_abi/src/payload/layout.rs:67
[source-info]: /home/francesco/Sources/onda-shader2/include/onda.h:1085
[batches]: /home/francesco/Sources/onda-shader2/include/onda.h:160
[errors]: /home/francesco/Sources/onda-shader2/include/onda.h:29
[encoding]: /home/francesco/Sources/onda-shader2/docs/api.md:126
[value-cache]: /home/francesco/Sources/onda-shader2/crates/onda_api/src/value_info.rs:80
[neutral]: /home/francesco/Sources/onda-shader2/docs/api.md:546
[init-doc]: /home/francesco/Sources/onda-shader2/docs/api.md:481
[runtime-process]: /home/francesco/Sources/onda-shader2/crates/onda_runtime/src/lib.rs:1486
[event-status]: /home/francesco/Sources/onda-shader2/crates/onda_runtime/src/lib.rs:1544
[fp]: /home/francesco/Sources/onda-shader2/crates/onda_realtime/src/lib.rs:3
[sc-fp]: /home/francesco/Sources/supercollider/server/scsynth/SC_World.cpp:154
[sc-jack-fp]: /home/francesco/Sources/supercollider/server/scsynth/SC_Jack.cpp:373
[sc-pa-fp]: /home/francesco/Sources/supercollider/server/scsynth/SC_PortAudio.cpp:126
[sc-ca-fp]: /home/francesco/Sources/supercollider/server/scsynth/SC_CoreAudio.cpp:1322
[nova-fp]: /home/francesco/Sources/supercollider/server/supernova/server/server.cpp:228
[nova-worker-fp]: /home/francesco/Sources/supercollider/server/supernova/server/server.cpp:311
[nova-main-fp]: /home/francesco/Sources/supercollider/server/supernova/server/server.cpp:343
[create-impl]: /home/francesco/Sources/onda-shader2/crates/onda_api/src/lib.rs:3690
[handle]: /home/francesco/Sources/onda-shader2/crates/onda_api/src/lib.rs:1459
[runtime-create]: /home/francesco/Sources/onda-shader2/crates/onda_runtime/src/lib.rs:1283
[destroy]: /home/francesco/Sources/onda-shader2/crates/onda_api/src/lib.rs:3755
[enum-tests]: /home/francesco/Sources/onda-shader2/crates/onda_api/tests/enums_c_api.rs:5
[output]: /home/francesco/Sources/onda-shader2/docs/api.md:641
[main]: /home/francesco/Sources/onda-shader2/docs/syntax.md:92
[main-impl]: /home/francesco/Sources/onda-shader2/crates/onda_semantics/src/processor_lowering/entry_adapter.rs:27
[smoothing]: /home/francesco/Sources/onda-shader2/docs/syntax.md:1002
[buffer-length]: /home/francesco/Sources/onda-shader2/docs/syntax.md:1844
[bind-header]: /home/francesco/Sources/onda-shader2/include/onda.h:806
[compiler-changes]: /home/francesco/Sources/onda-shader2/CHANGELOG.md:44
[views]: /home/francesco/Sources/onda-shader2/docs/architecture.md:879
[intervening]: /home/francesco/Sources/onda-shader2/CHANGELOG.md:117
[llvm]: /home/francesco/Sources/onda-shader2/docs/architecture.md:857
[projects]: /home/francesco/Sources/onda-shader2/docs/projects.md:257
[api-source]: /home/francesco/Sources/onda-shader2/docs/api.md:207
[sdk]: /home/francesco/Sources/onda-shader2/README.md:58
[ui]: /home/francesco/Sources/onda-shader2/docs/architecture.md:522
