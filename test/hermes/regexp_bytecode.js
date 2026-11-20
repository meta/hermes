/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -dump-bytecode -target=HBC -non-strict %s | %FileCheck --match-full-lines %s

print(/a\x01\u017f/i);
print(/a\x01\u017f/i);

// CHECK: RegExp Bytecodes:
// CHECK:       0: /a\x01\u017f/i
// CHECK-NEXT:    Header: marked: 0 loops: 0 flags: 1 constraints: 5
// CHECK-NEXT:    StartSet: 0x41 0x61
// CHECK-NEXT:    0000  MatchCharICase8: 'A'
// CHECK-NEXT:    0002  MatchCharICase8: 0x01
// CHECK-NEXT:    0004  MatchCharICase16: 0x17f
// CHECK-NEXT:    0007  Goal

print(/^a\u017f\x01$/);
// CHECK:       1: /^a\u017f\x01$/
// CHECK-NEXT:    Header: marked: 0 loops: 0 flags: 0 constraints: 7
// CHECK-NEXT:    StartSet: 0x61
// CHECK-NEXT:    0000  LeftAnchor
// CHECK-NEXT:    0001  MatchChar8: 'a'
// CHECK-NEXT:    0003  MatchChar16: 0x17f
// CHECK-NEXT:    0006  MatchChar8: 0x01
// CHECK-NEXT:    0008  RightAnchor
// CHECK-NEXT:    0009  Goal

print(/^a|b/);
// CHECK:       2: /^a|b/
// CHECK-NEXT:    Header: marked: 0 loops: 0 flags: 0 constraints: 4
// CHECK-NEXT:    StartSet: 0x61-0x62
// CHECK-NEXT:    0000  Alternation: Target 0x0f, constraints 6,4
// CHECK-NEXT:    0007  LeftAnchor
// CHECK-NEXT:    0008  MatchChar8: 'a'
// CHECK-NEXT:    000a  Jump32: 0x11
// CHECK-NEXT:    000f  MatchChar8: 'b'
// CHECK-NEXT:    0011  Goal

print(/[a-z][^A-Z0-9_\d][\s][abc]/);
// CHECK:       3: /[a-z][^A-Z0-9_\d][\s][abc]/
// CHECK-NEXT:    Header: marked: 0 loops: 0 flags: 0 constraints: 4
// CHECK-NEXT:    StartSet: 0x61-0x7a
// CHECK-NEXT:  0000  Bracket: [a-z]
// CHECK-NEXT:  000e  Bracket: [^\d0-9A-Z_]
// CHECK-NEXT:  002c  Bracket: [\s]
// CHECK-NEXT:  0032  Bracket: [a-c]
// CHECK-NEXT:  0040  Goal

print(/a(b(c)(d))e\1\2/);
// CHECK:       4: /a(b(c)(d))e\1\2/
// CHECK-NEXT:    Header: marked: 3 loops: 0 flags: 0 constraints: 4
// CHECK-NEXT:    StartSet: 0x61
// CHECK-NEXT:    0000  MatchChar8: 'a'
// CHECK-NEXT:    0002  BeginMarkedSubexpression: 0
// CHECK-NEXT:    0005  MatchChar8: 'b'
// CHECK-NEXT:    0007  BeginMarkedSubexpression: 1
// CHECK-NEXT:    000a  MatchChar8: 'c'
// CHECK-NEXT:    000c  EndMarkedSubexpression: 1
// CHECK-NEXT:    000f  BeginMarkedSubexpression: 2
// CHECK-NEXT:    0012  MatchChar8: 'd'
// CHECK-NEXT:    0014  EndMarkedSubexpression: 2
// CHECK-NEXT:    0017  EndMarkedSubexpression: 0
// CHECK-NEXT:    001a  MatchChar8: 'e'
// CHECK-NEXT:    001c  BackRefInsn: 0
// CHECK-NEXT:    001f  BackRefInsn: 1
// CHECK-NEXT:    0022  Goal

