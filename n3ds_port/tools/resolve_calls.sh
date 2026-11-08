#!/bin/bash
# Turn the FIRST "[stack]" dump of log.txt (taken 10 s into a stall) into function names, one thread at a time.
#   n3ds_port/tools/resolve_calls.sh /c/path/to/log.txt build-3ds/nectar_3ds.elf
# Frame #0 is the outermost call. Needs devkitARM's addr2line (added to PATH below).
# Also works for a few loose addresses:  arm-none-eabi-addr2line -Cfe nectar_3ds.elf 0x2317a0 0x232a5c
LOG="${1:?usage: resolve_calls.sh log.txt nectar_3ds.elf}"; ELF="${2:-build-3ds/nectar_3ds.elf}"
export PATH="$PATH:/opt/devkitpro/devkitARM/bin"
mapfile -t LINES < <(awk '/^\[stack\] begin/{n++} n==1 && /^\[stack\] (T|  #)/' "$LOG" | tr -d '\r')
[ ${#LINES[@]} -gt 0 ] || { echo "no [stack] lines in $LOG (build with -DN3DS_TRACE_CALLS=ON?)"; exit 1; }
ADDRS=(); IDX=()
for i in "${!LINES[@]}"; do
  case "${LINES[$i]}" in
    "[stack]   #"*) ADDRS+=("0x$(echo "${LINES[$i]}" | awk '{print $3}')"); IDX+=("$i");;
  esac
done
mapfile -t FN < <(arm-none-eabi-addr2line -Cf -e "$ELF" "${ADDRS[@]}" | paste - -)
k=0
for i in "${!LINES[@]}"; do
  case "${LINES[$i]}" in
    "[stack]   #"*) echo "  $(echo "${LINES[$i]}" | awk '{print $2}') ${LINES[$i]##* }   ${FN[$k]//$'\t'/   }"; k=$((k+1));;
    *) echo "${LINES[$i]#\[stack\] }";;
  esac
done
