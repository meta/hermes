/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

//===----------------------------------------------------------------------===//
/// \file
///
/// Clone an AST with its semantic information.
///
/// Each AST node is cloned recursively, and each recursive step has 3 phases:
/// 1. cloneSemaPre(): Set up the semantic info for the current node,
/// so that the semantic information for its children can be populated in the
/// correct place.
///   * Create FunctionInfo and/or LexicalScope if necessary.
///   * Record mapping from old decls to new decls in new LexicalScope.
/// 2. clone(): Actually perform the cloning recursively.
/// 3. cloneSemaPost(): Finalize the semantic info.
///   * Populate AST decoration information and label indices.
///   * Clone DeclCollector.
///   * Populate hoistedFunctions on new LexicalScope.
///
/// Cloning semantic information requires mapping from old->new for certain
/// nodes. This is done via the \c clonedNodes_ map.
//===----------------------------------------------------------------------===//

#include "ESTreeClone.h"

#include "hermes/AST/ESTree.h"
#include "hermes/AST/RecursiveVisitor.h"
#include "hermes/Sema/SemContext.h"

#include "llvh/Support/SaveAndRestore.h"

#define DEBUG_TYPE "ESTreeClone"

namespace hermes::sema {

using namespace ESTree;

namespace {

/// Helper class for cloning AST nodes.
class Cloner {
  /// Context to allocate the new nodes in.
  Context &astContext_;

  /// Context to allocate the new nodes in.
  SemContext &semContext_;

  /// Map to add the DeclCollector for the cloned node to.
  DeclCollectorMapTy &declCollectorMap_;

  /// The FunctionInfo for the function in which we're performing the clone.
  /// Not the function being cloned.
  FunctionInfo *const rootFunction_;

  /// The scope in which we're performing the clone.
  LexicalScope *const rootScope_;

  /// Node to start cloning from.
  ESTree::Node *const root_;

  /// Remaining recursion depth available.
  /// When this reaches 0, report an error.
  unsigned recursionDepth_ = kASTMaxRecursionDepth;

  /// Whether we've failed to clone the whole tree.
  bool failed_ = false;

  /// Current FunctionInfo of the cloned AST.
  FunctionInfo *curNewFunction_ = nullptr;

  /// Current FunctionInfo of the cloned AST.
  LexicalScope *curNewScope_ = nullptr;

  /// Stack of mappings of old->new nodes.
  /// Push an element when entering a new function, pop when leaving.
  /// Note that elements here can move around, so don't hold pointers to
  /// elements across recursive calls to clone().
  llvh::SmallVector<llvh::DenseMap<ESTree::Node *, ESTree::Node *>, 2>
      clonedNodesMaps_{};

  /// Mapping from old->new decls.
  llvh::DenseMap<Decl *, Decl *> clonedDecls_{};

 public:
  explicit Cloner(
      Context &astContext,
      SemContext &semContext,
      DeclCollectorMapTy &declCollectorMap,
      ESTree::Node *root,
      FunctionInfo *functionInfo,
      LexicalScope *scope)
      : astContext_(astContext),
        semContext_(semContext),
        declCollectorMap_(declCollectorMap),
        rootFunction_(functionInfo),
        rootScope_(scope),
        root_(root) {}

  /// Clone the root.
  /// \return the cloned node, nullptr if there were errors.
  Node *run() {
    curNewFunction_ = rootFunction_;
    curNewScope_ = rootScope_;
    clonedNodesMaps_.emplace_back();

    // Clone the old decl if it exists.
    // This is only used to reassign the semantic info for the top-level
    // declaration's name.
    // Inside the newly cloned AST, the old decl will be used,
    // because the old decl is the actual definition of the generic.
    Decl *oldDecl = nullptr;
    if (auto *func = llvh::dyn_cast<FunctionDeclarationNode>(root_)) {
      oldDecl =
          semContext_.getDeclarationDecl(llvh::cast<IdentifierNode>(func->_id));
    } else if (auto *classDecl = llvh::dyn_cast<ClassDeclarationNode>(root_)) {
      oldDecl = semContext_.getDeclarationDecl(
          llvh::cast<IdentifierNode>(classDecl->_id));
    }

    Decl *newDecl =
        oldDecl ? semContext_.cloneDeclIntoScope(oldDecl, rootScope_) : nullptr;

    // Clone the whole AST.
    Node *result;
    clone(root_, result);
    if (failed_)
      return nullptr;

    if (auto *funcResult = llvh::dyn_cast<FunctionDeclarationNode>(result)) {
      // Insert final semantic information into the function/scope.
      rootScope_->hoistedFunctions.push_back(funcResult);
      assert(newDecl && "no declaration for function");
      semContext_.setBothDecl(
          llvh::cast<IdentifierNode>(funcResult->_id), newDecl);
    } else if (
        auto *classResult = llvh::dyn_cast<ClassDeclarationNode>(result)) {
      assert(newDecl && "no declaration for function");
      semContext_.setBothDecl(
          llvh::cast<IdentifierNode>(classResult->_id), newDecl);
    }

    return result;
  }

