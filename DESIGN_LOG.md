# Design Log

## 2026-10-02: Frame buffers and job ownership

`Frame` keeps `orig` and `recon` in two separate contiguous buffers. `orig` is
the immutable ground truth for the current frame, while `recon` holds exactly
the pixels that a decoder would have available after prediction, transform,
quantization, and reconstruction. Keeping them separate prevents reconstructed
lossy pixels from overwriting the source and ensures causal prediction uses
decoder-visible neighbors rather than unavailable original pixels. Each buffer
packs its Y, U, and V planes contiguously, and `frame_plane()` is the only place
where their YUV420 offsets may be calculated.

Each `Job` carries a direct `Frame *` instead of only a frame ID that workers
would resolve through a shared lookup table. The pointer gives a worker the
already-resolved object it needs, avoids a synchronized table lookup for every
job, and makes the job's target unambiguous even when several frames are in
flight. The scheduler that creates jobs is responsible for keeping the pointed
to frame alive until every job using it has completed.

## 2026-10-02: Initial reconstructed-frame contents

`read_frame()` zero-initializes `recon` instead of copying `orig` into it.
Reconstructed pixels are outputs of the encoder's prediction and reconstruction
path, so a location must not contain meaningful image data until that location
has actually been processed. Zero initialization gives deterministic storage
without pretending that unprocessed pixels are available to causal prediction.

Copying `orig` into `recon` would silently expose perfect source pixels as if
the decoder had reconstructed them. A scheduling or dependency bug could then
read an unprocessed neighbor and still produce plausible-looking output, hiding
the bug. It could also make encoder predictions depend on information the
decoder never possesses, causing encoder and decoder state to diverge.

## 2026-10-03: Separable 8x8 DCT

The DCT and inverse DCT are implemented as separable transforms: one 1D
transform is applied to every row, followed by one 1D transform on every
column. The 2D DCT basis is the product of two 1D bases, so this produces the
same result as evaluating the direct 2D formula while keeping the code and
intermediate calculations simpler.

For an N-by-N block, the direct formula evaluates an N-by-N input sum for each
of N-by-N output coefficients, requiring O(N^4) work. The separable version
performs 2N one-dimensional transforms, each requiring O(N^2) work, for O(N^3)
total work. At N=8, that is 1,024 multiply-accumulate terms instead of 4,096,
roughly one quarter as many in the core transform.

## 2026-10-03: Prediction uses reconstructed pixels only

`predict_from_neighbors()` may read only `recon`, never `orig`, because the
encoder must form each prediction from the same already-decoded neighbor pixels
that will be available to the decoder. This prevents encoder/decoder prediction
drift caused by the encoder using perfect source pixels that the decoder does
not possess.

## 2026-10-03: Reconstruct each block immediately

`reconstruct_block()` runs immediately after a block is encoded because the
next block's causal prediction may depend on the current block's reconstructed
right or bottom boundary. Deferring all reconstruction to a final frame pass
would leave those neighbor samples unavailable during encoding, forcing later
blocks to predict from zeros, stale data, or original pixels and making their
predictions differ from the decoder's.

## 2026-10-03: Zig-zag order before run-length encoding

Quantized DCT coefficients are scanned in zig-zag order before RLE because this
visits low spatial frequencies first and gradually moves toward high spatial
frequencies, where quantization produces most of the zeros. Grouping the
usually-zero high-frequency coefficients near the end creates longer
consecutive zero runs than row-major order, so RLE represents them with fewer
pairs.

## 2026-10-03: Decoder-side causal prediction

The decoder calls `predict_from_neighbors()` against its own progressively
built `recon` buffer instead of accepting prediction values from the encoder.
This guarantees that prediction is derived solely from pixels the decoder has
actually reconstructed, prevents hidden encoder-state dependencies, and keeps
the bitstream from wasting space transmitting values that are deterministically
available at decode time.

## 2026-10-03: Broadcasting wavefront progress

Wavefront progress uses `pthread_cond_broadcast()` instead of
`pthread_cond_signal()` because multiple rows and columns can be waiting on the
same condition variable, and a signal may wake a waiter whose particular
dependency is still unsatisfied while leaving an eligible waiter asleep.
Broadcasting lets every waiter recheck its own predicate and guarantees the
eligible workers can proceed. Its known inefficiency is the *thundering herd*:
threads whose dependencies are not ready wake unnecessarily, contend for the
mutex, and then go back to sleep.

## 2026-10-03: Operation order within a wavefront row

