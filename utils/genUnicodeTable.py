#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# -*- coding: utf-8 -*-

# Generates list of unicode ranges belonging to a set of categories, downloading
# Unicode data files as needed.
#
# Usage: genUnicodeTable.py
#
# For regeneration commands, input provenance and focused validation, see
# lib/Platform/Unicode/README.md. Generate to a temporary file before replacing
# UnicodeData.inc so a failed download does not truncate the checked-in data.

import argparse
import datetime
import hashlib
import re
import sys
import urllib.request
from collections import defaultdict, OrderedDict
from functools import reduce
from itertools import islice
from string import Template
from textwrap import indent
from typing import Iterable, Optional, Tuple


class UnicodeDataFiles:
    # VERSION = "UCD/latest"  # The bleeding edge version of Unicode.
    VERSION = "17.0.0"
    URLS = {
        "UnicodeData.txt": f"http://unicode.org/Public/{VERSION}/ucd/UnicodeData.txt",
        "SpecialCasing.txt": f"http://unicode.org/Public/{VERSION}/ucd/SpecialCasing.txt",
        "CaseFolding.txt": f"http://unicode.org/Public/{VERSION}/ucd/CaseFolding.txt",
        "DerivedGeneralCategory.txt": f"http://unicode.org/Public/{VERSION}/ucd/extracted/DerivedGeneralCategory.txt",
        "Scripts.txt": f"http://unicode.org/Public/{VERSION}/ucd/Scripts.txt",
        "ScriptExtensions.txt": f"http://unicode.org/Public/{VERSION}/ucd/ScriptExtensions.txt",
        "DerivedCoreProperties.txt": f"http://unicode.org/Public/{VERSION}/ucd/DerivedCoreProperties.txt",
        "DerivedNormalizationProps.txt": f"http://unicode.org/Public/{VERSION}/ucd/DerivedNormalizationProps.txt",
        "DerivedBinaryProperties.txt": f"http://unicode.org/Public/{VERSION}/ucd/extracted/DerivedBinaryProperties.txt",
        "PropertyValueAliases.txt": f"http://unicode.org/Public/{VERSION}/ucd/PropertyValueAliases.txt",
        "PropertyAliases.txt": f"http://unicode.org/Public/{VERSION}/ucd/PropertyAliases.txt",
        "PropList.txt": f"http://unicode.org/Public/{VERSION}/ucd/PropList.txt",
        "emoji-data.txt": f"http://unicode.org/Public/{VERSION}/ucd/emoji/emoji-data.txt",
        "NormalizationTest.txt": f"http://unicode.org/Public/{VERSION}/ucd/NormalizationTest.txt",
        "allkeys.txt": f"http://unicode.org/Public/UCA/{VERSION}/allkeys.txt",
        "CollationTest_NON_IGNORABLE_SHORT.txt": f"http://unicode.org/Public/UCA/{VERSION}/CollationTest/CollationTest_NON_IGNORABLE_SHORT.txt",
    }
    # Set to True to keep the downloaded files in the local directory.
    KEEP_LOCAL_CACHE = False

    __cache = {}

    @classmethod
    def get(cls, filename):
        """Retrieve a Unicode data file, fetching it if necessary."""
        if filename not in cls.__cache:
            data = cls.__local_or_fetch(cls.URLS[filename], filename)
            cls.__cache[filename] = {
                "sha1": hashlib.sha1(data).hexdigest(),
                "lines": cls.__data_to_lines(data),
            }
        return cls.__cache[filename]

    @classmethod
    def __local_or_fetch(cls, url, filename) -> bytes:
        """Read a local file's contents or fetch them from a URL."""
        try:
            with open(filename, "rb") as f:
                print(f"Found {filename} locally!", file=sys.stderr)
                return f.read()
        except IOError:
            print(f"Fetching {url}...", file=sys.stderr)
            with urllib.request.urlopen(url) as f:
                data = f.read()
                if cls.KEEP_LOCAL_CACHE:
                    print(f"Caching {filename} locally...", file=sys.stderr)
                    with open(filename, "wb") as f:
                        f.write(data)
                return data

    @classmethod
    def __data_to_lines(cls, data) -> Iterable[str]:
        return [
            line
            for line in data.decode("utf-8").splitlines()
            if line and not line.startswith("#")
        ]

    @classmethod
    def get_lines(cls, filename) -> Iterable[str]:
        return cls.get(filename)["lines"]

    @classmethod
    def get_sha1(cls, filename) -> str:
        return cls.get(filename)["sha1"]


# Which UCD files each generated table derives from. Only these are hashed
# into a file's provenance header, and only these are downloaded, so adding a
# data file for one table cannot perturb another table's output.
PROPERTIES_FILES = [
    f
    for f in UnicodeDataFiles.URLS
    if f
    not in (
        "NormalizationTest.txt",
        "allkeys.txt",
        "CollationTest_NON_IGNORABLE_SHORT.txt",
    )
]
NORMALIZATION_FILES = ["UnicodeData.txt", "DerivedNormalizationProps.txt"]
NORMTEST_FILES = ["NormalizationTest.txt"]
CASING_FILES = [
    "UnicodeData.txt",
    "SpecialCasing.txt",
    "DerivedCoreProperties.txt",
    "PropList.txt",
]
CASETEST_FILES = ["UnicodeData.txt", "SpecialCasing.txt"]
COLLATION_FILES = ["allkeys.txt", "UnicodeData.txt", "PropList.txt"]
COLLATIONTEST_FILES = [
    "CollationTest_NON_IGNORABLE_SHORT.txt",
    "allkeys.txt",
    "UnicodeData.txt",
]


# Unicode data field indexes. See UnicodeData.txt.
CODEPOINT_FIELD = 0
GENERAL_CATEGORY_FIELD = 2
DECOMPOSITION_FIELD = 5
UPPERCASE_FIELD = 12
LOWERCASE_FIELD = 13


# Low bits encode the inclusive endpoint delta; the all-ones value refers to
# a full range in the overflow table. The high bits encode a start or index.
RANGE_DELTA_BITS = 11
RANGE_DELTA_MASK = (1 << RANGE_DELTA_BITS) - 1


def print_template(s, **kwargs):
    """Substitute in the keyword arguments to the template string
    (or direct template) s, and print the result, followed by a
    newline.
    """
    text = Template(s).substitute(**kwargs)
    print(text.strip())
    print("")


# A filename long enough to push its SHA1 line past the 80 column limit
# would otherwise be reflowed by clang-format, which wraps the hash onto the
# next line and leaves every hash attached to the wrong filename.
PROVENANCE_TEMPLATE = """
//
// File generated by genUnicodeTable.py
// using Unicode data files downloaded on ${today}
// for Unicode version ${version}
// clang-format off
${sha1s}
// clang-format on
// *** DO NOT EDIT BY HAND ***
"""

SHARED_STRUCTS = """
/// An inclusive range of Unicode characters.
struct UnicodeRange { uint32_t first; uint32_t second; };

/// A UnicodeTransformRange expresses a mapping such as case folding.
/// A character cp is mapped to cp + delta if cp is 0 for the given modulus.
struct UnicodeTransformRange {
    /// The first codepoint of the range.
    unsigned start:24;

    /// The number of characters in the range.
    unsigned count:8;

    /// The signed delta amount.
    int delta:24;

    /// The modulo amount.
    unsigned modulo:8;
};

/// A reference to a string pool entry.
struct StringPoolRef {
  uint16_t offset;
  uint16_t size;
};

/// A reference to a UnicodeRange pool entry.
struct UnicodeRangePoolRef {
  uint16_t offset;
  uint16_t size;
};

/// A reference to a string pool name that maps to a string pool canonical name.
struct NameMapEntry {
    StringPoolRef name;
    StringPoolRef canonical;
};

/// A reference to a string pool name that maps to a range array pool offset
/// and size.
struct RangeMapEntry {
  StringPoolRef name;
  uint16_t rangeArrayPoolOffset;
  uint16_t rangeArraySize;
};
"""


def print_header(files, structs=True):
    """Emit the provenance comment for a generated file, hashing only the UCD
    files it derives from. Emit the shared struct definitions only for the
    property tables, which are the ones that use them."""
    print_template(
        PROVENANCE_TEMPLATE,
        today=str(datetime.date.today()),
        sha1s="\n".join(
            f"// {filename:<30} SHA1: {UnicodeDataFiles.get_sha1(filename)}"
            for filename in files
        ),
        version=UnicodeDataFiles.VERSION,
    )
    if structs:
        print_template(SHARED_STRUCTS)


def run_interval(unicode_data_lines, args):
    name = args[0]
    categories = set(args[1:])
    begin = 0
    intervals = []
    last_cp = 0
    openi = False
    for line in unicode_data_lines:
        fields = line.split(";")
        cp_str, category = fields[CODEPOINT_FIELD], fields[GENERAL_CATEGORY_FIELD]
        cp = int(cp_str, 16)
        if category in categories:
            if not openi:
                begin = cp
                openi = True
            else:
                pass  # do nothing we are still in interval
        else:
            if openi:
                intervals.append((begin, last_cp))
                openi = False
            else:
                pass  # keep looking
        last_cp = cp

    if openi:
        intervals.append((begin, last_cp))

    print_template(
        """
// ${args}
// static constexpr uint32_t ${name}_SIZE = $interval_count;
static constexpr UnicodeRange ${name}[] = {
${intervals}
};
    """,
        args=" ".join(args),
        name=name,
        interval_count=len(intervals),
        intervals="\n".join(
            "{" + hex(i[0]) + ", " + hex(i[1]) + "}," for i in intervals
        ),
    )


def print_categories(unicode_data_lines):
    """Output UnicodeRanges for Unicode General Categories."""
    categories = [
        "UNICODE_LETTERS Lu Ll Lt Lm Lo Nl",
        "UNICODE_COMBINING_MARK Mn Mc",
        "UNICODE_DIGIT Nd",
        "UNICODE_CONNECTOR_PUNCTUATION Pc",
    ]
    for cat in categories:
        run_interval(unicode_data_lines, cat.split())


def run_property_select(unicode_data_lines, args):
    """
    Output UnicodeRanges for PropList.txt.
    args[0] is the name of the variable.

    Includes all codepoints that match any property name given,
    and if any 2-character arg is given, it must match one of the specified
    category.
    """
    name = args[0]
    include_prop = set()
    include_cat = set()
    for arg in args[1:]:
        if len(arg) == 2:
            include_cat.add(arg)
        else:
            include_prop.add(arg)
    # Lines in the DerivedCoreProperties file look like:
    # 0041..005A    ; Alphabetic # L&  [26]
    # We want to extract the range, provided the property matches.
    intervals = []
    for line in unicode_data_lines:
        fields = line.split()
        # Skip lines that don't have the property.
        prop = fields[2] if len(fields) > 2 else None
        cat = fields[4] if len(fields) > 4 else None
        if len(include_prop) > 0 and prop not in include_prop:
            continue
        if len(include_cat) > 0 and cat not in include_cat:
            continue
        # Parse the range, which comes before the ';'
        range = parse_range(fields[0].strip())
        intervals.append(range)

    print_template(
        """
// static constexpr uint32_t ${name}_SIZE = $interval_count;
static constexpr std::array<UnicodeRange, ${interval_count}> ${name} = {{
${intervals}
}};
    """,
        name=name,
        interval_count=len(intervals),
        intervals="\n".join(
            "{" + hex(i[0]) + ", " + hex(i[1]) + "}," for i in intervals
        ),
    )


