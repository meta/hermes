/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "SemanticValidator.h" // @manual

#include "hermes/AST/RecursiveVisitor.h"
#include "hermes/Support/InternalIdentifierMaker.h"
#include "hermes/Support/StringTable.h"
#include "llvh/ADT/SetVector.h"
#include "llvh/Support/SaveAndRestore.h"

namespace hermes {
namespace sem {
namespace {

/// Implements an AST traversal that performs transformations that simplify
/// block scoping downstream.
class BlockScopingTransformations {
 public:
  explicit BlockScopingTransformations(Context &astContext)
      : astContext_(astContext),
        internalIDs_(astContext.getStringTable()),
        identAssign_(astContext.getIdentifier("=").getUnderlyingPointer()),
        identExclaim_(astContext.getIdentifier("!").getUnderlyingPointer()),
        identLet_(astContext.getIdentifier("let").getUnderlyingPointer()),
        identUndefined_(
            astContext.getIdentifier("undefined").getUnderlyingPointer()),
        identVar_(astContext.getIdentifier("var").getUnderlyingPointer()) {}

  void visit(Node *node) {
    visitESTreeChildren(*this, node);
  }

  ESTree::VisitResult visit(ForInStatementNode *forInStmt) {
    return visitAndRewriteForInOf(forInStmt, forInStmt);
  }

  ESTree::VisitResult visit(ForOfStatementNode *forOfStmt) {
    return visitAndRewriteForInOf(forOfStmt, forOfStmt);
  }

  ESTree::VisitResult visit(ForStatementNode *forStmt) {
    return visitAndRewriteFor(forStmt, forStmt);
  }

  /// Intercepts LabeledStatements
  ///
  ///    label : [label:]+ for-in
  ///    label : [label:]+ for-of
  ///    label : [label:]+ for(const/let
  ///
  /// and applies the relevant transformation.
  ESTree::VisitResult visit(LabeledStatementNode *labeledStmt) {
    LabeledStatementNode *firstLabel = labeledStmt;
    LabeledStatementNode *lastLabel = labeledStmt;

    while (auto *bodyLabel =
               llvh::dyn_cast<LabeledStatementNode>(lastLabel->_body)) {
      lastLabel = bodyLabel;
    }

    Node *body = lastLabel->_body;
    switch (body->getKind()) {
      case NodeKind::ForInStatement:
      case NodeKind::ForOfStatement:
        return visitAndRewriteForInOf(body, firstLabel);

      case NodeKind::ForStatement:
        return visitAndRewriteFor(
            llvh::cast<ForStatementNode>(body), firstLabel);

      default:
        visitESTreeNode(*this, body, lastLabel);
        return ESTree::Unmodified;
    }
  }

  bool incRecursionDepth(Node *n) {
    if (LLVM_UNLIKELY(recursionDepth_ == 0)) {
      return false;
    }
    --recursionDepth_;
    if (LLVM_UNLIKELY(recursionDepth_ == 0)) {
      return false;
    }
    return true;
  }

  void decRecursionDepth() {
    assert(
        recursionDepth_ < MAX_RECURSION_DEPTH &&
        "recursionDepth_ cannot go negative");
    if (LLVM_LIKELY(recursionDepth_ != 0))
      ++recursionDepth_;
  }

 private:
  Context &astContext_;
  InternalIdentifierMaker internalIDs_;