For each block column, `encode_row()` performs this exact sequence:

1. Wait for the top-row dependency with `wavefront_wait_for_dependency()`.
2. Predict from already reconstructed top and left neighbors.
3. Subtract the prediction from the original pixels to form the residual.
4. Apply the DCT.
5. Quantize the transform coefficients.
6. Apply zig-zag RLE and Huffman entropy coding.
7. Dequantize, inverse-transform, add the prediction, and write the block with
   `reconstruct_block()`.
8. At a synchronization-group boundary (every block when the default
   `SYNC_GRANULARITY=1` is used), publish progress through the current column
   with `wavefront_mark_done()`.

Reconstruction must precede the progress update because the progress value is
a promise to dependent rows that the block and its boundary pixels are fully
available. Marking completion first could wake the row below while `recon`
still contains zeros, stale pixels, or a partially written block. Keeping the
write before the mutex-protected progress publication also establishes the
required cross-thread visibility for the multithreaded implementation.

The standalone `encode_row()` test helper performs Huffman coding per block so
the complete codec operation is exercised locally. In the final production
pipeline, row workers retain each block's RLE pairs in its deterministic frame
position, and the last row to finish performs one frame-wide Huffman encode.
This changes where entropy coding is finalized, but not the dependency-critical
rule that reconstruction precedes progress publication.

## 2026-10-03: Bounded circular job queue

The thread pool uses a bounded circular array sized to four jobs per worker.
The fixed bound limits queued memory and makes the producer wait on `not_full`
when workers fall behind, providing backpressure. Head and tail indices reuse
the preallocated slots with good cache locality and no allocation in the hot
push/pop path.

A linked-list queue would require allocating and freeing a node for every row
job. At row granularity that can mean thousands of small allocations performed
around the same time by multiple threads, creating malloc contention on
allocator locks and metadata in addition to fragmentation and pointer-chasing
overhead. The circular array pays its allocation cost once when the pool is
created.

## 2026-10-03: Validate the pool with one worker first

The first real-job thread-pool test deliberately uses exactly one worker. This
still exercises job copying, queue ordering, blocking push/pop behavior, worker
dispatch, pointer lifetimes, shutdown sentinels, and joins, but it removes
simultaneous row execution from the experiment. Its reconstructed output must
match the established serial traversal byte for byte.

If this test fails, the defect is in thread-pool integration or job execution,
not in wavefront concurrency. Starting immediately with four workers would mix
those failures with dependency races, wake-up ordering, and shared-state timing,
making a bad job pointer or dropped/reordered job much harder to distinguish
from a wavefront synchronization bug.

## 2026-10-07: Multiworker and Helgrind validation

The real row-job pipeline was compared byte for byte with the plain serial
baseline for 12 runs with two workers and another 12 runs with four workers;
every run matched. The same test also retained its 12 one-worker runs.

Because native Valgrind is unavailable on Apple Silicon macOS, Helgrind 3.18.1
was run in an x86-64 Ubuntu container with debug symbols, full history, fair
scheduling, and a nonzero exit code on detected errors. The real threaded
integration test reported `0 errors from 0 contexts`. At that development
stage, the separately checked `video_compressor` entry point was still serial;
the production threaded entry point was added and rechecked in the later Day 14
and synchronization-granularity passes. It also reported `0 errors from 0
contexts`. Helgrind produced no
race or lock-order warnings, so there were no warning-specific explanations or
code fixes to apply in this pass.

Based on these results, the present wavefront synchronization is stable enough
to continue development rather than fall back to flat tiling. This is evidence
for the tested row ordering and frame size, not a proof for every workload;
larger randomized frames and production integration should remain required
regression coverage.

## 2026-10-07: Ordered frame output and Day 14 gates

The reorder buffer has one slot per input frame and a writer-owned
`next_expected_frame` counter. A completed frame is placed directly into the
slot matching its frame number, but the writer reads only the slot named by the
counter. For example, if frames 2 and 1 finish while frame 0 is still running,
slots 2 and 1 become ready but neither is written. When slot 0 becomes ready,
the writer emits frame 0 and advances the counter to 1, then emits frame 1 and
frame 2. Worker completion order therefore cannot change bitstream order.

The C11 multidimensional-array qualifier warnings were resolved with explicit
pointer-to-array casts matching each function parameter. The full build and
test suite passes without warnings under both Apple Clang 17 and GCC 11 with
`-Wall -Wextra -Wpedantic -Werror`.

Day 14 correctness gates:

