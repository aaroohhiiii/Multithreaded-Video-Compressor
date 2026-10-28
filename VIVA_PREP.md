# VIVA Preparation: Concurrency and Architecture

This document prepares answers for the concurrency and architecture portion of
the multithreaded video-compressor project. It is grounded in the current source
tree and in the measurements recorded in `DESIGN_LOG.md`; it does not treat the
project as a generic pthread example.

## How to use this document

Every answer is marked as one of the following:

- **HARD CORRECTNESS REQUIREMENT** — violating the stated property makes the
  codec incorrect, racy, non-deterministic, or capable of deadlocking. A
  different implementation is possible, but it must preserve the same
  invariant.
- **DESIGN CHOICE — ALTERNATIVES EXIST** — the project made a defensible
  engineering choice, but another implementation could also be correct with
  different cost or complexity.
- **MATHEMATICAL PROPERTY** — the result follows from the transform or
  dependency graph rather than from a pthread API choice.

Line references are to the repository state used to generate this file.

## Architecture map to remember before the viva

The encoder exposes parallelism at two levels:

1. **Across frames:** `main()` creates one independent `FrameWork` per input
   frame (`src/main.c:244-299`) and submits row jobs from several frames to one
   shared `ThreadPool` (`src/main.c:398-419`). Each `FrameWork` owns its own
   `Frame`, encoded-block array, completion state, and three `WavefrontSync`
   objects (`src/main.c:24-35`).
2. **Within a frame:** each Y, U, or V plane has one row job per 8x8 block row.
   A row walks left-to-right, while `WavefrontSync` allows it to trail the row
   above by one or more blocks. The production worker is
   `encode_plane_row()` (`src/main.c:133-217`).

The thread pool itself is a bounded circular producer-consumer queue. Its
capacity is four jobs per worker (`src/threadpool.c:147-170`), workers dispatch
jobs in `worker_loop()` (`src/threadpool.c:126-145`), and shutdown uses one
sentinel job per worker followed by joins (`src/threadpool.c:212-244`).

There are two similarly named row encoders which must not be confused:

- `encode_row()` in `src/threadpool.c:261-344` is the luma-only reference/test
  path. It performs Huffman coding for every block so the entire local codec
  sequence is exercised.
- `encode_plane_row()` in `src/main.c:133-217` is the production Y/U/V path.
  It stores each block's RLE pairs at a deterministic index
  (`src/main.c:191-197`); the last completed row triggers `finalize_frame()`,
  which performs one frame-wide Huffman encode (`src/main.c:95-130` and
  `src/main.c:219-241`). This distinction is recorded in
  `DESIGN_LOG.md:118-123`.

The output side is separately ordered. Completed frames are placed in numbered
`ReorderSlot`s by `reorder_submit()` (`src/reorder.c:117-145`), while
`writer_loop()` writes only `next_expected_frame` (`src/reorder.c:26-64`). Thus
parallel completion order cannot change file order.

---

## 1. Core concept questions

### 1.1 Why is data parallelism the right classification for this design, and where does it stop being purely data-parallel?

**Status: DESIGN CHOICE — ALTERNATIVES EXIST, but the dependency constraints are
hard correctness requirements.**

This is primarily **data parallelism** because many jobs apply the same codec
operations to different pieces of video data. Every production row job calls
the same `encode_pipeline_row()` callback (`src/main.c:219-242`), which calls
`encode_plane_row()` for the job's frame, plane, and row. Inside that function,
each block goes through the same prediction, residual, DCT, quantization, RLE,
and reconstruction stages (`src/main.c:167-210`). The data coordinates differ;
the algorithm does not.

There are three data partitions:

- distinct frames, represented by separate `FrameWork` instances;
- the three YUV planes, selected by `job.plane` and using separate
  `work->sync[plane]` objects (`src/main.c:398-410`);
- block rows within a plane, selected by `job.row`.

It is not *embarrassingly* data-parallel, because a block is not independent of
all other blocks. `predict_from_neighbors()` reads the reconstructed boundary
above and to the left (`src/codec.c:268-323`). Therefore, the row tasks are
partially ordered by data dependencies. `wavefront_wait_for_dependency()`
enforces the top dependency (`src/wavefront.c:40-63`), and the row's own
left-to-right loop enforces the left dependency (`src/main.c:153-215`).

The design also contains genuine **task-parallel** components:

- worker threads execute row-coding tasks;
- the last row performs frame-finalization/Huffman work
  (`src/main.c:229-240`);
- the reorder buffer owns a dedicated writer task (`src/reorder.c:26-64`);
- the decoder is a separate pipeline rather than another instance of the row
  job.

So the most accurate viva description is: **a data-parallel codec with a
dependency-aware task scheduler and a task-parallel ordered-output stage.**
Frame-level parallelism and intra-frame wavefront parallelism are combined; the
wavefront does not replace frame-level parallelism.

### 1.2 Why does left+top-only dependency guarantee a valid execution order?

**Status: MATHEMATICAL PROPERTY and HARD CORRECTNESS REQUIREMENT.**

Consider one plane divided into `R` block rows and `C` block columns. Represent
block `(r,c)` as a graph vertex, where `0 <= r < R` and `0 <= c < C`. Add the
following directed dependency edges:

- `(r,c-1) -> (r,c)` when `c > 0`, because `(r,c)` reads the reconstructed right
  boundary of its left neighbour;
- `(r-1,c) -> (r,c)` when `r > 0`, because `(r,c)` reads the reconstructed bottom
  boundary of its top neighbour.

Define a rank function

```text
rank(r,c) = r + c.
```

Every edge increases the rank by exactly one:

