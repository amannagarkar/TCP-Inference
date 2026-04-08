#!/usr/bin/env python3
"""
simulate_demo.py — Generate realistic synthetic experiment data and produce
all plots without needing the physical testbed.

Models 802.11n power-save behaviour:
  - PS off : RTT ≈ proc_delay + 2*tx_time (Gaussian noise)
  - PS on  : RTT = PS-off baseline + wake-up delay
             wake-up delay ~ mixture of:
               * "already awake" (small delay, ~5% of DTIM interval)
               * "sleeping"      (uniform in [0, DTIM_ms])  DTIM = 2 × 100 ms

Usage:
  python3 simulate_demo.py [--out-dir results]
"""

import os
import csv
import argparse
import numpy as np

# ── 802.11n / Jetson testbed parameters ──────────────────────────────────────
BEACON_INTERVAL_MS = 100.0          # 802.11n default
DTIM_PERIOD        = 2              # DTIM every 2 beacons → 200 ms max delay
DTIM_MS            = BEACON_INTERVAL_MS * DTIM_PERIOD

# Fraction of messages where client wakes before next beacon (already active)
P_AWAKE            = 0.30

# Transmission overhead per byte at 54 Mbps effective (802.11n, conservative)
BYTES_PER_MS       = 54e6 / 8 / 1000   # ≈ 6750 bytes/ms

RNG = np.random.default_rng(42)


def tx_time_ms(size_bytes: int) -> float:
    """One-way transmission time for payload + headers."""
    headers = 66    # TCP/IP/WiFi headers ≈ 66 bytes
    return (size_bytes + headers) / BYTES_PER_MS


def wakeup_delay_ms() -> float:
    """Sample one wake-up delay event."""
    if RNG.random() < P_AWAKE:
        # already awake — small jitter
        return abs(RNG.normal(0, 3.0))
    else:
        # sleeping — uniformly distributed within DTIM window
        return RNG.uniform(0, DTIM_MS)


def simulate_run(
    server_ip, port, msg_sizes, num_messages, inter_gap_ms,
    proc_delay_ms, power_save, iface, run_id, out_path,
):
    os.makedirs(os.path.dirname(out_path) if os.path.dirname(out_path) else ".", exist_ok=True)

    # build shuffled size sequence
    per = max(1, num_messages // len(msg_sizes))
    seq = []
    for s in msg_sizes:
        seq.extend([s] * per)
    seq = seq[:num_messages]
    RNG.shuffle(seq)

    rows = []
    t_wall = 1700000000.0      # synthetic epoch start
    for i, sz in enumerate(seq):
        tx = tx_time_ms(sz)
        base_rtt = proc_delay_ms + 2 * tx + abs(RNG.normal(0, 0.8))   # baseline noise

        if power_save:
            wu = wakeup_delay_ms()
            rtt = base_rtt + wu
        else:
            rtt = base_rtt

        rows.append({
            "run_id":               run_id,
            "msg_index":            i,
            "msg_size_bytes":       sz,
            "rtt_ms":               round(rtt, 4),
            "t_send":               round(t_wall, 9),
            "t_recv":               round(t_wall + rtt / 1000.0, 9),
            "power_save":           int(power_save),
            "ps_set_ok":            1,
            "processing_delay_ms":  proc_delay_ms,
            "iface":                iface,
            "inter_gap_ms":         inter_gap_ms,
        })
        t_wall += rtt / 1000.0 + inter_gap_ms / 1000.0

    fieldnames = ["run_id", "msg_index", "msg_size_bytes", "rtt_ms",
                  "t_send", "t_recv", "power_save", "ps_set_ok",
                  "processing_delay_ms", "iface", "inter_gap_ms"]
    with open(out_path, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=fieldnames)
        w.writeheader()
        w.writerows(rows)

    print(f"  {run_id}  → {out_path}")
    return rows


def main():
    parser = argparse.ArgumentParser(description="Generate synthetic experiment data")
    parser.add_argument("--out-dir",      default="results",     help="Results root dir")
    parser.add_argument("--num-messages", type=int, default=300)
    parser.add_argument("--server",       default="192.168.1.100")
    parser.add_argument("--port",         type=int, default=5001)
    parser.add_argument("--iface",        default="wlan0")
    args = parser.parse_args()

    csv_dir = os.path.join(args.out_dir, "csv")
    os.makedirs(csv_dir, exist_ok=True)
    os.makedirs(os.path.join(args.out_dir, "plots"), exist_ok=True)

    PROC_DELAYS  = [5, 10, 50, 100]
    PS_MODES     = [False, True]
    INTER_GAPS   = [0, 20, 100]
    MSG_SIZES    = [64, 256, 1024, 4096, 16384]

    print(f"Generating synthetic data → {csv_dir}/")
    manifest_rows = []

    for proc_delay in PROC_DELAYS:
        for ps in PS_MODES:
            for gap in INTER_GAPS:
                ps_str = "on" if ps else "off"
                run_id = f"sim_pd{proc_delay}_ps{ps_str}_gap{gap}"
                out_path = os.path.join(csv_dir, f"{run_id}.csv")
                simulate_run(
                    server_ip=args.server,
                    port=args.port,
                    msg_sizes=MSG_SIZES,
                    num_messages=args.num_messages,
                    inter_gap_ms=gap,
                    proc_delay_ms=proc_delay,
                    power_save=ps,
                    iface=args.iface,
                    run_id=run_id,
                    out_path=out_path,
                )
                manifest_rows.append({
                    "run_id":        run_id,
                    "csv_path":      out_path,
                    "proc_delay_ms": proc_delay,
                    "power_save":    ps_str,
                    "inter_gap_ms":  gap,
                    "n_messages":    args.num_messages,
                    "msg_sizes":     ";".join(str(s) for s in MSG_SIZES),
                    "success":       1,
                    "timestamp":     "simulated",
                })

    # write manifest
    manifest_path = os.path.join(args.out_dir, "manifest.csv")
    fields = ["run_id", "csv_path", "proc_delay_ms", "power_save",
              "inter_gap_ms", "n_messages", "msg_sizes", "success", "timestamp"]
    with open(manifest_path, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=fields)
        w.writeheader()
        w.writerows(manifest_rows)

    print(f"\nManifest: {manifest_path}")
    print(f"Total runs: {len(manifest_rows)}")
    print(f"\nNow run:")
    print(f"  python3 analyze_plot.py --results-dir {args.out_dir}")


if __name__ == "__main__":
    main()
