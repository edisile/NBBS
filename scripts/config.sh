#!/bin/bash

THREAD_list="1 4 8 16"		# thread list
RUN_list="1 2 3"					# run list
ALLOC_list="1lvl-nb 4lvl-nb new-base new-ff new-d2"        # kernel-sl # list of allocators
SIZE_list="4096 32768 262144"
TEST_list="TBTT TBLS TBFS TBCA"

MIN=4096 # 2^12 = 2^m
MAX=4194304
NUM_LEVELS=22 # TOTAL_MEM(NUM_LEVELS, m) = 2^(NUM_LEVELS - 1 + m) -> 8 GB

FOLDER="results_${NUM_LEVELS}_${MAX}_${MIN}"



