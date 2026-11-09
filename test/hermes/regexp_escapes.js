/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -non-strict -O -gc-sanitize-handles=0 -target=HBC %s  | %FileCheck --match-full-lines %s
// RUN: %hermes -non-strict -O -gc-sanitize-handles=0 -target=HBC -emit-binary -out %t.hbc %s && %hermes %t.hbc | %FileCheck --match-full-lines %s
// RUN: %shermes -exec %s -Wx,-gc-sanitize-handles=0  | %FileCheck --match-full-lines %s

print('RegExp Escapes');
// CHECK-LABEL: RegExp Escapes

function printCharCodesMatching(re) {
    // Try one-char codes
    var matches = [];
    for (var i = 0; i < 256; i++) {
        if (re.exec(String.fromCharCode(i))) {
            matches.push(i);
        }
    }
    if (matches.length > 0) {
        print(re + ": " + matches.join(","));
        return;
    }
    // Try two-char codes.
    for (var i = 0; i < 256; i++) {
        var is = String.fromCharCode(i);
        for (var j = 0; j < 256; j++) {
            if (re.exec(is + String.fromCharCode(j))) {
                print(re + ": " + [i,j].join(","));
                return;
            }
        }
    }
}

printCharCodesMatching(/\0/);
printCharCodesMatching(/\1/);
printCharCodesMatching(/\2/);
printCharCodesMatching(/\3/);
printCharCodesMatching(/\4/);
printCharCodesMatching(/\5/);
printCharCodesMatching(/\6/);
printCharCodesMatching(/\7/);
printCharCodesMatching(/\8/);
printCharCodesMatching(/\9/);
// CHECK-NEXT: /\0/: 0
// CHECK-NEXT: /\1/: 1
// CHECK-NEXT: /\2/: 2
// CHECK-NEXT: /\3/: 3
// CHECK-NEXT: /\4/: 4
// CHECK-NEXT: /\5/: 5
// CHECK-NEXT: /\6/: 6
// CHECK-NEXT: /\7/: 7
// CHECK-NEXT: /\8/: 56
// CHECK-NEXT: /\9/: 57

printCharCodesMatching(/\00/);
printCharCodesMatching(/\10/);
printCharCodesMatching(/\20/);
printCharCodesMatching(/\30/);
printCharCodesMatching(/\40/);
printCharCodesMatching(/\50/);
printCharCodesMatching(/\60/);
printCharCodesMatching(/\70/);
// CHECK-NEXT: /\00/: 0
// CHECK-NEXT: /\10/: 8
// CHECK-NEXT: /\20/: 16
// CHECK-NEXT: /\30/: 24
// CHECK-NEXT: /\40/: 32
// CHECK-NEXT: /\50/: 40
// CHECK-NEXT: /\60/: 48
// CHECK-NEXT: /\70/: 56

printCharCodesMatching(/\80/);
printCharCodesMatching(/\90/);
// CHECK-NEXT: /\80/: 56,48
// CHECK-NEXT: /\90/: 57,48

printCharCodesMatching(/\11/);
printCharCodesMatching(/\22/);
printCharCodesMatching(/\33/);
printCharCodesMatching(/\44/);
printCharCodesMatching(/\55/);
printCharCodesMatching(/\66/);
printCharCodesMatching(/\77/);
printCharCodesMatching(/\88/);
printCharCodesMatching(/\99/);
// CHECK-NEXT: /\11/: 9
// CHECK-NEXT: /\22/: 18
// CHECK-NEXT: /\33/: 27
// CHECK-NEXT: /\44/: 36
// CHECK-NEXT: /\55/: 45
// CHECK-NEXT: /\66/: 54
// CHECK-NEXT: /\77/: 63
// CHECK-NEXT: /\88/: 56,56
// CHECK-NEXT: /\99/: 57,57


printCharCodesMatching(/\10/);
printCharCodesMatching(/\11/);
printCharCodesMatching(/\12/);
printCharCodesMatching(/\13/);
printCharCodesMatching(/\14/);
printCharCodesMatching(/\18/);
printCharCodesMatching(/\19/);
printCharCodesMatching(/\81/);
// CHECK-NEXT: /\10/: 8
// CHECK-NEXT: /\11/: 9
// CHECK-NEXT: /\12/: 10
// CHECK-NEXT: /\13/: 11
// CHECK-NEXT: /\14/: 12
// CHECK-NEXT: /\18/: 1,56
// CHECK-NEXT: /\19/: 1,57
// CHECK-NEXT: /\81/: 56,49