1. **PASS — full Y/U/V.** Three colorful 1280x720 YUV420 frames extracted by
   ffmpeg were encoded and decoded. A side-by-side visual inspection showed
   correctly aligned colors with no U/V swap or chroma-plane offset shift.
   Per-plane PSNR was Y 39.27 dB, U 38.50 dB, and V 36.42 dB.
2. **PASS — production scale.** On the same three-frame 1280x720 input, the
   one-worker and four-worker encoders produced byte-identical bitstreams in
   each of 10 repeated runs.
3. **PASS — retained disk output.** The comparison used actual `.bin` files
   written by the reorder thread. Both files were read back by
   `video_decoder`; their 4,147,200-byte decoded YUV files were byte-identical.
   The four-worker bitstream was 105,453 bytes and the decoded result measured
   38.53 dB overall PSNR against the extracted input.

## 2026-10-08: Full-encoder thread-count benchmark

The benchmark used the same three-frame, 1280x720 planar YUV420 input as the
Day 14 production-scale test. It timed the complete encoder process, including
input reads, block processing, entropy coding, reorder-buffer output, and disk
writes. Each thread count received one untimed warm-up followed by five timed
runs. Wall-clock measurements from `/usr/bin/time -p` were:

| Threads | Run 1 | Run 2 | Run 3 | Run 4 | Run 5 | Average | Speedup |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 0.51 s | 0.56 s | 0.56 s | 0.54 s | 0.53 s | 0.540 s | 1.000x |
| 2 | 0.29 s | 0.28 s | 0.25 s | 0.25 s | 0.25 s | 0.264 s | 2.045x |
| 4 | 0.13 s | 0.18 s | 0.19 s | 0.20 s | 0.24 s | 0.188 s | 2.872x |
| 8 | 0.17 s | 0.16 s | 0.14 s | 0.14 s | 0.14 s | 0.150 s | 3.600x |

Scaling is useful but not linear beyond two workers. A causal wavefront begins
with only a narrow diagonal of runnable blocks: during ramp-up, many workers
must wait for top-row progress. Near the end, the runnable diagonal narrows
again and workers idle during ramp-down. More workers help most in the broad
middle of a frame, but cannot remove these dependency stalls. Serial or
less-parallel work such as setup, frame-wide Huffman finalization, ordered
output, and file I/O also limits total speedup, while mutex, condition-variable,
queue, and scheduling overhead grows with worker count. The slight measured
2.045x result at two threads is normal timing/cache noise rather than evidence
that two CPUs perform more than twice the codec work. Four and eight threads
show the expected diminishing returns: doubling from four to eight reduces the
average by only 0.038 seconds and raises speedup from 2.872x to 3.600x.

The raw measurements and average rows are retained in
`benchmark/results_1280x720.csv` for graphing.

## 2026-10-08: Wavefront synchronization granularity

`SYNC_GRANULARITY` is a compile-time constant, defaulting to 1. A row still
reconstructs every block immediately, but publishes progress only after that
many blocks, or after the final partial group. Progress publication now stores
`column + 1` rather than blindly incrementing by one, so one notification can
truthfully announce an entire reconstructed group. The dependent row remains
blocked until all top-neighbor pixels it needs have actually been published.

The same warmed five-run benchmark used for Day 15 produced these raw wall
times and averages on the three-frame 1280x720 input:

| Granularity | Threads | Runs (seconds) | Average | Speedup |
| ---: | ---: | --- | ---: | ---: |
| 1 | 1 | 0.48, 0.50, 0.48, 0.49, 0.49 | 0.488 s | 1.000x |
| 1 | 2 | 0.25, 0.26, 0.25, 0.25, 0.25 | 0.252 s | 1.937x |
| 1 | 4 | 0.13, 0.17, 0.17, 0.17, 0.17 | 0.162 s | 3.012x |
| 1 | 8 | 0.14, 0.13, 0.13, 0.13, 0.13 | 0.132 s | 3.697x |
| 2 | 1 | 0.48, 0.48, 0.49, 0.50, 0.49 | 0.488 s | 1.000x |
| 2 | 2 | 0.25, 0.26, 0.25, 0.25, 0.26 | 0.254 s | 1.921x |
| 2 | 4 | 0.20, 0.19, 0.18, 0.19, 0.19 | 0.190 s | 2.568x |
| 2 | 8 | 0.14, 0.14, 0.14, 0.13, 0.13 | 0.136 s | 3.588x |
| 4 | 1 | 0.48, 0.49, 0.48, 0.48, 0.48 | 0.482 s | 1.000x |
| 4 | 2 | 0.25, 0.25, 0.25, 0.25, 0.25 | 0.250 s | 1.928x |
| 4 | 4 | 0.19, 0.18, 0.18, 0.18, 0.18 | 0.182 s | 2.648x |
| 4 | 8 | 0.13, 0.14, 0.13, 0.14, 0.15 | 0.138 s | 3.493x |

