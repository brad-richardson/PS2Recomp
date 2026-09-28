#!/bin/bash
# IB3: close the OM1 offline-microVU island behind `ld -r` so its ARMSX2 PCSX2
# symbols can never leak into (or win over) the app's own PCSX2 core (GE1).
#
# Why: the OM1 stage objects + OM1 archives and the GE1 archives are two full
# PCSX2 cores (~20k duplicate defined globals: EmuFolders, CocoaTools,
# StringUtil, ...). The stage genuinely shares 172 definitions with OM1's
# archives (header-defined microVU helpers), so a single `ld -r` errors out;
# the sandwich below demotes the stage side of exactly those 172 first.
# (`ld -r` demotes private-externals, so the archive side can't be fixed by a
# pre-pass; the stage side can, because the final export list restores it.)
#
# Steps:
#   1. keep1 = stage-defined globals minus (stage-defined ∩ archive-defined).
#   2. ld -r stage objects -> stage_closed.o, exporting keep1 only.
#   3. exports = stage-defined _om1* and _ps2x_microvu_* (the dlsym slot names
#      the tables resolve via dlsym(RTLD_DEFAULT) at init + the 5 C ABI funcs
#      the runtime calls; nothing else crosses the boundary).
#   4. ld -r stage_closed.o + OM1 archives -> closed.o, exporting `exports`.
#   5. Checks (fail the build): the 5 ABI funcs are global; every dlsym slot
#      name from *_tables.c is global; no closed global is also defined by
#      the GE1 archives (when given: combined GE1+OM1 app builds).
#
# Usage:
#   ps2x_om1_close.sh LD NM AR STAGE_CLOSED CLOSED TABLES_DIR STAGE_LIB -- ARC... -- GE1ARC...
# The stage arrives as one static lib (extracted below: object-first `ld -r`
# infers arch+platform, and $<TARGET_FILE> is the one spelling the Xcode
# generator gets right). The GE1 section may be empty, for OM1-only builds
# (the trailing -- stays).
set -euo pipefail
export LC_ALL=C

if [ $# -lt 8 ]; then
  echo "usage: $0 LD NM AR STAGE_CLOSED CLOSED TABLES_DIR STAGE_LIB -- ARC... -- GE1ARC..." >&2
  exit 2
fi
LD=$1; NM=$2; AR=$3; STAGE_CLOSED=$4; CLOSED=$5; TABLES_DIR=$6; shift 6

OBJS=(); ARCS=(); GE1=(); SEEN=0
for a in "$@"; do
  if [ "$a" = "--" ]; then SEEN=$((SEEN + 1)); continue; fi
  case $SEEN in
    0) OBJS+=("$a");;
    1) ARCS+=("$a");;
    *) GE1+=("$a");;
  esac
done
if [ "$SEEN" -lt 2 ]; then echo "$0: need two -- separators" >&2; exit 2; fi

TMP=$(dirname "$CLOSED")/om1-close-tmp
rm -rf "$TMP"; mkdir -p "$TMP"

