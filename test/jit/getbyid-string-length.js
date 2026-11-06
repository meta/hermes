/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xjit-max-recompiles=8 %s | %FileCheck --check-prefix=OUT %s
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xjit-max-recompiles=8 -gc-sanitize-handles=1 %s | %FileCheck --check-prefix=OUT %s
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xjit-max-recompiles=8 -Xdump-jitcode=3 %s | %FileCheck --check-prefix=DUMP %s
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xjit-max-recompiles=8 -Xjit-emit-counters %s 2>&1 | %FileCheck --check-prefix=CNT %s
// RUN: %hermes -fno-inline -dump-bytecode %s | %FileCheck --check-prefix=BC %s
// REQUIRES: jit

// The Length block over every StringPrimitive kind. getLen's version 1
// is compiled before any string arrives (NotPrepared): calls 1-16
// decline, the 16th recompiles, and version 2 holds the Length block,
// which serves the thirteen reads printed on the second line. lenSelf
// writes its result into its source register (BC; review focus 1); its
// version 2 serves the two reads after its warm-up.
// NumByIdStringHits = 13 + 2 = 15; NumRecompiles = 2. main() runs six
// recording reads (fromCharCode, keys and repeat, twice each; its global
// reads are TryGetById), once each, under 16.
// The kinds (checked with a temporary print of each receiver's cell kind
// and flag bits in _jit_get_by_id):
// - plain, ascii: concatenations shorter than 256 characters,
//   DynamicASCII (StringPrimitive.cpp:294-308);
// - utf16: String.fromCharCode with three arguments, DynamicUTF16 (one
//   argument returns a cached single-character string, String.cpp:428-435);
// - uniq: an Object.keys name, a uniqued identifier string
//   (JSObject.cpp:607-609) with LENGTH_FLAG_UNIQUED (bit 31) set and no
//   cached hash (bit 30 clear: the print showed flags 2);
// - uniqU: an Object.keys name holding a non-Latin-1 character,
//   DynamicUniquedUTF16, flags 2 like uniq;
// - hashed: a flat dynamic UTF-16 string (String.fromCharCode builds
//   UTF-16, String.cpp:438) used as a computed property key, which
//   caches its hash (bit 30) on it without uniquing it
//   (IdentifierTable.cpp:101-116, :317-354);
// - prefix, buf: repeated += reaching 256 characters gives a
//   BufferedASCII string; prefix is buf before two more appends, so its
//   length (300) is less than its buffer's (302)
//   (StringPrimitive.h:749-760);
// - bufU: the same with a UTF-16 character, 280 appends of 'Ā',
//   BufferedUTF16;
// - ext: repeat() to 65537 characters, built at the final length as
//   UTF-16 (repeat passes no ASCII flag, String.cpp:1454) and external
//   from 65536 on (String.cpp:1451-1465, StringPrimitive.h:124-125,
//   :143-144);
// - extA: btoa() of 49155 characters, 65540 ASCII characters: btoa
//   builds ASCII (Base64.cpp:42), external at that length;
// - the empty string and a one-character literal, both uniqued
//   (bit 31 set).
// Fails before the feature (no Length block; NumByIdStringHits 0), and if
// the mask lets bit 31 (uniq, uniqU, '', 'z') or bit 30 (hashed)
// through.
function main() {
  function getLen(s) {
    return s.length;
  }
  function lenSelf(s) {
    s = s.length;
    return s;
  }
  function cat(a, b) {
    return a + b;
  }
  function grow(c, n) {
    var s = '';
    for (var i = 0; i < n; ++i)
      s += c;
    return s;
  }
  // n calls of f(s); the sum of the results.
  function drive(f, s, n) {
    var r = 0;
    for (var i = 0; i < n; ++i)
      r += f(s);
    return r;
  }
  var plain = cat('ab', 'cd');
  print(drive(getLen, plain, 16));
  var ascii = cat('abc', 'defg');
  var utf16 = String.fromCharCode(0x100, 0x101, 0x102);
  var uniq = Object.keys({uniqueStrLenKey: 1})[0];
  var uniqU = Object.keys({'uniqueKeyĀ': 1})[0];
  var hashed = String.fromCharCode(104, 97, 115, 104, 101, 100);
  var keyed = {};
  keyed[hashed] = 1;
  var buf = grow('x', 300);
  var prefix = buf;
  buf += 'yz';
  var bufU = grow('Ā', 280);
  var ext = 'Ā'.repeat(65537);
  var extA = btoa('a'.repeat(49155));
  print(
      getLen(plain),
      getLen(ascii),
      getLen(utf16),
      getLen(uniq),
      getLen(uniqU),
      getLen(hashed),
      getLen(prefix),
      getLen(buf),
      getLen(bufU),
      getLen(ext),
      getLen(extA),
      getLen(''),
      getLen('z'));
  print(drive(lenSelf, plain, 16), lenSelf(ascii), lenSelf(uniq));
}
main();

// OUT: 64
// OUT-NEXT: 4 7 3 15 10 6 300 302 280 65537 65540 0 1
// OUT-NEXT: 64 7 15
// DUMP: JIT compilation of FunctionID {{[0-9]+}}, 'getLen' (version 2)
// DUMP: // Get string length (site {{[0-9]+}})
// DUMP: JIT successfully compiled FunctionID {{[0-9]+}}, 'getLen' (version 2)
// DUMP-NOT: JIT compilation of
// DUMP: JIT ById string sites: 1 length, 0 method
// DUMP: JIT compilation of FunctionID {{[0-9]+}}, 'lenSelf' (version 2)
// DUMP: // Get string length (site {{[0-9]+}})
// DUMP: JIT successfully compiled FunctionID {{[0-9]+}}, 'lenSelf' (version 2)
// DUMP-NOT: JIT compilation of
// DUMP: JIT ById string sites: 1 length, 0 method
// CNT: NumRecompiles: 2{{$}}
// CNT: NumByIdStringHits: 15{{$}}
// BC-LABEL: Function<lenSelf>(
// BC: GetById{{[A-Za-z]*}} [[R:r[0-9]+]], [[R]],
