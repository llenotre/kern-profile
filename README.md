This tool allows profiling of the CPU usage of a Rust kernel running in QEMU, using flamegraphs.

It works by sampling the execution of the code at a given rate.
For each sample, the plugin collects the current callstack of the code being executed.

The assumption is made that more time the CPU spends executing a function, the higher is the probability the function shows up in the callstack.

Aggregating all the callstacks together allows to build the flamegraph.

The repository contains the following components:
- a QEMU TCG plugin for data acquisition (written in C)
- an aggregator to convert the data into a form that can be processed by `flamegraph.pl` (written in Rust)



## Requirements

The following programs are required:
- QEMU version 8.2.0 **exactly**
- [Flamegraph](https://github.com/brendangregg/FlameGraph)

**Note**: another version of QEMU cannot be used because the API can change from a version to another



## Build

Build the QEMU plugin using:

```sh
QEMU_SRC=<source to QEMU> make
```

Then, build the aggregator:

```sh
cargo build --release
```



## Usage

Run QEMU with the plugin by adding the following argument (adapt parameters to your needs):

```sh
-plugin 'kern-profile.so,out=raw_data,delay=10'
```

This will acquire data from QEMU until exiting and write the output to `raw_data`. The `delay` parameter is the amount of microseconds between each sample.

The output file can then be processed by the aggregator:

```sh
kern-profile raw_data <path-to-kernel-ELF> flamegraph-input
```

Then, you can generate the flamegraph with:

```sh
cat flamegraph-input | flamegraph.pl >flamegraph.svg
```



## Caveats/missing features

The following issues need to be fixed in the future:
- Only one CPU core is supported
- Only x86 in 32 bits is supported