  /// Rewrites
  ///
  ///                   |---- left ---|
  ///   [label:]* for (let/const/var x in/of right) body
  ///
  /// into
  ///
  ///   wrapperBlock: {
  ///     let temp;
  ///     [label:]* for (temp in/of right) {
  ///       let/const/var x = temp;
  ///       body;
  ///     }
  ///     break wrapperBlock;
  ///     let x;  // for TDZ (e.g., right: { x })
  ///   }
  ESTree::VisitResult visitAndRewriteForInOf(Node *forInOfStmt, Node *current) {
    visitESTreeChildren(*this, forInOfStmt);

    Node **left{};
    VariableDeclarationNode *decl;
    Node **body;

    if (auto *forIn = llvh::dyn_cast<ForInStatementNode>(forInOfStmt)) {
      left = &forIn->_left;
      body = &forIn->_body;
    } else if (auto *forOf = llvh::dyn_cast<ForOfStatementNode>(forInOfStmt)) {
      left = &forOf->_left;
      body = &forOf->_body;
    } else {
      assert(false && "If not for-in or for-of, then what?");
    }

    decl = llvh::dyn_cast_or_null<VariableDeclarationNode>(*left);

    if (!decl) {
      // forInOfStmt is not in the form
      //
      //   for (const/let/var
      //
      // so nothing to rewrite.
      return ESTree::Unmodified;
    }

    // The parser ensures there's a single declaration.
    assert(
        decl->_declarations.size() == 1 &&
        "for-in/of(let/const/var should have a single declaration");

    auto *varDecl =
        llvh::cast<VariableDeclaratorNode>(&decl->_declarations.front());

    if (varDecl->_init != nullptr) {
      // This is a semantic error (unless ES2023 B.3.5 is implemented), so don't
      // transform the AST.
      return ESTree::Unmodified;
    }

    UniqueString *temp = internalIDs_.next("forInOf").getUnderlyingPointer();

    // The outer block is:
    //
    // wrapperBlock: {
    //   let temp;
    //   [label:]* for (temp in right) ...
    //   break wrapperBlock;
    //   let x; // for TDZ
    // }
    UniqueString *wrapperBlockLabel =
        internalIDs_.next("wrapperBlock").getUnderlyingPointer();
    BlockStatementNode *wrapperBlock = makeBlock();

    VariableDeclarationNode *tempDecl = makeVarDeclaration(identLet_);
    tempDecl->_declarations.push_back(*makeVarDeclarator(temp));
    wrapperBlock->_body.push_back(*tempDecl);

    wrapperBlock->_body.push_back(*current);

    if (decl->_kind != identVar_) {
      // forInOfStmt is in the for
      //
      //   for (const/let ...
      //
      // Thus emit a fake "let x" declaration after the loop to preserve TDZ
      // semantics in case right is, e.g., right: { x }.
      VariableDeclarationNode *xTDZDecl = makeVarDeclaration(identLet_);
      llvh::SetVector<UniqueString *> ids;
      collectAllIDs(varDecl->_id, ids);
      for (UniqueString *id : ids) {
        xTDZDecl->_declarations.push_back(*makeVarDeclarator(id));
      }
      wrapperBlock->_body.push_back(*makeBreak(wrapperBlockLabel));
      wrapperBlock->_body.push_back(*xTDZDecl);
    }

    // Patching forInOfStmt in-place so that it no longer has forDeclarations.
    *left = makeIdentifier(temp);

    // Now add an initializer (= temp) to decl.
    varDecl->_init = makeIdentifier(temp);

    // Build the new for-in/of body:
    //
    // {
    //   const/let/var x = temp;
    //   body;
    // }
    BlockStatementNode *newBody = makeBlock();
    newBody->_body.push_back(*decl);
    newBody->_body.push_back(**body);
    *body = newBody;

    return makeLabel(wrapperBlockLabel, wrapperBlock);
  }