 private:
  /// Report the recursion depth error.
  void recursionDepthExceeded(Node *node) {
    astContext_.getSourceErrorManager().error(
        node->getEndLoc(),
        "too many nested expressions/statements/declarations");
  }

  /// \return the current clonedNodes map.
  llvh::DenseMap<ESTree::Node *, ESTree::Node *> &clonedNodes() {
    return clonedNodesMaps_.back();
  }

  /// Set up any cloned SemContext state needed prior to cloning the children.
  void cloneSemaPre(Node *oldNode) {
    // Determine if this node created a scope.
    LexicalScope *oldScope = nullptr;
    if (auto *func = llvh::dyn_cast<FunctionLikeNode>(oldNode)) {
      LLVM_DEBUG(
          llvh::dbgs() << "cloning function scope for node: "
                       << oldNode->getNodeName() << "\n");
      FunctionInfo *oldInfo = func->getSemInfo();
      clonedNodesMaps_.emplace_back();
      curNewFunction_ = semContext_.prepareClonedFunction(
          oldInfo, curNewFunction_, curNewScope_);
      // Parameter expressions put the parameters and 'arguments' in a separate
      // parameter scope, the parent of the body scope.
      // Scopes in between have no decls the body can reference, and any scopes
      // created by the parameter expressions are parented to the body scope.
      if (oldInfo->hasParameterExpressions)
        cloneScope(oldInfo->getParameterScope());
      oldScope = oldInfo->getFunctionBodyScope();
    } else if (auto *scopeDec = getDecoration<ScopeDecorationBase>(oldNode);
               scopeDec && scopeDec->getScope()) {
      LLVM_DEBUG(
          llvh::dbgs() << "cloning scope for node: " << oldNode->getNodeName()
                       << "\n");
      oldScope = scopeDec->getScope();
    }

    // If the node created a scope, create a new cloned scope.
    // Hoisted functions will be transferred after the node is cloned,
    // because the new nodes haven't been created yet but we need a scope to
    // insert into as we go down.
    if (oldScope)
      cloneScope(oldScope);

    // Set function-specific information if necessary.
    if (auto *func = llvh::dyn_cast<FunctionLikeNode>(oldNode)) {
      FunctionInfo *oldInfo = func->getSemInfo();
      // The body scope index may differ, since not every scope is cloned.
      curNewFunction_->functionBodyScopeIdx = curNewScope_->idxInParentFunction;
      if (oldInfo->argumentsDecl) {
        auto it = clonedDecls_.find(oldInfo->argumentsDecl.getValue());
        assert(it != clonedDecls_.end() && "argumentsDecl not cloned");
        curNewFunction_->argumentsDecl = it->second;
      }
    }
  }

  /// Create a clone of \p oldScope in the current function and scope, map its
  /// decls so identifiers can be cloned properly, and make it the current
  /// scope.
  void cloneScope(LexicalScope *oldScope) {
    curNewScope_ =
        semContext_.prepareClonedScope(oldScope, curNewFunction_, curNewScope_);
    assert(curNewScope_->decls.size() == oldScope->decls.size());
    for (size_t i = 0, e = oldScope->decls.size(); i < e; ++i) {
      clonedDecls_[oldScope->decls[i]] = curNewScope_->decls[i];
    }
  }