print(/\b\B/);
// CHECK:       5: /\b\B/
// CHECK-NEXT:    Header: marked: 0 loops: 0 flags: 0 constraints: 0
// CHECK-NEXT:    0000  WordBoundary: \b
// CHECK-NEXT:    0002  WordBoundary: \B
// CHECK-NEXT:    0004  Goal

print(/abc(?=^)(?!def)/i);
// CHECK: Header: marked: 0 loops: 0 flags: 1 constraints: 6
// CHECK-NEXT:    StartSet: 0x41 0x61
// CHECK-NEXT: 0000  MatchNCharICase8: 'ABC'
// CHECK-NEXT: 0005  Lookaround: = (constraints: 2, marked expressions=[0,0), continuation 0x13)
// CHECK-NEXT: 0011  LeftAnchor
// CHECK-NEXT: 0012  Goal
// CHECK-NEXT: 0013  Lookaround: ! (constraints: 4, marked expressions=[0,0), continuation 0x25)
// CHECK-NEXT: 001f  MatchNCharICase8: 'DEF'
// CHECK-NEXT: 0024  Goal
// CHECK-NEXT: 0025  Goal

print(/ab*c+d{3,5}/);
// CHECK:        7: /ab*c+d{3,5}/
// CHECK-NEXT:    Header: marked: 0 loops: 3 flags: 0 constraints: 4
// CHECK-NEXT:    StartSet: 0x61
// CHECK-NEXT:    0000  MatchChar8: 'a'
// CHECK-NEXT:    0002  Width1Loop: 0 greedy {0, 4294967295}
// CHECK-NEXT:    0014  MatchChar8: 'b'
// CHECK-NEXT:    0016  Width1Loop: 1 greedy {1, 4294967295}
// CHECK-NEXT:    0028  MatchChar8: 'c'
// CHECK-NEXT:    002a  Width1Loop: 2 greedy {3, 5}
// CHECK-NEXT:    003c  MatchChar8: 'd'
// CHECK-NEXT:    003e  Goal

print(/a((b+){3})*/);
// CHECK:        8: /a((b+){3})*/
// CHECK-NEXT:    Header: marked: 2 loops: 3 flags: 0 constraints: 4
// CHECK-NEXT:    StartSet: 0x61
// CHECK-NEXT:     0000  MatchChar8: 'a'
// CHECK-NEXT:     0002  BeginLoop: 2 greedy {0, 4294967295} (constraints: 4)
// CHECK-NEXT:     0019  BeginMarkedSubexpression: 0
// CHECK-NEXT:     001c  BeginLoop: 1 greedy {3, 3} (constraints: 4)
// CHECK-NEXT:     0033  BeginMarkedSubexpression: 1
// CHECK-NEXT:     0036  Width1Loop: 0 greedy {1, 4294967295}
// CHECK-NEXT:     0048  MatchChar8: 'b'
// CHECK-NEXT:     004a  EndMarkedSubexpression: 1
// CHECK-NEXT:     004d  EndLoop: 0x1c
// CHECK-NEXT:     0052  EndMarkedSubexpression: 0
// CHECK-NEXT:     0055  EndLoop: 0x02
// CHECK-NEXT:     005a  Goal

print(/(^b)+(c)*?/);
// CHECK:        9: /(^b)+(c)*?/
// CHECK-NEXT:    Header: marked: 2 loops: 2 flags: 0 constraints: 6
// CHECK-NEXT:    StartSet: 0x62
// CHECK-NEXT:     0000  BeginLoop: 0 greedy {1, 4294967295} (constraints: 6)
// CHECK-NEXT:     0017  BeginMarkedSubexpression: 0
// CHECK-NEXT:     001a  LeftAnchor
// CHECK-NEXT:     001b  MatchChar8: 'b'
// CHECK-NEXT:     001d  EndMarkedSubexpression: 0
// CHECK-NEXT:     0020  EndLoop: 0x00
// CHECK-NEXT:     0025  BeginLoop: 1 nongreedy {0, 4294967295} (constraints: 4)
// CHECK-NEXT:     003c  BeginMarkedSubexpression: 1
// CHECK-NEXT:     003f  MatchChar8: 'c'
// CHECK-NEXT:     0041  EndMarkedSubexpression: 1
// CHECK-NEXT:     0044  EndLoop: 0x25
// CHECK-NEXT:     0049  Goal

