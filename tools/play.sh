#!/usr/bin/env bash
# play.sh — render one engine in the simulator and play it.
#
#   tools/play.sh                      crease, its basic scenario
#   tools/play.sh table                another engine
#   tools/play.sh dx knobs             a named scenario from engines/dx/tests/
#   tools/play.sh dx engines/dx/tests/knobs.scn   any path
#   tools/play.sh crease --fx plate    through an effect slot
#   tools/play.sh --list               every engine with the slot and trackmask it will be given
#
# Options: --no-play (render and report only)  --out FILE  --trackmask M  --bits 16|32  --fx NAME
#          --slot N / --fx-slot N   also run the simulator's static-size gate (a host-build estimate)
#
# Why a script: the simulator is a CLI, so hearing a voice meant reading the engine's plug.json for its slot and
# trackmask, finding a scenario, and knowing that the engine must be built first. This resolves all three from the
# tree, renders, reports peak and RMS, and hands the file to whatever player exists.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SIM="$ROOT/sim/build/opzsim"
OUT=""
NO_PLAY=0
FX=""
SLOT_OVERRIDE=""
MASK_OVERRIDE=""
BITS="32"
ENGINE=""
SCENARIO=""

die() { printf 'play.sh: %s\n' "$*" >&2; exit 2; }

list_engines() {
  python3 - "$ROOT" <<'PY'
import glob, json, os, sys
TR = ["kick", "snare", "perc", "sample", "bass", "lead", "arp", "chord", "fx1", "fx2", "tape", "master"]
root = sys.argv[1]
rows = []
for p in sorted(glob.glob(os.path.join(root, "engines", "*", "plug.json"))):
    j = json.load(open(p))
    plug = j.get("plug") or {}
    mask = plug.get("trackmask") or 0
    tracks = ",".join(TR[i] for i in range(12) if mask >> i & 1) or "-"
    kind = "synth" if mask & 0x00f0 else ("drum" if mask & 0x000f else "fx")
    tests = sorted(glob.glob(os.path.join(os.path.dirname(p), "tests", "*.scn")))
    rows.append((os.path.basename(os.path.dirname(p)), plug.get("id", "-"), j.get("memoryslot", "-"),
                 mask, tracks, kind, len(tests)))
print("%-12s %-7s %-5s %-5s %-6s %-30s %s" % ("engine", "id", "slot", "mask", "kind", "tracks", "scenarios"))
for name, pid, slot, mask, tracks, kind, n in rows:
    print("%-12s %-7s %-5s %-5s %-6s %-30s %d" % (name, pid, slot, mask, kind, tracks[:30], n))
print("\n%d engines" % len(rows))
PY
}

while [ $# -gt 0 ]; do
  case "$1" in
    --list)       list_engines; exit 0 ;;
    --no-play)    NO_PLAY=1; shift ;;
    --out)        OUT="${2:-}"; [ -n "$OUT" ] || die "--out needs a path"; shift 2 ;;
    --fx)         FX="${2:-}"; [ -n "$FX" ] || die "--fx needs an engine name"; shift 2 ;;
    --slot)       SLOT_OVERRIDE="${2:-}"; shift 2 ;;
    --fx-slot)    FX_SLOT_OVERRIDE="${2:-}"; shift 2 ;;
    --trackmask)  MASK_OVERRIDE="${2:-}"; shift 2 ;;
    --bits)       BITS="${2:-}"; shift 2 ;;
    -h|--help)    sed -n '2,16p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
    -*)           die "unknown option $1" ;;
    *)            if [ -z "$ENGINE" ]; then ENGINE="$1"; elif [ -z "$SCENARIO" ]; then SCENARIO="$1";
                  else die "unexpected argument: $1"; fi; shift ;;
  esac
done

ENGINE="${ENGINE:-crease}"
ENG_DIR="$ROOT/engines/$ENGINE"
[ -f "$ENG_DIR/plug.json" ] || die "no engine '$ENGINE' (try --list)"
[ -f "$ENG_DIR/$ENGINE.c" ] || die "$ENG_DIR has a plug.json but no $ENGINE.c — is it a template?"

# slot, trackmask and a default scenario come from the engine's own plug.json and tests/
read -r PLUG_SLOT PLUG_MASK < <(python3 - "$ENG_DIR/plug.json" <<'PY'
import json, sys
p = json.load(open(sys.argv[1]))
print(p.get("memoryslot", 0), (p.get("plug") or {}).get("trackmask", 0))
PY
)
MASK="${MASK_OVERRIDE:-$PLUG_MASK}"

if [ -z "$SCENARIO" ]; then
  for cand in "$ENG_DIR/tests/basic.scn" "$ENG_DIR/tests/knobs.scn"; do
    [ -f "$cand" ] && SCENARIO="$cand" && break
  done
  [ -n "$SCENARIO" ] || SCENARIO="$(ls "$ENG_DIR"/tests/*.scn 2>/dev/null | head -1 || true)"
  [ -n "$SCENARIO" ] || die "$ENGINE has no scenarios in $ENG_DIR/tests/"
elif [ ! -f "$SCENARIO" ] && [ -f "$ENG_DIR/tests/$SCENARIO" ]; then
  SCENARIO="$ENG_DIR/tests/$SCENARIO"
