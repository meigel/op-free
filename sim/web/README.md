# opzsim web

Play OP-Z synths and drums on eight tracks through effects on FX1 and FX2 in the browser before installing any of
them, from the computer keyboard or an OP-Z over MIDI (each track on its own channel, the OP-Z's dials moving the
knobs; opz-as-controller.md). The page runs each engine with opzsim's host C compiled
to WebAssembly (one module per engine, `make -C sim wasm`), in an AudioWorklet at 44.1 kHz with 64-frame blocks.
Scenario renders are bit-identical to the `opzsim` CLI. How it works and how it differs from the CLI:
[sim/README.md](../README.md#web-simulator). Setup from a fresh clone: [QUICKSTART.md](../../sdk/docs/QUICKSTART.md#play-it-in-the-browser).

```bash
make -C sim wasm
npm --prefix sim/web ci
npm --prefix sim/web run dev        # http://localhost:5174/?engine=engines/fm2
```

### On Linux without root, and without the wasm packages

The three prerequisites are a clang with the `wasm32` target, `wasm-ld`, and a WASI libc — the Makefile's defaults
assume a Homebrew or `/opt/wasi-sdk` layout. Ubuntu 26.04 ships none of them, and installing them needs root. They can
be unpacked instead, which is how this was verified:

```bash
cd ~ && mkdir -p opt/wasi && cd opt/wasi
for p in lld-19 libllvm19 wasi-libc libclang-rt-21-dev-wasm32; do apt-get download $p; done
for d in *.deb; do dpkg-deb -x "$d" ~/opt/wasi; done

# compiler-rt ships its wasm builtins under lib/wasi; clang looks for them under lib/wasm32-unknown-wasip1
mkdir -p ~/opt/wasi/rt21 && cp -r /usr/lib/llvm-21/lib/clang/21/. ~/opt/wasi/rt21/
cp -r ~/opt/wasi/usr/lib/llvm-21/lib/clang/21/lib/. ~/opt/wasi/rt21/lib/
cp ~/opt/wasi/rt21/lib/wasi/libclang_rt.builtins-wasm32.a \
   ~/opt/wasi/rt21/lib/wasm32-unknown-wasip1/libclang_rt.builtins.a

# /usr/bin/clang is a shim that discards -resource-dir: wrap the real driver
printf '#!/bin/sh\nexec /usr/lib/llvm-21/bin/clang -resource-dir=%s/rt21 "$@"\n' "$HOME/opt/wasi" > ~/opt/wasi/clang-wasm
chmod +x ~/opt/wasi/clang-wasm

export LD_LIBRARY_PATH=$HOME/opt/wasi/usr/lib/x86_64-linux-gnu:$LD_LIBRARY_PATH
make -C sim wasm WASM_CC=$HOME/opt/wasi/clang-wasm \
     WASM_LD=$HOME/opt/wasi/usr/lib/llvm-19/bin/wasm-ld WASI_SYSROOT=$HOME/opt/wasi/usr
```

32 modules build. `npm --prefix sim/web ci` then `run dev`, and the page plays from the computer keyboard (A W S E D F …
from C3) or from an OP-Z over MIDI. The **Start audio** button needs a real click — a scripted `.click()` leaves the
AudioContext suspended and the meters at −∞ dBFS, which reads like a silent engine.

With root the same thing is three packages, and the Makefile's defaults then work unchanged:

```bash
sudo apt-get install lld-21 libclang-rt-21-dev-wasm32 wasi-libc
make -C sim wasm WASI_SYSROOT=/usr
```

| File | Role |
|---|---|
| `catalog.ts` | Vite plugin: `/api/catalog` (engines and templates with plug.json, presets, scenarios, module URL) and `/files/…` (only `engines/`, `sdk/templates/`, `sim/build/wasm/`, `sim/build/samples/`) |
| `src/wasi.ts` | the WASI preview1 calls the modules import, over an in-memory file system |
| `src/opzsim.ts` | `runScenario` (opzsim's `main`) and `LiveEngine` (the `ws_*` entry points of `sim/wasm/websim.c`) |
| `src/worklet.ts` | the AudioWorklet: the eight instrument tracks and the FX1 and FX2 slots |
| `src/midimap.ts` | MIDI in: channel → track, OP-Z CC → parameter byte, MIDI learn (no DOM) |
| `src/mixer.ts` | the send/return model: each send crossfades, an effect's dry level keeps the dry path, FX1 and FX2 in parallel, the tracks summed |
| `src/main.ts`, `knob.ts`, `keyboard.ts`, `scope.ts` | the page |

| Test | Checks | Needs |
|---|---|---|
| `npm test` | every `engines/*/tests/*.scn` rendered in wasm hashes as its golden file; the live path gives the same samples as the scenario path (templates, fm2, kit, swirl, filter); the mixer model; the MIDI map and learn | `make -C sim wasm` (skips without) |
| `npm run test:e2e` | headless Chrome: each template, swirl, fm2 and kit played from the computer keyboard; effects on a generator; fm2 through a passthrough effect keeps its level as the send crossfades, and gains ~6 dB with dry level 255; through passthroughs on FX1 and FX2 at full send the returns add up (+6 dB); fm2 through swirl; a scenario run in the page matches its golden; MIDI: each channel plays its track, OP-Z CCs move the knobs and the page follows, learn survives a reload, the virtual OP-Z | the above and Chrome (`CHROME_PATH`) |
| `npm run typecheck` | TypeScript | |

Host CPU in the page is this browser's WebAssembly, not Blackfin cycles: for those run `python3 tools/opz_cycles.py`.