  /// Rewrites
  ///
  ///   [label:]* for (const/let x = init; test; update) body
  ///
  /// into
  ///
  ///  {
  ///    const/let x = init;
  ///    let temp_x = x;
  ///    let first = true;
  ///    undefined;
  ///    for (;;) {
  ///      const/let x = temp_x;
  ///      if (first) {
  ///        first = false;
  ///      } else {
  ///        update;
  ///      }
  ///      if (!test) break;
  ///      control = true;
  ///      [label:]* for (; control; control = false, temp_x = x) {
  ///        body
  ///      }
  ///      if (control) break;
  ///    }
  ///  }
  ///
  /// which preserves the semantics required by ES2023 14.7.4.3:
  ///
  ///  * each iteration happens in a new environment in which the
  ///    forDeclarations are copied to.
  ///  * update executes in the next iteration's context.
  ///
  /// while preserving the original statement's completion value.
  ESTree::VisitResult visitAndRewriteFor(
      ForStatementNode *forStmt,
      Node *current) {
    visitESTreeChildren(*this, forStmt);

    auto *init =
        llvh::dyn_cast_or_null<VariableDeclarationNode>(forStmt->_init);
    if (!init || init->_kind == identVar_) {
      // forStmt is
      //
      //   for (var ...; ...; ...)
      //   for (expr; ...; ...)
      //
      // which doesn't need to be rewritten.
      return ESTree::Unmodified;
    }

    BlockStatementNode *wrapperBlock = makeBlock();

    BlockStatementNode *outerBlock = makeBlock(); // outer for body.

    Node *test = forStmt->_test;
    Node *update = forStmt->_update;

    // forStmt becomes the inner loop in the transformed AST, and its init,
    // test, and update expressions are used by the outer loop. Thus, "break"
    // the link between forStmt and those components early to prevent unintended
    // usage.
    forStmt->_init = nullptr;
    forStmt->_test = nullptr;
    forStmt->_update = nullptr;

    // The transformation introduces one temporary variable for each declaration
    // in init, and then "copies" between then at specific points:
    //
    //   * tempsDecl declares the temp variables (one per declaration in init),
    //     and each temporary is initialized with the values introduced by init.
    //
    //   * initFromTemps re-declares all variables in init inside the outer
    //     loop; this is a const/let declaration (based on the original init
    //     kind), and each variable is initialize with its corresponding
    //     temporary.
    //
    //   * newUpdate copies the values from the declarations introduced by
    //     initFromTemps back into the temporaries; this means that next time
    //     the outer loop executes initFromTemps will use the updated values.
    VariableDeclarationNode *tempsDecl = makeVarDeclaration(identLet_);
    VariableDeclarationNode *initFromTemps = makeVarDeclaration(init->_kind);
    SequenceExpressionNode *newUpdate = makeSequenceExpression();

    llvh::DenseMap<UniqueString *, UniqueString *> tempIds;
    for (Node &n : init->_declarations) {
      auto *decl = llvh::cast<VariableDeclaratorNode>(&n);
      traverseForLexicalDecl(
          decl->_id, tempIds, tempsDecl, initFromTemps, newUpdate);
    }

    // Creating the wrapper block:
    //
    // {
    //   const/let x = init;
    //   let temp_x = x, first = true, first;
    //   undefined;
    //   outer : for (;;) outerBlock
    // }
    wrapperBlock->_body.push_back(*init);
    wrapperBlock->_body.push_back(*tempsDecl);
    wrapperBlock->_body.push_back(
        *toStatement(makeIdentifier(identUndefined_)));
    wrapperBlock->_body.push_back(
        *makeFor(nullptr, nullptr, nullptr, outerBlock));

    // Creating outerBlock:
    // {
    //   const/let x = temp_x;
    //   if (first)                //
    //     first = false;          // only when
    //   else                      //   update != nullptr
    //     update;                 //
    //   if (!cond);               // only when
    //     break;                  //   cond != nullptr
    //   control = true;
    //   [label:]* for (; control; control = 0, temp_x = x) body;
    //   if (control) break;
    // }
    outerBlock->_body.push_back(*initFromTemps);

    if (update) {
      // if (first)
      //   first = false;
      // else
      //   update;
      UniqueString *first = internalIDs_.next("first").getUnderlyingPointer();
      tempsDecl->_declarations.push_back(
          *makeVarDeclarator(first, makeBooleanLiteral(true)));

      IdentifierNode *firstIsTrue = makeIdentifier(first);
      Node *firstEqFalse = toStatement(
          makeAssignment(makeIdentifier(first), makeBooleanLiteral(false)));
      outerBlock->_body.push_back(
          *makeIf(firstIsTrue, firstEqFalse, toStatement(update)));
    }

    if (test) {
      // if (!cond) break;
      outerBlock->_body.push_back(*makeIf(makeNot(test), makeBreak()));
    }

    // control = true (also add the variable to tempsDecl in the wrapperBlock)
    UniqueString *control =
        internalIDs_.next("forControl").getUnderlyingPointer();
    tempsDecl->_declarations.push_back(*makeVarDeclarator(control));
    outerBlock->_body.push_back(*toStatement(
        makeAssignment(makeIdentifier(control), makeBooleanLiteral(true))));

    // Add control = false to new update expression list.
    newUpdate->_expressions.push_front(
        *makeAssignment(makeIdentifier(control), makeBooleanLiteral(false)));

    // Rewrite forStatement into
    //
    //   for (; control; control = 0, temp_x = x)
    forStmt->_test = makeIdentifier(control);
    forStmt->_update = newUpdate;
    outerBlock->_body.push_back(*current);

    // if (control) break;
    outerBlock->_body.push_back(*makeIf(makeIdentifier(control), makeBreak()));

    return wrapperBlock;
  }