```text
rank(r,c) - rank(r,c-1) = 1
rank(r,c) - rank(r-1,c) = 1.
```

Suppose a directed cycle existed. Following the cycle would have to return to
the starting vertex and therefore to the original rank. But every edge strictly
increases rank, so after any non-empty path the rank is larger. This is a
contradiction. The graph is therefore a **directed acyclic graph (DAG)**.

A valid topological order is increasing `r+c`: diagonal 0, then diagonal 1,
and so on. A simpler valid order is ordinary row-major order, because both the
left and top predecessors occur earlier. The wavefront scheduler is less
restrictive: all vertices on the same `r+c` diagonal have no edges between
them, so they may run concurrently once the earlier diagonal has completed.

The implementation realizes the same proof in two pieces:

- `encode_plane_row()` increments `block_column`, so `(r,c-1)` finishes before
  `(r,c)` in the same row (`src/main.c:153-215`);
- before `(r,c)`, `wavefront_wait_for_dependency()` waits until
  `row_progress[r-1] > c` (`src/wavefront.c:55-61`), proving `(r-1,c)` has been
  reconstructed and published.

`predict_from_neighbors()` then reads only those two legal reconstructed
boundaries (`src/codec.c:296-321`). This is not merely a convenient scan order;
it is a topological execution of an explicitly acyclic dependency graph.

### 1.3 What is the theoretical maximum parallelism at an instant in the wavefront?

**Status: MATHEMATICAL PROPERTY; using row jobs rather than block jobs is a
DESIGN CHOICE.**

Let the block size be `B=8`, as defined by `DCT_BLOCK_SIZE` in
`src/codec.h:8`. For a plane of pixel width `W` and height `H`, assume both are
divisible by eight:

```text
R = H / B block rows
C = W / B block columns.
```

For diagonal index `k = r+c`, where `0 <= k <= R+C-2`, the number of blocks on
that diagonal is

```text
D(k) = max(0,
           min(R-1, k) - max(0, k-(C-1)) + 1).
```

An equivalent compact form inside the valid range is

```text
D(k) = min(k+1, R, C, R+C-1-k).
```

This function explains ramp-up and ramp-down:

- early diagonals contain `1, 2, 3, ...` blocks;
- the middle reaches at most `min(R,C)` blocks;
- late diagonals shrink `..., 3, 2, 1`.

Thus the ideal maximum instantaneous parallelism of one plane is

```text
max D(k) = min(R,C).
```

For the retained 1280x720 test:

- Y: `R=720/8=90`, `C=1280/8=160`, so the ideal diagonal width peaks at 90;
- U and V are each 640x360, so `R=45`, `C=80`, and the peak is 45.

This is an upper bound, not a promise of 90x measured speedup. The actual
program schedules **whole rows**, not independent block jobs. Each active row
has only one current column, so at most one worker can execute a given row; the
worker count, queue availability, frame count, plane, serial Huffman
finalization, and I/O reduce the realized concurrency. `SYNC_GRANULARITY > 1`
also delays publication and narrows the set of runnable rows.

Across `F` independent frames, ideal ready work can be the sum of their
diagonal widths, and `main()` deliberately interleaves frames while submitting
row jobs (`src/main.c:398-419`). The hardware bound remains the number of
available worker threads/cores.

### 1.4 Why must `pthread_cond_wait()` be called while holding the mutex?

**Status: HARD CORRECTNESS REQUIREMENT.**

A condition variable is not a stored event flag. Correct waiting requires a
shared predicate—in this project, `row_progress[row-1] > col`—protected by a
mutex. `wavefront_wait_for_dependency()` locks `sync->mutex` before examining
the predicate (`src/wavefront.c:51-55`) and passes that same mutex to
`pthread_cond_wait()` (`src/wavefront.c:56`).

The essential property of `pthread_cond_wait()` is that it **atomically**:

1. releases the mutex; and
2. puts the calling thread to sleep.

It reacquires the mutex before returning. “Atomically” here means no producer
can acquire the same mutex, update the predicate, and notify in the dangerous
gap between the waiter's unlock and its transition to sleep.

Without that protocol, this lost-wakeup interleaving is possible:

1. Consumer checks `row_progress[row-1] <= col` without the mutex and decides
   it must sleep.
2. Before it actually waits, the producer reconstructs the top block, updates
   `row_progress`, and broadcasts.
3. No thread is yet asleep, so that notification wakes nobody.
4. The consumer now sleeps even though the predicate is already true. If no
   later progress notification occurs, it sleeps forever.

With the mutex held during the check, the producer cannot update progress until
the consumer enters `pthread_cond_wait()` and atomically releases the mutex.
Alternatively, if the producer updates first, the consumer later acquires the
mutex, sees the true predicate, and never waits.

The same mutex also publishes reconstructed pixels. The producer writes
`frame->recon` in `reconstruct_block()` (`src/codec.c:687-700`) and then locks,
updates, broadcasts, and unlocks in `wavefront_mark_done()`
(`src/wavefront.c:75-83`). The waiter's reacquisition of that mutex establishes
the required happens-before visibility before it reads the top boundary.

### 1.5 What is a spurious wakeup, and does this code handle it?

**Status: HARD CORRECTNESS REQUIREMENT.**

A spurious wakeup occurs when `pthread_cond_wait()` returns even though no
state change has made this thread's predicate true. A non-spurious broadcast
can have the same practical effect for an ineligible row: the broadcast is
real, but it was caused by progress that does not satisfy this row's particular
column dependency.

The code handles both cases correctly at **`src/wavefront.c:55-61`**:

```c
while (sync->row_progress[row - 1] <= col) {
    result = pthread_cond_wait(&sync->condition, &sync->mutex);
    /* error handling omitted here */
}
```