def print_properties(unicode_data_lines):
    """Output UnicodeRanges for PropLists."""
    categories = [
        "UNICODE_OTHER_ID_START Other_ID_Start",
        "UNICODE_OTHER_ID_CONTINUE Other_ID_Continue",
        "UNICODE_PATTERN_LETTER Pattern_Syntax Pattern_White_Space L& Ll Lt Lu Lm Lo",
        "UNICODE_PATTERN_CONTINUE Pattern_Syntax Pattern_White_Space Mn Mc Nd Pc",
    ]
    for cat in categories:
        run_property_select(unicode_data_lines, cat.split(" "))


def get_assigned_codepoints(unicode_data_lines):
    """Gather intervals for all assigned Unicode codepoints."""
    cp_begin = None
    cp_end = None

    def empty_buf():
        if cp_begin is not None:
            intervals.append((cp_begin, cp_begin if cp_end is None else cp_end))

    intervals = []
    lines = iter(unicode_data_lines)
    last_cp = 0

    while lines:
        line = next(lines, None)
        if line is None:
            break

        fields = split_fields(line)
        cp = int(fields[0], 16)
        # Handle UnicodeData.txt legacy codepoint ranges.
        # <https://www.unicode.org/reports/tr44/#Code_Point_Ranges>
        if fields[1].startswith("<") and fields[1].endswith("First>"):
            empty_buf()
            rng_begin = cp
            rng_end = int(split_fields(next(lines))[0], 16)
            intervals.append((rng_begin, rng_end))
            cp_begin = cp_end = None
        else:
            if cp - last_cp == 1:
                cp_end = cp
            else:
                if cp_begin is not None:
                    empty_buf()
                    cp_end = None
                cp_begin = cp

        last_cp = cp

    return intervals


def split_fields(line) -> list[str]:
    """
    Split a semicolon-separated line into fields, ignoring comments.
    """
    return [f.strip() for f in line.split("#")[0].split(";")]


# A Unicode codepoint range, represented as a tuple of (start, end).
range_tuple = tuple[int, int]


def merge_adjacent_ranges(ranges: list[range_tuple]) -> list[range_tuple]:
    """Merge adjacent or overlapping ranges in a sorted list."""
    if not ranges:
        return ranges
    merged = [ranges[0]]
    for start, end in ranges[1:]:
        prev_start, prev_end = merged[-1]
        if start <= prev_end + 1:
            merged[-1] = (prev_start, max(prev_end, end))
        else:
            merged.append((start, end))
    return merged


def parse_range(range_str) -> range_tuple:
    """
    Parse the alternative codepoint range format, e.g. "1..10", or "1" into a
    tuple of `(range_start, range_end)`.
    """
    start, end = range_str.split("..") if ".." in range_str else (range_str, range_str)
    return (int(start, 16), int(end, 16))


def parse_codepoint_ranges(lines: Iterable[str], pred) -> dict[str, list[range_tuple]]:
    """
    Create a mapping of canonical property names to lists of Unicode codepoint
    ranges (start, end) for those properties, from the lines of a Unicode
    Database data file.

    Codepoint ranges will be merged if they are adjacent.

    Example input:

        0000..001F    ; Cc # Cc       [32] <control-0000>-<control-001F>
        0020          ; Zs # Zs       [1] SPACE
    """
    ranges = defaultdict(list)
    last_name = None
    begin = 0
    last_cp = 0
    openi = False
    for line in lines:
        # Ignore empty lines and comment lines.
        if not line or line.startswith("#"):
            continue

        fields = split_fields(line)
        codepoint_range = fields[0]
        canonical_name = fields[1]
        if not pred(canonical_name):
            continue

        if last_name is None:
            last_name = canonical_name

        cp_start, cp_end = parse_range(codepoint_range)
        if last_name != canonical_name:
            # We have crossed over a property name boundary.
            if openi:
                ranges[last_name].append((begin, last_cp))
            openi = True
            last_name = canonical_name
            begin = cp_start
        else:
            if openi:
                if cp_start != last_cp + 1:
                    # We have crossed over an interval boundary.
                    ranges[last_name].append((begin, last_cp))
                    begin = cp_start
            else:
                begin = cp_start
                openi = True

        last_cp = cp_end

    if openi:
        ranges[last_name].append((begin, last_cp))

    return ranges


def binary_property_ranges(lines, prop):
    """Merged inclusive ranges of code points having binary property \\p prop."""
    out = []
    for line in lines:
        f = split_fields(line)
        if len(f) < 2 or f[1] != prop:
            continue
        out.append(parse_range(f[0]))
    out.sort()
    return merge_adjacent_ranges(out)


# One collation element: (variable, primary, secondary, tertiary).
CollationElement = Tuple[bool, int, int, int]

ALLKEYS_CE_RE = re.compile(
    r"\[([.*])([0-9A-Fa-f]{4,6})\.([0-9A-Fa-f]{4,6})\.([0-9A-Fa-f]{4,6})\]"
)


def parse_allkeys(lines):
    """Parse allkeys.txt into (single, multi, contractions, implicit).

    single:       cp -> one collation element
    multi:        cp -> list of collation elements (an expansion)
    contractions: tuple of cps -> list of collation elements
    implicit:     list of (first, last, base) from @implicitweights

    Entries unreachable after NFD are dropped by the caller, not here, so
    this function stays a faithful reading of the file."""
    single, multi, contractions, implicit = {}, {}, {}, []
    for line in lines:
        text = line.split("#")[0].strip()
        if not text:
            continue
        if text.startswith("@implicitweights"):
            body = text.split(None, 1)[1]
            rng, base = body.split(";")
            first, last = rng.strip().split("..")
            implicit.append((int(first, 16), int(last, 16), int(base, 16)))
            continue
        if text.startswith("@") or ";" not in text:
            continue
        lhs, rhs = text.split(";", 1)
        cps = tuple(int(x, 16) for x in lhs.split())
        ces = [
            (
                m.group(1) == "*",
                int(m.group(2), 16),
                int(m.group(3), 16),
                int(m.group(4), 16),
            )
            for m in ALLKEYS_CE_RE.finditer(rhs)
        ]
        assert ces, f"no collation elements parsed from: {text}"
        if len(cps) > 1:
            contractions[cps] = ces
        elif len(ces) == 1:
            single[cps[0]] = ces[0]
        else:
            multi[cps[0]] = ces
    assert implicit, "no @implicitweights ranges found"
    return single, multi, contractions, implicit


def canonically_decomposable(unicode_data_lines):
    """\\return the set of code points that cannot survive NFD, so their
    collation entries need never be generated.

    Only *canonical* decompositions count: compatibility decompositions
    survive NFD, so ligatures and compatibility forms stay in the table.
    Hangul syllables decompose arithmetically and are not listed in
    UnicodeData.txt, so they are added explicitly."""
    result = set()
    for line in unicode_data_lines:
        fields = line.split(";")
        decomp = fields[DECOMPOSITION_FIELD]
        if decomp and not decomp.startswith("<"):
            result.add(int(fields[CODEPOINT_FIELD], 16))
    result.update(range(0xAC00, 0xD7A4))
    return result


# A run's count is stored in 10 bits, so runs are split at this length.
MAX_COLL_RUN = 1023

# Primary weight bases for code points with no explicit entry, from UTS #10
# section 10.1.3. Emitted into the generated table and also used to check
# that no `@implicitweights` base collides with them.
HAN_CORE_BASE = 0xFB40
HAN_OTHER_BASE = 0xFB80
UNASSIGNED_BASE = 0xFBC0


def build_collation_runs(single, style_index):
    """Run-encode the single-element mappings.

    Within a script block DUCET primary weights advance in step with the code
    point, so 34,787 mappings collapse to roughly 6,660 runs. A run covers
    consecutive code points that share a style and whose primary advances by a
    constant step of 0 or 1."""
    runs = []
    items = sorted(single.items())
    i = 0
    while i < len(items):
        cp, (var, prim, sec, ter) = items[i]
        style = style_index[(var, sec, ter)]
        step = None
        j = i + 1
        while j < len(items) and (j - i) < MAX_COLL_RUN:
            cp2, (var2, prim2, sec2, ter2) = items[j]
            if cp2 != cp + (j - i):
                break
            if style_index[(var2, sec2, ter2)] != style:
                break
            if step is None:
                step = prim2 - prim
                if step not in (0, 1):
                    break
            elif prim2 != prim + (j - i) * step:
                break
            j += 1
        # A run that failed to extend past its first code point can still
        # have `step` set to the rejected (non-0/1) delta that caused the
        # `break` above; a lone code point never uses `step`, so it must be
        # forced back to 0 here rather than passed through.
        runs.append((cp, j - i, prim, style, step if step in (0, 1) else 0))
        i = j
    return runs


def verify_collation_runs(runs, single, styles):
    """Assert the run encoding reproduces every input mapping exactly, and
    that its fields fit the C++ struct's bit widths.

    The encoder relies on primary weights advancing predictably; a data
    change that broke that assumption would otherwise emit silently wrong
    weights rather than failing."""
    rebuilt = {}
    for first, count, prim, style, step in runs:
        assert step in (0, 1), f"run at {first:04X} has invalid step {step}"
        last_prim = prim + (count - 1) * step
        assert last_prim < 65536, (
            f"run at {first:04X} overflows uint16_t primary: {last_prim}"
        )
        var, sec, ter = styles[style]
        for k in range(count):
            rebuilt[first + k] = (var, prim + k * step, sec, ter)
    assert rebuilt == single, (
        f"run encoding lost data: {len(rebuilt)} rebuilt vs {len(single)} input"
    )
    assert max(r[0] for r in runs) < (1 << 21), "code point must fit in 21 bits"
    assert max(r[2] for r in runs) < 65536, "primary must fit in uint16_t"


def verify_style_sizes(styles):
    """Assert the pooled secondary/tertiary/variable styles fit the C++
    struct's bit widths."""
    assert len(styles) < 65536, "style index must fit in uint16_t"
    assert max(s for _, s, _ in styles) < 65536, "secondary must fit in uint16_t"
    assert max(t for _, _, t in styles) < 256, "tertiary must fit in uint8_t"


def implicit_anchors(implicit):
    """\\return a map from primary weight base to the code point its offsets
    are measured from.

    UTS #10 section 10.1.3 measures the second implicit weight of a code
    point in an `@implicitweights` range from the start of the *first* range
    that uses the range's base, not from the start of the range the code
    point itself falls in. Two bases are shared by two ranges each, so the
    distinction changes the weights of four of the six ranges."""
    anchors = {}
    for first, _, base in sorted(implicit):
        anchors.setdefault(base, first)
    return anchors


