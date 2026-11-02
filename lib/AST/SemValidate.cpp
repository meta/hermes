/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes/AST/SemValidate.h"
#include "hermes/AST/ES6Class.h"

#include "hermes/Support/PerfSection.h"

#include "SemanticValidator.h" // @manual

namespace hermes {
namespace sem {

bool validateAST(Context &astContext, SemContext &semCtx, Node *root) {
  PerfSection validation("Validating JavaScript function AST");
  if (astContext.getCodeGenerationSettings().enableBlockScoping) {
    canonicalizeForBlockScoping(astContext, root);
  }
  if (astContext.getConvertES6Classes()) {
    // Convert ES6 classes to functions
    transformES6Classes(astContext, root);
  }

  // Validate the entire AST.
  SemanticValidator validator{astContext, semCtx, true};
  return validator.doIt(root);
}

bool validateASTForParser(Context &astContext, SemContext &semCtx, Node *root) {
  if (astContext.getCodeGenerationSettings().enableBlockScoping) {
    canonicalizeForBlockScoping(astContext, root);
  }
  SemanticValidator validator{astContext, semCtx, false};
  return validator.doIt(root);
}

bool validateFunctionAST(
    Context &astContext,
    SemContext &semCtx,
    Node *function,
    bool strict) {
  PerfSection validation("Validating JavaScript function AST: Deep");
  if (astContext.getCodeGenerationSettings().enableBlockScoping) {
    canonicalizeForBlockScoping(astContext, function);
  }
  SemanticValidator validator{astContext, semCtx, true};
  return validator.doFunction(function, strict);
}

} // namespace sem
} // namespace hermes