  /// The maximum AST nesting level. Once we reach it, we report an error and
  /// stop.
  static constexpr unsigned MAX_RECURSION_DEPTH =
#if defined(HERMES_LIMIT_STACK_DEPTH) || defined(_MSC_VER)
      512
#else
      1024
#endif
      ;

  /// MAX_RECURSION_DEPTH minus the current AST nesting level. Once it reaches
  /// 0 stop transforming it.
  unsigned recursionDepth_ = MAX_RECURSION_DEPTH;

  UniqueString *const identAssign_;
  UniqueString *const identExclaim_;
  UniqueString *const identLet_;
  UniqueString *const identUndefined_;
  UniqueString *const identVar_;

  /// Collect all IDs appearing in \p node and store them in \p ids. These IDs
  /// appear in a forInOf declaration.
  void collectAllIDs(Node *node, llvh::SetVector<UniqueString *> &ids) {
    switch (node->getKind()) {
      case NodeKind::Empty: {
        break;
      }
      case NodeKind::AssignmentPattern: {
        auto *assignment = llvh::cast<AssignmentPatternNode>(node);
        collectAllIDs(assignment->_left, ids);
        break;
      }
      case NodeKind::RestElement: {
        auto *rest = llvh::cast<RestElementNode>(node);
        collectAllIDs(rest->_argument, ids);
        break;
      }
      case NodeKind::Property: {
        auto *prop = llvh::cast<PropertyNode>(node);
        collectAllIDs(prop->_value, ids);
        break;
      }
      case NodeKind::ObjectPattern: {
        auto *objectPattern = llvh::cast<ObjectPatternNode>(node);
        for (Node &prop : objectPattern->_properties) {
          collectAllIDs(&prop, ids);
        }
        break;
      }
      case NodeKind::ArrayPattern: {
        auto *arrayPattern = llvh::cast<ArrayPatternNode>(node);
        for (Node &element : arrayPattern->_elements) {
          collectAllIDs(&element, ids);
        }
        break;
      }
      case NodeKind::Identifier: {
        auto *id = llvh::cast<IdentifierNode>(node);
        ids.insert(id->_name);
        break;
      }
      default:
        llvm_unreachable("unhandled node in collectAllIDs");
    }
  }

  /// Traverses \p node, which is a ForLexicalDeclaration in a
  ///
  ///   for (const/let ... ; ... ; ...) ...
  ///
  /// statement, and emits VariableDeclarators and AssignmentExpressions used
  /// during for rewrite.
  void traverseForLexicalDecl(
      Node *node,
      llvh::DenseMap<UniqueString *, UniqueString *> &tempIds,
      VariableDeclarationNode *tempsDecl,
      VariableDeclarationNode *initFromTemps,
      SequenceExpressionNode *newUpdate) {
    switch (node->getKind()) {
      case NodeKind::Empty: {
        break;
      }
      case NodeKind::AssignmentPattern: {
        auto *assignment = llvh::cast<AssignmentPatternNode>(node);
        traverseForLexicalDecl(
            assignment->_left, tempIds, tempsDecl, initFromTemps, newUpdate);
        break;
      }
      case NodeKind::RestElement: {
        auto *rest = llvh::dyn_cast<RestElementNode>(node);
        traverseForLexicalDecl(
            rest->_argument, tempIds, tempsDecl, initFromTemps, newUpdate);
        break;
      }
      case NodeKind::Property: {
        auto *prop = llvh::cast<PropertyNode>(node);
        traverseForLexicalDecl(
            prop->_value, tempIds, tempsDecl, initFromTemps, newUpdate);
        break;
      }
      case NodeKind::ObjectPattern: {
        auto *objectPattern = llvh::cast<ObjectPatternNode>(node);
        for (Node &prop : objectPattern->_properties) {
          traverseForLexicalDecl(
              &prop, tempIds, tempsDecl, initFromTemps, newUpdate);
        }
        break;
      }
      case NodeKind::ArrayPattern: {
        auto *arrayPattern = llvh::cast<ArrayPatternNode>(node);
        for (Node &elt : arrayPattern->_elements) {
          traverseForLexicalDecl(
              &elt, tempIds, tempsDecl, initFromTemps, newUpdate);
        }
        break;
      }
      case NodeKind::Identifier: {
        auto *id = llvh::cast<IdentifierNode>(node);
        auto res = tempIds.insert(std::make_pair(id->_name, nullptr));
        if (res.second) {
          res.first->second =
              internalIDs_.next("forDecl").getUnderlyingPointer();
        }

        UniqueString *temp = res.first->second;

        // let temp_x = x (in wrapper block)
        tempsDecl->_declarations.push_back(
            *makeVarDeclarator(temp, makeIdentifier(id->_name)));

        // const/let x = temp_x (in outer loop)
        initFromTemps->_declarations.push_back(
            *makeVarDeclarator(id->_name, makeIdentifier(temp)));

        // temp_x = x; (inner loop update)
        newUpdate->_expressions.push_back(
            *makeAssignment(makeIdentifier(temp), makeIdentifier(id->_name)));

        break;
      }
      default:
        llvm_unreachable("unhandled node in duplicateID");
    }
  }