  /// \return the cloned decl if it was cloned, the same decl if it wasn't
  /// cloned and should be reused.
  Decl *getClonedDecl(Decl *oldDecl) {
    if (oldDecl->scope->depth <= rootScope_->depth) {
      // If the decl is at the root level or above it, then it isn't cloned.
      // Reference the original one.
      return oldDecl;
    }
    auto it = clonedDecls_.find(oldDecl);
    assert(it != clonedDecls_.end() && "cloned decl not found");
    return it->second;
  }

  /// Finish cloning the node by copying any semantic info to the new node.
  /// Couldn't do that before the new node existed.
  void cloneSemaPost(Node *oldNode, Node *newNode) {
    newNode->setSourceRange(oldNode->getSourceRange());
    newNode->setDebugLoc(oldNode->getDebugLoc());

    // If we finished cloning a function, we pop the last clonedNodes map.
    // We also have to insert the old->new mapping into two maps, because
    // DeclCollector can use the mapping both for cloning the scope node and
    // for cloning the decl node.
    llvh::DenseMap<ESTree::Node *, ESTree::Node *> clonedNodesForFunc{};
    if (llvh::isa<FunctionLikeNode>(oldNode)) {
      clonedNodesForFunc = clonedNodesMaps_.pop_back_val();
      clonedNodesForFunc[oldNode] = newNode;
    }

    // Store the clone in the current map.
    clonedNodes()[oldNode] = newNode;

    // Copy IdentifierDecoration information.
    if (auto *oldIdent = llvh::dyn_cast<IdentifierNode>(oldNode)) {
      auto *exprDecl = semContext_.getExpressionDecl(oldIdent);
      auto *declDecl = semContext_.getDeclarationDecl(oldIdent);
      if (exprDecl && exprDecl == declDecl) {
        semContext_.setBothDecl(
            llvh::cast<IdentifierNode>(newNode), getClonedDecl(exprDecl));
      } else if (exprDecl) {
        semContext_.setExpressionDecl(
            llvh::cast<IdentifierNode>(newNode), getClonedDecl(exprDecl));
      } else if (declDecl) {
        semContext_.setDeclarationDecl(
            llvh::cast<IdentifierNode>(newNode), getClonedDecl(declDecl));
      }
    }

    if (auto *labelDec = getDecoration<LabelDecorationBase>(oldNode);
        labelDec && labelDec->isLabelIndexSet()) {
      getDecoration<LabelDecorationBase>(newNode)->setLabelIndex(
          labelDec->getLabelIndex());
    }

    if (auto *oldBlock = llvh::dyn_cast<BlockStatementNode>(oldNode)) {
      auto *newBlock = llvh::cast<BlockStatementNode>(newNode);
      newBlock->bufferId = oldBlock->bufferId;
      newBlock->isLazyFunctionBody = oldBlock->isLazyFunctionBody;
      newBlock->paramYield = oldBlock->paramYield;
      newBlock->paramAwait = oldBlock->paramAwait;
    }

    // Copy FunctionLikeDecoration information.
    if (auto *func = llvh::dyn_cast<FunctionLikeNode>(oldNode)) {
      llvh::cast<ESTree::FunctionLikeNode>(newNode)->setSemInfo(
          curNewFunction_);
      llvh::cast<ESTree::FunctionLikeNode>(newNode)->strictness =
          func->strictness;
      llvh::cast<ESTree::FunctionLikeNode>(newNode)->isMethodDefinition =
          func->isMethodDefinition;

      // Clone decorations attached via Hermes.decorate(...) calls.
      auto *oldDeco = ESTree::getDecoration<ESTree::FunctionLikeDecoration>(
          llvh::cast<ESTree::FunctionLikeNode>(oldNode));
      auto *newDeco = ESTree::getDecoration<ESTree::FunctionLikeDecoration>(
          llvh::cast<ESTree::FunctionLikeNode>(newNode));
      assert(
          oldDeco && newDeco &&
          "FunctionLikeNode missing FunctionLikeDecoration");
      clone(oldDeco->decorations, newDeco->decorations);

      // If a node has an entry in the declCollectorMap_,
      // clone that entry as well so that FlowChecker can use it.
      auto it = declCollectorMap_.find(func);
      if (it != declCollectorMap_.end()) {
        LLVM_DEBUG(
            llvh::dbgs() << "cloning DeclCollector for node: "
                         << oldNode->getNodeName() << "\n");
        DeclCollector &declCollector = *it->second;
        auto it = clonedNodesForFunc.find(func);
        assert(it != clonedNodesForFunc.end() && "didn't store clone");
        declCollectorMap_.try_emplace(
            llvh::cast<ESTree::FunctionLikeNode>(it->second),
            DeclCollector::clone(declCollector, clonedNodesForFunc));
      }

      // The body scope isn't associated with a node, so transfer its hoisted
      // functions here.
      for (ESTree::FunctionDeclarationNode *hoisted :
           func->getSemInfo()->getFunctionBodyScope()->hoistedFunctions) {
        auto it = clonedNodesForFunc.find(hoisted);
        assert(it != clonedNodesForFunc.end() && "didn't store clone");
        curNewFunction_->getFunctionBodyScope()->hoistedFunctions.push_back(
            llvh::cast<ESTree::FunctionDeclarationNode>(it->second));
      }

      // The function may have created more than one scope.
      curNewScope_ = curNewFunction_->parentScope;
      curNewFunction_ = curNewFunction_->parentFunction;
    }

    // Copy ScopeDecorationBase information.
    if (auto *scopeDec = getDecoration<ScopeDecorationBase>(oldNode);
        scopeDec && scopeDec->getScope()) {
      LexicalScope *oldScope = scopeDec->getScope();
      LexicalScope *newScope = curNewScope_;

      // Transfer hoisted functions.
      for (ESTree::FunctionDeclarationNode *func : oldScope->hoistedFunctions) {
        auto it = clonedNodes().find(func);
        assert(it != clonedNodes().end() && "didn't store clone");
        newScope->hoistedFunctions.push_back(
            llvh::cast<ESTree::FunctionDeclarationNode>(it->second));
      }

      getDecoration<ScopeDecorationBase>(newNode)->setScope(curNewScope_);
      curNewScope_ = curNewScope_->parentScope;
    }

    // Copy ClassLikeDecoration synthetic FunctionInfos.
    // This must be done after ScopeDecorationBase handling has restored
    // curNewScope_ to the parent scope, because the synthetic functions
    // use the enclosing function/scope as their parent.
    if (auto *classDec = getDecoration<ClassLikeDecoration>(oldNode)) {
      auto *newClassDec = getDecoration<ClassLikeDecoration>(newNode);
      auto cloneSyntheticFunc =
          [this](FunctionInfo *oldFunc) -> FunctionInfo * {
        if (!oldFunc)
          return nullptr;
        FunctionInfo *newFunc = semContext_.prepareClonedFunction(
            oldFunc, curNewFunction_, curNewScope_);
        semContext_.prepareClonedScope(
            oldFunc->getFunctionBodyScope(), newFunc, curNewScope_);
        return newFunc;
      };
      newClassDec->implicitCtorFunctionInfo =
          cloneSyntheticFunc(classDec->implicitCtorFunctionInfo);
      newClassDec->instanceElementsInitFunctionInfo =
          cloneSyntheticFunc(classDec->instanceElementsInitFunctionInfo);
      newClassDec->staticElementsInitFunctionInfo =
          cloneSyntheticFunc(classDec->staticElementsInitFunctionInfo);
    }
  }

