#!/bin/sh
# Archive helper for the Ninja build (see n3ds_port/CMakeLists.txt): n3ds_ar.sh <ar> <library> <objects...>
# The object list goes into a response file, so the native ar.exe gets a short command line (Windows limit ~32 KB).
# Absolute MSYS paths (/home/..., /opt/...) are turned into Windows paths: a native program reading @file does not translate them.
AR="$1"; LIB="$2"; shift 2
RSP="$LIB.objects.rsp"
: > "$RSP" || exit 1
HAVE_CYGPATH=0; command -v cygpath >/dev/null 2>&1 && HAVE_CYGPATH=1
for o in "$@"; do
  case "$o" in /*) [ $HAVE_CYGPATH = 1 ] && o="$(cygpath -m "$o")" ;; esac
  printf '"%s"\n' "$o" >> "$RSP"
done
"$AR" qc "$LIB" "@$RSP"
