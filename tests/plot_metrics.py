import pandas as pd
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import sys
import os

metrics_path = 'tests/results/endurance_metrics.csv'

if not os.path.exists(metrics_path):
    print("Error: endurance_metrics.csv not found.")
    sys.exit(1)

# Load the telemetry data
df = pd.read_csv(metrics_path)

# Convert seconds to minutes for cleaner X-axis
df['Minutes'] = df['Seconds_Elapsed'] / 60
total_minutes = int(df['Minutes'].max())

# Setup the plot style
plt.style.use('dark_background')
plt.figure(figsize=(10, 5))

# Plot RPS
plt.plot(df['Minutes'], df['RPS'], color='#00ffcc', linewidth=1.5, label='Requests Per Second')

# Title and Labels
plt.title(f'C++ KV-Store: {total_minutes}-Minute Endurance Soak Test', fontsize=14, pad=15)
plt.xlabel('Time (Minutes)', fontsize=12)
plt.ylabel('Throughput (RPS)', fontsize=12)
plt.grid(True, linestyle='--', alpha=0.3)
plt.legend()

# Highlight the average
avg_rps = df['RPS'].mean()
plt.axhline(avg_rps, color='red', linestyle='dashed', alpha=0.7)
plt.text(0.2, avg_rps * 1.05, f'Average: {int(avg_rps):,} RPS', color='white', fontsize=10)

# Save and show
plt.tight_layout()
plt.savefig('tests/results/soak_test_results.png', dpi=300)
print("Graph saved successfully as tests/results/soak_test_results.png!")