  //****** Helpers for creating AST nodes ******//

  /// \return (IdentifierNode <<name>>)
  IdentifierNode *makeIdentifier(UniqueString *name) {
    return new (astContext_) IdentifierNode(name, nullptr, false);
  }

  /// \return (VariableDeclarationNode <<kind>> <<{}>>)
  VariableDeclarationNode *makeVarDeclaration(UniqueString *kind) {
    return new (astContext_) VariableDeclarationNode(kind, {});
  }

  /// \return (VariableDeclaratorNode <<init>> (Identifier <<name>>))
  VariableDeclaratorNode *makeVarDeclarator(
      UniqueString *name,
      Node *init = nullptr) {
    return new (astContext_) VariableDeclaratorNode(init, makeIdentifier(name));
  }

  /// \return (AssignmentExpressionNode <<"=">> <<dst>> <<src>>)
  AssignmentExpressionNode *makeAssignment(Node *dst, Node *src) {
    return new (astContext_) AssignmentExpressionNode(identAssign_, dst, src);
  }

  /// \return <<n>> if n is a StatementNode;
  ///         (ExpressionStatementNode <<n>> <<>>) otherwise
  StatementNode *toStatement(Node *n) {
    if (auto *stmt = llvh::dyn_cast<StatementNode>(n)) {
      return stmt;
    }
    return new (astContext_) ExpressionStatementNode(n, nullptr);
  }

  /// \return (UnaryExpressionNode "!" <<n>>)
  UnaryExpressionNode *makeNot(Node *n) {
    return new (astContext_) UnaryExpressionNode(identExclaim_, n, false);
  }

  /// \return (LabeledStatementNode (Identifier <<label>>) <<body>>)
  LabeledStatementNode *makeLabel(UniqueString *label, Node *body) {
    return new (astContext_) LabeledStatementNode(makeIdentifier(label), body);
  }

  /// \return (BreakStatementNode (Identifier <<label>>)?)
  BreakStatementNode *makeBreak(UniqueString *label = nullptr) {
    Node *labelNode{};
    if (label) {
      labelNode = makeIdentifier(label);
    }
    return new (astContext_) BreakStatementNode(labelNode);
  }

  /// \return (BooleanLiteralNode <<value>>)
  BooleanLiteralNode *makeBooleanLiteral(bool value) {
    return new (astContext_) BooleanLiteralNode(value);
  }

  /// \return (BlockStatementNode <<{}>>)
  BlockStatementNode *makeBlock() {
    return new (astContext_) BlockStatementNode({});
  }

  /// \return (SequenceExpressionNode <<{}>>)
  SequenceExpressionNode *makeSequenceExpression() {
    return new (astContext_) SequenceExpressionNode({});
  }

  /// \return (IfStatementNode <<test>> <<consequent>> <<alternate>>?)
  IfStatementNode *
  makeIf(Node *test, Node *consequent, Node *alternate = nullptr) {
    return new (astContext_) IfStatementNode(test, consequent, alternate);
  }

  /// \return (ForStatementNode <<init>>? <<test>>? <<update>>? <<body>>)
  ForStatementNode *makeFor(Node *init, Node *test, Node *update, Node *body) {
    return new (astContext_) ForStatementNode(init, test, update, body);
  }
};

} // namespace

void canonicalizeForBlockScoping(Context &astContext, Node *root) {
  BlockScopingTransformations BST{astContext};
  visitESTreeNode(BST, root);
}
} // namespace sem
} // namespace hermes