def verify_implicit_weights(implicit):
    """Assert the `@implicitweights` ranges satisfy the formula in UTS #10
    section 10.1.3 rather than assuming they do.

    A code point in such a range gets the two collation elements
    AAAA = base and BBBB = (cp - anchor) | 0x8000. That yields distinct and
    correctly ordered keys only if the ranges are disjoint, every offset
    from the anchor fits in the 15 bits below the 0x8000 flag, and the
    ranges that share a base stay disjoint after being shifted onto their
    common anchor. Nothing in allkeys.txt guarantees any of that, so a
    future data file that broke it must fail here rather than silently
    emitting colliding weights."""
    ranges = sorted(implicit)
    for (first, last, _), (next_first, _, _) in zip(ranges, ranges[1:]):
        assert first <= last, f"@implicitweights range U+{first:04X} is empty"
        assert last < next_first, (
            f"@implicitweights ranges U+{first:04X}..U+{last:04X} and "
            f"U+{next_first:04X}.. overlap"
        )
    anchors = implicit_anchors(ranges)
    spans = {}
    for first, last, base in ranges:
        assert base not in (HAN_CORE_BASE, HAN_OTHER_BASE, UNASSIGNED_BASE), (
            f"@implicitweights base 0x{base:04X} collides with a base "
            "reserved for Han and unassigned code points"
        )
        lo, hi = first - anchors[base], last - anchors[base]
        assert 0 <= lo <= hi <= 0x7FFF, (
            f"@implicitweights range U+{first:04X}..U+{last:04X} is "
            f"0x{lo:X}..0x{hi:X} from the anchor of base 0x{base:04X}, which "
            "does not fit the 15 bits UTS #10 section 10.1.3 allows"
        )
        for prev_lo, prev_hi in spans.setdefault(base, []):
            assert hi < prev_lo or lo > prev_hi, (
                f"@implicitweights range U+{first:04X}..U+{last:04X} shares "
                f"base 0x{base:04X} with an earlier range and overlaps it "
                "once both are measured from their common anchor"
            )
        spans[base].append((lo, hi))


def verify_no_implicit_overlap(single, multi, implicit):
    """Assert no code point in an `@implicitweights` range also has an
    explicit `single`/`multi` entry, which would make the lookup order
    between the two tables ambiguous."""
    for first, last, _ in implicit:
        for cp in range(first, last + 1):
            assert cp not in single and cp not in multi, (
                f"U+{cp:04X} is in an @implicitweights range but also has an "
                "explicit entry; the lookup order would be ambiguous"
            )


def verify_collation_pool(pool, expansions):
    """Assert the shared collation element pool and the expansions indexing
    into it fit the C++ struct's bit widths."""
    assert len(pool) < 65536, "pool offset must fit in uint16_t"
    assert all(length < 256 for _, _, length in expansions), (
        "expansion length must fit in uint8_t"
    )


def verify_collation_invariants(single, contractions):
    """Check the properties of the DUCET data, as opposed to Unicode itself,
    that the ASCII fast path in UnicodeCollation.cpp relies on, plus the
    contraction key size its C++ matcher hardcodes.

    ASCII's NFD-stability needs no check here: it is guaranteed by Unicode's
    stability policy, not by this data. These three properties have no such
    guarantee and could change with a future allkeys.txt, so the fast path
    would otherwise be relying on an unstated assumption."""
    max_contraction = max(len(cps) for cps in contractions)
    assert max_contraction <= 3, (
        f"contractions of {max_contraction} code points exceed the fixed "
        "three-code-point key; the C++ matcher would truncate them"
    )
    assert not any(all(c < 0x80 for c in cps) for cps in contractions), (
        "an all-ASCII contraction exists; the ASCII fast path in "
        "UnicodeCollation.cpp assumes none does and would be wrong"
    )
    assert all(cp in single for cp in range(0x80)), (
        "an ASCII code point has no single-element mapping in the DUCET "
        "data (it may expand instead, or be missing entirely); the ASCII "
        "fast path in UnicodeCollation.cpp would silently treat it as "
        "completely ignorable instead of weighing it"
    )


def print_collation_tables(
    single, multi, contractions, implicit, decomposable, unified_ideographs
):
    """Emit CollationData.inc."""
    # Entries a code point can never reach after NFD are omitted entirely.
    single = {k: v for k, v in single.items() if k not in decomposable}
    multi = {k: v for k, v in multi.items() if k not in decomposable}
    contractions = {
        k: v for k, v in contractions.items() if not any(c in decomposable for c in k)
    }

    # The secondary and tertiary weights come from a tiny alphabet, so they
    # are pooled and referenced by index instead of stored per element.
    styles = sorted(
        {(v, s, t) for (v, p, s, t) in single.values()}
        | {
            (v, s, t)
            for ces in list(multi.values()) + list(contractions.values())
            for (v, p, s, t) in ces
        }
    )
    style_index = {k: i for i, k in enumerate(styles)}
    verify_style_sizes(styles)

    runs = build_collation_runs(single, style_index)
    verify_collation_runs(runs, single, styles)

    verify_implicit_weights(implicit)
    verify_no_implicit_overlap(single, multi, implicit)

    print_template(
        """
/// The secondary weight, tertiary weight, and variable flag shared by many
/// collation elements. All of DUCET uses only a few hundred combinations, so
/// elements store an index into this table rather than the weights.
struct CollStyle {
  uint16_t secondary;
  uint8_t tertiary;
  uint8_t variable;
};

/// A run of consecutive code points that share a style and whose primary
/// weight advances by \\c step per code point. Code point \\c cp in
/// [first, first + count) has primary weight
/// \\c primary + (cp - first) * step.
struct CollRun {
  uint32_t first : 21;
  uint32_t count : 10;
  uint32_t step : 1;
  uint16_t primary;
  uint16_t style;
};
"""
    )

    print("static constexpr CollStyle COLL_STYLES[] = {")
    for var, sec, ter in styles:
        print(f"    {{0x{sec:04X}, 0x{ter:02X}, {1 if var else 0}}},")
    print("};")
    print("")

    print("/// Sorted by `first`, so lookup is a binary search.")
    print("static constexpr CollRun COLL_RUNS[] = {")
    for first, count, prim, style, step in runs:
        print(f"    {{0x{first:04X}, {count}, {step}, 0x{prim:04X}, {style}}},")
    print("};")
    print("")

    # Expansions and contractions share one pool of collation elements, so a
    # repeated element sequence costs nothing extra to reference twice.
    pool = []
    pool_offsets = {}

    def intern(ces):
        """\\return the offset of \\p ces in the shared pool, appending it if
        it is not already present. Sequences repeat often enough between
        expansions and contractions to be worth deduplicating."""
        key = tuple(ces)
        if key not in pool_offsets:
            pool_offsets[key] = len(pool)
            pool.extend(ces)
        return pool_offsets[key]

    expansions = []
    for cp in sorted(multi):
        ces = multi[cp]
        expansions.append((cp, intern(ces), len(ces)))

    contraction_rows = []
    for cps in sorted(contractions):
        ces = contractions[cps]
        contraction_rows.append((cps, intern(ces), len(ces)))

    verify_collation_pool(pool, expansions)
    max_contraction = max(len(cps) for cps, _, _ in contraction_rows)
    verify_collation_invariants(single, contractions)

    print_template(
        """
/// One collation element: a primary weight plus an index into COLL_STYLES for
/// its secondary weight, tertiary weight, and variable flag.
struct CollElem {
  uint16_t primary;
  uint16_t style;
};

/// A code point whose mapping is an expansion: \\c length collation elements
/// beginning at \\c offset in COLL_CE_POOL.
struct CollExpansion {
  uint32_t cp;
  uint16_t offset;
  uint8_t length;
};

/// A contraction. The sequence cp0, cp1, cp2 maps to \\c length collation
/// elements beginning at \\c offset in COLL_CE_POOL. \\c cp2 is 0 for a
/// two-code-point contraction; 0 is never a contraction member.
struct CollContraction {
  uint32_t cp0;
  uint32_t cp1;
  uint32_t cp2;
  uint16_t offset;
  uint8_t length;
};

/// An inclusive code point range.
struct CollRange {
  uint32_t first;
  uint32_t last;
};

/// An @implicitweights range, the primary weight base it uses, and the code
/// point its secondary implicit weight is measured from. Per UTS #10 section
/// 10.1.3 the anchor is the start of the first range that uses this base,
/// which is not this range's own start when two ranges share a base.
struct CollImplicitRange {
  uint32_t first;
  uint32_t last;
  uint32_t anchor;
  uint16_t base;
};
"""
    )

    print("/// Collation elements referenced by COLL_EXPANSIONS and")
    print("/// COLL_CONTRACTIONS.")
    print("static constexpr CollElem COLL_CE_POOL[] = {")
    for var, prim, sec, ter in pool:
        print(f"    {{0x{prim:04X}, {style_index[(var, sec, ter)]}}},")
    print("};")
    print("")

    print("/// Sorted by `cp`, so lookup is a binary search. Disjoint from")
    print("/// COLL_RUNS: a code point appears in one or the other.")
    print("static constexpr CollExpansion COLL_EXPANSIONS[] = {")
    for cp, offset, length in expansions:
        print(f"    {{0x{cp:04X}, {offset}, {length}}},")
    print("};")
    print("")

    print("/// Sorted lexicographically by (cp0, cp1, cp2).")
    print("static constexpr CollContraction COLL_CONTRACTIONS[] = {")
    for cps, offset, length in contraction_rows:
        padded = list(cps) + [0] * (3 - len(cps))
        cols = ", ".join(f"0x{c:04X}" for c in padded)
        print(f"    {{{cols}, {offset}, {length}}},")
    print("};")
    print("")
    print("/// The longest contraction in the generated data.")
    print(f"static constexpr size_t COLL_MAX_CONTRACTION_LENGTH = {max_contraction};")
    print("")

    print("/// Ranges given explicit primary weight bases by")
    print("/// @implicitweights in allkeys.txt.")
    print("static constexpr CollImplicitRange COLL_IMPLICIT_RANGES[] = {")
    anchors = implicit_anchors(implicit)
    for first, last, base in sorted(implicit):
        print(
            f"    {{0x{first:04X}, 0x{last:04X}, 0x{anchors[base]:04X}, 0x{base:04X}}},"
        )
    print("};")
    print("")

    print("/// Code points with the Unified_Ideograph property, sorted.")
    print("static constexpr CollRange COLL_UNIFIED_IDEOGRAPHS[] = {")
    for first, last in unified_ideographs:
        print(f"    {{0x{first:04X}, 0x{last:04X}}},")
    print("};")
    print("")

    print_template(
        """
/// Primary weight bases for code points with no table entry, from UTS #10
/// section 10.1.3. A Unified_Ideograph in the CJK Unified Ideographs or CJK
/// Compatibility Ideographs blocks uses the core base, any other
/// Unified_Ideograph uses the other base, and everything else, including
/// unassigned code points, uses the unassigned base.
static constexpr uint16_t COLL_HAN_CORE_BASE = 0x${han_core_base};
static constexpr uint16_t COLL_HAN_OTHER_BASE = 0x${han_other_base};
static constexpr uint16_t COLL_UNASSIGNED_BASE = 0x${unassigned_base};

/// The CJK Unified Ideographs and CJK Compatibility Ideographs blocks, which
/// select COLL_HAN_CORE_BASE. Block boundaries are stable, so they are
/// constants rather than generated.
static constexpr CollRange COLL_HAN_CORE_BLOCKS[] = {
    {0x4E00, 0x9FFF},
    {0xF900, 0xFAFF},
};
""",
        han_core_base=f"{HAN_CORE_BASE:04X}",
        han_other_base=f"{HAN_OTHER_BASE:04X}",
        unassigned_base=f"{UNASSIGNED_BASE:04X}",
    )