  /// Clone the \p node.
  /// \p[out] result set to the cloned node. If it failed to clone, return
  /// EmptyNode and set failed_ to true. If it was nullptr, return nullptr.
  void clone(Node *node, Node *&result) {
    if (!node) {
      result = nullptr;
      return;
    }

    llvh::SaveAndRestore<unsigned> oldDepth{
        recursionDepth_, recursionDepth_ - 1};
    if (LLVM_UNLIKELY(recursionDepth_ == 0)) {
      recursionDepthExceeded(node);
      failed_ = true;
      // Don't return nullptr yet, to avoid creating an invalid AST.
      // The failed_ flag will be checked at the end of Cloner::run.
      result = new (astContext_) ESTree::EmptyNode();
      return;
    }

    cloneSemaPre(node);

    switch (node->getKind()) {
      default:
        llvm_unreachable("invalid node kind");

#define CLONE(NAME)                             \
  case NodeKind::NAME:                          \
    result = cloneImpl(cast<NAME##Node>(node)); \
    break;

#define ESTREE_NODE_0_ARGS(NAME, ...) CLONE(NAME)
#define ESTREE_NODE_1_ARGS(NAME, ...) CLONE(NAME)
#define ESTREE_NODE_2_ARGS(NAME, ...) CLONE(NAME)
#define ESTREE_NODE_3_ARGS(NAME, ...) CLONE(NAME)
#define ESTREE_NODE_4_ARGS(NAME, ...) CLONE(NAME)
#define ESTREE_NODE_5_ARGS(NAME, ...) CLONE(NAME)
#define ESTREE_NODE_6_ARGS(NAME, ...) CLONE(NAME)
#define ESTREE_NODE_7_ARGS(NAME, ...) CLONE(NAME)
#define ESTREE_NODE_8_ARGS(NAME, ...) CLONE(NAME)
#define ESTREE_NODE_9_ARGS(NAME, ...) CLONE(NAME)
#define ESTREE_NODE_10_ARGS(NAME, ...) CLONE(NAME)

#include "hermes/AST/ESTree.def"

#undef CLONE
    }

    cloneSemaPost(node, result);
  }

  /// \p[out] result populated with the cloned nodes.
  void clone(NodeLabel n, NodeLabel &result) const {
    result = n;
  }
  /// \p[out] result populated with the cloned nodes.
  void clone(NodeBoolean n, NodeBoolean &result) const {
    result = n;
  }
  /// \p[out] result populated with the cloned nodes.
  void clone(NodeNumber n, NodeNumber &result) const {
    result = n;
  }

  /// Clone a list by cloning each element of the list.
  /// \p[out] result populated with the cloned nodes.
  void clone(NodeList &list, NodeList &result) {
    for (Node &node : list) {
      Node *newNode;
      clone(&node, newNode);
      result.push_back(*newNode);
    }
  }

  /// Declare helper functions to recursively clone the node's children.
  /// These operate a bit obtusely, primarily because we can't return
  /// NodeList from its `clone` overload, so we use output parameters for all
  /// calls to `clone`.
  /// Then we have to call std::move() because NodeList needs to be passed with
  /// std::move to each `Node` constructor.

#define ESTREE_NODE_0_ARGS(NAME, BASE)      \
  NAME##Node *cloneImpl(NAME##Node *node) { \
    return new (astContext_) NAME##Node();  \
  }

#define ESTREE_NODE_1_ARGS(NAME, BASE, ARG0TY, ARG0NM, ARG0OPT) \
  NAME##Node *cloneImpl(NAME##Node *node) {                     \
    ARG0TY arg0;                                                \
    clone(node->_##ARG0NM, arg0);                               \
    return new (astContext_) NAME##Node(std::move(arg0));       \
  }

#define ESTREE_NODE_2_ARGS(                                                \
    NAME, BASE, ARG0TY, ARG0NM, ARG0OPT, ARG1TY, ARG1NM, ARG1OPT)          \
  NAME##Node *cloneImpl(NAME##Node *node) {                                \
    ARG0TY arg0;                                                           \
    ARG1TY arg1;                                                           \
    clone(node->_##ARG0NM, arg0);                                          \
    clone(node->_##ARG1NM, arg1);                                          \
    return new (astContext_) NAME##Node(std::move(arg0), std::move(arg1)); \
  }

#define ESTREE_NODE_3_ARGS(                                            \
    NAME,                                                              \
    BASE,                                                              \
    ARG0TY,                                                            \
    ARG0NM,                                                            \
    ARG0OPT,                                                           \
    ARG1TY,                                                            \
    ARG1NM,                                                            \
    ARG1OPT,                                                           \
    ARG2TY,                                                            \
    ARG2NM,                                                            \
    ARG2OPT)                                                           \
  NAME##Node *cloneImpl(NAME##Node *node) {                            \
    ARG0TY arg0;                                                       \
    ARG1TY arg1;                                                       \
    ARG2TY arg2;                                                       \
    clone(node->_##ARG0NM, arg0);                                      \
    clone(node->_##ARG1NM, arg1);                                      \
    clone(node->_##ARG2NM, arg2);                                      \
    return new (astContext_)                                           \
        NAME##Node(std::move(arg0), std::move(arg1), std::move(arg2)); \
  }

#define ESTREE_NODE_4_ARGS(                                                  \
    NAME,                                                                    \
    BASE,                                                                    \
    ARG0TY,                                                                  \
    ARG0NM,                                                                  \
    ARG0OPT,                                                                 \
    ARG1TY,                                                                  \
    ARG1NM,                                                                  \
    ARG1OPT,                                                                 \
    ARG2TY,                                                                  \
    ARG2NM,                                                                  \
    ARG2OPT,                                                                 \
    ARG3TY,                                                                  \
    ARG3NM,                                                                  \
    ARG3OPT)                                                                 \
  NAME##Node *cloneImpl(NAME##Node *node) {                                  \
    ARG0TY arg0;                                                             \
    ARG1TY arg1;                                                             \
    ARG2TY arg2;                                                             \
    ARG3TY arg3;                                                             \
    clone(node->_##ARG0NM, arg0);                                            \
    clone(node->_##ARG1NM, arg1);                                            \
    clone(node->_##ARG2NM, arg2);                                            \
    clone(node->_##ARG3NM, arg3);                                            \
    return new (astContext_) NAME##Node(                                     \
        std::move(arg0), std::move(arg1), std::move(arg2), std::move(arg3)); \
  }

#define ESTREE_NODE_5_ARGS(                 \
    NAME,                                   \
    BASE,                                   \
    ARG0TY,                                 \
    ARG0NM,                                 \
    ARG0OPT,                                \
    ARG1TY,                                 \
    ARG1NM,                                 \
    ARG1OPT,                                \
    ARG2TY,                                 \
    ARG2NM,                                 \
    ARG2OPT,                                \
    ARG3TY,                                 \
    ARG3NM,                                 \
    ARG3OPT,                                \
    ARG4TY,                                 \
    ARG4NM,                                 \
    ARG4OPT)                                \
  NAME##Node *cloneImpl(NAME##Node *node) { \
    ARG0TY arg0;                            \
    ARG1TY arg1;                            \
    ARG2TY arg2;                            \
    ARG3TY arg3;                            \
    ARG4TY arg4;                            \
    clone(node->_##ARG0NM, arg0);           \
    clone(node->_##ARG1NM, arg1);           \
    clone(node->_##ARG2NM, arg2);           \
    clone(node->_##ARG3NM, arg3);           \
    clone(node->_##ARG4NM, arg4);           \
    return new (astContext_) NAME##Node(    \
        std::move(arg0),                    \
        std::move(arg1),                    \
        std::move(arg2),                    \
        std::move(arg3),                    \
        std::move(arg4));                   \
  }

#define ESTREE_NODE_6_ARGS(                 \
    NAME,                                   \
    BASE,                                   \
    ARG0TY,                                 \
    ARG0NM,                                 \
    ARG0OPT,                                \
    ARG1TY,                                 \
    ARG1NM,                                 \
    ARG1OPT,                                \
    ARG2TY,                                 \
    ARG2NM,                                 \
    ARG2OPT,                                \
    ARG3TY,                                 \
    ARG3NM,                                 \
    ARG3OPT,                                \
    ARG4TY,                                 \
    ARG4NM,                                 \
    ARG4OPT,                                \
    ARG5TY,                                 \
    ARG5NM,                                 \
    ARG5OPT)                                \
  NAME##Node *cloneImpl(NAME##Node *node) { \
    ARG0TY arg0;                            \
    ARG1TY arg1;                            \
    ARG2TY arg2;                            \
    ARG3TY arg3;                            \
    ARG4TY arg4;                            \
    ARG5TY arg5;                            \
    clone(node->_##ARG0NM, arg0);           \
    clone(node->_##ARG1NM, arg1);           \
    clone(node->_##ARG2NM, arg2);           \
    clone(node->_##ARG3NM, arg3);           \
    clone(node->_##ARG4NM, arg4);           \
    clone(node->_##ARG5NM, arg5);           \
    return new (astContext_) NAME##Node(    \
        std::move(arg0),                    \
        std::move(arg1),                    \
        std::move(arg2),                    \
        std::move(arg3),                    \
        std::move(arg4),                    \
        std::move(arg5));                   \
  }

#define ESTREE_NODE_7_ARGS(                 \
    NAME,                                   \
    BASE,                                   \
    ARG0TY,                                 \
    ARG0NM,                                 \
    ARG0OPT,                                \
    ARG1TY,                                 \
    ARG1NM,                                 \
    ARG1OPT,                                \
    ARG2TY,                                 \
    ARG2NM,                                 \
    ARG2OPT,                                \
    ARG3TY,                                 \
    ARG3NM,                                 \
    ARG3OPT,                                \
    ARG4TY,                                 \
    ARG4NM,                                 \
    ARG4OPT,                                \
    ARG5TY,                                 \
    ARG5NM,                                 \
    ARG5OPT,                                \
    ARG6TY,                                 \
    ARG6NM,                                 \
    ARG6OPT)                                \
  NAME##Node *cloneImpl(NAME##Node *node) { \
    ARG0TY arg0;                            \
    ARG1TY arg1;                            \
    ARG2TY arg2;                            \
    ARG3TY arg3;                            \
    ARG4TY arg4;                            \
    ARG5TY arg5;                            \
    ARG6TY arg6;                            \
    clone(node->_##ARG0NM, arg0);           \
    clone(node->_##ARG1NM, arg1);           \
    clone(node->_##ARG2NM, arg2);           \
    clone(node->_##ARG3NM, arg3);           \
    clone(node->_##ARG4NM, arg4);           \
    clone(node->_##ARG5NM, arg5);           \
    clone(node->_##ARG6NM, arg6);           \
    return new (astContext_) NAME##Node(    \
        std::move(arg0),                    \
        std::move(arg1),                    \
        std::move(arg2),                    \
        std::move(arg3),                    \
        std::move(arg4),                    \
        std::move(arg5),                    \
        std::move(arg6));                   \
  }

#define ESTREE_NODE_8_ARGS(                 \
    NAME,                                   \
    BASE,                                   \
    ARG0TY,                                 \
    ARG0NM,                                 \
    ARG0OPT,                                \
    ARG1TY,                                 \
    ARG1NM,                                 \
    ARG1OPT,                                \
    ARG2TY,                                 \
    ARG2NM,                                 \
    ARG2OPT,                                \
    ARG3TY,                                 \
    ARG3NM,                                 \
    ARG3OPT,                                \
    ARG4TY,                                 \
    ARG4NM,                                 \
    ARG4OPT,                                \
    ARG5TY,                                 \
    ARG5NM,                                 \
    ARG5OPT,                                \
    ARG6TY,                                 \
    ARG6NM,                                 \
    ARG6OPT,                                \
    ARG7TY,                                 \
    ARG7NM,                                 \
    ARG7OPT)                                \
  NAME##Node *cloneImpl(NAME##Node *node) { \
    ARG0TY arg0;                            \
    ARG1TY arg1;                            \
    ARG2TY arg2;                            \
    ARG3TY arg3;                            \
    ARG4TY arg4;                            \
    ARG5TY arg5;                            \
    ARG6TY arg6;                            \
    ARG7TY arg7;                            \
    clone(node->_##ARG0NM, arg0);           \
    clone(node->_##ARG1NM, arg1);           \
    clone(node->_##ARG2NM, arg2);           \
    clone(node->_##ARG3NM, arg3);           \
    clone(node->_##ARG4NM, arg4);           \
    clone(node->_##ARG5NM, arg5);           \
    clone(node->_##ARG6NM, arg6);           \
    clone(node->_##ARG7NM, arg7);           \
    return new (astContext_) NAME##Node(    \
        std::move(arg0),                    \
        std::move(arg1),                    \
        std::move(arg2),                    \
        std::move(arg3),                    \
        std::move(arg4),                    \
        std::move(arg5),                    \
        std::move(arg6),                    \
        std::move(arg7));                   \
  }

#define ESTREE_NODE_9_ARGS(                 \
    NAME,                                   \
    BASE,                                   \
    ARG0TY,                                 \
    ARG0NM,                                 \
    ARG0OPT,                                \
    ARG1TY,                                 \
    ARG1NM,                                 \
    ARG1OPT,                                \
    ARG2TY,                                 \
    ARG2NM,                                 \
    ARG2OPT,                                \
    ARG3TY,                                 \
    ARG3NM,                                 \
    ARG3OPT,                                \
    ARG4TY,                                 \
    ARG4NM,                                 \
    ARG4OPT,                                \
    ARG5TY,                                 \
    ARG5NM,                                 \
    ARG5OPT,                                \
    ARG6TY,                                 \
    ARG6NM,                                 \
    ARG6OPT,                                \
    ARG7TY,                                 \
    ARG7NM,                                 \
    ARG7OPT,                                \
    ARG8TY,                                 \
    ARG8NM,                                 \
    ARG8OPT)                                \
  NAME##Node *cloneImpl(NAME##Node *node) { \
    ARG0TY arg0;                            \
    ARG1TY arg1;                            \
    ARG2TY arg2;                            \
    ARG3TY arg3;                            \
    ARG4TY arg4;                            \
    ARG5TY arg5;                            \
    ARG6TY arg6;                            \
    ARG7TY arg7;                            \
    ARG8TY arg8;                            \
    clone(node->_##ARG0NM, arg0);           \
    clone(node->_##ARG1NM, arg1);           \
    clone(node->_##ARG2NM, arg2);           \
    clone(node->_##ARG3NM, arg3);           \
    clone(node->_##ARG4NM, arg4);           \
    clone(node->_##ARG5NM, arg5);           \
    clone(node->_##ARG6NM, arg6);           \
    clone(node->_##ARG7NM, arg7);           \
    clone(node->_##ARG8NM, arg8);           \
    return new (astContext_) NAME##Node(    \
        std::move(arg0),                    \
        std::move(arg1),                    \
        std::move(arg2),                    \
        std::move(arg3),                    \
        std::move(arg4),                    \
        std::move(arg5),                    \
        std::move(arg6),                    \
        std::move(arg7),                    \
        std::move(arg8));                   \
  }

#define ESTREE_NODE_10_ARGS(                \
    NAME,                                   \
    BASE,                                   \
    ARG0TY,                                 \
    ARG0NM,                                 \
    ARG0OPT,                                \
    ARG1TY,                                 \
    ARG1NM,                                 \
    ARG1OPT,                                \
    ARG2TY,                                 \
    ARG2NM,                                 \
    ARG2OPT,                                \
    ARG3TY,                                 \
    ARG3NM,                                 \
    ARG3OPT,                                \
    ARG4TY,                                 \
    ARG4NM,                                 \
    ARG4OPT,                                \
    ARG5TY,                                 \
    ARG5NM,                                 \
    ARG5OPT,                                \
    ARG6TY,                                 \
    ARG6NM,                                 \
    ARG6OPT,                                \
    ARG7TY,                                 \
    ARG7NM,                                 \
    ARG7OPT,                                \
    ARG8TY,                                 \
    ARG8NM,                                 \
    ARG8OPT,                                \
    ARG9TY,                                 \
    ARG9NM,                                 \
    ARG9OPT)                                \
  NAME##Node *cloneImpl(NAME##Node *node) { \
    ARG0TY arg0;                            \
    ARG1TY arg1;                            \
    ARG2TY arg2;                            \
    ARG3TY arg3;                            \
    ARG4TY arg4;                            \
    ARG5TY arg5;                            \
    ARG6TY arg6;                            \
    ARG7TY arg7;                            \
    ARG8TY arg8;                            \
    ARG9TY arg9;                            \
    clone(node->_##ARG0NM, arg0);           \
    clone(node->_##ARG1NM, arg1);           \
    clone(node->_##ARG2NM, arg2);           \
    clone(node->_##ARG3NM, arg3);           \
    clone(node->_##ARG4NM, arg4);           \
    clone(node->_##ARG5NM, arg5);           \
    clone(node->_##ARG6NM, arg6);           \
    clone(node->_##ARG7NM, arg7);           \
    clone(node->_##ARG8NM, arg8);           \
    clone(node->_##ARG9NM, arg9);           \
    return new (astContext_) NAME##Node(    \
        std::move(arg0),                    \
        std::move(arg1),                    \
        std::move(arg2),                    \
        std::move(arg3),                    \
        std::move(arg4),                    \
        std::move(arg5),                    \
        std::move(arg6),                    \
        std::move(arg7),                    \
        std::move(arg8),                    \
        std::move(arg9));                   \
  }

#include "hermes/AST/ESTree.def"
};

} // namespace

ESTree::Node *cloneNode(
    Context &astContext,
    SemContext &semContext,
    DeclCollectorMapTy &declCollectorMap,
    ESTree::Node *node,
    FunctionInfo *functionInfo,
    LexicalScope *scope) {
  return Cloner(
             astContext,
             semContext,
             declCollectorMap,
             node,
             functionInfo,
             scope)
      .run();
}

} // namespace hermes::sema