printCharCodesMatching(/\10/);
printCharCodesMatching(/\00/);
printCharCodesMatching(/\001/);
printCharCodesMatching(/\002/);
// CHECK-NEXT: /\10/: 8
// CHECK-NEXT: /\00/: 0
// CHECK-NEXT: /\001/: 1
// CHECK-NEXT: /\002/: 2


printCharCodesMatching(/[\0]/);
printCharCodesMatching(/[\1]/);
printCharCodesMatching(/[\2]/);
printCharCodesMatching(/[\3]/);
printCharCodesMatching(/[\4]/);
printCharCodesMatching(/[\5]/);
printCharCodesMatching(/[\6]/);
printCharCodesMatching(/[\7]/);
printCharCodesMatching(/[\8]/);
printCharCodesMatching(/[\9]/);
// CHECK-NEXT: /[\0]/: 0
// CHECK-NEXT: /[\1]/: 1
// CHECK-NEXT: /[\2]/: 2
// CHECK-NEXT: /[\3]/: 3
// CHECK-NEXT: /[\4]/: 4
// CHECK-NEXT: /[\5]/: 5
// CHECK-NEXT: /[\6]/: 6
// CHECK-NEXT: /[\7]/: 7
// CHECK-NEXT: /[\8]/: 56
// CHECK-NEXT: /[\9]/: 57

printCharCodesMatching(/[\00]/);
printCharCodesMatching(/[\10]/);
printCharCodesMatching(/[\20]/);
printCharCodesMatching(/[\30]/);
printCharCodesMatching(/[\40]/);
printCharCodesMatching(/[\50]/);
printCharCodesMatching(/[\60]/);
printCharCodesMatching(/[\70]/);
printCharCodesMatching(/[\80]/);
printCharCodesMatching(/[\90]/);
// CHECK-NEXT: /[\00]/: 0
// CHECK-NEXT: /[\10]/: 8
// CHECK-NEXT: /[\20]/: 16
// CHECK-NEXT: /[\30]/: 24
// CHECK-NEXT: /[\40]/: 32
// CHECK-NEXT: /[\50]/: 40
// CHECK-NEXT: /[\60]/: 48
// CHECK-NEXT: /[\70]/: 56
// CHECK-NEXT: /[\80]/: 48,56
// CHECK-NEXT: /[\90]/: 48,57

printCharCodesMatching(/\00/);
printCharCodesMatching(/\01/);
printCharCodesMatching(/\02/);
printCharCodesMatching(/\03/);
printCharCodesMatching(/\04/);
printCharCodesMatching(/\05/);
printCharCodesMatching(/\06/);
printCharCodesMatching(/\07/);
printCharCodesMatching(/\08/);
printCharCodesMatching(/\09/);
// CHECK-NEXT: /\00/: 0
// CHECK-NEXT: /\01/: 1
// CHECK-NEXT: /\02/: 2
// CHECK-NEXT: /\03/: 3
// CHECK-NEXT: /\04/: 4
// CHECK-NEXT: /\05/: 5
// CHECK-NEXT: /\06/: 6
// CHECK-NEXT: /\07/: 7
// CHECK-NEXT: /\08/: 0,56
// CHECK-NEXT: /\09/: 0,57

printCharCodesMatching(/[\11]/);
printCharCodesMatching(/[\22]/);
printCharCodesMatching(/[\33]/);
printCharCodesMatching(/[\44]/);
printCharCodesMatching(/[\55]/);
printCharCodesMatching(/[\66]/);
printCharCodesMatching(/[\77]/);
printCharCodesMatching(/[\88]/);
printCharCodesMatching(/[\99]/);
// CHECK-NEXT: /[\11]/: 9
// CHECK-NEXT: /[\22]/: 18
// CHECK-NEXT: /[\33]/: 27
// CHECK-NEXT: /[\44]/: 36
// CHECK-NEXT: /[\55]/: 45
// CHECK-NEXT: /[\66]/: 54
// CHECK-NEXT: /[\77]/: 63
// CHECK-NEXT: /[\88]/: 56
// CHECK-NEXT: /[\99]/: 57

