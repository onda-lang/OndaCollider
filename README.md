# OndaCollider

SuperCollider plugin for running Onda code in `scsynth`/`supernova`.

Pre-built binaries are available in the Release page.

## Build

Default flow: download the latest published Onda SDK and install the plugin.

Windows:

```bat
build.bat "C:\path\to\supercollider" "C:\path\to\Extensions"
```

Unix:

```bash
./build.sh /path/to/supercollider /path/to/Extensions
```

If the Onda SDK path is omitted, the build scripts resolve the latest published `onda-lang/onda` release and download the matching SDK for the current platform into `build/onda-sdk`.
Set `ONDA_VERSION` to pin a specific release tag instead.

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

Current constraints:

- `ins` and `params` must use `f32` endpoint types for SC integration.
- `events` must use a single `f32` payload (one control argument per event endpoint).
- `outs` must use `f32` endpoint types (`f32` or `f32[N]`, flattened to SC channels).
- Only `buffer[f32...]` endpoints are supported for SC integration.

## Examples

- `examples/dualOsc.scd`
- `examples/fxChain.scd`
- `examples/lorenz.scd`
- `examples/svf.scd`
- `examples/syncGranulator.scd`
- `examples/syncGranulatorDual.scd`
