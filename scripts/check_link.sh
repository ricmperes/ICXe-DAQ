#!/bin/bash
# Read-only probe of the V1730SB optical link on this PC.
# Does not load kernel modules and does not need root.

set -u

pass=0
fail=0

ok() {
    printf '[PASS] %s\n' "$1"
    pass=$((pass + 1))
}

bad() {
    printf '[FAIL] %s\n' "$1"
    fail=$((fail + 1))
}

check_lib() {
    local label="$1"
    shift
    local name line
    for name in "$@"; do
        line=$(ldconfig -p 2>/dev/null | grep -F "$name" | head -n 1 || true)
        if [[ -n "$line" ]]; then
            line=${line#"${line%%[![:space:]]*}"}
            ok "${label}: ${line}"
            return 0
        fi
    done
    bad "${label}: not in ldconfig cache ($(printf '%s ' "$@"))"
    return 0
}

echo "ICXe DAQ link check"
echo "Running kernel: $(uname -r)"
echo

pci=$(lspci -nn -d 202c:5818 2>/dev/null || true)
if [[ -n "$pci" ]]; then
    ok "A5818 present: ${pci}"
else
    bad "lspci has no CAEN A5818 [202c:5818]"
fi

check_lib "libCAENVME" "libCAENVME.so.v4.0.2" "libCAENVME.so.4.0.2"
check_lib "libCAENComm" "libCAENComm.so.v1.7.0" "libCAENComm.so.1.7.0"
check_lib "libCAENDigitizer" "libCAENDigitizer.so.v2.18.0" "libCAENDigitizer.so.2.18.0"

missing=()
for header in CAENDigitizer.h CAENComm.h CAENVMElib.h; do
    if [[ ! -f "/usr/include/${header}" ]]; then
        missing+=("${header}")
    fi
done
if [[ ${#missing[@]} -eq 0 ]]; then
    ok "headers in /usr/include: CAENDigitizer.h CAENComm.h CAENVMElib.h"
else
    bad "missing /usr/include headers: ${missing[*]}"
fi

running=$(uname -r)
vermagic=$(modinfo -F vermagic a5818 2>/dev/null || true)
mod_rel=${vermagic%% *}
if [[ -z "$vermagic" ]]; then
    bad "modinfo a5818 returned no vermagic"
elif [[ "$mod_rel" == "$running" ]]; then
    ok "vermagic matches running kernel (${mod_rel})"
else
    bad "vermagic ${mod_rel} does not match running kernel ${running}"
fi

if lsmod | awk '$1=="a5818"{found=1} END{exit found?0:1}'; then
    ok "a5818 module is loaded"
else
    bad "a5818 module is not loaded"
fi

shopt -s nullglob
xdma=(/dev/xdma*)
a5818_nodes=(/dev/a5818*)
shopt -u nullglob
# Driver 1.0.5 creates /dev/a5818*. /dev/xdma* is an optional legacy prefix.
if [[ ${#a5818_nodes[@]} -gt 0 ]]; then
    ok "/dev/a5818* exists: ${a5818_nodes[*]}"
elif [[ ${#xdma[@]} -eq 0 ]]; then
    bad "neither /dev/a5818* nor /dev/xdma* exists"
else
    bad "/dev/a5818* does not exist"
fi

echo
if [[ ${#xdma[@]} -gt 0 ]]; then
    echo "Note: optional legacy /dev/xdma* present: ${xdma[*]}"
else
    echo "Note: optional legacy /dev/xdma* absent (driver 1.0.5 uses /dev/a5818*)"
fi
echo
echo "Summary: ${pass} passed, ${fail} failed"
if [[ "$fail" -eq 0 ]]; then
    echo "OVERALL: PASS"
    exit 0
fi
echo "OVERALL: FAIL"
exit 1