The critical word is `while`, not `if`. Every return from
`pthread_cond_wait()` leads to another predicate test under the mutex. If the
row above has not published column `col`, the thread sleeps again. An `if`
would let a spurious or irrelevant wakeup fall through and read an
unreconstructed top boundary.

The randomized wavefront test also checks the predicate after every wait while
holding the same mutex (`test/test_wavefront.c:52-71`) and repeats randomized
row creation and pauses 25 times (`test/test_wavefront.c:118-160`).

---

## 2. “What if we changed X?” hypothetical questions

### 2.1 What breaks if `pthread_cond_signal()` replaces `pthread_cond_broadcast()`?

**Status: current broadcast is a DESIGN CHOICE, but some correct mechanism for
waking every newly eligible predicate is a HARD CORRECTNESS REQUIREMENT.**

All rows of one plane share one `sync->condition` (`src/wavefront.h:16-20`), but
they wait for different predicates. POSIX does not require
`pthread_cond_signal()` to choose an eligible waiter.

Concrete scenario:

1. Row 1 has published columns 0 through 4 and is asleep waiting for column 5,
   so its predicate is `row_progress[0] > 5`.
2. Row 2 has also advanced through column 4 and is asleep waiting for column 5,
   so its predicate is `row_progress[1] > 5`. At this point
   `row_progress[1] == 5`, so row 2 is not eligible.
3. Row 0 reconstructs and publishes column 5. Now row 1 is eligible, but row 2
   is still not, because row 1 has not yet published column 5.
4. A single `pthread_cond_signal()` is allowed to wake row 2.
5. Row 2 reacquires the mutex, sees that its predicate remains false, and
   sleeps again.
6. If column 5 was row 0's last publication, no future signal from row 0 is
   guaranteed. Row 1 remains asleep despite being eligible. Because row 1
   cannot run, it cannot publish for row 2. The plane deadlocks.

`pthread_cond_broadcast()` at `src/wavefront.c:82` wakes all waiters; each
rechecks its own predicate in the `while` at line 55. The cost is unnecessary
wakeups and mutex contention—the *thundering herd* recorded in
`DESIGN_LOG.md:84-93`.

Broadcast is not the only possible correct design. Per-row condition variables,
semaphores, explicit waiter lists, or targeted notifications could avoid the
herd, but they add state and scheduling complexity. Simply replacing broadcast
with signal while keeping one shared condition variable is not safe.

### 2.2 What if `SYNC_GRANULARITY` covered the entire row?

**Status: DESIGN CHOICE; correctness is retained only because every block is
still reconstructed before the final truthful publication.**

The default is one block (`src/wavefront.h:8-10`). Production publishes when
`(block_column + 1) % SYNC_GRANULARITY == 0` or at the final block
(`src/main.c:207-210`). If granularity were `C`, the number of block columns,
row 0 would process and reconstruct all `C` blocks before publishing
`row_progress[0]=C`. Row 1, which needs `row_progress[0] > 0` even for its first
block, could not start until row 0 completed. Row 2 would then wait for all of
row 1, and so on.

Within one plane, the execution becomes:

```text
entire row 0 -> entire row 1 -> ... -> entire row R-1.
```

The maximum intra-plane concurrency falls from `min(R,C)` ideal diagonal
blocks to one active row. The critical path expands from `R+C-1` block times to
approximately `R*C` block times. Multiple independent frames and planes may
still overlap, so the whole program is not necessarily single-threaded, but the
intra-plane wavefront has disappeared.

This resembles flat row tiling in storage and job shape, but not independent
parallel row tiling: causal top dependencies force the row tiles to execute
serially. The granularity experiment confirmed the same trend at smaller
values. At four workers, granularity 1 averaged 0.162 s, versus 0.190 s for
granularity 2 and 0.182 s for granularity 4 (`DESIGN_LOG.md:250-270`). Fewer
locks and broadcasts did not compensate for the narrower wavefront.

### 2.3 What if a spinlock replaced the mutex around `row_progress`?

**Status: DESIGN CHOICE, subject to the hard requirement for synchronization
and memory ordering.**

A spin-based design can be faster when:

- the expected wait is only a few tens or hundreds of CPU cycles;
- there are enough physical cores for every spinning thread;
- the protected owner is already running and will release almost immediately;
- avoiding a kernel-assisted sleep/wakeup is worth burning CPU time.

That is not the dominant wait in this project. A dependent row may wait for the
row above to perform residual formation, an 8x8 DCT, quantization, RLE, inverse
DCT, clipping, and 64 reconstructed-pixel writes. The production sequence spans
`src/main.c:173-205`, and the DCT itself contains two cubic loop nests
(`src/codec.c:183-203`). The wait is therefore much longer than the tiny
critical section that reads or updates `row_progress`.

If “spinlock” means threads repeatedly poll `row_progress`, waiting threads
consume cores that could execute the predecessor blocks they need. With more
threads than cores, a spinner may even delay the producer through scheduler
contention. Cache-line bouncing on `row_progress` also increases. A hybrid
adaptive mutex or brief spin-then-sleep policy could be tested, but a pure spin
wait is likely worse for these block-sized dependencies.

A spinlock used only to protect a quick load/store does not by itself replace
the condition variable's sleep/wakeup protocol. A complete redesign would need
atomic progress loads/stores with explicit acquire/release ordering and a
backoff/parking mechanism.

### 2.4 What if blocks also depended on right or bottom neighbours?

**Status: HARD CORRECTNESS PROPERTY of the dependency graph.**

Adding the opposite direction to an existing dependency creates a cycle. The
smallest horizontal example is a 1x2 block grid:

