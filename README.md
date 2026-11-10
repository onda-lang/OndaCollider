<h1>
  <img src="assets/svg/onda-logo-dark-circle.svg" alt="onda logo" width="40" align="absmiddle" /> OndaCollider
</h1>

SuperCollider plugin to run the [Onda](https://github.com/onda-lang/onda) audio programming language in `scsynth`/`supernova`.

[Pre-built binaries](https://github.com/onda-lang/OndaCollider/releases) are available for Windows, macOS and Linux.

Release signing status:

- **macOS:** binaries are not signed with an Apple Developer ID and are not notarized. macOS may
  block downloaded plug-ins until you approve them in **System Settings → Privacy & Security**.
  See [Apple's instructions](https://support.apple.com/102445).
- **Windows:** plug-in binaries are not code-signed (Authenticode).

## Usage

For usage and examples, check the `OndaDef` and `Onda` help files.

`OndaDef` accepts `.onda`/`.on` source files and self-contained `.ondaproject` manifests:

```supercollider
OndaDef(\project, "/path/to/project.ondaproject").send;
```

Project source files and file-backed assets are loaded relative to the manifest. Project buffer
assets become the runtime defaults. An `f32` project buffer can still be overridden by passing an
SC `Buffer` number to `Onda.ar`; omit it (or pass `-1`) to use the project asset.

Buffer arrays have no corresponding SuperCollider endpoint. They are accepted only when loading an
`.ondaproject`, are omitted from `OndaDef.ins`, and always retain their project-provided assets (or
Onda's neutral default for an unbound array slot).

Programs may also use `proc Main`; its public endpoints use the same mappings.
Onda input `ramp`/`lag` attributes run in the DSP program. OndaCollider preserves
the server thread's floating-point mode and does not enable denormal flushing itself.

Current constraints:

- `ins` must use `f32`; `params` may use any scalar Onda primitive type. SC controls are converted
  to the declared parameter type: integer values truncate toward zero and saturate at the type
  limits, while `bool` uses a `0.5` threshold.
- `events` must use a single scalar primitive payload (one control argument per event endpoint).
  An event fires whenever its control value changes, and the new value is converted to the declared
  payload type. Integer conversion uses truncation and saturation, while `bool` uses a `0.5`
  threshold. Event controls default to zero.
- Scalar enum params and event payloads use zero-based numeric ordinals, clamped to the enum's
  member range. Enum params expose stepped ControlSpecs; member names and labels are not exported
  to sclang.
- `outs` must use `f32` endpoint types (`f32` or `f32[N]`, flattened to SC channels).
- SC `Buffer` overrides support `buffer<f32...>` endpoints. Project-owned buffers may use any Onda
  primitive element type.
- Buffer arrays require an `.ondaproject` and cannot be overridden from SuperCollider.
- Missing or incompatible SC buffers are unbound from Onda, and the UGen outputs silence until every required buffer is valid and rebound.
- Onda `print(...)` output and top-level delegate occurrences are written to the SuperCollider
  server log. Output capture is bounded per UGen; excess occurrences or unusually large formatted
  lines are dropped with a server warning rather than allocating while processing.
  Structured delegate fields are logged by their dotted paths; arrays and slices are shown as
  flattened lists.

## Examples

- `examples/dualOsc.scd`
- `examples/fxChain.scd`
- `examples/lorenz.scd`
- `examples/svf.scd`
- `examples/syncGranulator.scd`
- `examples/syncGranulatorDual.scd`
- `examples/project/project.scd`

## Requirements

- SuperCollider 3.14.1 source tree (`SC_PATH`).
- Extracted Onda SDK with:
  - `include/onda.h`
  - a static library in `lib/`
- Or a local Onda checkout/build with:
  - `include/onda.h`
  - `target/release/onda.lib` on Windows or `target/release/libonda.a` on Unix

The Apple Silicon build targets macOS 11.0 or later, matching SuperCollider
3.14.1. The Onda SDK must be built for the same or an earlier deployment target.
The Linux release workflow builds inside a pinned Debian 12 container on an Ubuntu
runner, targeting glibc 2.36 and libstdc++ from GCC 12 (GLIBCXX 3.4.30). Windows x64
uses `windows-2022` with the static MSVC runtime (`/MT`). macOS uses `macos-latest`
with an explicit 11.0 deployment target.

Supported SDK asset patterns:

- `onda-<version>-linux-x64.tar.xz`
- `onda-<version>-macos-arm64.tar.xz`
- `onda-<version>-windows-x64.zip`

## Build

Default flow: download the pinned Onda SDK and install the plugin.

Unix:

```bash
./build.sh /path/to/supercollider /path/to/Extensions
```

Windows:

```bat
build.bat "C:\path\to\supercollider" "C:\path\to\Extensions"
```

If the Onda SDK path is omitted, the build scripts download the release pinned in [`onda-version`](onda-version) for the current platform into `build/onda-sdk`.
Set `ONDA_VERSION` to select a different release tag without changing the pin. Cached SDKs are reused only when their recorded version matches.

To use a local Onda checkout/build instead of the downloaded SDK, pass the Onda repo path explicitly and the install destination as the third argument.

Unix:

```bash
./build.sh /path/to/supercollider /path/to/onda /path/to/Extensions
```

Windows:

```bat
build.bat "C:\path\to\supercollider" "C:\path\to\onda" "C:\path\to\Extensions"
```
When using a local Onda repo, the build uses:

- `include/onda.h`
- `lib/` if present
- otherwise `target/release/` for locally built libraries
