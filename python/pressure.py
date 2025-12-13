#!/usr/bin/env python3
import matplotlib.pyplot as plt
import numpy as np

def format_size_label(size_mb):
    if size_mb >= 1024:
        return f"{size_mb // 1024} GB"
    return f"{size_mb} MB"

def plot_with_pressure_times():
    # ---- Data from your table ----
    sizes_mb = [4096, 8192, 16384, 32768]

    bind_node0 = [29.972151, 89.310996, 221.140414, 508.693458]      # note: 32GB run happened after pressure proc was killed
    preferred_node0 = [26.624624, 67.295162, 193.475032, 468.095167]
    interleave_01 = [28.532189, 82.775332, 183.326639, 441.735394]

    # ---- Styling / layout (matching your script vibe) ----
    plt.figure(figsize=(12, 6))
    ax = plt.gca()

    x = np.arange(len(sizes_mb))
    width = 0.27

    bars1 = ax.bar(x - width, bind_node0, width,
                   label='BIND_NODE0 (Strict)', color='#ff7f0e',
                   alpha=0.85, edgecolor='black', linewidth=0.5)
    bars2 = ax.bar(x, preferred_node0, width,
                   label='PREFERRED_NODE0 (Fallback)', color='#2ca02c',
                   alpha=0.85, edgecolor='black', linewidth=0.5)
    bars3 = ax.bar(x + width, interleave_01, width,
                   label='INTERLEAVE_01', color='#1f77b4',
                   alpha=0.85, edgecolor='black', linewidth=0.5)

    # ---- Value labels on bars (like your counter plot) ----
    def add_labels(bars):
        for bar in bars:
            val = bar.get_height()
            if val > 0:
                ax.text(bar.get_x() + bar.get_width()/2, val,
                        f"{val:.1f}", ha='center', va='bottom', fontsize=12)

    add_labels(bars1)
    add_labels(bars2)
    add_labels(bars3)

    # ---- Annotate the “pressure generator killed” event at 32GB for BIND_NODE0 ----
    kill_x = x[-1] - width
    kill_y = bind_node0[-1]
    ax.annotate(
        "Pressure generator killed\n(during 32GB BIND_NODE0 run)",
        xy=(kill_x, kill_y),
        xytext=(x[-2] - 0.2, max(bind_node0[-1], preferred_node0[-1], interleave_01[-1]) * 1.08),
        arrowprops=dict(arrowstyle='->', color='red', lw=2.5, alpha=0.7),
        fontsize=12, color='red', fontweight='bold',
        bbox=dict(boxstyle='round,pad=0.4', facecolor='white',
                  edgecolor='red', alpha=0.9, linewidth=2)
    )

    # ---- Axes formatting to match your style ----
    ax.set_xlabel("Memory Size", fontsize=13, fontweight='bold')
    ax.set_ylabel("Average Execution Time (seconds)", fontsize=13, fontweight='bold')
    ax.set_title("NUMA Policy Comparison Under Memory Pressure\n(8 Threads, Computation on Node 0)",
                 fontsize=14, fontweight='bold')

    ax.set_xticks(x)
    ax.tick_params(axis='x', labelsize=13)
    ax.set_xticklabels([format_size_label(s) for s in sizes_mb], rotation=45, ha='right')

    ax.grid(True, alpha=0.3, axis='y')
    ax.legend(fontsize=13)

    plt.tight_layout()
    plt.savefig("with_pressure_times.png", dpi=300, bbox_inches="tight")
    print("✓ Saved: with_pressure_times.png")
    plt.close()

if __name__ == "__main__":
    plot_with_pressure_times()