print(/[\u017f]/i);
// CHECK:        10: /[\u017f]/i
// CHECK-NEXT:    Header: marked: 0 loops: 0 flags: 1 constraints: 5
// CHECK-NEXT:    StartSet: nonascii
// CHECK-NEXT:     0000  BracketICase: [0x17f]
// CHECK-NEXT:     000e  Goal

print(/a*/);
// CHECK:        11: /a*/
// CHECK-NEXT:    Header: marked: 0 loops: 1 flags: 0 constraints: 0
// CHECK-NEXT:     0000  Width1Loop: 0 greedy {0, 4294967295}
// CHECK-NEXT:     0012  MatchChar8: 'a'
// CHECK-NEXT:     0014  Goal

print(/a+/);
// CHECK:        12: /a+/
// CHECK-NEXT:    Header: marked: 0 loops: 1 flags: 0 constraints: 4
// CHECK-NEXT:    StartSet: 0x61
// CHECK-NEXT:    0000  Width1Loop: 0 greedy {1, 4294967295}
// CHECK-NEXT:    0012  MatchChar8: 'a'
// CHECK-NEXT:    0014  Goal

print(/(?:a+)*/);
// CHECK:        13: /(?:a+)*/
// CHECK-NEXT:    Header: marked: 0 loops: 2 flags: 0 constraints: 0
// CHECK-NEXT:     0000  BeginSimpleLoop: (constraints: 4)
// CHECK-NEXT:     0006  Width1Loop: 0 greedy {1, 4294967295}
// CHECK-NEXT:     0018  MatchChar8: 'a'
// CHECK-NEXT:     001a  EndSimpleLoop: 0x00
// CHECK-NEXT:     001f  Goal

print(/(?:a*)*/);
// CHECK:        14: /(?:a*)*/
// CHECK-NEXT:    Header: marked: 0 loops: 2 flags: 0 constraints: 0
// CHECK-NEXT:     0000  BeginLoop: 1 greedy {0, 4294967295} (constraints: 0)
// CHECK-NEXT:     0017  Width1Loop: 0 greedy {0, 4294967295}
// CHECK-NEXT:     0029  MatchChar8: 'a'
// CHECK-NEXT:     002b  EndLoop: 0x00
// CHECK-NEXT:     0030  Goal

print(/[a-zA-Z]*/);
// CHECK:        15: /[a-zA-Z]*/
// CHECK-NEXT:    Header: marked: 0 loops: 1 flags: 0 constraints: 0
// CHECK-NEXT:     0000  Width1Loop: 0 greedy {0, 4294967295}
// CHECK-NEXT:     0012  Bracket: [A-Za-z]
// CHECK-NEXT:     0028  Goal

print(/.*/);
// CHECK:        16: /.*/
// CHECK-NEXT:   Header: marked: 0 loops: 1 flags: 0 constraints: 0
// CHECK-NEXT:    0000  Width1Loop: 0 greedy {0, 4294967295}
// CHECK-NEXT:    0012  MatchAnyButNewline
// CHECK-NEXT:    0013  Goal

print(/a*/i);
// CHECK:        17: /a*/i
// CHECK-NEXT:   Header: marked: 0 loops: 1 flags: 1 constraints: 0
// CHECK-NEXT:    0000  Width1Loop: 0 greedy {0, 4294967295}
// CHECK-NEXT:    0012  MatchCharICase8: 'A'
// CHECK-NEXT:    0014  Goal