def print_collation_test_data(single, decomposable):
    """Emit CollationTestData.inc.

    Rows are emitted as string literals in the upstream file's own format
    rather than as pre-parsed arrays, matching what the normalization
    conformance data does: it is far smaller, compiles faster, stays greppable
    against the upstream file, and lets a failure print the row verbatim.

    The table is const rather than constexpr; see MSVC_CONSTEXPR_LITERAL_NOTE."""
    rows = []
    for line in UnicodeDataFiles.get_lines("CollationTest_NON_IGNORABLE_SHORT.txt"):
        # The full upstream file terminates every row with ';' where the
        # short one does not, so the ';' is stripped as well as the comment.
        # Leaving it in would emit a row the C++ parser cannot read.
        text = line.split("#")[0].strip().rstrip(";").strip()
        if not text or text.startswith("@"):
            continue
        rows.append(" ".join(text.split()))
    assert len(rows) > 200000, f"only {len(rows)} conformance rows parsed"

    print_template(
        """
/// One row of CollationTest_NON_IGNORABLE.txt: a space-separated list of hex
/// code points. Each row must sort at or after the row before it.
struct CollTestRow { const char *codePoints; };
"""
    )

    # clang-format splits long string literals across lines, which stops the
    # rows from matching the upstream file when grepped.
    print("// clang-format off")
    print(MSVC_CONSTEXPR_LITERAL_NOTE)
    print("static const CollTestRow COLL_TEST_ROWS[] = {")
    for row in rows:
        print(f'  {{"{row}"}},')
    print("};")
    print("// clang-format on")
    print("")

    # An independent check on the compression: the expected weights are read
    # straight out of allkeys.txt here, so a bug in the run encoder shows up
    # as a mismatch rather than being reproduced identically on both sides.
    print_template(
        """
/// A code point and the single collation element allkeys.txt gives it, read
/// directly from the file rather than through the compressed table, so this
/// array is an independent check on the run encoding.
struct CollWeightCheck {
  uint32_t cp;
  uint16_t primary;
  uint16_t secondary;
  uint8_t tertiary;
};
"""
    )
    checks = [(cp, v) for cp, v in sorted(single.items()) if cp not in decomposable]
    print("static constexpr CollWeightCheck COLL_WEIGHT_CHECKS[] = {")
    for cp, (_var, prim, sec, ter) in checks:
        print(f"    {{0x{cp:04X}, 0x{prim:04X}, 0x{sec:04X}, 0x{ter:02X}}},")
    print("};")
    print("")


def parse_property_aliases(
    lines: Iterable[str], get_canonical_name
) -> dict[str, list[str]]:
    """
    Create a mapping of canonical property names to lists of aliases, from the
    lines of a Unicode Database data file.

    Example input:

        gc ; Cc                               ; Control                          ; cntrl
        gc ; Cf                               ; Format

    Example output:

        {
            "Cc": ["Control", "cntrl"],
            "Cf": ["Format"],
        }
    """
    property_aliases = {}
    for line in lines:
        fields = split_fields(line)
        canonical_name = get_canonical_name(fields)
        if canonical_name is not None:
            property_aliases[canonical_name] = fields[1:]
    return property_aliases


# A range pool entry, represented as a tuple of `(offset, size)`.
range_array_pool_entry = tuple[int, int]


class UnicodePropertyCategory:
    """
    A pool of property names and aliases, and codepoint range arrays, that exist
    with `UnicodeProperties` as the parent, and represent a specific category of
    Unicode properties, such as "General_Category" or "Script".
    """

    def __init__(self, parent=None):
        self.parent: UnicodeProperties = parent
        self._aliases: dict[str, list[str]] = {}
        self._range_array_pool: dict[str, range_array_pool_entry] = OrderedDict()

    def all_names(self) -> list[tuple[str, str]]:
        """
        Sorted list of `(alias, canonical_name)` tuples for all know property
        names in this pool.
        """
        # This is sorted so the C++ code can use a binary search.
        return sorted(
            {
                (alias, name)
                for name, aliases in self._aliases.items()
                for alias in aliases
            }
        )

    def range_array_pool(self):
        """
        Sorted list of `(name, (offset, size))` tuples for the range array pool
        data.
        """
        # This is sorted so the C++ code can use a binary search.
        return sorted(self._range_array_pool.items())

    def add_aliases(self, name, aliases):
        """
        See `UnicodeProperties.add_aliases`.
        """
        if name in self._aliases:
            raise ValueError(f"Duplicate name {name}")
        elif name not in aliases:
            raise ValueError(f"Canonical name {name} not in aliases")
        self._aliases[name] = aliases

        if self.parent is not None:
            self.parent.add_aliases(name, aliases)

    def mark_range_pool(
        self, category: str, name: str, ranges=None, offset=None, size=None
    ):
        """
        See `UnicodeProperties.mark_range_pool`.
        """
        if self.parent is not None:
            self.parent.mark_range_pool(category, name, ranges, offset, size)

    def mark_range_array_pool(self, name: str, canonical_names: list[str]):
        """
        For a compound property, mark the index and size of the compound
        property's ranges.

        For example: "C" is a compound property that refers to the ranges of the
        "Cc", "Cf", "Cn", "Co", and "Cs" properties.

        If all the canonical names are already in the range array pool, then
        this is an overlapping compound property, and the marked entry instead
        refers back to the existing pool entry, instead of adding a new one.
        """
        if name in self._range_array_pool:
            raise ValueError(f"Duplicate name {name}")

        size = len(canonical_names)

        if all(
            canonical_name in self._range_array_pool
            for canonical_name in canonical_names
        ):
            # This is an overlapping compound property, refer back to the first
            # existing pool entry, and do not increment the tracking index.
            offset = self._range_array_pool[canonical_names[0]][0]
            self._range_array_pool[name] = (
                offset,
                size,
            )
            return offset
        else:
            offset = self.parent._range_array_pool_index
            self._range_array_pool[name] = (offset, size)
            for canonical_name in canonical_names:
                self._range_array_pool[canonical_name] = (
                    self.parent._range_array_pool_index,
                    1,
                )
                self.parent._range_array_pool_index += 1
            return offset

    def mark_range_array_pool_manual(self, name: str, offset: int, size: int):
        """
        Like `mark_range_array_pool` but use manually provided offset and size.
        """
        if name in self._range_array_pool:
            raise ValueError(f"Duplicate name {name}")

        self._range_array_pool[name] = (offset, size)
        self.parent._range_array_pool_index += 1

    def get_range(self, category: str, name: str) -> Optional[range_array_pool_entry]:
        return self.parent._range_pool.get((category, name))


# Tuple of `(offset, (range_pool_index, ranges, size))` for a range pool entry.
range_pool_entry = tuple[int, tuple[int, Optional[list[range_tuple]], Optional[int]]]


