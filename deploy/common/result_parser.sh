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

# Reads "key=value" from the last SUMMARY_* block of a log; prints $3 if absent.
summary_field() {
    local log="$1" key="$2" default="${3:-0}" value
    [ -f "$log" ] || { echo "$default"; return; }
    value=$(awk -v key="$key" '
        /^SUMMARY_(GEN|PROC)[ \t]/  { last = $0; inblk = 0; next }
        /^SUMMARY_(GEN|PROC)$/      { inblk = 1; blk = ""; next }
        /^END_SUMMARY_(GEN|PROC)$/  { if (inblk) { last = blk; inblk = 0 } next }
        inblk                       { blk = blk " " $0 }
        END {
            if (inblk && blk != "") last = blk
            n = split(last, tok, /[ \t]+/)
            for (i = n; i >= 1; i--) {
                if (tok[i] ~ ("^" key "=")) {
                    sub(("^" key "="), "", tok[i])
                    print tok[i]
                    exit
                }
            }
        }' "$log")
    echo "${value:-$default}"
}

# --- Generator ---
gen_min=$(summary_field "$GEN_LOG" min_us)
gen_max=$(summary_field "$GEN_LOG" max_us)
gen_load=$(summary_field "$GEN_LOG" load_pct)
gen_err=$(summary_field "$GEN_LOG" err_send)
gen_tx_packets=$(summary_field "$GEN_LOG" tx_packets)

# Achieved offered rate; compare against what the scenario asked for.
tx_pps_avg=0
if [ "${DURATION:-0}" -gt 0 ]; then
    tx_pps_avg=$(( gen_tx_packets / DURATION ))
fi

# --- Processor ---
proc_goose_total=$(summary_field "$PROC_LOG" goose_total)
proc_sv_total=$(summary_field "$PROC_LOG" sv_total)
proc_goose_err=$(summary_field "$PROC_LOG" goose_parse_err)
proc_sv_err=$(summary_field "$PROC_LOG" sv_parse_err)
proc_goose_unknown=$(summary_field "$PROC_LOG" goose_unknown)
proc_sv_unknown=$(summary_field "$PROC_LOG" sv_unknown)
proc_auth_fail=$(summary_field "$PROC_LOG" auth_fail)
goose_seq_err=$(summary_field "$PROC_LOG" goose_err_seq)
sv_smp_err=$(summary_field "$PROC_LOG" sv_err_smp)
goose_spdu_err=$(summary_field "$PROC_LOG" goose_err_spdu)
sv_spdu_err=$(summary_field "$PROC_LOG" sv_err_spdu)

proc_ring_full=$(summary_field "$PROC_LOG" ring_full)
proc_rx_packets=$(summary_field "$PROC_LOG" rx_packets)
proc_imissed=$(summary_field "$PROC_LOG" imissed)
proc_ierrors=$(summary_field "$PROC_LOG" ierrors)
proc_nombuf=$(summary_field "$PROC_LOG" rx_nombuf)
proc_qerrors=$(summary_field "$PROC_LOG" q_errors)

# --- Throughput and per-core load: still read from the periodic tables ---
proc_rx_pps=0; proc_rx_mbps=0; proc_max_us=0; proc_lcore_load=""

if [ -f "$PROC_LOG" ]; then
    proc_rx_pps=$(grep -E '^\s*PPS' "$PROC_LOG" | tail -1 | awk -F'|' '{gsub(/[^0-9]/, "", $2); print $2}')
    proc_rx_pps=${proc_rx_pps:-0}

    proc_rx_mbps=$(grep -E '^\s*Load' "$PROC_LOG" | tail -1 | awk -F'|' '{gsub(/[^0-9.]/, "", $2); print $2}')
    proc_rx_mbps=${proc_rx_mbps:-0}

    proc_max_us=$(grep -E '^\s*(Main|LCore)' "$PROC_LOG" | awk -F'|' '{gsub(/[^0-9.]/, "", $3); if ($3+0 > max) max=$3+0} END {print max+0}')
    proc_max_us=${proc_max_us:-0}

    # Per-lcore Load%: "<name> | min | max | load | wait |" -> {"name": load, ...}
    proc_lcore_load=$(grep -E '^\s*(Main|LCore)' "$PROC_LOG" | awk -F'|' '
        {
            name=$1; gsub(/^[ \t]+|[ \t]+$/, "", name);
            load=$4; gsub(/[^0-9.]/, "", load);
            printf "%s\"%s\": %s", (NR>1 ? ", " : ""), name, (load == "" ? "0" : load);
        }')
fi

# --- Pass/fail ---
pass=true
fail_reason=""

add_fail() {
    pass=false
    fail_reason="${fail_reason:+$fail_reason, }$1"
}

[ "${goose_seq_err:-0}" -gt 0 ]  && add_fail "goose_seq_err=$goose_seq_err"
[ "${sv_smp_err:-0}" -gt 0 ]     && add_fail "sv_smp_err=$sv_smp_err"
[ "${goose_spdu_err:-0}" -gt 0 ] && add_fail "goose_spdu_err=$goose_spdu_err"
[ "${sv_spdu_err:-0}" -gt 0 ]    && add_fail "sv_spdu_err=$sv_spdu_err"
[ "${proc_auth_fail:-0}" -gt 0 ] && add_fail "auth_fail=$proc_auth_fail"
[ "${proc_goose_err:-0}" -gt 0 ] && add_fail "goose_parse_err=$proc_goose_err"
[ "${proc_sv_err:-0}" -gt 0 ]    && add_fail "sv_parse_err=$proc_sv_err"
[ "${proc_ring_full:-0}" -gt 0 ] && add_fail "ring_full=$proc_ring_full"
[ "${proc_imissed:-0}" -gt 0 ]   && add_fail "imissed=$proc_imissed"
[ "${proc_ierrors:-0}" -gt 0 ]   && add_fail "ierrors=$proc_ierrors"
[ "${proc_nombuf:-0}" -gt 0 ]    && add_fail "rx_nombuf=$proc_nombuf"
[ "${proc_qerrors:-0}" -gt 0 ]   && add_fail "q_errors=$proc_qerrors"
[ "${gen_err:-0}" -gt 0 ]        && add_fail "gen_send_err=$gen_err"

if [ -n "$MAX_LATENCY" ] && [ "$MAX_LATENCY" -gt 0 ]; then
    if awk "BEGIN {exit !($proc_max_us > $MAX_LATENCY)}"; then
        add_fail "max_us=${proc_max_us}>${MAX_LATENCY}"
    fi
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
    "err_send": $gen_err,
    "tx_packets": $gen_tx_packets,
    "tx_pps_avg": $tx_pps_avg
  },
  "processor": {
    "rx_pps": $proc_rx_pps,
    "rx_mbps": $proc_rx_mbps,
    "rx_packets": $proc_rx_packets,
    "goose_total": $proc_goose_total,
    "sv_total": $proc_sv_total,
    "goose_parse_err": $proc_goose_err,
    "sv_parse_err": $proc_sv_err,
    "goose_unknown": $proc_goose_unknown,
    "sv_unknown": $proc_sv_unknown,
    "auth_fail": $proc_auth_fail,
    "ring_full": ${proc_ring_full:-0},
    "imissed": $proc_imissed,
    "ierrors": $proc_ierrors,
    "rx_nombuf": $proc_nombuf,
    "q_errors": $proc_qerrors,
    "max_us": $proc_max_us,
    "lcore_load_pct": { $proc_lcore_load }
  },
  "streams": {
    "goose_err_seq_total": $goose_seq_err,
    "sv_err_smp_total": $sv_smp_err,
    "goose_err_spdu_total": $goose_spdu_err,
    "sv_err_spdu_total": $sv_spdu_err
  },
  "pass": $pass,
  "fail_reason": "$fail_reason"
}
EOF
