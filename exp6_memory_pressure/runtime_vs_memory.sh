#!/bin/bash
# -------------------------------------------------
# Experiment 6: Memory size sweep (starting 512 MB)
# Run 3 NUMA scenarios for increasing memory sizes
#
# X-axis: memory size (MB)
# Curves : bind / preferred / interleave
#
# Usage:
#   ./run_exp6_sizes_512MB.sh ./exp6_pressure_fallback
#
# Output:
#   results_exp6_sizes_512MB.csv
# -------------------------------------------------

PROG=$1
if [[ ! -x "$PROG" ]]; then
  echo "ERROR: Provide path to compiled exp6_pressure_fallback executable."
  exit 1
fi

# ---------------- CONFIG ----------------
THREADS=8
COMPUTE_NODE=0

# Memory sizes (MB) starting from 512 MB
SIZES_MB=(
  512
  1024
  2048
  4096
  8192
  16384
  32768
)

# Scenario IDs:
# 0 = bind_node0
# 1 = preferred_node0
# 2 = interleave_01
SCENARIOS=(0 1 2)

CSV_OUT="results_exp6_sizes_512MB.csv"

scenario_name() {
  case "$1" in
    0) echo "bind_node0" ;;
    1) echo "preferred_node0" ;;
    2) echo "interleave_01" ;;
    *) echo "unknown" ;;
  esac
}

# CSV header
echo "array_MB,threads,scenario_id,scenario_name,runtime_sec,exit_code" > "$CSV_OUT"

echo "=== Experiment 6: Memory Size Sweep ==="
echo "Program      : $PROG"
echo "Threads      : $THREADS"
echo "Compute node : $COMPUTE_NODE"
echo "Sizes (MB)   : ${SIZES_MB[*]}"
echo "Scenarios    : bind / preferred / interleave"
echo "Output CSV   : $CSV_OUT"
echo

# ---------------- RUN TESTS ----------------
for size in "${SIZES_MB[@]}"; do
  echo "=== Memory size: ${size} MB ==="

  for scen in "${SCENARIOS[@]}"; do
    name=$(scenario_name "$scen")
    echo -n "  Scenario ${scen} (${name}) ... "

    OUT=$("$PROG" "$size" "$THREADS" "$scen" "$COMPUTE_NODE" 2>&1)
    EC=$?

    TIME="NA"
    if [[ $EC -eq 0 ]]; then
      TIME=$(echo "$OUT" | awk '/Approx\. total time/ {print $(NF-1)}')
      [[ -z "$TIME" ]] && TIME="NA"
    fi

    echo "time=${TIME}, exit=${EC}"
    echo "${size},${THREADS},${scen},${name},${TIME},${EC}" >> "$CSV_OUT"
  done
  echo
done

echo "Done. Results written to: $CSV_OUT"