class UnicodeProperties:
    """
    The parent pool for all Unicode property categories, which share common
    string, range, and range array pools. The point of the pools is to
    generate code that is able to efficiently reference the large amount of
    Unicode property data, by using indexes into shared pools.

    The string pool is a shared pool of all property names and aliases, which
    contains all canonical names and aliases.

    The range pool is a shared pool of all codepoint ranges, which is referenced
    by a canonical name.

    The range array pool is a shared pool of codepoint range arrays, which is
    referenced by a conical name, and refers to one or more ranges in the range
    pool.
    """

    INCLUDE_COMMENTS = True

    def __init__(self):
        self.general_category_pool = UnicodePropertyCategory(parent=self)
        self.binary_property_pool = UnicodePropertyCategory(parent=self)
        self.script_property_pool = UnicodePropertyCategory(parent=self)
        self.script_extensions_property_pool = UnicodePropertyCategory(parent=self)

        # All seen names, that comprise the shared string pool.
        self._names = set()

        # Mapping of `(category, name)` to `(offset, ranges)` for the range
        # pool, which is shared across the other property pools.
        #
        # The offset is used by the individual range array pools, to refer back
        # to the shared range pool, and the individual ranges are used to build
        # the shared range pool data.
        self._range_pool: dict[str, range_pool_entry] = OrderedDict()

        # Track the offset into the range pool, every time a new range is marked
        # the index is incremented by the size of the new range.
        self._range_pool_index = 0

        # Track the offset into the shared range array pool, every time a new
        # range array is added to one of the property pools, this is incremented
        # by 1.
        self._range_array_pool_index = 0
        self._metrics = defaultdict(lambda: 0)

    def log_metrics(self):
        print(
            f"""
string_offset_bits: {self._metrics["string_offset"].bit_length()}
string_size_bits: {self._metrics["string_size"].bit_length()}
range_pool_offset_bits: {self._metrics["range_pool_offset"].bit_length()}
range_pool_size_bits: {self._metrics["range_pool_size"].bit_length()}
range_array_pool_offset_bits: {self._metrics["range_array_pool_offset"].bit_length()}
range_array_pool_size_bits: {self._metrics["range_array_pool_size"].bit_length()}
              """,
            file=sys.stderr,
        )

    def add_aliases(self, name: str, aliases: list[str]):
        """
        Add a name and aliases to the shared string pool.
        """
        self._names.add(name)
        self._names.update(aliases)

    def mark_range_pool(
        self,
        category: str,
        name: str,
        ranges: list[int] = None,
        offset: int = None,
        size: int = None,
    ):
        """
        Mark a range pool entry, with optional codepoint ranges.

        The category is necessary to disambiguate in cases where the name itself
        may not be unique across properties, such as `Scripts` and
        `Script_Extensions` where they share names.
        """
        key = (category, name)
        if key in self._range_pool:
            raise ValueError(f"Duplicate key {key}")

        pool_offset = self._range_pool_index if offset is None else offset
        self._range_pool[key] = (pool_offset, ranges, size)
        if ranges:
            self._range_pool_index += len(ranges)

    def gather_general_category_properties(self):
        """
        Gather the aliases and codepoint ranges for General_Category properties,
        into the shared string and range pools.

        Example aliases input:

            gc ; Cc                               ; Control                          ; cntrl
            gc ; Cf                               ; Format

        Example codepoint input:

            00D8..00DE    ; Lu #   [7] LATIN CAPITAL LETTER O WITH STROKE..LATIN CAPITAL LETTER THORN
            0100          ; Lu #       LATIN CAPITAL LETTER A WITH MACRON
        """
        gc_property_aliases = parse_property_aliases(
            UnicodeDataFiles.get_lines("PropertyValueAliases.txt"),
            get_canonical_name=lambda fields: fields[1] if fields[0] == "gc" else None,
        )
        gc_property_ranges = parse_codepoint_ranges(
            UnicodeDataFiles.get_lines("DerivedGeneralCategory.txt"),
            lambda canonical_name: canonical_name in gc_property_aliases.keys(),
        )

        pool = self.general_category_pool

        # Update the string pool with the General_Category property names and
        # aliases.
        for name, aliases in gc_property_aliases.items():
            pool.add_aliases(name, aliases)

        # These General_Category properties are never directly associated with
        # codepoints, but exist conceptually as unions of other properties.
        #
        # NOTE: It's important that any ranges shared by compound groups overlap,
        #       so that the offset+size can be contiguous for each of them.
        #
        # <https://www.unicode.org/reports/tr44/#General_Category_Values>
        COMPOUND_GC_PROPERTIES = {
            "C": ["Cc", "Cf", "Cn", "Co", "Cs"],
            "L": ["Ll", "Lt", "Lu", "Lm", "Lo"],
            "LC": ["Ll", "Lt", "Lu"],
            "M": ["Mc", "Me", "Mn"],
            "N": ["Nd", "Nl", "No"],
            "P": ["Pc", "Pd", "Pe", "Pf", "Pi", "Po", "Ps"],
            "S": ["Sc", "Sk", "Sm", "So"],
            "Z": ["Zl", "Zp", "Zs"],
        }

        cat = "General_Category"
        for compound_name, canonical_names in COMPOUND_GC_PROPERTIES.items():
            pool.mark_range_pool(cat, compound_name)

            for canonical_name in canonical_names:
                ranges = gc_property_ranges[canonical_name]
                if pool.get_range(cat, canonical_name) is None:
                    pool.mark_range_pool(cat, canonical_name, ranges)

            pool.mark_range_array_pool(compound_name, canonical_names)

        # Add any extra ranges that are not part of the compound groups.
        for canonical_name, ranges in gc_property_ranges.items():
            if pool.get_range(cat, canonical_name) is None:
                pool.mark_range_pool(cat, canonical_name, ranges)
                pool.mark_range_array_pool(canonical_name, [canonical_name])

    def gather_binary_properties(self):
        """
        Gather allowed binary (in the true/false sense) property aliases and
        codepoint ranges, explicitly given by ECMA262, into the shared string
        and range pools.

        <https://tc39.es/ecma262/multipage/text-processing.html#table-binary-unicode-properties>

        Example property aliases input:

            # Alias ; Canonical name  ; Additional alias

            AHex    ; ASCII_Hex_Digit
            Alpha   ; Alphabetic
            WSpace  ; White_Space     ; space
        """
        BINARY_PROPERTY_NAMES = [
            "ASCII",
            "ASCII_Hex_Digit",
            "Alphabetic",
            "Bidi_Control",
            "Bidi_Mirrored",
            "Case_Ignorable",
            "Cased",
            "Changes_When_Casefolded",
            "Changes_When_Casemapped",
            "Changes_When_Lowercased",
            "Changes_When_NFKC_Casefolded",
            "Changes_When_Titlecased",
            "Changes_When_Uppercased",
            "Dash",
            "Default_Ignorable_Code_Point",
            "Deprecated",
            "Diacritic",
            "Emoji",
            "Emoji_Component",
            "Emoji_Modifier",
            "Emoji_Modifier_Base",
            "Emoji_Presentation",
            "Extended_Pictographic",
            "Extender",
            "Grapheme_Base",
            "Grapheme_Extend",
            "Hex_Digit",
            "IDS_Binary_Operator",
            "IDS_Trinary_Operator",
            "ID_Continue",
            "ID_Start",
            "Ideographic",
            "Join_Control",
            "Logical_Order_Exception",
            "Lowercase",
            "Math",
            "Noncharacter_Code_Point",
            "Pattern_Syntax",
            "Pattern_White_Space",
            "Quotation_Mark",
            "Radical",
            "Regional_Indicator",
            "Sentence_Terminal",
            "Soft_Dotted",
            "Terminal_Punctuation",
            "Unified_Ideograph",
            "Uppercase",
            "Variation_Selector",
            "White_Space",
            "XID_Continue",
            "XID_Start",
        ]

        binary_property_aliases = {
            canonical_name: [] for canonical_name in BINARY_PROPERTY_NAMES
        }

        for line in UnicodeDataFiles.get_lines("PropertyAliases.txt"):
            fields = split_fields(line)
            canonical_name = fields[1]
            if canonical_name in binary_property_aliases:
                assert len(binary_property_aliases[canonical_name]) == 0, (
                    "Duplicate canonical name"
                )
                binary_property_aliases[canonical_name] = list(set(fields))

        is_known_name_or_alias = (
            lambda canonical_name: canonical_name in binary_property_aliases.keys()
        )
        binary_property_ranges = {
            # Used for binary properties such as `ASCII_Hex_Digit`.
            **parse_codepoint_ranges(
                UnicodeDataFiles.get_lines("PropList.txt"), is_known_name_or_alias
            ),
            # Used for general category properties such as `Cased_Letter`.
            **parse_codepoint_ranges(
                UnicodeDataFiles.get_lines("DerivedCoreProperties.txt"),
                is_known_name_or_alias,
            ),
            # Used for case folding properties such as `Changes_When_Casefolded`.
            **parse_codepoint_ranges(
                UnicodeDataFiles.get_lines("DerivedNormalizationProps.txt"),
                is_known_name_or_alias,
            ),
            # Used for binary properties such as `Bidi_Mirrored`.
            **parse_codepoint_ranges(
                UnicodeDataFiles.get_lines("DerivedBinaryProperties.txt"),
                is_known_name_or_alias,
            ),
            # Used for emoji-related binary properties such as
            # `Emoji_Presentation`.
            **parse_codepoint_ranges(
                UnicodeDataFiles.get_lines("emoji-data.txt"), is_known_name_or_alias
            ),
        }

        # Manually add cases that are not part of the enumerations.
        # <https://unicode.org/reports/tr18/#General_Category_Property>
        binary_property_aliases["ASCII"] = ["ASCII"]
        binary_property_ranges["ASCII"] = [(0x0, 0x7F)]

        binary_property_aliases["Any"] = ["Any"]
        binary_property_ranges["Any"] = [(0x0, 0x10FFFF)]

        binary_property_aliases["Assigned"] = ["Assigned"]
        binary_property_ranges["Assigned"] = get_assigned_codepoints(
            UnicodeDataFiles.get_lines("UnicodeData.txt")
        )

        pool = self.binary_property_pool

        # Update the string pool with the binary property names and aliases
        for name, aliases in binary_property_aliases.items():
            pool.add_aliases(name, aliases)

        cat = "Binary"
        for canonical_name, ranges in binary_property_ranges.items():
            if pool.get_range(cat, canonical_name) is None:
                pool.mark_range_pool(cat, canonical_name, ranges)
            pool.mark_range_array_pool(canonical_name, [canonical_name])

    def gather_script_properties(self):
        """
        Gather script and script extensions property aliases and codepoint
        ranges, as they exist in the Unicode Database, into the string and range
        pools.

        Script and script extensions are interleaved so that the ranges are
        contiguous.

        NOTE: Script extensions don't have their own names, instead they re-use
        the Script property names. However, the ranges are referenced by the
        alias, not the canonical name, which differs from how scripts are
        handled.

        Example property values aliases input:

            # Category ; Alias ; Canonical name

            sc         ; Arab  ; Arabic
            sc         ; Latn  ; Latin

        Example scripts input:

            0041..005A    ; Latin # L&  [26] LATIN CAPITAL LETTER A..LATIN CAPITAL LETTER Z
            0600..0604    ; Arabic # Cf   [5] ARABIC NUMBER SIGN..ARABIC SIGN SAMVAT
        """
        script_property_aliases = parse_property_aliases(
            UnicodeDataFiles.get_lines("PropertyValueAliases.txt"),
            get_canonical_name=lambda fields: fields[2] if fields[0] == "sc" else None,
        )

        # This property is fictional, and is never directly referenced in the
        # codepoint data. Instead, Katakana (Kana) and Hiragana (Hira) are used
        # separately.
        #
        # <https://www.unicode.org/reports/tr44/#Allowed_Changes>
        del script_property_aliases["Katakana_Or_Hiragana"]

        script_property_ranges = parse_codepoint_ranges(
            UnicodeDataFiles.get_lines("Scripts.txt"),
            lambda canonical_name: canonical_name in script_property_aliases,
        )

        script_property_aliases_by_alias = parse_property_aliases(
            UnicodeDataFiles.get_lines("PropertyValueAliases.txt"),
            get_canonical_name=lambda fields: fields[1] if fields[0] == "sc" else None,
        )
        raw_property_ranges = parse_codepoint_ranges(
            UnicodeDataFiles.get_lines("ScriptExtensions.txt"), lambda _: True
        )
        # Because script extension codepoints are referenced by the script
        # property alias, not the canonical name, the ranges need to be manually
        # remapped.
        script_extensions_property_ranges = defaultdict(list)
        for key, ranges in raw_property_ranges.items():
            for short_key in key.split():
                # Script extension codepoints use the script property alias, not
                # the canonical name.
                canonical_name = script_property_aliases_by_alias[short_key][1]
                script_extensions_property_ranges[canonical_name].extend(ranges)

        # Sort and merge ranges. The ranges were collected from multiple
        # multi-script lines in ScriptExtensions.txt, so they may be out of
        # order and adjacent ranges from different lines may not be merged.
        for name in script_extensions_property_ranges:
            script_extensions_property_ranges[name] = merge_adjacent_ranges(
                sorted(script_extensions_property_ranges[name])
            )

        pool = self.script_property_pool
        ext_pool = self.script_extensions_property_pool
        # Update the string pool with the script property names and aliases
        for name, aliases in script_property_aliases.items():
            pool.add_aliases(name, aliases)

        cat = "Script"
        ext_cat = "Script_Extensions"
        for canonical_name, ranges in script_property_ranges.items():
            if pool.get_range(cat, canonical_name) is None:
                pool.mark_range_pool(cat, canonical_name, ranges)
            script_range_array_offset = pool.mark_range_array_pool(
                canonical_name, [canonical_name]
            )

            # Script extensions are a superset of the script property ranges,
            # they are added immediately after the corresponding script so that
            # the ranges are contiguous.
            ext_ranges = script_extensions_property_ranges[canonical_name]
            if ext_ranges and ext_pool.get_range(ext_cat, canonical_name) is None:
                script_range_offset = pool.get_range(cat, canonical_name)[0]
                ext_pool.mark_range_pool(
                    ext_cat,
                    canonical_name,
                    ext_ranges,
                    # Start the range pool offset at the same offset as the
                    # corresponding script, and extend the size to include both
                    # the script and script extension ranges.
                    offset=script_range_offset,
                    size=len(ranges) + len(ext_ranges),
                )
                # Point directly at the combined Script_Extensions pool ref,
                # which already includes both Script and Script_Extensions
                # ranges as a single entry.
                ext_pool.mark_range_array_pool_manual(
                    canonical_name,
                    script_range_array_offset + 1,
                    1,
                )

        # Manually map the "Zzzz" / "Unknown" script property to the "Cn" /
        # "Unassigned" range.
        pool._range_array_pool["Unknown"] = (
            self.general_category_pool._range_array_pool["Cn"]
        )

    def print_template(self):
        """
        Produce the generated C++ code for the gathered Unicode properties data.

        This includes the string pool, range pool, and range array pool data.
        """
        all_strings = sorted(
            self._names,
            key=lambda name: (len(name), name),
            reverse=True,
        )
        string_pool = reduce(
            lambda acc, item: acc if item in acc else acc + item,
            all_strings,
            "",
        )

        def string_coord(name):
            """
            Build a string pool lookup reference for a given name.

            Example output:

                { offset, size }
            """
            offset = string_pool.index(name)
            size = len(name)
            self._metrics["string_offset"] = max(self._metrics["string_offset"], offset)
            self._metrics["string_size"] = max(self._metrics["string_size"], size)
            assert offset + size < 0xFFFF, "String pool offset+size exceeds uint16_t"
            return f"{{ {offset}, {size} }}"

        def _range_pool():
            """
            Using the range pool, generate the UnicodeRange entries for the C++
            code, that reference the shared range pool.

            Example output:

                static constexpr UnicodeRange UNICODE_RANGE_POOL[] = {
                    // General_Category: Cc
                    {0x0000, 0x001F},
                    {0x007F, 0x009F},
                };
            """
            for (cat, name), (offset, ranges, range_size) in self._range_pool.items():
                if self.INCLUDE_COMMENTS:
                    yield f"// {cat}: {name}"
                if ranges:
                    # Manually batch the ranges into 3 per line, because
                    # clang-format wants to format them to a single item per
                    # line.
                    for batch in batched(ranges, 3):
                        yield "".join(
                            f"{{{as_hex(start)}, {as_hex(end)}}},".ljust(20)
                            for start, end in batch
                        ).strip()

        def _range_array_pool():
            """
            Using the range array pool, generate the UnicodeRangePoolRef entries
            for the C++ code, that reference the shared range pool.

            Example output:

                static constexpr UnicodeRangePoolRef UNICODE_RANGE_ARRAY_POOL[] {
                    // General_Category: Cc
                    {0, 2},
                    // General_Category: Cf
                    {2, 21},
                };
            """
            for (cat, name), (offset, ranges, range_size) in self._range_pool.items():
                if self.INCLUDE_COMMENTS:
                    yield f"// {cat}: {name}"
                if ranges is not None:
                    size = len(ranges) if range_size is None else range_size
                    assert offset + size < 0xFFFF, (
                        "Range array offset+size exceeds uint16_t"
                    )
                    self._metrics["range_pool_offset"] = max(
                        self._metrics["range_pool_offset"], offset
                    )
                    self._metrics["range_pool_size"] = max(
                        self._metrics["range_pool_size"], size
                    )
                    yield f"{{ {offset}, {size} }},"

        def _build_name_map(pool: UnicodePropertyCategory):
            """
            For a given pool, build the NameMapEntry entries for the C++ code,
            that reference the shared string pool.

            Example output:

                static constexpr NameMapEntry canonicalPropertyNameMap_GeneralCategory[] = {
                    // "C", "C"
                    {{18, 1}, {18, 1}},
                    // "Cased_Letter", "LC"
                    {{1368, 12}, {3008, 2}},
                };
            """
            for alias, name in pool.all_names():
                if self.INCLUDE_COMMENTS:
                    yield f'// "{alias}", "{name}"'
                yield f"{{ {string_coord(alias)}, {string_coord(name)} }},"

        def _build_range_map(pool: UnicodePropertyCategory):
            """
            For a given pool, build the RangeMapEntry entries for the C++ code,
            that reference the shared range array pool.

            Example output:

                static constexpr RangeMapEntry unicodePropertyRangeMap_GeneralCategory[] = {
                    // "C"
                    {{18, 1}, 0, 5},
                    // "Cc"
                    {{3018, 2}, 0, 1},
                };
            """
            for name, (offset, size) in pool.range_array_pool():
                if self.INCLUDE_COMMENTS:
                    yield f'// "{name}"'
                assert offset + size < 0xFFFF, (
                    "Range array map offset+size exceeds uint16_t"
                )
                self._metrics["range_array_pool_offset"] = max(
                    self._metrics["range_array_pool_offset"], offset
                )
                self._metrics["range_array_pool_size"] = max(
                    self._metrics["range_array_pool_size"], size
                )
                yield f"{{ {string_coord(name)}, {offset}, {size} }},"

        print_template(
            """
#ifdef HERMES_ENABLE_UNICODE_REGEXP_PROPERTY_ESCAPES

static constexpr std::string_view UNICODE_DATA_STRING_POOL = "${string_pool}";

/// Low bits hold an inclusive endpoint delta, or the overflow sentinel.
/// High bits hold the starting code point, or the overflow-table index.
static constexpr uint32_t UNICODE_RANGE_POOL_DELTA_BITS = ${range_delta_bits};
static constexpr uint32_t UNICODE_RANGE_POOL_DELTA_MASK = ${range_delta_mask};

/// Check that every input is an ordered range of Unicode code points.
template <size_t N>
constexpr bool isValidUnicodeRangePool(const UnicodeRange (&ranges)[N]) {
  for (const auto &range : ranges)
    if (range.first > range.second || range.second > 0x10FFFF)
      return false;
  return true;
}

/// Count ranges whose endpoint delta needs the overflow representation.
template <size_t N>
constexpr size_t countUnicodeRangeOverflow(const UnicodeRange (&ranges)[N]) {
  size_t count = 0;
  for (const auto &range : ranges)
    count += range.second - range.first >= UNICODE_RANGE_POOL_DELTA_MASK;
  return count;
}

/// Compact property ranges and the endpoints of ranges too long to inline.
template <size_t N, size_t OverflowCount>
struct PackedUnicodeRangePool {
  /// Encoded ranges in the original property-pool order.
  std::array<uint32_t, N> ranges{};
  /// Full endpoints indexed by entries with the sentinel delta.
  std::array<UnicodeRange, OverflowCount> overflow{};
};

/// Encode the readable inputs during constant evaluation.
template <size_t OverflowCount, size_t N>
constexpr auto packUnicodeRanges(const UnicodeRange (&ranges)[N]) {
  static_assert(N <= (1u << (32 - UNICODE_RANGE_POOL_DELTA_BITS)),
                "Overflow indices must fit in the packed field");
  PackedUnicodeRangePool<N, OverflowCount> result{};
  size_t overflowIndex = 0;
  for (size_t i = 0; i < N; ++i) {
    const auto range = ranges[i];
    const auto delta = range.second - range.first;
    if (delta < UNICODE_RANGE_POOL_DELTA_MASK) {
      result.ranges[i] = (range.first << UNICODE_RANGE_POOL_DELTA_BITS) | delta;
    } else {
      result.ranges[i] =
          (overflowIndex << UNICODE_RANGE_POOL_DELTA_BITS) |
          UNICODE_RANGE_POOL_DELTA_MASK;
      result.overflow[overflowIndex++] = range;
    }
  }
  return result;
}

/// Check the encoded result independently, including every overflow index.
template <size_t N, size_t OverflowCount>
constexpr bool packedUnicodeRangesMatch(
    const UnicodeRange (&ranges)[N],
    const PackedUnicodeRangePool<N, OverflowCount> &packed) {
  size_t overflowCount = 0;
  for (size_t i = 0; i < N; ++i) {
    const auto word = packed.ranges[i];
    const auto delta = word & UNICODE_RANGE_POOL_DELTA_MASK;
    const auto startOrIndex = word >> UNICODE_RANGE_POOL_DELTA_BITS;
    UnicodeRange decoded{startOrIndex, startOrIndex + delta};
    if (delta == UNICODE_RANGE_POOL_DELTA_MASK) {
      if (startOrIndex != overflowCount || startOrIndex >= OverflowCount)
        return false;
      decoded = packed.overflow[startOrIndex];
      ++overflowCount;
    }
    if (decoded.first != ranges[i].first || decoded.second != ranges[i].second)
      return false;
  }
  return overflowCount == OverflowCount;
}

// The readable source ranges are used only during constant evaluation.
static constexpr auto UNICODE_RANGE_POOL = [] {
// clang-format off
  constexpr UnicodeRange ranges[] = {
${range_pool}
  };
// clang-format on
  static_assert(isValidUnicodeRangePool(ranges), "Invalid Unicode range");
  constexpr auto packed =
      packUnicodeRanges<countUnicodeRangeOverflow(ranges)>(ranges);
  static_assert(
      packedUnicodeRangesMatch(ranges, packed),
      "Unicode range packing must preserve every endpoint and overflow entry");
  return packed;
}();

static constexpr UnicodeRangePoolRef UNICODE_RANGE_ARRAY_POOL[] {
${range_array_pool}
};

static constexpr NameMapEntry canonicalPropertyNameMap_GeneralCategory[] = {
${name_map_general_category}
};

static constexpr RangeMapEntry unicodePropertyRangeMap_GeneralCategory[] = {
${range_map_general_category}
};

static constexpr NameMapEntry canonicalPropertyNameMap_BinaryProperty[] = {
${name_map_binary_property}
};

static constexpr RangeMapEntry unicodePropertyRangeMap_BinaryProperty[] = {
${range_map_binary_property}
};

static constexpr NameMapEntry canonicalPropertyNameMap_Script[] = {
${name_map_script_property}
};

static constexpr RangeMapEntry unicodePropertyRangeMap_Script[] = {
${range_map_script_property}
};

static constexpr RangeMapEntry unicodePropertyRangeMap_ScriptExtensions[] = {
${range_map_script_extensions_property}
};

#endif
    """,
            string_pool=string_pool,
            range_pool=indent("\n".join(_range_pool()), "    "),
            range_delta_bits=RANGE_DELTA_BITS,
            range_delta_mask=RANGE_DELTA_MASK,
            range_array_pool="\n".join(_range_array_pool()),
            name_map_general_category="\n".join(
                _build_name_map(self.general_category_pool)
            ),
            range_map_general_category="\n".join(
                _build_range_map(self.general_category_pool)
            ),
            name_map_binary_property="\n".join(
                _build_name_map(self.binary_property_pool)
            ),
            range_map_binary_property="\n".join(
                _build_range_map(self.binary_property_pool)
            ),
            name_map_script_property="\n".join(
                _build_name_map(self.script_property_pool)
            ),
            range_map_script_property="\n".join(
                _build_range_map(self.script_property_pool)
            ),
            # NOTE: There is no canonical name mapping for Script_Extensions,
            # instead the one for Script is reused.
            range_map_script_extensions_property="\n".join(
                _build_range_map(self.script_extensions_property_pool)
            ),
        )


