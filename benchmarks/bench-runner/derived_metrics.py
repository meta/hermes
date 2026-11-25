# (c) Meta Platforms, Inc. and affiliates. Confidential and proprietary.

"""Process Statistics Coming from the Benchmark

These operations calculate derived metrics from those given by the benchmark.
"""

from stats import Metric


def _assertNotIn(d, *keys):
    for key in keys:
        assert key not in d, "Dictionary already contains '{}'".format(key)


def _pct(num, denom):
    """Calculate percentage

    Calculates the fraction `num/denom` as a percentage. If `denom` is `0`, this
    function will also return `0`, or more specifically a Metric with the same
    cardinality whose values are all zero.

    """
    return num * 100 / denom if denom else Metric([0] * len(num.samples))


def updateGeneralStats(stats):
    _assertNotIn(stats, "mutTime", "gcPct")

    totalTime = stats["totalTime"]
    gcTime = stats["totalGCTime"]
    mutTime = totalTime - gcTime
    gcPct = _pct(gcTime, totalTime)

    stats.update(mutTime=mutTime, gcPct=gcPct)


def updateJitStats(stats):
    _assertNotIn(stats, "jitGcPct")

    # Estimate the mutator and total time if we had a JIT that sped mutator
    # ops up by 10x
    gcTime = stats["totalGCTime"]
    jitMutTime = stats["mutTime"] / 10.0
    jitTotTime = jitMutTime + gcTime

    stats.update(jitGcPct=_pct(gcTime, jitTotTime))


def updateGenerationalStats(totalGCTime, stats):
    YG_PHASES = [
        "ygMarkOldToYoung",
        "ygMarkRoots",
        "ygScanTransitive",
        "ygUpdateWeakRefs",
        "ygFinalizers",
    ]
    FULL_PHASES = [
        "fullMarkRoots",
        "fullMarkTransitive",
        "fullSweep",
        "fullUpdateRefs",
        "fullCompact",
    ]

    def key(phase):
        return phase + "Time"

    def time(phase):
        return stats[key(phase)]

    _assertNotIn(stats, *(key(phase) + "Pct" for phase in YG_PHASES))
    _assertNotIn(stats, *(key(phase) + "Pct" for phase in FULL_PHASES))

    _assertNotIn(stats, "ygOtherGCTime", "fullOtherGCTime")
    _assertNotIn(stats, "ygGCTimePct", "fullGCTimePct")

    ygTotalGCTime = stats["ygTotalGCTime"]
    fullTotalGCTime = stats["fullTotalGCTime"]

    # Good form to make sure almost everything is accounted for...
    ygOtherGCTime = ygTotalGCTime - sum((time(phase) for phase in YG_PHASES), Metric())
    fullOtherGCTime = fullTotalGCTime - sum(
        (time(phase) for phase in FULL_PHASES), Metric()
    )

    stats.update(
        {key(phase) + "Pct": _pct(time(phase), ygTotalGCTime) for phase in YG_PHASES}
    )

    stats.update(
        {
            key(phase) + "Pct": _pct(time(phase), fullTotalGCTime)
            for phase in FULL_PHASES
        }
    )

    stats.update(
        ygOtherGCTime=ygOtherGCTime, ygOtherGCTimePct=_pct(ygOtherGCTime, ygTotalGCTime)
    )

    stats.update(
        fullOtherGCTime=fullOtherGCTime,
        fullOtherGCTimePct=_pct(fullOtherGCTime, fullTotalGCTime),
    )

    stats.update(
        ygGCTimePct=_pct(ygTotalGCTime, totalGCTime),
        fullGCTimePct=_pct(fullTotalGCTime, totalGCTime),
    )


def updateRuntimeStats(stats):
    def key(root):
        return "MarkRoots_" + root

    def time(root):
        return stats[key(root) + "Time"]

    INDIVIDUAL_ROOTS = [
        "Registers",
        "Locals",
        "SHLocals",
        "CharStrings",
        "IdentifierTable",
    ]

    # Every other root section the runtime reported (one MarkRoots_<Section>Time
    # per entry of include/hermes/VM/RootSections.def) is summed as "other". We
    # predict that they make trivial contributions; if that becomes false, move
    # them to INDIVIDUAL_ROOTS.
    prefix, suffix = "MarkRoots_", "Time"
    OTHER_ROOTS = [
        name
        for name in (
            k[len(prefix) : -len(suffix)]
            for k in stats
            if k.startswith(prefix) and k.endswith(suffix)
        )
        if name not in INDIVIDUAL_ROOTS
    ]

    _assertNotIn(stats, *(key(root) + "Pct" for root in INDIVIDUAL_ROOTS))
    _assertNotIn(stats, "MarkRoots_otherTime", "MarkRoots_otherTimePct")

    totalTime = stats["totalMarkRootsTime"]
    otherTime = sum((time(root) for root in OTHER_ROOTS), Metric())

    stats.update(
        {key(root) + "Pct": _pct(time(root), totalTime) for root in INDIVIDUAL_ROOTS}
    )

    stats.update(
        MarkRoots_otherTime=otherTime, MarkRoots_otherTimePct=_pct(otherTime, totalTime)
    )
