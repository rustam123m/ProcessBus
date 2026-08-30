#!/bin/bash
# Captures the i225 link/PCIe state that explains RX drops the application
# cannot account for: ASPM and L1 substates, PCIe link width/speed, AER error
# counters. Read-only; changes nothing. No arguments. The NIC is bound to
# uio_pci_generic here, so there is no netdev and no ethtool.

NIC_PCI_ADDR="${NIC_PCI_ADDR:-0000:01:00.0}"
D="/sys/bus/pci/devices/$NIC_PCI_ADDR"

echo "=== device ==="
lspci -s "${NIC_PCI_ADDR#0000:}" -nn
echo "  driver: $(basename "$(readlink -f "$D/driver" 2>/dev/null)" 2>/dev/null)"

echo "=== PCIe link (LnkCap advertised vs LnkSta negotiated) ==="
lspci -s "${NIC_PCI_ADDR#0000:}" -vv 2>/dev/null |
    grep -E "LnkCap:|LnkSta:|LnkCtl:|ASPM|L1SubCtl|DevCtl:|MaxPayload|MaxReadReq"

echo "=== ASPM policy ==="
cat /sys/module/pcie_aspm/parameters/policy 2>/dev/null || echo "  (pcie_aspm not exposed)"

echo "=== upstream bridge ==="
BR=$(basename "$(dirname "$(readlink -f "$D")")")
echo "  bridge: $BR"
lspci -s "${BR#0000:}" -vv 2>/dev/null | grep -E "LnkCap:|LnkSta:|ASPM|L1SubCtl"

echo "=== AER / correctable errors ==="
for f in "$D"/aer_dev_correctable "$D"/aer_dev_fatal "$D"/aer_dev_nonfatal; do
    [ -r "$f" ] && { echo "-- $(basename "$f")"; cat "$f"; }
done

echo "=== kernel messages for this device ==="
dmesg 2>/dev/null | grep -iE "igc|${NIC_PCI_ADDR}|aer|pcieport" | tail -30
