import re
import os
import sys
import pandas as pd

import warnings
with warnings.catch_warnings():
    warnings.filterwarnings("ignore", category=DeprecationWarning)
    import imp

output = "out.csv"
if len(sys.argv) > 1:
    output = sys.argv[1]

# import bash variables from config.sh
config = imp.load_source("bash_module", "./config.sh")
directory = f"results_{config.NUM_LEVELS}_{config.MAX}_{config.MIN}"

print("Test config:")
print(f"\tallocators: {config.ALLOC_list}")
print(f"\tsizes: {config.SIZE_list}")
print(f"\truns: {config.RUN_list}")
print(f"\ttests: {config.TEST_list}")
print(f"\tthreads: {config.THREAD_list}")
allocs = config.ALLOC_list.split(" ")
sizes = config.SIZE_list.split(" ")
runs = config.RUN_list.split(" ")
tests = config.TEST_list.split(" ")
threads = config.THREAD_list.split(" ")

# print(allocs, sizes, runs, tests, threads)

# build the data structures for a pandas dataframe
columns = "Test Allocator Size Threads Run Time".split(" ")
data = {c : [] for c in columns}

print("Allocator config:")
print(f"\tmin size:{config.MIN}")
print(f"\tmax size: {config.MAX}")
print(f"\tlevels: {config.NUM_LEVELS}")

print(f"\nParsing files in {directory}")

# regex match the timer line in the result files
parser = re.compile("Timer\s+\(clocks\): [0-9]*")

file_list = []
for test in tests:
    for alloc in allocs:
        for size in sizes:
            for th in threads:
                for run in runs:
                    #file_list.append(f"{test}-{alloc}-sz{size}-TH{th}-R{run}")
                    file_list.append((test, alloc, size, th, run))

errors = False
for idx, filetup in enumerate(file_list, start = 1):
    print(f"\rProgress: {idx}/{len(file_list)}", end="")

    test, alloc, size, th, run = filetup
    filename = f"{test}-{alloc}-sz{size}-TH{th}-R{run}"

    data["Test"].append(test)
    data["Allocator"].append(alloc)
    data["Size"].append(size)
    data["Threads"].append(th)
    data["Run"].append(run)
    
    try:
        with open(f"{directory}/{filename}", "r") as f:
            s = f.read()
            match = parser.findall(s)
            if match is not None:
                timer = match[0].split(" ")[-1] # get only the timer
                data["Time"].append(timer)
            else:
                print(f"\nFile has no timer: {filename}")
                data["Time"].append("NaN")
                errors = True
            
    except:
        print(f"\nFile not found: {filename}")
        errors = True
        pass

if not errors:
    print("\nParsing completed successfully")

    df = pd.DataFrame(data, columns = columns)
    df.to_csv(output, index=False)
else:
    print("\nParsing encountered errors, no output file generated")