```text
A = (0,0)       B = (0,1)
```

- A depends on B because B is A's right neighbour: `B -> A`.
- B depends on A because A is B's left neighbour: `A -> B`.

Therefore `A -> B -> A` is a directed cycle. A waits for B, while B waits for
A; neither can be the first reconstructed block.

The smallest vertical example is a 2x1 grid:

```text
A = (0,0)
B = (1,0)
```

- A depends on B as its bottom neighbour: `B -> A`.
- B depends on A as its top neighbour: `A -> B`.

Again there is a two-node cycle. No topological order exists, so adding more
threads cannot solve it.

If the design used **only** right and bottom dependencies, a reverse scan from
bottom-right could be valid. The impossibility arises when right is added to
left, or bottom is added to top. This project deliberately reads only top and
left reconstructed pixels in `predict_from_neighbors()`
(`src/codec.c:301-319`), preserving the DAG proved in Section 1.2.

### 2.5 What if 16 worker threads run on a four-core machine?

**Status: DESIGN CHOICE and performance effect, not normally a correctness
failure.**

Only about four CPU-bound workers can execute simultaneously on four cores.
The additional runnable threads cause:

- more context switches and scheduler bookkeeping;
- displacement of useful DCT and frame data from caches;
- more contention on the job-queue mutex (`src/threadpool.c:69-123`);
- more threads awakened by each wavefront broadcast, increasing the thundering
  herd;
- more time spent ready-but-not-running after a dependency is satisfied.

Oversubscription can help workloads that frequently block on independent I/O,
because another thread can use the core. Here the heavy work—DCT,
quantization, RLE, reconstruction, and Huffman coding—is CPU-bound. File output
is already separated into a writer thread. Therefore performance should
usually plateau and may regress beyond the number of useful hardware threads.

The measured diminishing returns appeared before such extreme
oversubscription: 1, 2, 4, and 8 workers achieved 1.000x, 2.045x, 2.872x, and
3.600x speedup (`DESIGN_LOG.md:215-233`). The 8-worker efficiency was 45%, so
16 workers on four cores would not plausibly produce linear improvement.

### 2.6 What if two frames shared the same `WavefrontSync`?

**Status: HARD CORRECTNESS REQUIREMENT: synchronization state must be scoped to
the data whose readiness it describes.**

Suppose frame A and frame B share one `row_progress` array. A's row 0 publishes
column 5 and sets `row_progress[0]=6`. B's row 1 waiting for B's top block at
column 5 now observes `row_progress[0] > 5` and proceeds, even if B's row 0 has
not reconstructed that block. B reads zero or stale data from **B's** `recon`
buffer because progress produced by A falsely satisfied B's predicate.

The mutex may prevent a C-level data race on the integer itself, but it does not
prevent this logical cross-frame readiness race. Output becomes
schedule-dependent and may diverge from decoder prediction.

The actual ownership prevents this: every `FrameWork` embeds its own
`WavefrontSync sync[3]` (`src/main.c:24-35`), initializes all three objects in
`create_frame_work()` (`src/main.c:289-298`), and each `Job` points to
`&frames[frame_index]->sync[plane]` (`src/main.c:403-410`). Thus synchronization
is scoped by both frame and plane.

### 2.7 What if `reconstruct_block()` ran after `wavefront_mark_done()`?

**Status: HARD CORRECTNESS REQUIREMENT.**

Consider worker T0 encoding block `(r,c)` and worker T1 encoding `(r+1,c)`:

1. T1 is sleeping in `wavefront_wait_for_dependency()` because
   `row_progress[r] <= c`.
2. T0 completes quantization/RLE but calls `wavefront_mark_done()` before
   reconstruction.
3. `wavefront_mark_done()` stores `row_progress[r]=c+1`, broadcasts, and unlocks
   the mutex (`src/wavefront.c:75-83`).
4. T1 wakes, reacquires the mutex, observes `row_progress[r] > c`, and returns.
5. T1 calls `predict_from_neighbors()` and reads the bottom boundary of block
   `(r,c)` from `frame->recon` (`src/codec.c:301-308`). T0 has not written it
   yet, so T1 reads initialization zeros, stale pixels, or a partially written
   block.
6. T1 forms its residual using the wrong prediction.
7. T0 finally reconstructs `(r,c)`, too late to repair T1's residual or entropy
   data.

If T0 and T1 access the same boundary pixels concurrently, this can also be a
true memory race. Even if writes happen to be atomic bytes, T1 can observe a
mixture of old and new boundary values.

The code correctly reconstructs first (`src/main.c:204-205` and
`src/threadpool.c:332`) and publishes afterward (`src/main.c:207-210` and
`src/threadpool.c:335-337`). Progress is therefore a release promise: every
pixel a dependent block may read is complete and visible. This ordering is
documented in `DESIGN_LOG.md:95-123`.

### 2.8 What if the mutex were removed because aligned `int` assignment is atomic on x86?

**Status: HARD CORRECTNESS REQUIREMENT.**

Hardware atomicity of a particular aligned store is insufficient for four
separate reasons:

1. **The C memory model:** `row_progress` is declared as ordinary `int`
   (`src/wavefront.h:16-20`), not `_Atomic int`. A concurrent unsynchronized
   read and write is a data race and therefore undefined behaviour in C,
   regardless of what x86 usually does electrically.
2. **Lost wakeups:** atomic assignment does not close the gap between checking
   the predicate and going to sleep. Section 1.4's check-notify-sleep race still
   exists.
3. **Visibility of `recon`:** the progress integer is a publication flag for 64
   pixel writes performed by `reconstruct_block()`. Plain atomicity of the flag
   does not, by itself, impose the release/acquire ordering needed to make those
   earlier writes visible to the consuming thread.
