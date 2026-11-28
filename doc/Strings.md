---
id: strings
title: Strings
---

## Hermes Bytecode String Kinds

Hermes bytecode has three kinds of string.  The simple JS program `Object.foo = "bar";` contains examples of all of them.

 - `Object` is **Predefined** because it is being used directly to access a property (i.e. it is an identifier) -- in this case on the global object -- and both the compiler and runtime are already aware of it.  See `hermes/VM/PredefinedStrings.def` for a full list of strings that could be Predefined (if they were used for property access).
 - `foo` is an **Identifier** because it is being used directly to access a property but it is not built-in to the VM or compiler.
 - `bar` is a **String**, because it is referred to as a literal but neither of the previous conditions apply.

The distinction between these is important during bytecode initialisation, particularly "Identifier Table Initialisation", detailed in the section below.

## String Table Format

In the Hermes bytecode format, the string table is split into the following consecutive sections of the file:

 1. String Kinds.  A sequence which describes the kinds of the strings in the table (as described in "Hermes Bytecode String Kinds").  Abstractly, the i-th element in the sequence is the kind of the i-th string in the table.  Represented as a run-length encoding.
 2. Identifier Hashes.  This sequence has an element corresponding to each identifier in the table.  The i-th element of this sequence corresponds to the i-th identifier in the table.  If the string is an Identifier, the element contains a hash of the identifier's string representation.
 3. Small String Table.  The primary index into the string table.  When a bytecode instruction refers to a string by its index, it uses the offset into this data structure.  Represents the string with three pieces of information:
  - Whether it is UTF16 or not.
  - Its offset into the character storage.
  - Its length.
 Each entry is packed in 32 bits: 1 bit for the UTF16 flag, 23 bits for the offset and 8 for the length.  If the length or the offset do not fit in the available space, the entry spills into the overflow table (the next section).  In this case the small entry's length is 255 and its offset field is interpreted as the offset into the overflow table.
 4. Overflow String Table.  Contains the offset and length information (32 bits for each) for strings that could not fit into a small entry.
 5. ASCII String Storage.  A contiguous sequence of ASCII character data.  Character data for individual strings can potentially overlap.
 6. UTF16 String Storage.  As above, but for UTF16 strings.

## Identifier Table

To speed up property accesses the Runtime interns property names, assigning a number to refer to them by.  Although bytecode instructions also refer to property names as numbers, the same string -> number mapping can't be used because a property name's number in a bytecode file stems from that file's string table and will not in general agree across all bytecode files (the Runtime could be running code from multiple files).

The Runtime's intern table is managed by the `IdentifierTable` which maintains a two-way mapping between strings and their numbers.  It is known to the GC which will clean up entries corresponding to unused names.

### Importing String Table IDs

The Runtime representation of a bytecode file maintains a mapping between the numbers that strings are referred to by in the bytecode (in instructions) and the numbers that strings are referred to by in the `IdentifierTable`, this is initialised when the bytecode is loaded.  Identifier Translations are used to speed this process up:

 - Only the entries in the mapping corresponding to identifiers are assigned.
 - If the identifier is Predefined its Identifier Translation is its IdentifierTable ID and the mapping can be initialised without querying the IdentifierTable.  This is because the compiler and Runtime agree ahead of time on the IdentifierTable IDs that Predefined strings will be assigned to.
 - If the identifier is not predefined, its Identifier Translation is its precomputed hash, this can be passed to the IdentifierTable when inserting it to avoid having to page in the contents of the string to calculate the hash during initialisation.

## Lowering Strings to Hermes Bytecode

When a string is mentioned as a literal in the source code, this is always represented by an instance of `LiteralString` in the IR (Intermediate Representation).  This is a pointer to a UTF8 string that has been interned in the compiler's memory.

In HBC (Hermes bytecode) opcodes refer to literal strings by an index into a string table (part of the bytecode file).  See "String Table Format", above, for a detailed description.

 - Before the IR is lowered, all the mentioned strings are gathered into a table and assigned indices.
 - Indices are not assigned until all strings are gathered, at which point the table is frozen.  It will remain immutable, but can now be queried:  Given the UTF8 representation of a string, it will return its ID.
 - The frozen table is used during lowering to emit String IDs in bytecode instructions.
 - Finally the table is serialized into the bytecode, following the format laid out above.

