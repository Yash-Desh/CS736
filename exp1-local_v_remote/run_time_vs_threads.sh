#!/bin/bash

# -------------------------------------------
#  Experiment: Time vs Number of Threads
#  Fixed array size, sweep thread count.
#
#  Usage:
#      ./run_time_vs_threads.sh ./exp1_local_vs_remote
#
#  Output:
#      results_time_vs_threads.csv
# -------------------------------------------

PROG=$1
if [[ ! -x "$PROG" ]]; then
    echo "ERROR: Provide path to compiled exp1_local_vs_remote program."
    echo "Example: ./run_time_vs_threads.sh ./exp1_local_vs_remote"
    exit 1
fi

# -------------------------
#  CONFIGURATION PARAMETERS
# -------------------------

ARRAY_MB=8192                # fixed array size (8 GB here, change if needed)
COMPUTE_NODE=0               # run all threads on node 0

# Sweep of thread counts to test.
# Adjust depending on how many cores are on node 0.
THREAD_LIST=(1 2 4 8 16 32)

CSV_OUT="results_time_vs_threads.csv"

echo "Experiment: Time vs Threads (array = ${ARRAY_MB} MB)"
echo "Thread counts: ${THREAD_LIST[*]}"
echo "Writing to: $CSV_OUT"
echo

# CSV header
echo "Threads,Placement,Runtime_sec" > "$CSV_OUT"


# -------------------------
#  MAIN EXPERIMENT LOOP
# -------------------------
for T in "${THREAD_LIST[@]}"; do
    echo "=== Threads = $T ==="

    # -------------------------
    # LOCAL CASE (mem=0 -> compute=0)
    # -------------------------
    echo -n "  Local  (mem=0 -> compute=0) ... "
    OUT=$("$PROG" "$ARRAY_MB" "$T" 0 "$COMPUTE_NODE")

    TIME_LOCAL=$(echo "$OUT" | awk '/Approx\. total time/ {print $(NF-1)}')
    if [[ -z "$TIME_LOCAL" ]]; then
        echo "ERROR: could not parse time (local). Output was:"
        echo "$OUT"
        exit 1
    fi
    echo "$TIME_LOCAL sec"
    echo "$T,local,$TIME_LOCAL" >> "$CSV_OUT"


    # -------------------------
    # REMOTE CASE (mem=1 -> compute=0)
    # -------------------------
    echo -n "  Remote (mem=1 -> compute=0) ... "
    OUT=$("$PROG" "$ARRAY_MB" "$T" 1 "$COMPUTE_NODE")

    TIME_REMOTE=$(echo "$OUT" | awk '/Approx\. total time/ {print $(NF-1)}')
    if [[ -z "$TIME_REMOTE" ]]; then
        echo "ERROR: could not parse time (remote). Output was:"
        echo "$OUT"
        exit 1
    fi
    echo "$TIME_REMOTE sec"
    echo "$T,remote,$TIME_REMOTE" >> "$CSV_OUT"

    echo
done

echo "Done."
echo "CSV saved at: $CSV_OUT"