4. **Condition-variable contract:** `pthread_cond_wait()` is designed around a
   mutex-protected predicate. Removing the mutex while retaining the condition
   variable violates the protocol.

A correct lock-free alternative would require C11 atomic progress variables,
release stores after reconstruction, acquire loads before prediction, and a
futex/semaphore/event mechanism that cannot lose a wakeup. That is possible,
but it is a different and substantially more complex design—not “remove the
mutex and trust x86.”

### 2.9 What happens for a frame with height 1?

**Status: actual input rejection is a HARD INPUT CONTRACT; the one-block-row
interpretation is a MATHEMATICAL DEGENERATION.**

Literally, a one-pixel-high frame is rejected by this program. `main()` requires
width and height to be positive multiples of 16 (`src/main.c:320-329`), which
also accommodates YUV420 chroma dimensions and complete 8x8 blocks.

If “height 1” means **one block row**, then `R=1`. Every job has `row==0`, so
`wavefront_wait_for_dependency()` returns immediately
(`src/wavefront.c:47-49`). There is no top dependency. The single row still has
left dependencies, and its one row worker processes columns sequentially.

The wavefront therefore degenerates to a one-dimensional left-to-right chain:

```text
(0,0) -> (0,1) -> ... -> (0,C-1).
```

The maximum intra-plane parallelism is one and the critical path is `C` blocks.
Other frames or planes can still execute concurrently.

### 2.10 What synchronization would P-frames require?

**Status: DESIGN CHOICE, with a HARD requirement that every referenced
reconstructed region be ready and remain alive.**

The current frames are intra-coded and therefore independent. That is why
`main()` can submit row jobs from many frames without reference-frame waits
(`src/main.c:398-419`). A P-frame would use motion-compensated pixels from a
previous reconstructed frame, introducing edges across frames.

The simplest correct extension is a **whole-reference-frame barrier**:

- each frame owns a mutex, condition variable, and `reconstruction_complete`
  flag;
- P-frame jobs wait until their reference frame is fully reconstructed;
- after the barrier, the existing per-plane `WavefrontSync` controls top/left
  dependencies inside the P-frame;
- reference-counting keeps the reference `FrameWork` and its `recon` buffer
  alive until all dependants finish.

That design is simple but removes frame-level parallelism along a P-frame chain.
Different GOPs could still overlap.

A finer-grained design would maintain a per-reference-frame block/region
readiness map. Before encoding a P-block, a worker would wait for every
reference block touched by its motion-compensation window **and** wait on the
current frame's `WavefrontSync`. Because motion vectors can point to arbitrary
regions, a single `row_progress` frontier may be insufficient; dependency
counters or per-tile readiness events are more natural. The combined graph must
remain acyclic in frame order. The reorder buffer only solves output ordering;
it does not prove reference pixels are ready.

---

## 3. Mathematical foundations

### 3.1 Derive the 2D DCT-II and the separable implementation

**Status: MATHEMATICAL PROPERTY; the direct versus separable implementation is
a DESIGN CHOICE with a clear complexity advantage.**

For an `N`-sample 1D signal `x[n]`, the orthonormal DCT-II is

```text
X[k] = alpha(k) * sum from n=0 to N-1 of
       x[n] * cos(pi(2n+1)k / (2N)),

alpha(0) = 1/sqrt(N),
alpha(k) = sqrt(2/N) for k > 0.
```

For `N=8`, these scale factors become `1/sqrt(8)` for DC and `1/2` for AC,
exactly matching `build_dct_basis()` at `src/codec.c:154-168`.

For an `N x N` image block `f[x,y]`, apply the 1D transform along one axis and
then the other:

```text
T[x,v] = alpha(v) * sum from y=0 to N-1 of
         f[x,y] * cos(pi(2y+1)v / (2N))

F[u,v] = alpha(u) * sum from x=0 to N-1 of
         T[x,v] * cos(pi(2x+1)u / (2N)).
```

Substituting the first equation into the second gives

```text
F[u,v] = alpha(u)alpha(v)
         * sum(x=0..N-1) sum(y=0..N-1) f[x,y]
         * cos(pi(2x+1)u/(2N))
         * cos(pi(2y+1)v/(2N)),
```

which is the direct 2D DCT-II. The equality follows because the 2D basis is the
outer product of two 1D cosine bases. In matrix form, if `B` is the 1D DCT
matrix, `F = B f B^T`.

The code implements precisely this separation:

- rows are transformed into `row_result` at `src/codec.c:183-191`;
- columns of that intermediate are transformed into `output` at
  `src/codec.c:194-203`.

Operation-count comparison:

- A direct 2D implementation computes `N^2` output coefficients. Each output
  sums `N^2` input contributions: `N^4` multiply-accumulate terms. At `N=8`,
  this is `8^4 = 4096` terms.
- One naive 1D DCT costs `N` outputs times `N` inputs, or `N^2` terms. The
  separable 2D transform performs `N` row transforms plus `N` column
  transforms: `2N * N^2 = 2N^3`. At `N=8`, this is
  `2 * 8^3 = 1024` terms.

Thus the full 2D complexity changes from `O(N^4)` to `O(N^3)`, with one quarter
as many core terms at 8x8. This is the reasoning recorded in
`DESIGN_LOG.md:35-47`.

### 3.2 Why does a constant block have only a DC coefficient?

**Status: MATHEMATICAL PROPERTY.**

Let every sample in the `N x N` block equal a constant `K`. Substitute
`f[x,y]=K` into the 2D formula:

```text
F[u,v] = K alpha(u)alpha(v)
         * [sum_x cos(pi(2x+1)u/(2N))]
         * [sum_y cos(pi(2y+1)v/(2N))].
```

For frequency zero, every cosine is 1, so each sum is `N`. Therefore

```text
F[0,0] = K * (1/sqrt(N))^2 * N^2 = K*N.
```

For `N=8`, the orthonormal DC coefficient is `8K`.

For any nonzero frequency index, the sampled cosine basis has equal positive
and negative contribution over the `N` positions and is orthogonal to the
constant basis. Its sum is zero. Therefore if `u>0` or `v>0`, at least one of
the bracketed cosine sums is zero and `F[u,v]=0`.

In exact mathematics, all 63 AC coefficients are exactly zero. The floating
implementation may produce tiny round-off residues because `cos()` and `double`
are finite precision; quantization normally rounds these to zero.

### 3.3 What is the quantization formula, and why do high frequencies become zero?

**Status: MATHEMATICAL PROPERTY; the particular table is a DESIGN CHOICE.**

For DCT coefficient `F[u,v]` and quantizer entry `Q[u,v]`, the encoder computes

```text
q[u,v] = round(F[u,v] / Q[u,v]).
```

This is implemented at `src/codec.c:245-266`; the division is at lines 254-255
and `lround()` is at line 262. Reconstruction computes

```text
F_hat[u,v] = q[u,v] * Q[u,v],
```

at `src/codec.c:325-338`.

Rounding produces zero whenever

```text
|F[u,v] / Q[u,v]| < 1/2,
```

or equivalently

```text
|F[u,v]| < Q[u,v]/2.
```

Larger `Q[u,v]` therefore creates a wider interval of coefficient magnitudes
that map to zero. The table at `src/codec.c:11-20` generally assigns larger
divisors to higher spatial frequencies, where natural-image residuals also tend
to have smaller energy. These two effects combine to create a zero-heavy AC
tail. That improves compression but discards more high-frequency detail.

### 3.4 State Amdahl's Law and apply it to the measured encoder

**Status: MATHEMATICAL MODEL; interpreting an effective serial fraction from
short measurements is a DESIGN CHOICE and must be qualified.**

If fraction `f` of execution is inherently serial and fraction `1-f` is evenly
parallelizable across `P` workers, normalized time is

```text
T(P) = f + (1-f)/P.
```

Therefore speedup is

```text
S(P) = 1 / [f + (1-f)/P].
```

As `P` approaches infinity, the parallel term approaches zero:

```text
S(infinity) = 1/f.
```

This means even unlimited processors cannot accelerate the serial part.

The original benchmark measured `S(8)=3.600` from 0.540 s at one worker and
0.150 s at eight workers (`DESIGN_LOG.md:215-220`). If we fit the simple Amdahl
model to that point:

```text
1/3.600 = f + (1-f)/8
0.27778 = 0.125 + 0.875f
f = (0.27778 - 0.125) / 0.875
f approximately 0.1746.
```

The corresponding asymptotic limit would be

```text
S(infinity) approximately 1/0.1746 approximately 5.73x.
```

This `17.5%` is an **effective serial/overhead fraction**, not a direct
measurement proving that prediction and reconstruction consume exactly 17.5%
of the code. It absorbs wavefront stalls, frame-wide Huffman coding, output
ordering, I/O, queue operations, mutex/condition-variable overhead, and timing
noise. Prediction/reconstruction are not globally serial; they form dependency
chains with parallel diagonals. The short five-run benchmark also varied, so a
precise fitted fraction would overstate the evidence. The defensible viva claim
is that the measured 8-thread result is consistent with a substantial effective
serial/span component and diminishing returns.

### 3.5 What is the wavefront critical path?

**Status: MATHEMATICAL PROPERTY.**

In the `R x C` top/left DAG, a longest dependency path from `(0,0)` to
`(R-1,C-1)` must move down `R-1` times and right `C-1` times. Counting the first
block, its length is

```text
L = (R-1) + (C-1) + 1 = R + C - 1 blocks.
```

Equivalently, the graph has `R+C-1` diagonal levels. If one block takes ideal
unit time `t_b`, no scheduler can complete that plane faster than

```text
T_min >= (R+C-1) * t_b,
```

even with infinitely many processors. With `P` processors and total work
`R*C*t_b`, the stronger lower bound is

```text
T_P >= max(R*C*t_b/P, (R+C-1)*t_b).
```

The ideal average-parallelism ceiling from work divided by span is

```text
R*C / (R+C-1).
```

For the 1280x720 luma plane, `R=90`, `C=160`:

```text
critical path = 249 blocks
work/span = 14,400/249 approximately 57.8.
```

For either chroma plane, `R=45`, `C=80`:

```text
critical path = 124 blocks
work/span = 3,600/124 approximately 29.0.
```

These are dependency-graph limits, not predicted speedups. Whole-row jobs,
unequal stage costs, finite workers, synchronization, multiple planes/frames,
Huffman finalization, and I/O make real time larger. The ramp-up/ramp-down
explanation in `DESIGN_LOG.md:222-233` is the operational manifestation of this
span.

### 3.6 Why is Huffman coding within one bit of entropy, and why does RLE first help?

**Status: MATHEMATICAL PROPERTY; the exact symbol representation is a DESIGN
CHOICE.**

For symbols with probabilities `p_i`, Shannon entropy is

```text
H = -sum_i p_i log2(p_i) bits/symbol.
```

Any uniquely decodable binary code has expected length at least `H`. Huffman's
algorithm repeatedly combines the two least-probable symbols, constructing an
optimal binary prefix code for the known symbol probabilities. For a
non-degenerate alphabet, its expected length `L` satisfies

