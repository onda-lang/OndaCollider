# OndaCollider

SuperCollider plugin to Onda (`onda`) code in `scsynth`/`supernova`.

## Build

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

## Requirements

- SuperCollider source tree (`SC_PATH`).
- Extracted Onda SDK with:
  - `include/onda.h`
  - a static library in `lib/`

Supported SDK asset patterns:

- `onda-<version>-linux-x64.tar.xz`
- `onda-<version>-macos-arm64.tar.xz`
- `onda-<version>-windows-x64.zip`

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
