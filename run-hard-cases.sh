#!/usr/bin/env bash
make

for case in 2025-09-15-hard 2025-10-18-hard 2025-10-14-hard 2025-10-20-hard 2025-10-29-hard 2025-10-30-hard 2025-10-05-hard 2025-10-24-hard 2025-10-25-hard 2025-10-28-hard; do
  echo $case
  time bin/mccw -v0 ../pips2mcc/generated-weighted-instances/$case
  echo
done