Granularity 1 performed best for the highly parallel four- and eight-worker
cases. At four workers it was 14.7% faster than granularity 2 and 11.0% faster
than granularity 4. At eight workers it was 2.9% and 4.3% faster,
respectively. Granularity 4's small advantage at one and two workers is within
the timer's 0.01-second resolution and normal run-to-run variation, while the
four-worker gap is larger and consistent enough to guide the default choice.

The trade-off is fewer synchronization operations versus a wider dependency
stall. Granularity 2 roughly halves, and granularity 4 roughly quarters, the
number of progress-lock acquisitions and condition broadcasts. However, a
finished top-row block remains invisible until its whole group finishes, so
the next row starts later and the wavefront has fewer runnable rows. DCT,
quantization, RLE, and reconstruction provide enough work per block that the
saved lock overhead did not repay this lost parallelism on the measured input.
The default therefore remains 1; the compile-time option is retained for
experiments on machines or workloads with different lock costs.

Correctness checks passed at granularities 2 and 4: the complete test suite
passed, and each real 1280x720 compressed file was byte-identical to the
granularity-1 file. Helgrind 3.18.1 was run under x86-64 Ubuntu with full
history and fair scheduling. At each setting, both `test/test_threadpool` and
the real four-thread `video_compressor` reported `0 errors from 0 contexts`.
There were no race or lock-order warnings to fix.

Raw CSVs are stored as `benchmark/results_granularity_1.csv`,
`benchmark/results_granularity_2.csv`, and
`benchmark/results_granularity_4.csv`; an average-only graphing table is in
`benchmark/results_granularity_comparison.csv`, and the Helgrind summaries are
in `benchmark/helgrind_granularity_results.txt`.

## 2026-10-08: Day 18 report and prediction ablation

The final report derives its architecture and timeline from this design log and
generates its figures from retained CSV data. The plotting tool uses only the
Python standard library and writes SVG files, keeping the report reproducible
without adding a plotting-library dependency.

The requested prediction comparison did not exist in the earlier saved data,
so it was measured rather than estimated. `ENABLE_PREDICTION` is now a
compile-time experimental switch controlled by `make PREDICTION=0` or `1`,
with prediction enabled by default. On the same three-frame 1280x720 input,
four workers, and synchronization granularity 1, the 4,147,200-byte raw input
compressed to 105,453 bytes with causal prediction and 150,083 bytes with a
constant 128 prediction. Those values correspond to 39.33:1 and 27.63:1.
Causal prediction reduced output size by 44,630 bytes, or 29.74% relative to
the no-prediction output. The normal prediction-enabled build was restored
after the experiment.

## 2026-10-08: Final release audit

A final clean native build and complete test run passed under Apple Clang 17
with `-std=c11 -Wall -Wextra -Wpedantic -Werror`. An independent Linux audit
copied only the Makefile and source/test trees into an empty directory, removed
any copied build products, and rebuilt and tested everything with GCC using the
same warning-as-error policy. It also passed without warnings. This workspace
does not contain Git metadata, so a literal fresh `git clone` was not possible;
the empty, isolated, read-only-source copy verifies the equivalent Makefile
property: no existing object, executable, generated fixture, or workspace path
is needed for a successful build and test.

Helgrind 3.18.1 then checked the full retained-file pipeline at the default
settings: a four-thread encoder wrote the compressed 32x32 fixture, and the
decoder read that file and produced raw YUV. The encoder and decoder each
reported `0 errors from 0 contexts`; the decoded output retained the expected
49.15 dB PSNR. The exact summary is stored in
`benchmark/helgrind_final_results.txt`.

The read-through found and corrected two pieces of documentation drift. The
wavefront operation sequence now describes conditional progress publication at
`SYNC_GRANULARITY` boundaries and distinguishes the test helper's per-block
Huffman exercise from production frame-wide Huffman finalization. The older
Helgrind entry now makes clear that its “serial main” description applied only
at that historical stage, before the threaded production entry point and
reorder buffer were added. All other entries remain consistent with the final
code and retained measurements.
