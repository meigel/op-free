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

### On Debian and Ubuntu

The same three pieces are packaged, and `make -C sim wasm` finds them without arguments:

```bash
sudo apt install lld-21 libclang-rt-21-dev-wasm32 wasi-libc
make -C sim wasm                       # 32 modules, no variables to set
npm --prefix sim/web ci
npm --prefix sim/web run dev           # http://localhost:5174/?engine=engines/crease
```

Two things this costs nobody and one that catches people out:

- `wasm-ld` is not on `PATH` (`/usr/lib/llvm-21/bin/wasm-ld`) — the Makefile looks there.
- Ubuntu ships compiler-rt's wasm builtins as `lib/wasi/libclang_rt.builtins-wasm32.a`, but the `wasm32-wasip1` triple
  looks for `lib/wasm32-unknown-wasip1/libclang_rt.builtins.a`, so the link fails with a path that does not exist. The
  Makefile picks `wasm32-wasi` when that is the only archive present; it is the same target under its older name.
- **Start audio needs a real click.** A scripted `.click()` leaves the `AudioContext` suspended and the meters sit at
  −∞ dBFS, which reads exactly like a silent engine.

Without root the packages can be unpacked instead — `apt-get download` the four debs (`lld-19`, `libllvm19`,
`wasi-libc`, `libclang-rt-21-dev-wasm32`), `dpkg-deb -x` them into a prefix, copy
`lib/wasi/libclang_rt.builtins-wasm32.a` to `lib/wasm32-unknown-wasip1/libclang_rt.builtins.a`, and pass `WASM_CC`,
`WASM_LD` and `WASI_SYSROOT`. Note that `/usr/bin/clang` is a shim that silently discards `-resource-dir`; a wrapper
has to exec `/usr/lib/llvm-21/bin/clang` directly.

