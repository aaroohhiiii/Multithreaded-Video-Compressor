# Multithreaded Video Compressor

A C11 video-compressor project organized around pthread worker threads,
causal block prediction, wavefront scheduling, and ordered frame output.

The architecture, design-decision timeline, validation evidence, and benchmark
figures are collected in [`DAY18_REPORT.md`](DAY18_REPORT.md).

For a live demonstration from an ordinary video file, run:

```sh
./demo.sh input.mp4 [seconds] [output_directory]
```

The script defaults to a five-second segment, runs the encoder with four
workers, prints its wall time and compression ratio, decodes the result, wraps
it as an MP4, and opens it in the available system viewer.

## Build

```sh
make
```

Wavefront progress is published after every block by default. To build with a
coarser compile-time synchronization granularity, clean first so every object
is rebuilt with the same value:

```sh
make clean
make SYNC_GRANULARITY=2
```

For controlled compression experiments, causal prediction can be disabled at
compile time with `make PREDICTION=0`. Normal builds use `PREDICTION=1`; clean
before switching so all encoder and decoder objects agree.

Run the threaded encoder with raw planar YUV420 input whose dimensions are
multiples of 16. It uses four workers by default; an optional final argument
overrides that count:

```sh
./video_compressor input.yuv output.bin WIDTH HEIGHT [threads]
```

Decode the bitstream back to planar YUV420 with:

```sh
./video_decoder input.bin decoded.yuv WIDTH HEIGHT
```

Run the unit tests and the generated 32x32 end-to-end compression check with:

```sh
make test
```

Benchmark the full encoder with 1, 2, 4, and 8 workers (at least three runs
per worker count) using:

```sh
benchmark/benchmark_encoder.sh input.yuv WIDTH HEIGHT [runs] [results.csv]
```

The script prints an average-time/speedup table and saves both the individual
runs and averages as graph-ready CSV.

The output currently contains independently decodable frame records with frame
metadata, the frame's Huffman frequency table, and its entropy-coded payload.
Y, U, and V block rows are processed by the thread pool, while a reorder writer
commits completed frames to the output strictly by frame number.
