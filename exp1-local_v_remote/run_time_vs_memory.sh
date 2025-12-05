#!/bin/bash

# -------------------------------------------
#  Experiment: Time vs Memory Size
#  Fixed number of threads, sweep array size.
#
#  Usage:
#      ./run_time_vs_memory.sh ./exp1_local_vs_remote
#
#  Output:
#      results_time_vs_memory.csv
# -------------------------------------------

PROG=$1
if [[ ! -x "$PROG" ]]; then
    echo "ERROR: Provide path to compiled exp1_local_vs_remote program."
    exit 1
fi

THREADS=8                  # fixed thread count
COMPUTE_NODE=0             # all threads run on node 0
MEM_SIZES_MB=(512 1024 2048 4096 8192)

CSV_OUT="results_time_vs_memory.csv"

echo "Experiment: Time vs Memory Size (threads = $THREADS)"
echo "Writing to: $CSV_OUT"
echo

# CSV header
echo "Memory_MB,Placement,Runtime_sec" > "$CSV_OUT"

for SIZE in "${MEM_SIZES_MB[@]}"; do
    echo "=== Array = $SIZE MB ==="

    # -------------------------
    # LOCAL CASE (mem=0 -> compute=0)
    # -------------------------
    echo -n "  Local  (mem=0 -> compute=0) ... "
    OUT=$("$PROG" "$SIZE" "$THREADS" 0 "$COMPUTE_NODE")
    # Uncomment this if you want to see raw output while debugging:
    # echo "$OUT"

    TIME_LOCAL=$(echo "$OUT" | awk '/Approx\. total time/ {print $(NF-1)}')
    if [[ -z "$TIME_LOCAL" ]]; then
        echo "ERROR: could not parse time (local). Output was:"
        echo "$OUT"
        exit 1
    fi
    echo "$TIME_LOCAL sec"
    echo "$SIZE,local,$TIME_LOCAL" >> "$CSV_OUT"


    # -------------------------
    # REMOTE CASE (mem=1 -> compute=0)
    # -------------------------
    echo -n "  Remote (mem=1 -> compute=0) ... "
    OUT=$("$PROG" "$SIZE" "$THREADS" 1 "$COMPUTE_NODE")
    TIME_REMOTE=$(echo "$OUT" | awk '/Approx\. total time/ {print $(NF-1)}')
    if [[ -z "$TIME_REMOTE" ]]; then
        echo "ERROR: could not parse time (remote). Output was:"
        echo "$OUT"
        exit 1
    fi
    echo "$TIME_REMOTE sec"
    echo "$SIZE,remote,$TIME_REMOTE" >> "$CSV_OUT"

    echo
done

echo "Done."
echo "CSV saved at: $CSV_OUT"
