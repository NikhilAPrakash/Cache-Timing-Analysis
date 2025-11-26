# Cache-Timing-Analysis ✅

A small experimental toolkit for measuring cache hit / memory access timing on x86_64 and AArch64 (ARMv8) systems.

The repository contains two self-contained C programs that demonstrate low-level timing approaches (x86 RDTSC + CPUID and ARM PMU counters) and write measured timings to text files for further analysis.

Why this exists
- Useful for learning how CPU caches (L1/L2/L3) and main memory affect access latencies
- Can help researchers and students collect micro-benchmarks for cache timing analysis

---

## Contents

- `x86_cache_hits.c` — x86_64 program that uses CPUID/RDTSC/RDTSCP assembly to time cache hits/misses and writes to output files (L1/L2/L3/SDRAM)
- `Aarch64_cache_hits.c` — AArch64 (ARMv8) program that uses the cycle counter (PMU) to measure L1/L2/SDRAM timings and writes to output files
- `README.md` — this file

---

## Features / what the programs do
- Allocate large buffers sized to typical cache layers (L1/L2/L3) and repeatedly perform memory accesses designed to: 1) ensure the data is loaded into a particular cache level and 2) measure the time of an access that hits that level.
- Collects a large number of timing samples (default: 1,000,000 samples) and writes them into text files for post-processing/plotting.

Each program produces output files in the current working directory:
- `L1_cache_hits.txt` — measured L1 hit cycles and last value read
- `L2_cache_hits.txt` — measured L2 hit cycles and last value read
- `L3_cache_hits.txt` — measured L3 hit cycles and last value read (x86 only)
- `SDRAM_cache_hits.txt` — measured main-memory (DRAM) misses

---

## Requirements

- A modern x86_64 CPU for running `x86_cache_hits.c` (RDTSC + CPUID instructions used)
- AArch64 (ARMv8) CPU with user access to cycle counter for `Aarch64_cache_hits.c` (kernel configuration may affect access)
- gcc or clang (a typical Linux toolchain available on Linux distributions)
- Sufficient memory — the test programs allocate multi-megabyte buffers and large arrays to store results. Also the default number of traces (1,000,000) produces large output files and may take considerable time to run.

Notes regarding AArch64 and PMU/cycle counters
- On some Linux distributions, reading/writing PMU control registers or enabling user access to PMU counter may be restricted. If you get permission errors or measurements that look invalid, check your kernel/perf settings and any platform-specific guidance about enabling user-mode PMU access (e.g., `perf_event_paranoid` and/or platform-specific sysfs knobs).

---

## Build

General recommendation: compile with optimization enabled (e.g. -O2) for representative timings. If you want to debug, use -O0.

Example (native x86_64):

```bash
gcc -O2 x86_cache_hits.c -o x86_cache_hits
```

Example (native AArch64 / cross-compile from x86):

```bash
# native on ARM64
gcc -O2 Aarch64_cache_hits.c -o aarch64_cache_hits

# or cross-compile using a toolchain (example):
aarch64-linux-gnu-gcc -O2 Aarch64_cache_hits.c -o aarch64_cache_hits
```

Tip: reduce the number of traces while experimenting to get faster runs, e.g. by editing the `NUM_OF_TRACES` macro or passing a compile-time define:

```bash
gcc -O2 -DNUM_OF_TRACES=10000 x86_cache_hits.c -o x86_cache_hits_small
```

---

## Run / Example

Run the binary from the repository folder. It will print progress and produce the output files in the same directory.

```bash
# on x86_64
./x86_cache_hits

# on ARM64
./aarch64_cache_hits
```

Each output file is textual and contains one measurement per line. Example lines (two-columns separated by whitespace):

```
1234        42
4567        12
```

Columns: <cycles> <data-value-at-time>

From these outputs you can compute statistics (mean / median / histogram), build visualizations and determine typical latency ranges for each cache level.

---

## Performance and safety notes

- These programs run tight loops and allocate large arrays — expect heavy CPU usage and large memory usage.
- The default configuration is intended for collecting high-quality, high-sample-count traces. That can take a long time; when exploring or debugging, reduce `NUM_OF_TRACES` first.
- The programs use inline assembly and architecture-specific registers — do not run an x86 binary on ARM or vice-versa.

---

## Extending this repo

- Add scripts to parse and plot the produced datasets (e.g., a Python script using matplotlib / pandas).
- Add a small harness to automatically compile, run with fewer traces and produce sample plots for CI or notebooks.

---

## License & contributions

This repository doesn't currently include a LICENSE file. If you want to add a license, consider MIT, Apache-2.0 or similar. Contributions and pull requests are welcome — please open an issue first to propose larger changes.

---

If you'd like, I can also add a small Python notebook or scripts that parse the output and produce example histograms. Would you like that next? 👇