print(/(a)(?=(.))/i);
// CHECK:        18: /(a)(?=(.))/i
// CHECK-NEXT:   Header: marked: 2 loops: 0 flags: 1 constraints: 4
// CHECK-NEXT:    StartSet: 0x41 0x61
// CHECK-NEXT:   0000  BeginMarkedSubexpression: 0
// CHECK-NEXT:   0003  MatchCharICase8: 'A'
// CHECK-NEXT:   0005  EndMarkedSubexpression: 0
// CHECK-NEXT:   0008  Lookaround: = (constraints: 4, marked expressions=[1,2), continuation 0x1c)
// CHECK-NEXT:   0014  BeginMarkedSubexpression: 1
// CHECK-NEXT:   0017  MatchAnyButNewline
// CHECK-NEXT:   0018  EndMarkedSubexpression: 1
// CHECK-NEXT:   001b  Goal
// CHECK-NEXT:   001c  Goal

print(/(a)(?<!(.))/i);
// CHECK:        19: /(a)(?<!(.))/i
// CHECK-NEXT:   Header: marked: 2 loops: 0 flags: 1 constraints: 4
// CHECK-NEXT:    StartSet: 0x41 0x61
// CHECK-NEXT:   0000  BeginMarkedSubexpression: 0
// CHECK-NEXT:   0003  MatchCharICase8: 'A'
// CHECK-NEXT:   0005  EndMarkedSubexpression: 0
// CHECK-NEXT:   0008  Lookaround: <! (constraints: 4, marked expressions=[1,2), continuation 0x1c)
// CHECK-NEXT:   0014  BeginMarkedSubexpression: 1
// CHECK-NEXT:   0017  MatchAnyButNewline
// CHECK-NEXT:   0018  EndMarkedSubexpression: 1
// CHECK-NEXT:   001b  Goal
// CHECK-NEXT:   001c  Goal

// There are 255 'a's here.
print(/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaoverflow/);
// CHECK:        20: /{{a{255}overflow}}/
// CHECK-NEXT:   Header: marked: 0 loops: 0 flags: 0 constraints: 4
// CHECK-NEXT:    StartSet: 0x61
// CHECK-NEXT:   0000  MatchNChar8: {{'a{255}'}}
// CHECK-NEXT:   0101  MatchNChar8: 'overflow'
// CHECK-NEXT:   010b  Goal

print(/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaoverflow/i);
// CHECK:        21: /{{a{255}overflow}}/i
// CHECK-NEXT:   Header: marked: 0 loops: 0 flags: 1 constraints: 4
// CHECK-NEXT:    StartSet: 0x41 0x61
// CHECK-NEXT:   0000  MatchNCharICase8: {{'A{255}'}}
// CHECK-NEXT:   0101  MatchNCharICase8: 'OVERFLOW'
// CHECK-NEXT:   010b  Goal

print(/./);
// CHECK-LABEL: 22: /./
// CHECK-NEXT:   Header: marked: 0 loops: 0 flags: 0 constraints: 4
// CHECK-NEXT:    StartSet: 0x00-0x09 0x0b-0x0c 0x0e-0x7f nonascii
// CHECK-NEXT:   0000  MatchAnyButNewline
// CHECK-NEXT:   0001  Goal

print(/./u);
// CHECK-LABEL: 23: /./u
// CHECK-NEXT:   Header: marked: 0 loops: 0 flags: 8 constraints: 4
// CHECK-NEXT:    StartSet: 0x00-0x09 0x0b-0x0c 0x0e-0x7f nonascii
// CHECK-NEXT:   0000  U16MatchAnyButNewline
// CHECK-NEXT:   0001  Goal

print(/./s);
// CHECK-LABEL: 24: /./s
// CHECK-NEXT:   Header: marked: 0 loops: 0 flags: 16 constraints: 4
// CHECK-NEXT:    StartSet: 0x00-0x7f nonascii
// CHECK-NEXT:   0000  MatchAny
// CHECK-NEXT:   0001  Goal

print(/./us);
// CHECK-LABEL: 25: /./us
// CHECK-NEXT:   Header: marked: 0 loops: 0 flags: 24 constraints: 4
// CHECK-NEXT:    StartSet: 0x00-0x7f nonascii
// CHECK-NEXT:   0000  U16MatchAny
// CHECK-NEXT:   0001  Goal