```text
H <= L < H + 1.
```

The intuitive reason for the upper bound is that ideal information lengths are
`-log2(p_i)`, which are usually fractional, while prefix-code lengths must be
whole numbers. Choosing lengths near the ceiling of the ideal values costs less
than one additional bit on average; Huffman is at least as good as that valid
construction for a binary prefix code.

For the degenerate one-symbol case, entropy is zero but this implementation
still emits one bit per symbol (`src/codec.c:558-560`), so `L=1=H+1`; the usual
strict upper-bound statement assumes at least two positive-probability symbols
or permits a zero-length representation when the symbol count is known.

This project's Huffman symbols are not raw coefficients. `rle_encode()` scans
the 8x8 quantized block in zig-zag order and emits `(zero_count, value)` pairs
(`src/codec.c:340-380`). A long zero tail becomes one compact pair with
`value=0` (`src/codec.c:374-377`) instead of dozens of independent zero
symbols. `huffman_encode()` counts frequencies of distinct `RLEPair`s
(`src/codec.c:471-485`), repeatedly combines the two least frequent active
nodes (`src/codec.c:503-538`), and emits the resulting prefix bits
(`src/codec.c:540-569`). Production collects pairs from the whole frame before
calling it (`src/main.c:95-126`), providing a larger frequency sample.

RLE before Huffman helps in two ways:

1. It reduces the number of symbols by collapsing runs created by quantization.
2. It concentrates probability on recurring run/value patterns, allowing
   Huffman to give them short codes.

The `< H+1` theorem applies to the symbol stream's expected code length; the
actual file also transmits the symbol/frequency table
(`src/codec.c:610-642`), frame headers, and byte padding. On small frames that
metadata means total file size need not be within one bit per source
coefficient of an entropy bound.

---

## 4. Failure-mode and debugging questions

### 4.1 Give a race that incorrect `WavefrontSync` would cause. Why are 12 byte-identical runs strong evidence but not proof?

**Status: synchronization is a HARD CORRECTNESS REQUIREMENT; the test strategy
is a DESIGN CHOICE.**

A concrete race is premature top-boundary access. If row `r+1` observes
progress for column `c` before row `r` completes `reconstruct_block(r,c)`, the
lower worker reads `recon` while the upper worker writes the same boundary.
Depending on timing, the predictor may see zeros, old values, or a mixture of
new and old bytes. Its residual and quantized coefficients change, causing a
different compressed bitstream and potentially decoder drift.

The project attacks this in several ways:

- the top predicate is protected by `sync->mutex` and a `while` loop
  (`src/wavefront.c:51-62`);
- reconstruction precedes publication (`src/main.c:204-210`);
- `test/test_wavefront.c` injects randomized pauses and row-creation order over
  25 runs, verifying that no row returns before its dependency
  (`test/test_wavefront.c:47-79` and `test/test_wavefront.c:118-160`);
- `test/test_threadpool.c` builds a plain serial baseline, then clears `recon`,
  executes threaded row jobs, and uses `memcmp()` on the complete reconstructed
  buffer (`test/test_threadpool.c:25-81` and `test/test_threadpool.c:83-119`);
- that test runs 12 times each with 1, 2, and 4 workers
  (`test/test_threadpool.c:9` and `test/test_threadpool.c:146-148`).

That 12-run unit/integration test exercises the luma-only `encode_row()` helper;
the chroma bytes remain identically initialized on both sides. The separate
production-scale gate exercised Y, U, and V through `encode_plane_row()` and
compared one-worker and four-worker retained bitstreams across ten runs
(`DESIGN_LOG.md:192-205`). The two tests support different layers and should not
be described as if the 12-run helper alone proved the full-colour pipeline.

Byte-identical results across worker counts are strong evidence because thread
scheduling changes while the expected deterministic codec result does not. A
dependency race that affects prediction is likely to propagate into many later
blocks, making a full-buffer comparison sensitive.

It is not absolute proof because only finitely many schedules, inputs, frame
sizes, compiler transformations, and hardware behaviours execute. A rare
interleaving may never occur in 12 runs. Absolute assurance would require a
formal proof of the algorithm and its C/POSIX memory-order implementation, or
exhaustive model checking of every possible interleaving and state. Exhaustive
checking is impractical for production frames because the number of schedules
grows combinatorially with threads, blocks, and events. Even a proof must state
assumptions about the pthread implementation, compiler, allocation success, and
valid inputs.

The correct claim is therefore: **the design has a clear happens-before
argument, deterministic stress tests, production-scale comparisons, and clean
dynamic race analysis; together these provide strong evidence for the tested
configuration, not a mathematical proof for all executions.** This matches
`DESIGN_LOG.md:154-175`.

### 4.2 What would a real Helgrind race report look like, and what does “0 errors from 0 contexts” prove?

**Status: dynamic race checking is a DESIGN/VERIFICATION CHOICE; data-race
freedom is a HARD CORRECTNESS REQUIREMENT.**

A typical genuine Helgrind report includes:

```text
==PID== Possible data race during read of size 4 at 0x...
==PID==    at 0x...: wavefront_wait_for_dependency (src/wavefront.c:...)
==PID==    by 0x...: encode_plane_row (src/main.c:...)
==PID==
==PID== This conflicts with a previous write of size 4 by thread #...
==PID==    at 0x...: wavefront_mark_done (src/wavefront.c:...)
==PID== Locks held: ...
==PID== Address ... is ... inside a block allocated at ...
```

For lock-order problems, it reports the observed lock acquisition order and a
conflicting order that could form a cycle. Useful fields are the access size,
address, both thread stack traces, creation sites, and locks held at each
access. That evidence points to the exact shared object and missing
happens-before edge.

