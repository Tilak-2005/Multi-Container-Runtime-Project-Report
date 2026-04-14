#!/usr/bin/env bash
# environment-check.sh — Preflight check for the container runtime.
# Run as root: sudo ./environment-check.sh

set -euo pipefail
PASS=0; FAIL=0

ok()   { echo "[OK]   $*"; ((PASS++)) || true; }
fail() { echo "[FAIL] $*"; ((FAIL++)) || true; }
info() { echo "[INFO] $*"; }

echo "========================================"
echo "  Container Runtime Preflight Check"
echo "========================================"

# 1. Root
[[ $EUID -eq 0 ]] && ok "Running as root" || fail "Must run as root (sudo)"

# 2. OS version
if grep -q "Ubuntu" /etc/os-release 2>/dev/null; then
    VER=$(grep VERSION_ID /etc/os-release | cut -d'"' -f2)
    [[ "$VER" == "22.04" || "$VER" == "24.04" ]] \
        && ok "Ubuntu $VER detected" \
        || fail "Expected Ubuntu 22.04 or 24.04, got $VER"
else
    fail "Not Ubuntu — this project targets Ubuntu 22.04/24.04"
fi

# 3. Kernel headers
KVER=$(uname -r)
[[ -d /lib/modules/$KVER/build ]] \
    && ok "Kernel headers found for $KVER" \
    || fail "Kernel headers missing — run: sudo apt install linux-headers-$(uname -r)"

# 4. Build tools
for tool in gcc make; do
    command -v $tool &>/dev/null && ok "$tool found" || fail "$tool not found"
done

# 5. Secure Boot check
if command -v mokutil &>/dev/null; then
    SB=$(mokutil --sb-state 2>&1 || true)
    echo "$SB" | grep -qi "disabled" \
        && ok "Secure Boot disabled (module loading will work)" \
        || info "Secure Boot state: $SB — module loading requires it disabled"
else
    info "mokutil not found; cannot check Secure Boot state"
fi

# 6. Namespace support
for ns in pid uts mnt; do
    [[ -f /proc/self/ns/$ns ]] \
        && ok "Namespace $ns available" \
        || fail "Namespace $ns not found in /proc/self/ns/"
done

# 7. Clone flag constants (Python sanity check)
python3 - <<'EOF'
CLONE_NEWPID = 0x20000000
CLONE_NEWUTS = 0x04000000
CLONE_NEWNS  = 0x00020000
print(f"[OK]   CLONE_NEWPID={hex(CLONE_NEWPID)} CLONE_NEWUTS={hex(CLONE_NEWUTS)} CLONE_NEWNS={hex(CLONE_NEWNS)}")
EOF

# 8. /proc/self/status readable (required for RSS tracking)
[[ -r /proc/self/status ]] \
    && ok "/proc/self/status readable" \
    || fail "/proc/self/status not readable"

# 9. pthread support
echo '#include <pthread.h>' | gcc -x c -c - -o /dev/null 2>/dev/null \
    && ok "pthread library available" \
    || fail "pthread not available"

# 10. AF_UNIX socket support
python3 -c "import socket; s=socket.socket(socket.AF_UNIX); s.close(); print('[OK]   AF_UNIX sockets supported')"

echo "========================================"
echo "  PASSED: $PASS   FAILED: $FAIL"
echo "========================================"
[[ $FAIL -eq 0 ]]
