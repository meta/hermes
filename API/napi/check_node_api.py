#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

"""
Checks that the node-api library forwards exactly the Node-API functions that
hermesvm exports. It runs after node-api is linked.
"""

import argparse
import re
import subprocess
import sys

NODE_API_FUNCTION = re.compile(r"_?((?:napi|node_api)_\w+)")
ARCH_HEADER = re.compile(r"\(for architecture (\w+)\):$")


def defined_functions(nm, path, nm_args):
    """Returns the Node-API functions path defines, per architecture."""
    output = subprocess.run(
        [nm, "-arch", "all", "-g", "-U", *nm_args, path],
        check=True,
        capture_output=True,
        text=True,
    ).stdout
    functions = {}
    arch = ""
    for line in output.splitlines():
        header = ARCH_HEADER.search(line)
        if header:
            arch = header.group(1)
            functions.setdefault(arch, set())
            continue
        fields = line.split()
        match = NODE_API_FUNCTION.fullmatch(fields[-1]) if fields else None
        if match:
            functions.setdefault(arch, set()).add(match.group(1))
    return functions or {"": set()}


def compare(hermesvm, actual, source, entry):
    """Returns the lines to add to and remove from source to match hermesvm."""
    errors = []
    missing = sorted(hermesvm - actual)
    if missing:
        errors.append(f"Add what hermesvm exports to {source}:")
        errors += [f"  {entry.format(f)}" for f in missing]
    extra = sorted(actual - hermesvm)
    if extra:
        errors.append(f"Remove what hermesvm doesn't export from {source}:")
        errors += [f"  {entry.format(f)}" for f in extra]
    return errors


def check_android(args):
    hermesvm = defined_functions(args.nm, args.hermesvm, ["-D"])[""]
    stub = defined_functions(args.nm, args.stub, ["-D"])[""]
    errors = compare(hermesvm, stub, "API/napi/node-api-stub.c", "NODE_API_STUB({})")

    forwarded = defined_functions(args.nm, args.node_api, ["-D"])[""]
    if forwarded:
        errors.append(f"{args.node_api} defines {len(forwarded)} Node-API functions")
    dynamic = subprocess.run(
        [args.readelf, "-d", args.node_api], check=True, capture_output=True, text=True
    ).stdout
    if "[libhermesvm.so]" not in dynamic:
        errors.append(f"{args.node_api} must depend on libhermesvm.so")
    return errors


def check_apple(args):
    with open(args.reexports) as f:
        reexports = {line.strip().lstrip("_") for line in f if line.strip()}
    return [
        error
        for arch, hermesvm in sorted(defined_functions(args.nm, args.hermesvm, []).items())
        for error in compare(
            hermesvm,
            reexports,
            "API/napi/node-api.reexports" + (f" for {arch}" if arch else ""),
            "_{}",
        )
    ]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    platforms = parser.add_subparsers(dest="platform", required=True)

    android = platforms.add_parser("android")
    android.add_argument("--nm", required=True)
    android.add_argument("--readelf", required=True)
    android.add_argument("--hermesvm", required=True)
    android.add_argument("--stub", required=True)
    android.add_argument("--node-api", required=True)
    android.set_defaults(check=check_android)

    apple = platforms.add_parser("apple")
    apple.add_argument("--nm", required=True)
    apple.add_argument("--hermesvm", required=True)
    apple.add_argument("--reexports", required=True)
    apple.set_defaults(check=check_apple)

    args = parser.parse_args()
    errors = args.check(args)
    for error in errors:
        print(error, file=sys.stderr)
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
