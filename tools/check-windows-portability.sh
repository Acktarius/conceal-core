#!/usr/bin/env bash
# Audit Windows/MSVC portability issues before CI finds them one-by-one.
#
# Usage:
#   ./tools/check-windows-portability.sh
#
# Strategy:
#   - Flag sources that include POSIX headers WITHOUT any _WIN32 guard in the file
#   - Flag raw send/recv passing non-char buffers (MSVC C2664)
#   - Run C++11 drift scan (check-cpp11-drift.sh)

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC="${ROOT}/src"
TESTS="${ROOT}/tests"
FAILED=0

if ! command -v rg >/dev/null 2>&1; then
  echo "ERROR: ripgrep (rg) is required." >&2
  exit 1
fi

echo "== Windows portability audit =="
echo

hasPlatformGuard() {
  rg -q '#ifdef _WIN32|#if defined\(_WIN32\)|#ifndef _WIN32|#if defined WIN32|#ifdef __linux__|#if defined __linux__|#ifdef CONCEAL_WITH_OPENCL' "$1"
}

POSIX_HEADERS='#include <(sys/socket\.h|sys/epoll\.h|sys/wait\.h|unistd\.h|dirent\.h|arpa/inet\.h|netinet/in\.h|fcntl\.h|poll\.h|resolv\.h)>'

RG_SCOPE=(
  --glob '!**/Platform/Linux/**'
  --glob '!**/Platform/OSX/**'
  --glob '!**/BoltHttpServer.cpp'
  --glob '!**/BoltSocket.hpp'
  --glob '!**/external/**'
)

echo "-- POSIX headers in files with NO _WIN32 guard --"
while IFS= read -r file; do
  [[ -z "$file" ]] && continue
  if hasPlatformGuard "$file"; then
    continue
  fi
  echo "FAIL [unguarded-posix] $file"
  rg -n "$POSIX_HEADERS" "$file" || true
  echo
  FAILED=1
done < <(rg -l "$POSIX_HEADERS" "${RG_SCOPE[@]}" "$SRC" "$TESTS" 2>/dev/null || true)

echo "-- Raw send/recv without char* cast (MSVC C2664 risk) --"
# Exclude BoltSocket and files using reinterpret_cast/static_cast<char>
while IFS= read -r line; do
  [[ -z "$line" ]] && continue
  file="${line%%:*}"
  if rg -q 'reinterpret_cast<\s*const char|static_cast<\s*const char|reinterpret_cast<\s*char|static_cast<\s*char|boltSend|boltRecv|sendBytes|recvBytes' "$file" 2>/dev/null; then
    continue
  fi
  echo "FAIL [raw-send-recv] $line"
  FAILED=1
done < <(rg -n '(^|[^a-zA-Z])(send|recv)\([^;]*(,&|\.data\(\))' "${RG_SCOPE[@]}" "$SRC" "$TESTS" 2>/dev/null || true)

if [[ $FAILED -eq 0 ]]; then
  echo "OK: no actionable Windows portability issues found."
else
  echo
  echo "Guidance:"
  echo "  - Sockets: use BoltHttp/BoltSocket.hpp (boltSend/boltRecv with char* casts)"
  echo "  - Directory listing: boost::filesystem"
  echo "  - Process/fork: guard with #ifndef _WIN32 or exclude target on Windows"
fi

echo
if [[ -x "${ROOT}/tools/check-cpp11-drift.sh" ]]; then
  echo "-- C++11 drift (nested check) --"
  if ! "${ROOT}/tools/check-cpp11-drift.sh"; then
    FAILED=1
  fi
fi

exit "$FAILED"
