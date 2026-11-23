---
id: hades
title: The Hades Garbage Collector
---

Hades is a garbage collector for Hermes that aims to improve pause times by an
order of magnitude over GenGC. The main principle Hades uses to achieve those
low pause times is that most of the garbage collection work happens in a
background thread concurrently with the interpreter running JavaScript code.
This is distinct from GenGC, which only runs on a single thread which is shared
with the interpreter.

# Enabling Hades

In local builds on the command line using CMake (which forwards to CMake), Hades
is the default GC used, and currently the only GC supported for production use.
The GC being used is controlled by the CMake variable `-DHERMESVM_GCKIND=value`.

To use a pre-built package of Hermes with Hades enabled, check the
[Releases page on Github](https://github.com/facebook/hermes/releases).
As of right now, there aren't any available, but we'll be making one available
with v0.8 and later.

## Check Which GC is Used

If you want to know what GC is being used in your application, you can find out
with some JS:

```js
const gcName = HermesInternal.getRuntimeProperties().GC;
// If you're running Hermes on the command line, use print.
print(gcName);
// If you're running Hermes in some kind of framework like React Native,
// console.log should exist.
console.log(gcName);
```

This will print one of:

- `"hades (concurrent)"`: You're using Hades in concurrent mode
- `"hades (incremental)"`: You're using Hades in [incremental mode](#incremental-mode)

# Basics

Most of the basic heap structure of Hades is similar to GenGC, so it is
recommended to read the [GenGC Documentation](./GenGC.md) first, in particular
the following sections:

- [Heap Segments](./GenGC.md#heap-segments)
- [Object Types](./GenGC.md#object-types)
- [Generations](./GenGC.md#generations)
- [Write Barriers](./GenGC.md#write-barriers)

# Generations

Similarly to GenGC, Hades also has two generations: the **Young Generation**
(YG) and **Old Generation** (OG). Allocations go initially into YG, and if they
survive the first collection they go into OG. YG works exactly the same as
GenGC, but OG has a different allocation strategy that allows for gaps.

## Freelist Allocator

Hades's OG is a list of heap segments, and each heap segment maintains a
**Free List** of empty space. Each **Free List Cell** points to the next cell,
called an explicit free list. This is opposed to an implicit free list, where
the length is used to traverse both free and used cells. A free list is used
because it allows empty space to be left where it is, without requiring
compaction. This is a requirement for concurrent allocations and sweeping.

Furthermore, the free list is **size-segregated**, meaning each size class gets
a separate free list. In other words, cells of size `N` only point to other
cells of size `N`. This allows an allocation of size `N` to be satisfied
instantly with the head of the free list. Each free list head is stored at an
index in a fixed-size array for small cell sizes. These are known as
**buckets**.

Hades does not do any rounding up of sizes beyond the required heap alignment
of 8 bytes. This means there is one bucket for each multiple of 8, up to 2048
bytes. From 2048 bytes to the maximum heap segment size (4 MiB) the buckets go
by powers of 2. For large buckets, we store cells that are greater than or equal
to the size bucket, but less than the next power of 2. Cells that need less than
the size of the free list cell **carve** out a small piece of the cell, and put
the remaining piece on a the free list corresponding to its new size.

Due to having these free lists be per-segment, we need a quick way to find which
segment has free space for a given size. We do this with a series of bit arrays,
where bits are flipped to 0 as a free list is exhausted in a segment, and
flipped back to 1 when sweeping frees some cells. We have per-segment free lists
so that sweeping and compaction (which also operate on a per-segment basis) can
destroy them efficiently and create a new list. It can do this to easily
coalesce adjacent free regions.

# Collection Cycles

Hades has two different types of collections: a YG collection (YG GC) and an
OG collection (OG GC). The former is almost exactly the same as GenGC's, so we
won't repeat it here. The OG GC is very different though, because it runs
concurrently in a background thread.

For the purposes of distinguishing these two threads, we'll name them as
follows:

- **Mutator Thread**: The thread running the JS interpreter
- **GC Thread**: The thread running any GC operations such as marking or
sweeping

Note that currently there is only ever a single GC thread at any point in time.
We also cache the thread and reuse it instead of making a new one for each
collection.

There are three different locks used throughout the GC:

- The **GC Mutex** is used to protect structures like mark bits, card tables,
and the free lists
- The **WeakRef Mutex** is used to protect structures used during weak ref
marking
- The **Write Barrier Mutex** is used to protect a small buffer used by write
barriers and concurrent marking

The GC mutex is used to protect most things, as they tend to all be accessed at
the same time. There was no need for finer grained locks yet, with the exception
of the write barrier mutex because write barriers are executed all the time.

An OG GC is started once the OG is about 75% full. We start it a bit early so
that it can complete sweeping before reaching 100% full and avoid blocking any
allocations.

## Mark Phase

The first step of an OG GC is to mark all of the roots of the object graph.
We only ever start an OG GC when YG is empty, so there's no need to mark any of
YG.

Marking an object consists of the following steps:

- Using mark bits, check if an object has been visited already
- If it has been visited already, there's nothing to do
- If not, push it onto a **mark stack** that will be drained later
- Set its mark bit
- If the object pointed to is a WeakMap, put it onto a separate stack
(see [Weak Map Resolution](./GenGC.md#weak-map-resolution))

Draining the mark stack works as follows:

- Acquire a lock on the GC mutex
- Check if the write barrier buffer has objects that need to be marked, if so,
add them to the mark stack
- While we've marked fewer than a certain number of bytes, defaults to 8 KiB
  - Pull one object off the mark stack
  - Get its type metadata (see [Object Types](./GenGC.md#object-types))
  - Use the metadata to find pointers to other objects
  - Those other objects will be pushed on the stack
- Release the lock on the GC mutex

This can run almost entirely uninterrupted on the background thread since very
few things need to acquire the GC mutex. The most common way to interrupt
marking is when YG fills up, as it requires the GC mutex in order to evacuate
YG.

### Write Barriers

There's an important race condition to consider when thinking about concurrent
marking: what happens if a pointer is modified while we're reading it?

There are two different races that are possible here:

- A non-atomic read of the pointer might race with a non-atomic write, and the
reads or writes might "tear" (meaning you see only part of the write)
- You might miss marking the old value or the new value

The first is handled on 64-bit platforms because all of the reads are of a 64
bit value, which can be atomically handled cheaply on a 64-bit CPU. See the
[Incremental Mode](#incremental-mode) section for what we do on a 32-bit CPU.

The second problem is harder to solve. If we see the old value, and the new
object isn't marked anywhere else, we would accidentally think it's garbage and
collect it! Alternatively, if we see the new value, the old value won't be
marked. This would be a problem if the old pointer was moved from one object to
another, but we had already marked the second object.

In order to fix this, we need to know when a pointer is modified during
concurrent marking. Hades implements this through an additional write barrier.
This write barrier is based on a principle called "Snapshot at the Beginning"
(SATB). The principle is that we want to collect the OG based on a snapshot
of the heap when the collection began. Which means if a pointer is changed, we
want to make sure we mark the old value instead of the new value.

This might feel counter-intuitive compared to the more common alternative
approach known as "Incremental Update" (IU), where the new value is marked. The
reason Hades uses SATB instead is that it has a nice guarantee: you'll never
need to revisit any object you have already marked. This means there is a finite
upper bound on the amount of work marking has to do. IU write barrier based
systems often have a race near the end, where the GC thread needs to pause the
mutator thread to try and complete marking as fast as possible. If it exceeds
a time quota, it resumes the mutator and tries again later. We avoid this
complexity with our SATB barrier.

A second benefit of SATB is that we can treat any allocations made into OG
between the start and end of the collection as alive by default, without
needing to mark them.

And the final benefit of SATB is that there is never a need to mark the roots
again to finish a collection, as their old values were handled at the start
of the collection while the mutator was paused.

The barrier works by pushing the old value onto a small fixed size buffer, which
has space for 128 elements. Once it fills up, the Write Barrier Lock is taken to
"flush" the buffer into a separate mark stack used by the concurrent marker.
This means a lock is only taken every 128 write barriers. It uses a separate
mutex from the GC mutex to ensure a write barrier is not blocked for very long
if the GC thread happens to be reading from the separate mark stack.

## Complete Marking

Once the mark stack is empty, there are a few details that need to be handled in
order to complete marking and move on to sweeping:

- Flush any remaining write barrier pointers left
- Handle WeakMap resolution
- Fix weak references (WeakRefs)

Handling these things can be very tricky concurrently, so in order to prevent
bugs and infinite loops, we pause the mutator during this time. Even though SATB
write barriers don't require the mutator to pause, these other operations do
require a pause, so unfortunately this is still required.

Flushing the remaining write barrier pointers just means copying the pointers
into the mark stack and draining it one more time. This could potentially take
a very long time, but in practice that is exceedingly rare.

WeakMap resolution is handled in the same way that GenGC handles it. We do this
during a mutator pause mostly because we didn't want to rewrite the algorithm
to work in a concurrent context, as it's already very complicated on its own.

Weak References also need to be cleared if they point to something that is
now garbage, and this is much easier to do with the mutator paused. Otherwise
the mutator would need a lock to read a weak reference value. This could be
improved to simply be an atomic operation in the future, but for now this can't
be atomic.

Once that's all taken care of, we can move on to sweeping.

### WeakRef Read Barriers

There's a caveat to mention about WeakRefs and the SATB barrier. If you read a
pointer out of a weak reference and store it in an object on the heap, SATB
won't record the change, and the object might not be found reachable. Something
similar can happen if a weak ref is read and placed into a root.

To fix this, we have WeakRef reads perform a barrier on the pointer being read.
This conservatively assumes the pointer being read is alive. WeakRefs are not
read from that often, so this was deemed an acceptable cost.

Note that there also weak roots, such as the HiddenClass cache stored in each
CodeBlock. These do not perform a read barrier, specifically because they are
only ever used for comparisons. They never produce a pointer that was otherwise
dead. A possible simplification of this in the future could use a HiddenClass ID
instead of a pointer, as it achieves the same effect without requiring a special
case.

## Sweep Phase

Once complete reachability information is known, the OG GC turns off the
SATB barriers. The sweeper iterates over one segment at a time, allowing the
mutator to interleave. This is specifically allowed because the sweeper only
ever modifies garbage objects that aren't used, therefore there's no races.
It holds the GC mutex to prevent YG from allocating into the OG while it's
being swept.

The process works as follows:

- Acquire a lock on the GC mutex
- Clear one heap segment's free list
- Iterate linearly over cells, using the embedded length to skip over live cells
- Check if a cell is new garbage using its type tag
- If it is not new garbage, continue to the next cell
- If it is new garbage, turn it into a free list cell.
- Contiguous unused regions are added as a single region onto the new free list
- Once all cells in the segment have been processed, release the lock

Once that process is completed for every heap segment, sweeping completes and
the OG collection is over.

## Compact Phase

Compacting live memory to be closer together is still a beneficial concept in
Hades, as it allows us to return unused memory to the OS, and reduces the
fragmentation of the free lists for mostly empty heap segments. Implementing it
is more tricky than GenGC though, as we can't modify pointers concurrently with
the mutator thread.

Due to these restrictions, we can currently only compact a single segment
(called the compactee) for each full collection. Compaction runs as part of the
collection cycle and flows as follows:

1. At the start of an OG collection, determine whether the heap is currently
larger than its target size. If so, select and record a segment to compact.
2. Write barriers start dirtying cards for pointers pointing into the
compaction candidate. This will continue until the compaction is fully complete.
3. Marking begins. During marking, we dirty cards in the card table
corresponding to any on-heap pointers that point into the compaction candidate.
Any YG collection that occurs during marking needs special care. Promoted
objects will not be scanned by the OG since they are allocated as marked, so
they need to be scanned for compactee pointers after they have been promoted.
Furthermore, the card table cannot simply be cleared at the end of the YG
collection, since that would erase information from the ongoing compaction.
Instead, the YG collection clears each run of dirty cards right before scanning
it. The scan visits every object in the run, marked or not, and dirties the card
of each pointer into the compactee again. As a result, the cards of the pointers
into the compactee from outside it stay dirty, while the others are cleared and
are not scanned again by later YG collections.
4. During the STW pause, the internal state of the GC is updated to signal that
all pointers into the compactee have been marked, and that the next YG
collection should complete the compaction.
5. Sweeping. The segment identified for compaction will not be swept, however
compaction may take place during sweeping if the next YG collection starts
before sweeping is complete. Note that write barriers will continue to be active
until compaction is complete, since new pointers from the OG into the compactee
may be added.
6. Compaction. The next young gen collection evacuates both the YG and the
compactee. It will mark long lived roots and update pointers based on the
previously dirtied cards. Combining compaction with YG collections lets us
share the overhead of updating roots, and lets us avoid tracking pointers from
the YG into the compactee.
7. The now empty segment is released by the GC and returned to the OS.


## Incremental Mode

Hades's concurrent marking relies on being able to read a 64-bit value
atomically at the same time it might be modified by the mutator. If the
underlying hardware supports this natively, then we use it.
However, some hardware does not support doing those atomic reads in a lock-free
manner, primarily 32-bit ARM CPUs. Since Hermes's main target is mobile devices,
it's important for us to still support them, and have some of the fast pause
time guarantees that Hades gives.

In order to do this, on 32-bit platforms we don't use any other threads, and
instead run Hades in "incremental mode". This means instead of marking objects
concurrently on the GC thread, we use a portion of each YG GC to do some OG GC
work. This means the OG GC is completed incrementally on each individual YG GC.
Each YG GC takes a little bit longer while an OG GC is active, but the penalty
is small enough to still have better guarantees than running a fully blocking OG
GC.

The concurrent mode of Hades has faster pauses and is preferred to be used if
possible, but incremental mode has to be used on most 32-bit CPUs. You can also
use incremental mode if threads aren't supported on your platform, or if you
prefer to not use threads for some other reason.
