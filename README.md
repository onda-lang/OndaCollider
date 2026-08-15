# OndaCollider

SuperCollider plugin for running the [onda](https://github.com/onda-lang/onda) JIT compiler in `scsynth`/`supernova`.

Pre-built binaries are available in the Release page.

## Build

Default flow: download the pinned Onda SDK and install the plugin.

Windows:

```bat
build.bat "C:\path\to\supercollider" "C:\path\to\Extensions"
```

Unix:

```bash
./build.sh /path/to/supercollider /path/to/Extensions
```

If the Onda SDK path is omitted, the build scripts download the release pinned in [`onda-version`](onda-version) for the current platform into `build/onda-sdk`.
Set `ONDA_VERSION` to select a different release tag without changing the pin. Cached SDKs are reused only when their recorded version matches.

To use a local Onda checkout/build instead of the downloaded SDK, pass the Onda repo path explicitly and the install destination as the third argument.

Windows:

```bat
build.bat "C:\path\to\supercollider" "C:\path\to\onda" "C:\path\to\Extensions"
```

Unix:

```bash
./build.sh /path/to/supercollider /path/to/onda /path/to/Extensions
```

When using a local Onda repo, the build uses:

- `include/onda.h`
- `lib/` if present
- otherwise `target/release/` for locally built libraries

## Requirements

- SuperCollider source tree (`SC_PATH`).
- Extracted Onda SDK with:
  - `include/onda.h`
  - a static library in `lib/`
- Or a local Onda checkout/build with:
  - `include/onda.h`
  - `target/release/onda.lib` on Windows or `target/release/libonda.a` on Unix

Supported SDK asset patterns:

- `onda-<version>-linux-x64.tar.xz`
- `onda-<version>-macos-arm64.tar.xz`
- `onda-<version>-windows-x64.zip`

## Windows Notes

- OndaCollider is configured to use the static MSVC runtime (`/MT`, `/MTd`) on Windows.
- For fully static Windows linking, Onda should be built with a matching CRT configuration.

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

Current constraints:

- `ins` and `params` must use `f32` endpoint types for SC integration.
- `events` must use a single `f32` payload (one control argument per event endpoint). They follow
  SC trigger semantics: a transition from non-positive to positive fires once, and the positive
  edge value becomes the payload. Onda event parameter defaults do not replace the idle SC value.
- `outs` must use `f32` endpoint types (`f32` or `f32[N]`, flattened to SC channels).
- SC `Buffer` overrides support `buffer<f32...>` endpoints. Project-owned buffers may use any Onda
  primitive element type.
- Buffer arrays require an `.ondaproject` and cannot be overridden from SuperCollider.
- Missing or incompatible SC buffers are unbound from Onda, and the UGen outputs silence until every required buffer is valid and rebound.

## Examples

- `examples/dualOsc.scd`
- `examples/fxChain.scd`
- `examples/lorenz.scd`
- `examples/svf.scd`
- `examples/syncGranulator.scd`
- `examples/syncGranulatorDual.scd`
- `examples/project/project.scd`

## Tests

On Linux, after building, run the headless scsynth integration suite with:

```bash
ctest --test-dir build --output-on-failure
```

The suite exercises source and project compilation, buffer defaults and overrides, buffer-array
constraints, event edges, definition ordering, and invalid-buffer silence.
