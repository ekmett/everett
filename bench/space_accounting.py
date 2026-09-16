#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
# SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
"""Parameter sensitivity; no alleged exact implementation of either paper."""
import argparse
import csv
import json
import math
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--measurements',default='bench/results/space_accounting_dual.csv')
p.add_argument('--epsilon',type=float,default=.1)
p.add_argument('--hash-words',type=float,default=3)
p.add_argument('--top-bytes-per-key',type=float,default=4)
p.add_argument('--occupancy',type=float,default=.75)
p.add_argument('--redundant-per-native',type=float,default=.1)
a=p.parse_args()
assert a.epsilon>=0 and a.hash_words>=0 and a.top_bytes_per_key>=0 and 0<a.occupancy<=1 and a.redundant_per_native>=0
for r in csv.DictReader(open(a.measurements)):
    n=int(r['native_count']); value=int(r['raw_value_bytes'])/n
    # Proxy input only: the paper's particular FC framing is unspecified.
    fc=(int(r['native_bytes'])-int(r['raw_value_bytes']))/n
    everett=int(r['total_array_bytes'])/n
    key_upper=(1+a.epsilon)*fc+1/8
    cosb=key_upper+value+8*a.hash_words+a.top_bytes_per_key
    result={k:r[k] for k in ('unit','prefix_bytes','value_bytes','random','base_count')}
    result.update(everett_array_bytes_per_native=everett,fc_proxy_bytes_per_native=fc,
                  cosb_scenario_occupied=cosb,cosb_scenario_capacity=cosb/a.occupancy)
    if int(r['prefix_bytes'])==0 and int(r['value_bytes'])==8:
        result.update(cola_experiment_native_slot_floor=32,
                      cola_scenario_occupied=32*(1+a.redundant_per_native),
                      cola_scenario_capacity=32*(1+a.redundant_per_native)/a.occupancy)
    print(json.dumps(result,sort_keys=True))