elif [ ! -f "$SCENARIO" ] && [ -f "$ENG_DIR/tests/$SCENARIO.scn" ]; then
  SCENARIO="$ENG_DIR/tests/$SCENARIO.scn"
fi
[ -f "$SCENARIO" ] || die "no scenario '$SCENARIO'"

# build only what is missing: the host binary, this engine, and the effect if one was named
NEED_BUILD=0
[ -x "$SIM" ] || NEED_BUILD=1
[ -f "$ROOT/sim/build/engines/$ENGINE.dylib" ] || NEED_BUILD=1
if [ -n "$FX" ]; then
  [ -f "$ROOT/engines/$FX/plug.json" ] || die "no effect '$FX' (try --list)"
  [ -f "$ROOT/sim/build/engines/$FX.dylib" ] || NEED_BUILD=1
fi

if [ "$NEED_BUILD" = 1 ]; then
  printf 'building the simulator and engines (first run only)…\n'
  make -C "$ROOT/sim" >/dev/null || die "make -C sim failed"
fi

if [ -z "$OUT" ]; then
  OUT="$(mktemp "${TMPDIR:-/tmp}/play-XXXXXX.wav")"
  CLEAN=1
else
  CLEAN=0
fi
STATS="$(mktemp "${TMPDIR:-/tmp}/play-stats-XXXXXX.json")"
cleanup() { rm -f "$STATS"; [ "${CLEAN:-0}" = 1 ] && rm -f "$OUT"; }
trap cleanup EXIT

ARGS=(-e "$ROOT/sim/build/engines/$ENGINE.dylib" --trackmask "$MASK" -o "$OUT" --bits "$BITS" --stats "$STATS")
# --slot is not needed to hear a voice; it switches on the static-size gate, whose host-build estimate runs larger
# than the target's and rejects engines that fit on the device (plate is 14,490 B on the host, inside 10 KB here).
[ -n "$SLOT_OVERRIDE" ] && ARGS+=(--slot "$SLOT_OVERRIDE")
if [ -n "$FX" ]; then
  read -r FX_SLOT FX_MASK < <(python3 - "$ROOT/engines/$FX/plug.json" <<'PY'
import json, sys
p = json.load(open(sys.argv[1]))
print(p.get("memoryslot", 0), (p.get("plug") or {}).get("trackmask", 0))
PY
)
  ARGS+=(--fx "$ROOT/sim/build/engines/$FX.dylib" --fx-trackmask "$FX_MASK")
  [ -n "${FX_SLOT_OVERRIDE:-}" ] && ARGS+=(--fx-slot "$FX_SLOT_OVERRIDE")
fi

# opzsim's exit codes: 1 args or scenario, 2 engine load, 3 a scenario expectation failed, 4 an ABI violation.
# 3 renders the file anyway — an effect's tail or a different patch legitimately breaks a silence assertion — so it
# is reported and the render is still played. 1, 2 and 4 are real refusals.
RC=0
"$SIM" "${ARGS[@]}" "$SCENARIO" 2>"$STATS.err" || RC=$?
case "$RC" in
  0) rm -f "$STATS.err" ;;
  3) printf "note: the scenario's own expectations did not hold (rendered anyway):\n"
     grep -E '^expect ' "$STATS.err" | grep -v 'ok$' | sed 's/^/  /' || true
     rm -f "$STATS.err" ;;
  4) cat "$STATS.err" >&2; rm -f "$STATS.err"
     die "the engine broke an ABI rule (exit 4) — see the violations above; do not install it" ;;
  *) cat "$STATS.err" >&2; rm -f "$STATS.err"; die "opzsim refused the render (exit $RC)" ;;
esac

python3 - "$STATS" "$ENGINE" "$SCENARIO" "$MASK" "$FX" <<'PY'
import json, os, sys
stats, engine, scenario, mask, fx = sys.argv[1:6]
try:
    s = json.load(open(stats))
except (OSError, ValueError):
    s = {}
peak = s.get("peak_dbfs"); rms = s.get("rms_dbfs"); clipped = s.get("clipped")
print("%s  %s" % (engine, os.path.basename(scenario)) + ("  + %s" % fx if fx else ""))
if peak is not None:
    print("  peak %s dBFS   rms %s dBFS   clipped %s" % (peak, rms, clipped))
if not (int(mask) & 0x00ff):
    print("  note: this engine is an effect (trackmask %d) — it has no instrument tracks, so a bare render can be silent" % int(mask))
if peak is not None and float(peak) < -60:
    print("  note: silent output — the scenario may target a track this engine is not on")
PY

[ "$NO_PLAY" = 1 ] && { printf '  rendered: %s\n' "$OUT"; CLEAN=0; trap - EXIT; rm -f "$STATS"; exit "$RC"; }

PLAYER=""
for cand in paplay aplay afplay ffplay; do
  command -v "$cand" >/dev/null && PLAYER="$cand" && break
done
case "$PLAYER" in
  paplay) paplay "$OUT" ;;
  aplay)  aplay -q "$OUT" ;;
  afplay) afplay "$OUT" ;;
  ffplay) ffplay -nodisp -autoexit -loglevel error "$OUT" ;;
  *) printf '  no player found — the render is %s\n' "$OUT"; CLEAN=0; trap - EXIT; rm -f "$STATS" ;;
esac
exit "$RC"