print(/a|b|c|d|e|f/);
// CHECK:       26: /a|b|c|d|e|f/
// CHECK-NEXT:    Header: marked: 0 loops: 0 flags: 0 constraints: 4
// CHECK-NEXT:    StartSet: 0x61-0x66
// CHECK-NEXT:    0000  Alternation: Target 0x0e, constraints 4,4
// CHECK-NEXT:    0007  MatchChar8: 'a'
// CHECK-NEXT:    0009  Jump32: 0x48
// CHECK-NEXT:    000e  Alternation: Target 0x1c, constraints 4,4
// CHECK-NEXT:    0015  MatchChar8: 'b'
// CHECK-NEXT:    0017  Jump32: 0x48
// CHECK-NEXT:    001c  Alternation: Target 0x2a, constraints 4,4
// CHECK-NEXT:    0023  MatchChar8: 'c'
// CHECK-NEXT:    0025  Jump32: 0x48
// CHECK-NEXT:    002a  Alternation: Target 0x38, constraints 4,4
// CHECK-NEXT:    0031  MatchChar8: 'd'
// CHECK-NEXT:    0033  Jump32: 0x48
// CHECK-NEXT:    0038  Alternation: Target 0x46, constraints 4,4
// CHECK-NEXT:    003f  MatchChar8: 'e'
// CHECK-NEXT:    0041  Jump32: 0x48
// CHECK-NEXT:    0046  MatchChar8: 'f'
// CHECK-NEXT:    0048  Goal

print(/(abc|def)/);
// CHECK:       27: /(abc|def)/
// CHECK-NEXT:    Header: marked: 1 loops: 0 flags: 0 constraints: 4
// CHECK-NEXT:    StartSet: 0x61 0x64
// CHECK-NEXT:    0000  BeginMarkedSubexpression: 0
// CHECK-NEXT:    0003  Alternation: Target 0x14, constraints 4,4
// CHECK-NEXT:    000a  MatchNChar8: 'abc'
// CHECK-NEXT:    000f  Jump32: 0x19
// CHECK-NEXT:    0014  MatchNChar8: 'def'
// CHECK-NEXT:    0019  EndMarkedSubexpression: 0
// CHECK-NEXT:    001c  Goal

// A name shared by mutually exclusive capture groups makes \k<a> refer to all
// of them: one BackRefInsn per group, since a backreference to a group that did
// not participate matches empty. An ordinary backreference emits just one.
print(/(?:(?<a>x)|(?<a>y))\k<a>(?<b>z)\k<b>/);
// CHECK:       28: /(?:(?<a>x)|(?<a>y))\k<a>(?<b>z)\k<b>/
// CHECK-NEXT:    Header: marked: 3 loops: 0 flags: 0 constraints: 4
// CHECK-NEXT:    StartSet: 0x78-0x79
// CHECK-NEXT:    0000  Alternation: Target 0x14, constraints 4,4
// CHECK-NEXT:    0007  BeginMarkedSubexpression: 0
// CHECK-NEXT:    000a  MatchChar8: 'x'
// CHECK-NEXT:    000c  EndMarkedSubexpression: 0
// CHECK-NEXT:    000f  Jump32: 0x1c
// CHECK-NEXT:    0014  BeginMarkedSubexpression: 1
// CHECK-NEXT:    0017  MatchChar8: 'y'
// CHECK-NEXT:    0019  EndMarkedSubexpression: 1
// CHECK-NEXT:    001c  BackRefInsn: 0
// CHECK-NEXT:    001f  BackRefInsn: 1
// CHECK-NEXT:    0022  BeginMarkedSubexpression: 2
// CHECK-NEXT:    0025  MatchChar8: 'z'
// CHECK-NEXT:    0027  EndMarkedSubexpression: 2
// CHECK-NEXT:    002a  BackRefInsn: 2
// CHECK-NEXT:    002d  Goal
