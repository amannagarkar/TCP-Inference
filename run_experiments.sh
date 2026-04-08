#!/usr/bin/env bash
# run_experiments.sh — parameter sweep for the WiFi PS RTT testbed
#
# Run this ON THE CLIENT JETSON after starting server on the server Jetson.
#
# Usage:
#   ./run_experiments.sh [--quick] [--server <ip>] [--iface <iface>]
#
# Parameter sweep:
#   msg_sizes     : 64,256,1024,4096,16384 bytes
#   proc_delays   : 5, 10, 50, 100 ms
#   power_save    : on, off
#   inter_gap_ms  : 0, 20, 100 ms
#
# The server must already be running at the correct --delay for each group.
# The script pauses before each new proc_delay group so you can restart the server.

set -euo pipefail

# ── testbed config — edit for your setup ────────────────────────────────────
SERVER_IP="192.168.1.100"
PORT=5001
IFACE="wlan0"
N_MESSAGES=300
RESULTS_DIR="results"
CLIENT_BIN="./client"
# ────────────────────────────────────────────────────────────────────────────

QUICK=0
for arg in "$@"; do
    case "$arg" in
        --quick)          QUICK=1 ;;
        --server=*)       SERVER_IP="${arg#*=}" ;;
        --iface=*)        IFACE="${arg#*=}" ;;
        --results-dir=*)  RESULTS_DIR="${arg#*=}" ;;
    esac
done

if [[ $QUICK -eq 1 ]]; then
    PROC_DELAYS=(10)
    PS_MODES=("off" "on")
    INTER_GAPS=(0 100)
    MSG_SIZES="64,1024,4096"
    N_MESSAGES=100
else
    PROC_DELAYS=(5 10 50 100)
    PS_MODES=("off" "on")
    INTER_GAPS=(0 20 100)
    MSG_SIZES="64,256,1024,4096,16384"
fi

CSV_DIR="$RESULTS_DIR/csv"
mkdir -p "$CSV_DIR" "$RESULTS_DIR/plots"

MANIFEST="$RESULTS_DIR/manifest.csv"
echo "run_id,csv_path,proc_delay_ms,power_save,inter_gap_ms,n_messages,msg_sizes,success,timestamp" \
    > "$MANIFEST"

total_runs=$(( ${#PROC_DELAYS[@]} * ${#PS_MODES[@]} * ${#INTER_GAPS[@]} ))
echo "======================================================"
echo " TCP Power-Save RTT Experiment"
echo " Server : $SERVER_IP:$PORT"
echo " Iface  : $IFACE"
echo " Runs   : $total_runs"
echo "======================================================"

current_delay=""
run_num=0

for PROC_DELAY in "${PROC_DELAYS[@]}"; do
    if [[ "$PROC_DELAY" != "$current_delay" ]]; then
        current_delay="$PROC_DELAY"
        echo ""
        echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
        echo "  Next group: processing_delay = ${PROC_DELAY} ms"
        echo "  Please (re)start server with:"
        echo "    ./server --port $PORT --delay $PROC_DELAY --log $RESULTS_DIR/server_pd${PROC_DELAY}.csv"
        echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
        read -rp "  Press Enter when server is ready..."
    fi

    for PS in "${PS_MODES[@]}"; do
        for GAP in "${INTER_GAPS[@]}"; do
            run_num=$(( run_num + 1 ))
            TS=$(date +%Y%m%d_%H%M%S)
            RUN_ID="pd${PROC_DELAY}_ps${PS}_gap${GAP}_${TS}"
            OUT="$CSV_DIR/${RUN_ID}.csv"

            echo ""
            echo "  ── Run $run_num / $total_runs ──"
            echo "     PS=$PS  delay=${PROC_DELAY}ms  gap=${GAP}ms  id=$RUN_ID"

            SUCCESS=0
            if $CLIENT_BIN \
                    --server     "$SERVER_IP"  \
                    --port       "$PORT"       \
                    --msg-sizes  "$MSG_SIZES"  \
                    --num-messages "$N_MESSAGES" \
                    --inter-gap-ms "$GAP"      \
                    --processing-delay "$PROC_DELAY" \
                    --power-save "$PS"         \
                    --iface      "$IFACE"      \
                    --output     "$OUT"        \
                    --run-id     "$RUN_ID"; then
                SUCCESS=1
            fi

            echo "${RUN_ID},${OUT},${PROC_DELAY},${PS},${GAP},${N_MESSAGES},${MSG_SIZES},${SUCCESS},${TS}" \
                >> "$MANIFEST"

            # brief cooldown to let WiFi state settle
            sleep 2
        done
    done
done

echo ""
echo "======================================================"
echo " All $run_num runs complete."
echo " Manifest : $MANIFEST"
echo " Run analysis:"
echo "   python3 analyze_plot.py --results-dir $RESULTS_DIR"
echo "======================================================"