def stride_from(p1, p2):
    return p2[0] - p1[0]


def delta_within(p):
    return p[1] - p[0]


def as_hex(cp):
    return "0x%.4X" % cp


def batched(iterable, n):
    """
    Roughly equivalent to `itertools.batched` from Python 3.12, according to the
    Python3 documentation for batched.

    <https://docs.python.org/3/library/itertools.html#itertools.batched>

    >>> batched('ABCDEFG', 3) # ['ABC', 'DEF', 'G']
    """
    if n < 1:
        raise ValueError("n must be at least one")
    it = iter(iterable)
    while batch := tuple(islice(it, n)):
        yield batch


class DeltaMapBlock:
    def __init__(self):
        self.pairs = []

    def stride(self):
        return stride_from(self.pairs[0], self.pairs[1])

    def delta(self):
        return delta_within(self.pairs[0])

    def can_append(self, pair):
        if not self.pairs:
            return True
        if pair[0] - self.pairs[0][0] >= 256:
            return False
        if self.delta() != delta_within(pair):
            return False
        return len(self.pairs) < 2 or self.stride() == stride_from(self.pairs[-1], pair)

    @staticmethod
    def append_to_list(blocks, p):
        if not blocks or not blocks[-1].can_append(p):
            blocks.append(DeltaMapBlock())
        blocks[-1].pairs.append(p)

    def output(self):
        pairs = self.pairs
        if not pairs:
            return ""

        first = pairs[0][0]
        last = pairs[-1][0]
        modulo = self.stride() if len(pairs) >= 2 else 1
        delta = self.delta()
        code = Template("{$first, $count, $delta, $modulo}").substitute(
            first=as_hex(first), count=last - first + 1, delta=delta, modulo=modulo
        )
        return code.strip()