Gathering is handled in `hbc::generateBytecodeModule`. The `UniquingStringLiteralAccumulator` class is responsible for assigning indices to strings and functions in `include/BCGen/HBC/TraverseLiteralStrings.h` are responsible for visiting each string in the IR.  Before lowering instructions, the accumulator is converted into a `StringLiteralTable` which is an immutable representation of the mapping.

**NB** Every potential occurrence of a string must be visited by a function declared in `TraverseLiteralStrings.h`, otherwise they will not appear in the string table.  Similarly, every instance where a string could be used as an identifier should be enumerated in `isIdOperand` in `TraverseLiteralStrings.cpp`.  The compiler will throw an assertion failure in debug builds if it encounters a string without an ID or an identifier that has not been marked as such during lowering.

## String Table Index Ordering

The order of entries in the String Table is significant for two reasons:

 1. Instructions that access properties have variants.  E.g. `GetByIdShort`, `GetById` and `GetByIdLong`.  They differ in the number of bytes they have available to encode a string table index (1, 2 and 4 respectively) and their own size increases correspondingly.  The compiler emits the narrowest instruction that can fit the ID being accessed, so by arranging for strings that are accessed more often to have smaller IDs, the space taken by instructions can be saved.
 2. Because the String Kind section (see "String Table Format" above) is a run-length encoding, its size can be minimised by grouping together strings of the same kind.

In order to make good use of these properties, the compiler counts the occurrences of strings as it is gathering them.  The top 2^8 most accessed strings are given the first 2^8 IDs and so on, to minimise instruction size with respect to the first constraint.  Then within each category of ID (short, regular and long) strings are grouped by kind with all the Strings coming first, then Identifiers, and finally Predefineds.

## Delta Optimizing Mode

In delta-optimizing mode Hermes tries to minimise the effect of incremental changes to the source on the bytecode bundle.  Outside of this mode, small changes to string usage can cause the output bytecode's string table to be re-ordered.  This has knock-on effects throughout the instruction stream wherever strings are referred to.  When delta-optimizing mode is enabled, the order of strings that existed in the bundle before the change is preserved.

## How Hermes Packs Strings

This is a description of the string packing algorithm employed by Hermes. Its design is guided by the Hermes optimization principles: it must be predictable and compile-time efficient.

A Hermes bytecode file represents strings as a pair (offset, length) in a big character buffer, which necessarily contains all strings as substrings. By being clever about arranging the strings in the character buffer, we can take advantage of shared substrings. This reduces the size of the bytecode file and the memory footprint of the app.

There are two types of relationships between strings:

1. A full containment relationship. For example, `once upon a time` contains `upon`. In this case, we would like to emit only the characters of the first string, and have the second string reference a substring of the first. We say that the first string is a *parent* of the second.
2. An overlapping relationship: `splitpea` and `peasoup`. Here `pea` is a suffix of the first string and a prefix of the second, and so we would like to emit `splitpeasoup` where the `pea` is shared between strings. We say that the pair (`splitpea`, `peasoup`) has *overlap* of length 3.

The challenge is to find a *superstring* of all our strings which is shorter than simply concatenating them. Finding the shortest such string is the "Shortest Common Superstring" problem, and it is NP-Hard. We employ a greedy heuristic. The output is not as small as the Set Cover approach, but the Set Cover approach appears necessarily quadratic while ours is N log N. (Input size is on the order of 35k strings, so quadratic algorithms are not tolerable.)

The basic approach is inspired by "A greedy approximation algorithm for constructing shortest common superstrings" by Tarhio and Ukkonen.  Given a set of strings S:

1. Consider S to be vertices of a weighted directed graph.
2. Let there be an edge from s1 to s2 if s1 overlaps s2, that is, some string is both a suffix of s1 and a prefix of s2. The weight of the edge is the amount of overlap.
3. Construct a Hamiltonian Path through the graph by greedily choosing edges with the maximum weight, but ignoring edges that would result in a cycle.
4. Output the strings in the order of the path, applying the overlap.

The key bottleneck of the algorithm is step 2: identifying overlapping strings. Tarhio and Ukkonen suggest using KMP string matching on all pairs of strings, which is quadratic. We instead build a generalized suffix array on all strings, which allows both finding parent and overlap relationships via binary search.