`0 errors from 0 contexts` means Helgrind observed no reportable race or lock
error on the operations that actually executed. It does **not** mean the whole
source is proven race-free. If the test never called the faulty function, never
reached the contested branch, or ran only one worker, a latent race could remain
invisible.

The retained evidence shows relevant paths did execute:

- `benchmark/helgrind_final_results.txt:1-9` identifies Helgrind 3.18.1, full
  history, fair scheduling, a four-thread `video_compressor`, retained-file
  encode/decode, and `0 errors from 0 contexts` for encoder and decoder.
- `benchmark/helgrind_granularity_results.txt:1-10` records both the real
  four-thread encoder and `test/test_threadpool` at granularities 2 and 4.
- The tested encoder path creates the pool (`src/main.c:391`), submits real
  frame/plane/row jobs (`src/main.c:398-419`), executes
  `wavefront_wait_for_dependency()` and `wavefront_mark_done()` through
  `encode_plane_row()` (`src/main.c:167-210`), finalizes the frame, and submits
  it to the reorder writer (`src/main.c:95-130`).
- The final release audit records a 32x32 YUV420 fixture, four encoder threads,
  a decoded output, and 49.15 dB round-trip PSNR
  (`DESIGN_LOG.md:313-330`). This is evidence of end-to-end path execution, not
  merely process startup.

The earlier development-stage Helgrind run and the final production run must
not be conflated. `DESIGN_LOG.md:160-169` explicitly notes that the old entry
point was serial at that stage and that the later threaded production path was
rechecked.

### 4.3 What if output differences appear only under high system load?

**Status: debugging procedure is a DESIGN CHOICE; deterministic output and
race-free publication are HARD requirements.**

Timing-sensitive differences make a missing happens-before relationship the
first suspect. High load widens scheduling gaps and changes which worker wakes
or is preempted. The most likely areas are:

1. reconstruct-versus-publish ordering around `src/main.c:204-210`;
2. the `row_progress` predicate and mutex at `src/wavefront.c:51-83`;
3. a `FrameWork` or frame-buffer lifetime error while jobs still hold pointers;
4. accidental sharing of a `WavefrontSync` between frames or planes;
5. frame-finalization racing the final row count at `src/main.c:229-240`;
6. reorder-slot ownership transfer at `src/reorder.c:117-145`.

A disciplined isolation plan is:

1. **Fix the input and hash every output.** Run the same raw file repeatedly and
   record the first differing output byte/frame.
2. **Reduce the concurrency dimension.** Compare one worker with multiple
   workers. Then compare one frame with multiple frames. If one worker is
   stable but multiple workers fail, the codec math is less likely than shared
   state. If one frame is stable but several fail, inspect frame ownership,
   sync scoping, finalization, and reorder logic.
3. **Locate the first divergent block.** Add temporary checksums for each
   `work->blocks[block_index]` after `src/main.c:193-197` and for each
   reconstructed block after `src/main.c:204-205`. The earliest difference is
   more informative than the final propagated corruption.
4. **Perturb the suspicious window.** Insert test-only `sched_yield()` or short
   randomized sleeps immediately before and after reconstruction/publication to
   amplify the interleaving. The existing randomized-pause method in
   `test/test_wavefront.c:31-79` is a model for this.
5. **Run dynamic tools on the failing path.** Use Helgrind with full history and
   a nonzero error exit code, and use ThreadSanitizer where the platform supports
   the complete pipeline. Confirm the command uses multiple workers and reaches
   the frame size/path that fails.
6. **Vary `SYNC_GRANULARITY`.** If the failure moves or disappears, inspect
   publication semantics and boundary readiness rather than assuming the
   granularity itself is the root cause.
7. **Check undefined behaviour and initialization.** Build with warnings as
   errors plus AddressSanitizer/UndefinedBehaviorSanitizer. A high-load-only
   symptom can also be use-after-free or uninitialized memory whose timing
   merely resembles a synchronization race.

Do not “fix” the symptom by adding arbitrary sleeps. Sleeps change probability;
they do not create a correctness guarantee. The final repair must restore an
explicit ownership or happens-before invariant and then pass the serial
baseline comparison, randomized tests, production disk round trip, and dynamic
race analysis.

---

## Compact viva recap

- **Why causal top+left?** It gives edges that strictly increase `r+c`, so the
  dependency graph is a DAG.
- **Why a wavefront?** It executes the DAG's ready diagonals instead of waiting
  for a whole preceding row.
- **What does `row_progress[r]` mean?** The number of columns in row `r` whose
  reconstructed pixels have been truthfully published; `mark_done(r,c)` stores
  at least `c+1` (`src/wavefront.c:79-80`).
- **Why mutex plus condition variable?** The mutex makes predicate checking,
  sleep transition, progress publication, and reconstructed-pixel visibility
  coherent; the condition variable lets long-waiting threads sleep.
- **Why broadcast?** One shared condition has heterogeneous predicates; all
  waiters must recheck so an eligible row is not left asleep.
- **Why reconstruct before publish?** Progress is a promise that the boundary
  pixels are complete and visible.
- **Why is scaling sublinear?** The diagonal ramps up and down, critical-path
  work remains, frame Huffman/output have serial components, and queue/wakeup
  overhead grows.
- **What did testing establish?** The reference and 1/2/4-worker outputs matched
  byte-for-byte over 12 runs each; production 1-versus-4 output matched over 10
  retained disk runs; relevant four-thread Helgrind paths reported zero errors.
- **What did testing not establish?** It did not exhaust every input, failure,
  machine, compiler transformation, or thread interleaving.