class FullCaseData:
    """Full Unicode case mappings, including the multi-character mappings and
    the properties needed by the conditional rules. Distinct from CaseMap,
    which keeps only single-character locale-insensitive mappings for RegExp
    canonicalization."""

    def __init__(self, unicode_data_lines, special_casing_lines):
        self.simple_upper = {}  # cp -> cp
        self.simple_lower = {}  # cp -> cp
        self.full_upper = {}  # cp -> [cp, ...], only when longer than one
        self.full_lower = {}
        for line in unicode_data_lines:
            f = line.split(";")
            if len(f) < 14:
                continue
            cp = int(f[CODEPOINT_FIELD], 16)
            if f[UPPERCASE_FIELD].strip():
                self.simple_upper[cp] = int(f[UPPERCASE_FIELD], 16)
            if f[LOWERCASE_FIELD].strip():
                self.simple_lower[cp] = int(f[LOWERCASE_FIELD], 16)
        for line in special_casing_lines:
            f = [x.strip() for x in line.split("#")[0].split(";")]
            if len(f) < 5 or not f[0]:
                continue
            # Conditional entries are implemented in C++, not tabulated.
            if f[4]:
                continue
            cp = int(f[0], 16)
            lower = [int(x, 16) for x in f[1].split()]
            upper = [int(x, 16) for x in f[3].split()]
            if len(upper) > 1:
                self.full_upper[cp] = upper
            if len(lower) > 1:
                self.full_lower[cp] = lower

    def delta_pairs(self, simple, full):
        """(cp, mapped) pairs for the delta blocks, excluding code points that
        have a full mapping since those are handled by the side table."""
        return [
            (cp, m) for cp, m in sorted(simple.items()) if cp != m and cp not in full
        ]


class CaseMap:
    """Unicode case mapping helper.

    This class holds the list of codepoints, and their uppercase and
    lowercase mappings.

    """

    def __init__(self, unicode_data_lines, special_casing_lines, casefolding_lines):
        """Construct with the lines from UnicodeData and SpecialCasing."""
        self.toupper = {}
        self.tolower = {}
        self.codepoints = []
        for line in unicode_data_lines:
            fields = line.split(";")
            self.__set_casemap(
                fields[CODEPOINT_FIELD],
                upper=fields[UPPERCASE_FIELD],
                lower=fields[LOWERCASE_FIELD],
            )
        self.codepoints.extend(self.toupper.keys())

        # Apply special cases. This is to support ES5.1 Canonicalize, which is
        # cast in terms of toUpperCase(). The desire here is to have a
        # locale-independent result. Thus we ignore SpecialCasing rules that
        # are locale specific. We can also get away with ignoring
        # context-sensitive rules because Canonicalize only considers one
        # character. Thus ignore any rules that have a condition.
        # Format is codepoint, lower, title, upper, condition
        for line in special_casing_lines:
            # Trim comments
            line = line.split("#")[0]
            fields = line.split(";")
            if len(fields) < 5:
                continue
            cps, lower, title, upper, condition = fields[:5]
            # Title is unused
            _ = title  # noqa: F841
            if not condition.strip():
                self.__set_casemap(cps, upper=upper, lower=lower)

        # Characters default to folding to themselves.
        self.folds = {cp: cp for cp in self.codepoints}

        # Parse case folds.
        for line in casefolding_lines:
            fields = line.split("#")[0].split(";")
            if len(fields) != 4:
                continue
            orig, status, folded, _ = map(str.strip, fields)
            # We are only interested in common and simple case foldings.
            if status not in ["C", "S"]:
                continue
            self.folds[int(orig, 16)] = int(folded, 16)

    def __set_casemap(self, cp, upper, lower):
        """Set a case mapping.

        Mark the upper and lower case forms of cp. If a form is empty,
        the character is its own case mapping.
        All parameters are code points encoded via hex into a string.

        """
        # Parse the codepoint from hex.
        cp = int(cp, 16)

        # "The simple uppercase is omitted in the data file if the uppercase
        # is the same as the code point itself."
        # The same is true for the lowercase.
        # Skip eszett or anything else that maps to more than one character.
        self.toupper[cp] = int(upper, 16) if upper and len(upper.split()) == 1 else cp
        self.tolower[cp] = int(lower, 16) if lower and len(lower.split()) == 1 else cp

    def canonicalize(self, ch, unicode):
        """Canonicalize a character per ES9 21.2.2.8.2."""
        if unicode:
            return self.folds[ch]
        else:
            upper_ch = self.toupper[ch]
            # "If u does not consist of a single character, return ch"
            # We only store 1-1 mappings.
            # "If ch's code unit value is greater than or equal to decimal 128
            # and cu's code unit value is less than decimal 128, then return ch"
            # That is, only ASCII may canonicalize to ASCII.
            if upper_ch < 128 and ch >= 128:
                return ch
            return upper_ch


def print_canonicalizations(casemap, unicode):
    blocks = []
    for cp in casemap.codepoints:
        # legacy does not decode surrogate pairs, so we can skip large code points.
        if not unicode and cp > 0xFFFF:
            continue
        canon_cp = casemap.canonicalize(cp, unicode)
        if cp != canon_cp:
            DeltaMapBlock.append_to_list(blocks, (cp, canon_cp))

    print_template(
        """
// static constexpr uint32_t ${name}_SIZE = ${entry_count};
static constexpr UnicodeTransformRange ${name}[] = {
${entry_text}
};
""",
        name="UNICODE_FOLDS" if unicode else "LEGACY_CANONS",
        entry_count=len(blocks),
        entry_text=",\n".join(b.output() for b in blocks),
    )


def print_case_tables(case, cased, case_ignorable, soft_dotted):
    """Emit CaseData.inc."""
    print_template(
        """
/// A run of code points sharing a case-mapping delta, in the same encoding as
/// UNICODE_FOLDS: cp maps to cp + delta when (cp - start) % modulo == 0.
struct CaseDelta {
  unsigned start : 24;
  unsigned count : 8;
  int delta : 24;
  unsigned modulo : 8;
};

/// A multi-character case mapping, as an offset and length into
/// FULL_CASE_POOL.
struct FullCaseEntry { uint32_t cp; uint32_t offset; uint8_t length; };

/// An inclusive range of code points sharing a binary property.
struct CaseRange { uint32_t first; uint32_t last; };
"""
    )

    for name, pairs in (
        ("TO_UPPER_DELTAS", case.delta_pairs(case.simple_upper, case.full_upper)),
        ("TO_LOWER_DELTAS", case.delta_pairs(case.simple_lower, case.full_lower)),
    ):
        blocks = []
        for p in pairs:
            DeltaMapBlock.append_to_list(blocks, p)
        print(f"static constexpr CaseDelta {name}[] = {{")
        for b in blocks:
            print(f"  {b.output()},")
        print("};")
        print("")

    pool = []
    pool_index = {}

    def intern(seq):
        key = tuple(seq)
        if key not in pool_index:
            pool_index[key] = len(pool)
            pool.extend(seq)
        return pool_index[key]

    tables = {}
    for name, mapping in (
        ("FULL_UPPER", case.full_upper),
        ("FULL_LOWER", case.full_lower),
    ):
        entries = []
        for cp in sorted(mapping):
            seq = mapping[cp]
            assert all(c <= 0xFFFF for c in seq), f"{cp:04X} maps outside the BMP"
            entries.append((cp, intern(seq), len(seq)))
        tables[name] = entries

    print_codepoint_array("FULL_CASE_POOL", "char16_t", pool)

    for name, entries in tables.items():
        print(f"static constexpr FullCaseEntry {name}[] = {{")
        for cp, offset, length in entries:
            print(f"  {{{as_hex(cp)}, {offset}, {length}}},")
        print("};")
        print("")

    for name, ranges in (
        ("CASED_RANGES", cased),
        ("CASE_IGNORABLE_RANGES", case_ignorable),
        ("SOFT_DOTTED_RANGES", soft_dotted),
    ):
        print(f"static constexpr CaseRange {name}[] = {{")
        for first, last in ranges:
            print(f"  {{{as_hex(first)}, {as_hex(last)}}},")
        print("};")
        print("")


def print_case_test_data(case):
    """Emit CaseMappingTestData.inc: expectations derived directly from the
    UCD dictionaries, so the test cross-checks the delta-block compression
    rather than restating it.

    Emitted as two lists, one per direction, each excluding code points that
    have a multi-character mapping in THAT direction. Full mappings are
    asymmetric -- 102 code points have one only for uppercase and U+0130 only
    for lowercase -- so a single combined list would either lose coverage or
    force the test to guard its assertions, which would let it pass
    vacuously."""
    print_template(
        """
/// One code point's expected single-character mapping, taken straight from
/// UnicodeData.txt rather than through the compressed tables.
struct SimpleCaseExpectation { uint32_t cp; uint32_t mapped; };
"""
    )
    for name, simple, full in (
        ("SIMPLE_UPPER_EXPECTATIONS", case.simple_upper, case.full_upper),
        ("SIMPLE_LOWER_EXPECTATIONS", case.simple_lower, case.full_lower),
    ):
        print("// clang-format off")
        print(f"static constexpr SimpleCaseExpectation {name}[] = {{")
        for cp in sorted(simple):
            if cp in full:
                continue
            print(f"  {{{as_hex(cp)}, {as_hex(simple[cp])}}},")
        print("};")
        print("// clang-format on")
        print("")


class NormalizationData:
    """Decomposition, combining class, composition and Quick_Check data
    extracted from the UCD, in the form needed by UnicodeNormalization.cpp."""

    def __init__(self, unicode_data_lines, derived_norm_props_lines):
        self.ccc = {}  # cp -> canonical combining class, omitted when 0
        self.canon = {}  # cp -> primary canonical decomposition
        self.compat = {}  # cp -> primary compatibility decomposition
        self.exclusions = set()
        self.qc = {"NFC": set(), "NFD": set(), "NFKC": set(), "NFKD": set()}
        self.comp = {}  # (starter, combining) -> composite
        self._parse_unicode_data(unicode_data_lines)
        self._parse_derived(derived_norm_props_lines)
        self._build_compositions()

    def _parse_unicode_data(self, lines):
        for line in lines:
            f = split_fields(line)
            if len(f) < 6:
                continue
            cp = int(f[0], 16)
            ccc = int(f[3])
            if ccc:
                self.ccc[cp] = ccc
            d = f[5]
            if not d:
                continue
            if d.startswith("<"):
                # Compatibility mapping, e.g. "<compat> 0020 0308".
                self.compat[cp] = [int(x, 16) for x in d.split(">")[1].split()]
            else:
                self.canon[cp] = [int(x, 16) for x in d.split()]

    def _parse_derived(self, lines):
        for line in lines:
            f = split_fields(line)
            if len(f) < 2:
                continue
            first, last = parse_range(f[0])
            prop = f[1]
            if prop == "Full_Composition_Exclusion":
                self.exclusions.update(range(first, last + 1))
            elif prop in ("NFC_QC", "NFD_QC", "NFKC_QC", "NFKD_QC"):
                # Both No and Maybe mean the fast path cannot be taken.
                self.qc[prop[:-3]].update(range(first, last + 1))

    def _build_compositions(self):
        for cp, m in self.canon.items():
            if len(m) != 2:
                continue  # Singleton decompositions never compose.
            if cp in self.exclusions:
                continue
            if self.ccc.get(m[0], 0) != 0:
                continue  # Non-starter decompositions never compose.
            self.comp[(m[0], m[1])] = cp

    def full_canonical(self, cp):
        """Recursively expanded canonical decomposition of cp, or [cp]."""
        m = self.canon.get(cp)
        if not m:
            return [cp]
        out = []
        for c in m:
            out.extend(self.full_canonical(c))
        return out

    def full_compatibility(self, cp):
        """Recursively expanded compatibility decomposition of cp, or [cp].
        Compatibility decomposition subsumes canonical, so NFKD is a superset
        of NFD."""
        m = self.compat.get(cp) or self.canon.get(cp)
        if not m:
            return [cp]
        out = []
        for c in m:
            out.extend(self.full_compatibility(c))
        return out

    def ccc_ranges(self):
        """Runs of consecutive code points sharing a non-zero CCC."""
        out = []
        for cp in sorted(self.ccc):
            ccc = self.ccc[cp]
            if out and out[-1][1] == cp - 1 and out[-1][2] == ccc:
                out[-1] = (out[-1][0], cp, ccc)
            else:
                out.append((cp, cp, ccc))
        return out

    def qc_ranges(self, form):
        return merge_adjacent_ranges([(cp, cp) for cp in sorted(self.qc[form])])


