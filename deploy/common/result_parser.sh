#!/bin/bash
# Parses generator and processor logs into a JSON summary.
#
# Usage: result_parser.sh <generator.log> <processor.log> <scenario_name> <duration> <max_latency_us>
# Output: JSON to stdout

GEN_LOG="$1"
PROC_LOG="$2"
SCENARIO="$3"
DURATION="$4"
MAX_LATENCY="$5"
TIMESTAMP=$(date -Iseconds)

# --- Generator parsing ---
gen_min=0; gen_max=0; gen_load=0; gen_err=0

if [ -f "$GEN_LOG" ]; then
    # Parse CyclicStat table: "Main" | min | max | load | wait
    gen_line=$(grep -E '^\s*Main\s' "$GEN_LOG" | tail -1)
    if [ -n "$gen_line" ]; then
        gen_min=$(echo "$gen_line" | awk -F'|' '{gsub(/[^0-9.]/, "", $3); print $3}')
        gen_max=$(echo "$gen_line" | awk -F'|' '{gsub(/[^0-9.]/, "", $4); print $4}')
        gen_load=$(echo "$gen_line" | awk -F'|' '{gsub(/[^0-9.]/, "", $5); print $5}')
    fi
    # Send errors from generator (No-mbuf column)
    gen_err=$(grep -oP 'No-mbuf\s*\|\s*\K[0-9]+' "$GEN_LOG" | tail -1)
    gen_err=${gen_err:-0}
fi

# --- Processor parsing ---
proc_rx_pps=0; proc_rx_mbps=0
proc_goose_total=0; proc_sv_total=0
proc_goose_err=0; proc_sv_err=0
proc_goose_unknown=0; proc_sv_unknown=0
proc_max_us=0

if [ -f "$PROC_LOG" ]; then
    # PPS
    proc_rx_pps=$(grep -E '^\s*PPS' "$PROC_LOG" | tail -1 | awk -F'|' '{gsub(/[^0-9]/, "", $2); print $2}')
    proc_rx_pps=${proc_rx_pps:-0}

    # Load Mbps
    proc_rx_mbps=$(grep -E '^\s*Load' "$PROC_LOG" | tail -1 | awk -F'|' '{gsub(/[^0-9.]/, "", $2); print $2}')
    proc_rx_mbps=${proc_rx_mbps:-0}

    # Protocol totals
    proc_goose_total=$(grep -E '^\s*Total' "$PROC_LOG" | tail -1 | awk -F'|' '{gsub(/[^0-9]/, "", $2); print $2}')
    proc_goose_total=${proc_goose_total:-0}
    proc_sv_total=$(grep -E '^\s*Total' "$PROC_LOG" | tail -1 | awk -F'|' '{gsub(/[^0-9]/, "", $3); print $3}')
    proc_sv_total=${proc_sv_total:-0}

    # Parse errors
    proc_goose_err=$(grep -E '^\s*Error' "$PROC_LOG" | tail -1 | awk -F'|' '{gsub(/[^0-9]/, "", $2); print $2}')
    proc_goose_err=${proc_goose_err:-0}
    proc_sv_err=$(grep -E '^\s*Error' "$PROC_LOG" | tail -1 | awk -F'|' '{gsub(/[^0-9]/, "", $3); print $3}')
    proc_sv_err=${proc_sv_err:-0}

    # CyclicStat — max latency across all cores
    proc_max_us=$(grep -E '^\s*(Main|LCore)' "$PROC_LOG" | awk -F'|' '{gsub(/[^0-9.]/, "", $4); if ($4+0 > max) max=$4+0} END {print max}')
    proc_max_us=${proc_max_us:-0}

    # Per-stream sequence errors (GOOSE errSeqCnt, SV errSmpCnt)
    # These appear in the final results table — sum the error column
    goose_seq_err=$(grep -E '^\s*\|.*GOID' "$PROC_LOG" | awk -F'|' '{sum += $NF+0} END {print sum+0}')
    sv_smp_err=$(grep -E '^\s*\|.*SVID' "$PROC_LOG" | awk -F'|' '{sum += $NF+0} END {print sum+0}')
fi

goose_seq_err=${goose_seq_err:-0}
sv_smp_err=${sv_smp_err:-0}

# --- Pass/fail ---
pass=true
fail_reason=""

if [ "$goose_seq_err" -gt 0 ]; then
    pass=false
    fail_reason="goose_seq_err=$goose_seq_err"
fi
if [ "$sv_smp_err" -gt 0 ]; then
    pass=false
    fail_reason="${fail_reason:+$fail_reason, }sv_smp_err=$sv_smp_err"
fi
if [ -n "$MAX_LATENCY" ] && [ "$MAX_LATENCY" -gt 0 ]; then
    if awk "BEGIN {exit !($proc_max_us > $MAX_LATENCY)}"; then
        pass=false
        fail_reason="${fail_reason:+$fail_reason, }max_us=${proc_max_us}>${MAX_LATENCY}"
    fi
fi
if [ "$gen_err" -gt 0 ]; then
    pass=false
    fail_reason="${fail_reason:+$fail_reason, }gen_send_err=$gen_err"
fi

# --- JSON output ---
cat <<EOF
{
  "scenario": "$SCENARIO",
  "timestamp": "$TIMESTAMP",
  "duration_sec": $DURATION,
  "generator": {
    "min_us": $gen_min,
    "max_us": $gen_max,
    "load_pct": $gen_load,
    "err_send": $gen_err
  },
  "processor": {
    "rx_pps": $proc_rx_pps,
    "rx_mbps": $proc_rx_mbps,
    "goose_total": $proc_goose_total,
    "sv_total": $proc_sv_total,
    "goose_parse_err": $proc_goose_err,
    "sv_parse_err": $proc_sv_err,
    "max_us": $proc_max_us
  },
  "streams": {
    "goose_err_seq_total": $goose_seq_err,
    "sv_err_smp_total": $sv_smp_err
  },
  "pass": $pass,
  "fail_reason": "$fail_reason"
}
EOF