printCharCodesMatching(/[\10]/);
printCharCodesMatching(/[\11]/);
printCharCodesMatching(/[\12]/);
printCharCodesMatching(/[\13]/);
printCharCodesMatching(/[\14]/);
printCharCodesMatching(/[\18]/);
printCharCodesMatching(/[\19]/);
printCharCodesMatching(/[\81]/);
printCharCodesMatching(/[\10]/);
printCharCodesMatching(/[\00]/);
printCharCodesMatching(/[\001]/);
printCharCodesMatching(/[\002]/);
printCharCodesMatching(/[\008]/);
// CHECK-NEXT: /[\10]/: 8
// CHECK-NEXT: /[\11]/: 9
// CHECK-NEXT: /[\12]/: 10
// CHECK-NEXT: /[\13]/: 11
// CHECK-NEXT: /[\14]/: 12
// CHECK-NEXT: /[\18]/: 1,56
// CHECK-NEXT: /[\19]/: 1,57
// CHECK-NEXT: /[\81]/: 49,56
// CHECK-NEXT: /[\10]/: 8
// CHECK-NEXT: /[\00]/: 0
// CHECK-NEXT: /[\001]/: 1
// CHECK-NEXT: /[\002]/: 2
// CHECK-NEXT: /[\008]/: 0,56

printCharCodesMatching(/[\00]/);
printCharCodesMatching(/[\01]/);
printCharCodesMatching(/[\02]/);
printCharCodesMatching(/[\03]/);
printCharCodesMatching(/[\04]/);
printCharCodesMatching(/[\05]/);
printCharCodesMatching(/[\06]/);
printCharCodesMatching(/[\07]/);
printCharCodesMatching(/[\08]/);
printCharCodesMatching(/[\09]/);
// CHECK-NEXT: /[\00]/: 0
// CHECK-NEXT: /[\01]/: 1
// CHECK-NEXT: /[\02]/: 2
// CHECK-NEXT: /[\03]/: 3
// CHECK-NEXT: /[\04]/: 4
// CHECK-NEXT: /[\05]/: 5
// CHECK-NEXT: /[\06]/: 6
// CHECK-NEXT: /[\07]/: 7
// CHECK-NEXT: /[\08]/: 0,56
// CHECK-NEXT: /[\09]/: 0,57

// Without the u flag, \c that does not begin a control escape is a literal
// backslash followed by c, except that inside a class a digit or _ after \c
// forms a control character.
printCharCodesMatching(/\cJ/);
printCharCodesMatching(/\c/);
printCharCodesMatching(/\c*/);
printCharCodesMatching(/[\cj]/);
printCharCodesMatching(/[\c]/);
printCharCodesMatching(/[\c1]/);
printCharCodesMatching(/[\c_]/);
printCharCodesMatching(/[\c:]/);
printCharCodesMatching(/[\c1-\c3]/);
printCharCodesMatching(/[\c-e]/);
// CHECK-NEXT: /\cJ/: 10
// CHECK-NEXT: /\c/: 92,99
// CHECK-NEXT: /\c*/: 92
// CHECK-NEXT: /[\cj]/: 10
// CHECK-NEXT: /[\c]/: 92,99
// CHECK-NEXT: /[\c1]/: 17
// CHECK-NEXT: /[\c_]/: 31
// CHECK-NEXT: /[\c:]/: 58,92,99
// CHECK-NEXT: /[\c1-\c3]/: 17,18,19
// CHECK-NEXT: /[\c-e]/: 92,99,100,101

print(/^\c$/.test("\\c"), /^\c$/.test("c"), /^\c$/i.test("\\C"));
print(/^\c1$/.test("\\c1"), /^\c1$/.test("\x11"));
print(/^[\c1]$/.test("\x11"), /^[\c1]$/.test("1"));
// CHECK-NEXT: true false true
// CHECK-NEXT: true false
// CHECK-NEXT: true false

function printRegExpError(source, flags) {
  try {
    new RegExp(source, flags);
    print("no error");
  } catch (e) {
    print(e.name);
  }
}
printRegExpError("[\\c-a]", "");
printRegExpError("\\c", "u");
printRegExpError("[\\c1]", "u");
// CHECK-NEXT: SyntaxError
// CHECK-NEXT: SyntaxError
// CHECK-NEXT: SyntaxError