def print_normalization_structs():
    print_template(
        """
/// An inclusive range of code points.
struct NormRange { uint32_t first; uint32_t last; };

/// An inclusive range of code points sharing a canonical combining class.
struct CCCRange { uint32_t first; uint32_t last; uint8_t ccc; };

/// A decomposition mapping. The mapped-to code points live in DECOMP_POOL16,
/// or in DECOMP_POOL32 when \\c wide is set because the sequence contains a
/// supplementary-plane code point. Packed into 8 bytes rather than the
/// natural 12 because these are the two largest tables.
struct DecompEntry {
  uint32_t cp : 21;
  uint32_t length : 5;
  uint32_t wide : 1;
  uint32_t offset;
};

/// A canonical composition of two code points into one.
struct CompEntry { uint32_t starter; uint32_t combining; uint32_t composite; };
"""
    )


def print_codepoint_array(name, decl, values, per_line=8):
    print(f"static constexpr {decl} {name}[] = {{")
    for chunk in batched(values, per_line):  # noqa: B911 (local shim, not itertools)
        print("  " + " ".join(f"{as_hex(c)}," for c in chunk))
    print("};")
    print("")


def build_decomposition_tables(norm):
    """Build the decomposition entries and their shared pools.

    Sequences that fit in the BMP go in a char16_t pool and the rest in a
    char32_t one; most decompositions are BMP-only, so this nearly halves the
    pool. Identical sequences are shared between entries and between tables.

    \\return (canon, compat, pool16, pool32), where each entry is a tuple of
    (code point, pool offset, length, wide)."""
    pool16, pool32 = [], []
    pool_index = {}

    def intern(seq):
        key = tuple(seq)
        if key not in pool_index:
            wide = any(cp > 0xFFFF for cp in seq)
            pool = pool32 if wide else pool16
            pool_index[key] = (len(pool), wide)
            pool.extend(seq)
        return pool_index[key]

    def entry(cp, seq):
        assert len(seq) < 32, f"decomposition of {cp:04X} exceeds the 5-bit length"
        assert cp <= 0x10FFFF, f"code point {cp:04X} exceeds 21 bits"
        offset, wide = intern(seq)
        return (cp, offset, len(seq), 1 if wide else 0)

    canon = [
        entry(cp, seq)
        for cp in sorted(norm.canon)
        for seq in [norm.full_canonical(cp)]
        if seq != [cp]
    ]
    # A compatibility mapping is stored only when it differs from the canonical
    # one; getDecomposition falls back to CANON_DECOMP otherwise. That drops
    # roughly a third of the entries.
    compat = [
        entry(cp, seq)
        for cp in sorted(set(norm.canon) | set(norm.compat))
        for seq in [norm.full_compatibility(cp)]
        if seq != [cp] and seq != norm.full_canonical(cp)
    ]
    return canon, compat, pool16, pool32


def print_normalization_tables(norm):
    """Emit NormalizationData.inc."""
    canon, compat, pool16, pool32 = build_decomposition_tables(norm)

    print_normalization_structs()

    for form in ("NFC", "NFD", "NFKC", "NFKD"):
        print(f"// Code points whose {form}_QC is No or Maybe.")
        print(f"static constexpr NormRange {form}_QC_NOT_YES[] = {{")
        for first, last in norm.qc_ranges(form):
            print(f"  {{{as_hex(first)}, {as_hex(last)}}},")
        print("};")
        print("")

    print("static constexpr CCCRange CCC_RANGES[] = {")
    for first, last, ccc in norm.ccc_ranges():
        print(f"  {{{as_hex(first)}, {as_hex(last)}, {ccc}}},")
    print("};")
    print("")

    print_codepoint_array("DECOMP_POOL16", "char16_t", pool16)
    print_codepoint_array("DECOMP_POOL32", "char32_t", pool32)

    for name, entries in (("CANON_DECOMP", canon), ("COMPAT_DECOMP", compat)):
        print(f"static constexpr DecompEntry {name}[] = {{")
        for cp, offset, length, wide in entries:
            print(f"  {{{as_hex(cp)}, {length}, {wide}, {offset}}},")
        print("};")
        print("")

    print("static constexpr CompEntry CANON_COMP[] = {")
    for (starter, combining), composite in sorted(norm.comp.items()):
        print(f"  {{{as_hex(starter)}, {as_hex(combining)}, {as_hex(composite)}}},")
    print("};")
    print("")


# Emitted above any table whose rows are string literals. Such a table must be
# const, not constexpr: MSVC folds certain pairs of distinct string literals in
# a constexpr initializer into a single literal, so a few rows silently hold
# another row's text. It reproduces on every MSVC version tested, not just the
# current one. Minimal reproduction:
#   constexpr const char *A = "0F19 003F";
#   constexpr const char *B = "296F 0021";
#   // strcmp(A, B) == 0
MSVC_CONSTEXPR_LITERAL_NOTE = """\
// const, not constexpr: MSVC folds distinct string literals in a constexpr
// initializer (e.g. "0F19 003F" and "296F 0021") into one, silently giving a
// few rows another row's text. A const table is unaffected."""


def print_normalization_test_data():
    """Emit NormalizationTestData.inc from NormalizationTest.txt.

    The rows are emitted as string literals in the source file's own format
    rather than as pre-parsed tables. That is roughly four times smaller than
    an equivalent array of code points, compiles far faster than hundreds of
    thousands of integer tokens, keeps the data greppable against the upstream
    file, and lets a failing test print the row verbatim. Each literal is one
    short row, staying well under the 65535-character limit MSVC imposes on a
    single string literal."""
    rows = []
    part1 = []
    part = 0
    for line in UnicodeDataFiles.get_lines("NormalizationTest.txt"):
        if line.startswith("@Part"):
            part = int(line[5])
            continue
        f = split_fields(line)
        if len(f) < 5 or not f[0]:
            continue
        # Keep only the five data columns, normalizing away incidental spacing
        # so the literals stay compact and stable across UCD releases.
        cols = [" ".join(c.split()) for c in f[:5]]
        rows.append((";".join(cols), part))
        if part == 1:
            part1.append(int(f[0].split()[0], 16))

    print_template(
        """
/// One row of NormalizationTest.txt, as "c1;c2;c3;c4;c5", where each column
/// is a space-separated list of hex code points. Parsed by the conformance
/// test rather than pre-expanded, to keep this file small.
struct NormTestRow { const char *columns; uint8_t part; };
"""
    )

    # clang-format splits long string literals across lines, which triples the
    # file size and stops the rows from matching the upstream file when
    # grepped. The rows below are already one per line.
    print("// clang-format off")
    print(MSVC_CONSTEXPR_LITERAL_NOTE)
    print("static const NormTestRow NORM_TEST_ROWS[] = {")
    for row, part in rows:
        print(f'  {{"{row}", {part}}},')
    print("};")
    print("// clang-format on")
    print("")

    print("// Code points listed in Part 1. Every code point NOT in this list")
    print("// normalizes to itself under all four forms.")
    print_codepoint_array("NORM_TEST_PART1", "uint32_t", sorted(part1))


def print_properties_tables():
    print_categories(UnicodeDataFiles.get_lines("UnicodeData.txt"))
    print_properties(UnicodeDataFiles.get_lines("PropList.txt"))

    unicode_properties = UnicodeProperties()
    unicode_properties.gather_general_category_properties()
    unicode_properties.gather_binary_properties()
    unicode_properties.gather_script_properties()
    unicode_properties.print_template()
    # Show information about bit sizes for the string and range pools.
    # unicode_properties.log_metrics()

    casemap = CaseMap(
        unicode_data_lines=UnicodeDataFiles.get_lines("UnicodeData.txt"),
        special_casing_lines=UnicodeDataFiles.get_lines("SpecialCasing.txt"),
        casefolding_lines=UnicodeDataFiles.get_lines("CaseFolding.txt"),
    )
    print_canonicalizations(casemap, unicode=True)
    print_canonicalizations(casemap, unicode=False)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(
        description="Generate Unicode tables for the Hermes VM."
    )
    parser.add_argument(
        "--table",
        choices=(
            "properties",
            "normalization",
            "normtest",
            "casing",
            "casetest",
            "collation",
            "collationtest",
        ),
        default="properties",
        help="which generated file to emit (default: properties)",
    )
    args = parser.parse_args()

    if args.table == "properties":
        print_header(PROPERTIES_FILES)
        print_properties_tables()
    elif args.table == "normalization":
        print_header(NORMALIZATION_FILES, structs=False)
        print_normalization_tables(
            NormalizationData(
                UnicodeDataFiles.get_lines("UnicodeData.txt"),
                UnicodeDataFiles.get_lines("DerivedNormalizationProps.txt"),
            )
        )
    elif args.table == "casing":
        print_header(CASING_FILES, structs=False)
        derived = UnicodeDataFiles.get_lines("DerivedCoreProperties.txt")
        print_case_tables(
            FullCaseData(
                UnicodeDataFiles.get_lines("UnicodeData.txt"),
                UnicodeDataFiles.get_lines("SpecialCasing.txt"),
            ),
            binary_property_ranges(derived, "Cased"),
            binary_property_ranges(derived, "Case_Ignorable"),
            binary_property_ranges(
                UnicodeDataFiles.get_lines("PropList.txt"), "Soft_Dotted"
            ),
        )
    elif args.table == "casetest":
        print_header(CASETEST_FILES, structs=False)
        print_case_test_data(
            FullCaseData(
                UnicodeDataFiles.get_lines("UnicodeData.txt"),
                UnicodeDataFiles.get_lines("SpecialCasing.txt"),
            )
        )
    elif args.table == "collation":
        print_header(COLLATION_FILES, structs=False)
        single, multi, contractions, implicit = parse_allkeys(
            UnicodeDataFiles.get_lines("allkeys.txt")
        )
        print_collation_tables(
            single,
            multi,
            contractions,
            implicit,
            canonically_decomposable(UnicodeDataFiles.get_lines("UnicodeData.txt")),
            binary_property_ranges(
                UnicodeDataFiles.get_lines("PropList.txt"), "Unified_Ideograph"
            ),
        )
    elif args.table == "normtest":
        print_header(NORMTEST_FILES, structs=False)
        print_normalization_test_data()
    elif args.table == "collationtest":
        print_header(COLLATIONTEST_FILES, structs=False)
        single, _, _, _ = parse_allkeys(UnicodeDataFiles.get_lines("allkeys.txt"))
        print_collation_test_data(
            single,
            canonically_decomposable(UnicodeDataFiles.get_lines("UnicodeData.txt")),
        )
    else:
        raise AssertionError(f"unhandled --table {args.table}")