### Computing Parents and Overlap

Recall that an overlap is a pair (left, right) where there is some string that is simultaneously a suffix of left, and a prefix of right. For example, `splitpea` and `peasoup` has overlap of 3. We wish to find all parents and all overlaps. We are armed with a suffix array: a sorted list of suffixes of all our strings, where each suffix points back to the string(s) that contain it.

We loop over the right strings, and find all overlapping left mates. For each right string, we loop over its prefixes in increasing length. For example, given `peasoup` we loop over `p`, `pe`, `pea`... Call this the test prefix. We use binary search in our suffix array to identify suffixes that are prefixed by the test prefix. This set of suffixes is necessarily contiguous, because the suffix array is sorted. If a suffix is exactly equal to that prefix, then we have an overlap. This is necessarily the leftmost element of our range, because our range is sorted.

A key idea is that moving to the next prefix can only narrow the matching suffixes. For example, only suffixes that have `pe` as a prefix may have `pea` as a prefix. Therefore we do not need to reset the binary search range across iterations. Furthermore, for each iteration, we only need to consider one character in each suffix, since we know all previous characters necessarily match. This is key to performance.

When we find nonzero overlap, we add an `Arc` (left -> right) representing edges in our graph. The Arcs are maintained sorted by weight for our greedy algorithm.

#### Example

Say we wish to find overlaps with left string `splitpea` and right string `peasoup` (of course, there is only one overlap of length 3). Our proposed right string is `peasoup` and our suffix array is (considering only the suffixes of `splitpea`):

  `[a, ea, itpea, litpea, pea, plitpea, splitpea, tpea]`

We are going to iterate the prefixes of our right string: `p`, `pe`, `pea`, `peas`, `peaso`, `peasou`, `peasoup`, but we will see that we exit early.

1. Start with `p`. Binary search at index 0 to get the contiguous range `[pea, plitpea]` We then check if the first element is exactly equal to `p` (which we can do simply by checking its length); if it were, we would add an arc of weight 1.

2. Next is `pe`. Binary search on index 2 with `e`, yielding a one-element range `[pea]` Again we would add an arc if the first element were equal to `pe`.

3. Next is `pea`. Binary search on index 3 with `a`, again yielding `[pea]` The first string of this range is indeed equal to our prefix `pea`, so we add an Arc `peasoup -> splitpea` with overlap 3.

4. Next is `peas`. Binary search to narrow our range by the fourth character `s`. This yields an empty range, so we exit the loop.

After exhausting our prefixes, it may happen that our range of suffixes is not empty. For example, if we searched for `lit` we would end up with one suffix `litpea`. This is how we identify substring containment relationships. In this case we don't add an arc (since there's not actually any overlap) but we do set a pointer from `lit` to the parent string that contains it, and the offset within that parent.

#### Suffix Array Construction Optimizations

We construct the suffix array by uniquing suffixes via a hash set, and then sorting them. This was found to be faster than using a sorted data structure (such as `std::map`) because comparing long strings lexicographically is expensive.

There are some optimizations we take advantage of:

1. We are only interested in suffixes that share a prefix with some string in our set. For example, for the set [splitpea, peasoup] we only need to look at suffixes that start with s or p. We take this a bit further by making a hash set of three-character prefixes of our strings ("trigrams"), and only constructing suffixes prefixed by one of those trigrams. This causes us to miss overlaps of length 1 and 2, but massively reduces the number of suffixes that we have to consider.
2. There are many long strings with shared suffixes. When constructing the suffix array, using a three-way radix quicksort instead of `std::sort()` is a substantial win, because it avoids comparing known-matching prefixes.

#### Further Optimization

1. Computing overlap is a natural candidate to be done in parallel.
2. It's possible that we could employ a faster algorithm for suffix array construction such as SA-IS, but it is unclear how we would unique our suffixes with that approach.

### References

1. ["A greedy approximation algorithm for constructing shortest common superstrings" by Tarhio and Ukkonen](http://www.sciencedirect.com/science/article/pii/0304397588901673)
2. [3-way radix quicksort](https://en.wikipedia.org/wiki/Multi-key_quicksort)
