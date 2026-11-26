#!/bin/sh
DIAG_DIR="/var/log/boot-diag"
mkdir -p "$DIAG_DIR"

for item in dmesg clk_summary regulator_summary pm_genpd_summary dri-state; do
    if [ -f "$DIAG_DIR/last-$item.log" ]; then
        mv "$DIAG_DIR/last-$item.log" "$DIAG_DIR/prev-$item.log"
    fi
done

dmesg > "$DIAG_DIR/last-dmesg.log" 2>&1 || true
[ -r /sys/kernel/debug/clk/clk_summary ] && cat /sys/kernel/debug/clk/clk_summary > "$DIAG_DIR/last-clk_summary.log" 2>&1 || true
[ -r /sys/kernel/debug/regulator/regulator_summary ] && cat /sys/kernel/debug/regulator/regulator_summary > "$DIAG_DIR/last-regulator_summary.log" 2>&1 || true
[ -r /sys/kernel/debug/pm_genpd/pm_genpd_summary ] && cat /sys/kernel/debug/pm_genpd/pm_genpd_summary > "$DIAG_DIR/last-pm_genpd_summary.log" 2>&1 || true
[ -r /sys/kernel/debug/dri/0/state ] && cat /sys/kernel/debug/dri/0/state > "$DIAG_DIR/last-dri-state.log" 2>&1 || true
sync