# nm lists one archive's globals the same way regardless of member layout, so
# plain `nm -g --defined-only` over the file list is the whole census. (The
# two nm output shapes — `addr TYPE name` and `addr (seg,sect) state name` —
# both carry the symbol last; awk NF>=2 prints $NF.)
NOBJ=${#OBJS[@]}; NARC=${#ARCS[@]}; NGE1=${#GE1[@]}
echo "IB3 close: $NOBJ stage libs, $NARC OM1 archives, $NGE1 GE1 archives"
if [ "$NOBJ" != 1 ] || [ "$NARC" = 0 ]; then echo "$0: want 1 stage lib + OM1 archives" >&2; exit 2; fi
STAGE_LIB=${OBJS[0]}
case $STAGE_LIB in
  /*) ;;
  *) STAGE_LIB=$PWD/$STAGE_LIB;;
esac
if [ ! -f "$STAGE_LIB" ]; then echo "$0: stage lib missing: $STAGE_LIB" >&2; exit 2; fi

"$NM" -g --defined-only "$STAGE_LIB" 2>/dev/null | awk 'NF>=2{print $NF}' | sort -u > "$TMP/stage-defs.txt"
"$NM" -g --defined-only "${ARCS[@]}" 2>/dev/null | awk 'NF>=2{print $NF}' | sort -u > "$TMP/member-defs.txt"
comm -12 "$TMP/stage-defs.txt" "$TMP/member-defs.txt" > "$TMP/dups.txt"
comm -23 "$TMP/stage-defs.txt" "$TMP/member-defs.txt" > "$TMP/keep1.txt"
NDUP=$(wc -l < "$TMP/dups.txt" | tr -d ' ')
echo "IB3 close: $(wc -l < "$TMP/stage-defs.txt" | tr -d ' ') stage defs, $NDUP shared with OM1 archives"

# Step 1: close the stage, demoting the shared definitions (the archive side
# satisfies those references itself; the code is header-identical).
mkdir -p "$TMP/stage-obj"
(cd "$TMP/stage-obj" && "$AR" x "$STAGE_LIB")
STAGE_OBJS=("$TMP"/stage-obj/*.o)
"$LD" -r -o "$STAGE_CLOSED" "${STAGE_OBJS[@]}" -exported_symbols_list "$TMP/keep1.txt"

# Step 2: the API surface. The runtime calls exactly the 5 ps2x_microvu_* C
# functions; the tables dlsym their _om1* slot names at init. Both sets are
# stage-defined, so generate the list from the stage (no hardcoded names
# except the 5, which are asserted below).
grep -E '^_(om1|ps2x_microvu)' "$TMP/stage-defs.txt" > "$TMP/exports.txt" || true
for f in _ps2x_microvu_abi _ps2x_microvu_init _ps2x_microvu_shutdown _ps2x_microvu_run _ps2x_microvu_get_stats; do
  if ! grep -qxF "$f" "$TMP/exports.txt"; then echo "$0: bridge API $f not stage-defined" >&2; exit 2; fi
done
echo "IB3 close: $(wc -l < "$TMP/exports.txt" | tr -d ' ') exported (API + om1 slots)"
"$LD" -r -o "$CLOSED" "$STAGE_CLOSED" "${ARCS[@]}" -exported_symbols_list "$TMP/exports.txt"

"$NM" -g --defined-only "$CLOSED" 2>/dev/null | awk 'NF>=2{print $NF}' | sort -u > "$TMP/closed-defs.txt"

# Check A: every export survived (a missing export is a dead bridge/dlsym).
comm -23 "$TMP/exports.txt" "$TMP/closed-defs.txt" > "$TMP/missing.txt"
if [ -s "$TMP/missing.txt" ]; then echo "$0: exports lost in close:" >&2; head "$TMP/missing.txt" >&2; exit 2; fi

# Check B: every dlsym slot name in the tables resolves.
grep -hoE '"_om1[A-Za-z0-9_]+"' "$TABLES_DIR"/*_tables.c 2>/dev/null | tr -d '"' | sort -u > "$TMP/slots.txt" || true
if [ ! -s "$TMP/slots.txt" ]; then echo "$0: no slot names in $TABLES_DIR" >&2; exit 2; fi
comm -23 "$TMP/slots.txt" "$TMP/closed-defs.txt" > "$TMP/noslot.txt"
if [ -s "$TMP/noslot.txt" ]; then echo "$0: dlsym slots not global:" >&2; head "$TMP/noslot.txt" >&2; exit 2; fi
echo "IB3 close: $(wc -l < "$TMP/slots.txt" | tr -d ' ') dlsym slots all global"

# Check C: no duplicate strong definition vs the GE1 archives (the IB3 gate:
# with lagV the GS path is guest-observable, so the GE1 core must win every
# shared symbol; here the island exports none of them).
if [ "$NGE1" != 0 ]; then
  "$NM" -g --defined-only "${GE1[@]}" 2>/dev/null | awk 'NF>=2{print $NF}' | sort -u > "$TMP/ge1-defs.txt"
  comm -12 "$TMP/closed-defs.txt" "$TMP/ge1-defs.txt" > "$TMP/ge1dups.txt"
  if [ -s "$TMP/ge1dups.txt" ]; then
    echo "$0: closed island still defines $(wc -l < "$TMP/ge1dups.txt" | tr -d ' ') GE1 symbols:" >&2
    head -20 "$TMP/ge1dups.txt" >&2
    exit 2
  fi
  echo "IB3 close: 0 duplicate definitions vs GE1"
else
  echo "IB3 close: OM1-only build, GE1 duplicate check skipped"
fi
echo "IB3 close: OK $CLOSED"
