/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %shermes -Werror -typed -dump-sema %s 2>&1 | %FileCheck %s

// A long but finite chain of generic class specializations is accepted.

// CHECK-NOT: error
// CHECK: SemContext

class A0<T> {
  m(): void { new A1<T>(); }
}
class A1<T> {
  m(): void { new A2<T>(); }
}
class A2<T> {
  m(): void { new A3<T>(); }
}
class A3<T> {
  m(): void { new A4<T>(); }
}
class A4<T> {
  m(): void { new A5<T>(); }
}
class A5<T> {
  m(): void { new A6<T>(); }
}
class A6<T> {
  m(): void { new A7<T>(); }
}
class A7<T> {
  m(): void { new A8<T>(); }
}
class A8<T> {
  m(): void { new A9<T>(); }
}
class A9<T> {
  m(): void { new A10<T>(); }
}
class A10<T> {
  m(): void { new A11<T>(); }
}
class A11<T> {
  m(): void { new A12<T>(); }
}
class A12<T> {
  m(): void { new A13<T>(); }
}
class A13<T> {
  m(): void { new A14<T>(); }
}
class A14<T> {
  m(): void { new A15<T>(); }
}
class A15<T> {
  m(): void { new A16<T>(); }
}
class A16<T> {
  m(): void { new A17<T>(); }
}
class A17<T> {
  m(): void { new A18<T>(); }
}
class A18<T> {
  m(): void { new A19<T>(); }
}
class A19<T> {
  m(): void { new A20<T>(); }
}
class A20<T> {
  m(): void { new A21<T>(); }
}
class A21<T> {
  m(): void { new A22<T>(); }
}
class A22<T> {
  m(): void { new A23<T>(); }
}
class A23<T> {
  m(): void { new A24<T>(); }
}
class A24<T> {
  m(): void { new A25<T>(); }
}
class A25<T> {
  m(): void { new A26<T>(); }
}
class A26<T> {
  m(): void { new A27<T>(); }
}
class A27<T> {
  m(): void { new A28<T>(); }
}
class A28<T> {
  m(): void { new A29<T>(); }
}
class A29<T> {
  m(): void { new A30<T>(); }
}
class A30<T> {
  m(): void { new A31<T>(); }
}
class A31<T> {
  m(): void { new A32<T>(); }
}
class A32<T> {
  m(): void { new A33<T>(); }
}
class A33<T> {
  m(): void { new A34<T>(); }
}
class A34<T> {
  m(): void { new A35<T>(); }
}
class A35<T> {
  m(): void { new A36<T>(); }
}
class A36<T> {
  m(): void { new A37<T>(); }
}
class A37<T> {
  m(): void { new A38<T>(); }
}
class A38<T> {
  m(): void { new A39<T>(); }
}
class A39<T> {
  m(): void { new A40<T>(); }
}
class A40<T> {
  m(): void { new A41<T>(); }
}
class A41<T> {
  m(): void { new A42<T>(); }
}
class A42<T> {
  m(): void { new A43<T>(); }
}
class A43<T> {
  m(): void { new A44<T>(); }
}
class A44<T> {
  m(): void { new A45<T>(); }
}
class A45<T> {
  m(): void { new A46<T>(); }
}
class A46<T> {
  m(): void { new A47<T>(); }
}
class A47<T> {
  m(): void { new A48<T>(); }
}
class A48<T> {
  m(): void { new A49<T>(); }
}
class A49<T> {
  m(): void { new A50<T>(); }
}
class A50<T> {
  m(): void { new A51<T>(); }
}
class A51<T> {
  m(): void { new A52<T>(); }
}
class A52<T> {
  m(): void { new A53<T>(); }
}
class A53<T> {
  m(): void { new A54<T>(); }
}
class A54<T> {
  m(): void { new A55<T>(); }
}
class A55<T> {
  m(): void { new A56<T>(); }
}
class A56<T> {
  m(): void { new A57<T>(); }
}
class A57<T> {
  m(): void { new A58<T>(); }
}
class A58<T> {
  m(): void { new A59<T>(); }
}
class A59<T> {}
new A0<number>();
