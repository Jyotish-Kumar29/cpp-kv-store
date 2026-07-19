# ---------------------------------------------------------
# DATABASE CLEANUP
# ---------------------------------------------------------
echo -e "[CLEANUP] cleaning up the setup..."
rm -f data/kvstore.aof
rm -f tests/results/soak_test_results.png
rm -f tests/results/endurance_metrics.csv
rm -rf build/