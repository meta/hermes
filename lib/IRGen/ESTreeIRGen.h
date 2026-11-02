/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_IRGEN_ESTREEIRGEN_H
#define HERMES_IRGEN_ESTREEIRGEN_H

#include "IRInstrument.h" // @manual
#include "hermes/ADT/ScopedHashTable.h"
#include "hermes/AST/SemValidate.h"
#include "hermes/FrontEndDefs/NativeErrorTypes.h"
#include "hermes/IR/IRBuilder.h"
#include "hermes/IRGen/IRGen.h"
#include "hermes/Support/InternalIdentifierMaker.h"

#include "llvh/ADT/StringRef.h"
#include "llvh/Support/Debug.h"

#include <optional>

// Use this value to enable debug logging from the command line.
#define DEBUG_TYPE "irgen"

using llvh::dyn_cast_or_null;

namespace hermes {
namespace irgen {

// Forward declarations
class SurroundingTry;
class FunctionContext;
class ESTreeIRGen;

using VarDecl = sem::FunctionInfo::VarDecl;

//===----------------------------------------------------------------------===//
// Free standing helpers.

/// Emit a code sequence to load the global object property \p prop.
Instruction *loadGlobalObjectProperty(
    IRBuilder &builder,
    GlobalObjectProperty *prop,
    bool inhibitThrow = false);

/// Return the name field from ID nodes.
inline Identifier getNameFieldFromID(const ESTree::Node *ID) {
  return Identifier::getFromPointer(cast<ESTree::IdentifierNode>(ID)->_name);
}

/// \returns true if \p node is known to be a constant expression.
bool isConstantExpr(ESTree::Node *node);

/// Scoped hash table to represent the JS scope.
using NameTableTy = hermes::ScopedHashTable<Identifier, Value *>;
using NameTableScopeTy = hermes::ScopedHashTableScope<Identifier, Value *>;

//===----------------------------------------------------------------------===//
// EnterBlockScope

/// Enters a new block scope in the current function. Should be constructed on
/// the stack.
class EnterBlockScope {
  friend class FunctionContext;

 public:
  explicit EnterBlockScope(FunctionContext *currentContext);
  ~EnterBlockScope();

 private:
  FunctionContext *const currentContext_;
  ScopeDesc *const oldIRScopeDesc_;
  ScopeCreationInst *const oldIRScope_;
  NameTableScopeTy *const oldBlockScope_;
  NameTableScopeTy blockScope_;
};

//===----------------------------------------------------------------------===//
// FunctionContext

/// Holds the target basic block for a break and continue label.
/// It has a 1-to-1 correspondence to SemInfoFunction::GotoLabel.
/// Labels can be context dependent - one label can be initialized multiple
/// times if it is defined inside a finally block, because the body of the
/// finally block will be visited and compiled multiple times from different
/// contexts (on normal exit, on exceptional exit, on break/return/etc).
struct GotoLabel {
  BasicBlock *breakTarget = nullptr;
  BasicBlock *continueTarget = nullptr;
  /// A record on the stack defining the closest surrounding try/catch
  /// statement. We need this so we can call the finally handlers.
  SurroundingTry *surroundingTry = nullptr;
};

/// Holds per-function state, specifically label tables. Should be constructed
/// on the stack. Upon destruction it automatically restores the previous
/// function context.
class FunctionContext {
  friend class EnterBlockScope;

  /// Pointer to the "outer" object this is associated with.
  ESTreeIRGen *const irGen_;

  /// Semantic info of the function we are emitting.
  sem::FunctionInfo *const semInfo_;

  /// The old value which we save and will restore on destruction.
  FunctionContext *oldContext_;

  /// As we descend into a new function, we save the state of the builder
  /// here. It is automatically restored once we are done with the function.
  IRBuilder::SaveRestore builderSaveState_;

  /// A vector of labels corresponding 1-to-1 to the labels defined in
  /// \c semInfo_.
  llvh::SmallVector<GotoLabel, 2> labels_;

  /// Sets \p functionScope as the top-level scope for the function represented
  /// by this object.
  void setupFunctionScope(EnterBlockScope *enterBlockScope);

 public:
  /// This is the actual function associated with this context.
  Function *const function;

  /// The innermost surrounding try/catch node at any point.
  SurroundingTry *surroundingTry = nullptr;

  /// A new variable scope that is used throughout the body of the function.
  NameTableScopeTy *functionScope;

  /// The current local "scope" for block-scoped declarations.
  NameTableScopeTy *blockScope;

  /// Stack Register that will hold the return value of the global scope.
  AllocStackInst *globalReturnRegister{nullptr};

  InternalIdentifierMaker anonymousIDs_;

  /// This holds the CreateArguments instruction. We always insert it in the
  /// prologue and delete it in the epilogue if it wasn't used.
  CreateArgumentsInst *createArgumentsInst{};

  /// Parents of arrow functions need to capture their "this" parameter so the
  /// arrow function can use it. Normal functions and constructors store their
  /// "this" in a variable and record it here, if they contain at least one
  /// arrow function. Arrow functions always copy their parent's value.
  Variable *capturedThis{};

  /// Captured value of new target. In ES5 functions and ES6 constructors it
  /// is a Variable with the result of GetNewTargetInst executed at the start
  /// of the function. In ES6 methods it will be initialized to
  /// LiteralUndefined. In arrow functions it is a copy of the parent's
  /// capturedNewTarget.
  Value *capturedNewTarget{};

  /// Optionally captured value of the eagerly created Arguments object. Used
  /// when arrow functions need to access it.
  Variable *capturedArguments{};

  /// "Enters" the function scope. Must be defined after all non-EnterBlockScope
  /// objects as it manipulates the members.
  EnterBlockScope enterFunctionScope;

  /// ES2023 10.2.11 20, 28, and 30 define separate function scopes. Not all
  /// functions have those, so they are created when called for.
  using SeparateFunctionScope = std::optional<EnterBlockScope>;

  /// "Enters" the parameter scope. See ES2023 10.2.11.20
  SeparateFunctionScope enterParamScope;

  /// "Enters" the var declaration scope. See ES2023 10.2.11.28
  SeparateFunctionScope enterVarScope;

  /// "Enters" the new scope for top-level lexical declarations. See
  /// ES2023 10.2.11.30
  SeparateFunctionScope enterTopLevelLexicalDeclarationsScope;

  /// "Enters" given \p optScope (a separate, optional scope in the current
  /// function). Note that \p optScope is a pointer to a FunctionContext member
  /// variable.
  void enterOptionalFunctionScope(
      SeparateFunctionScope FunctionContext::*optScope) {
    assert(!(this->*optScope) && "can't enter scope twice.");
    (this->*optScope).emplace(this);
    setupFunctionScope(&*(this->*optScope));
  }

  /// Initialize a new function context, while preserving the previous one.
  /// \param irGen the associated ESTreeIRGen object.
  /// \param function the newly created Function IR node.
  /// \param semInfo semantic info obtained from the AST node.
  FunctionContext(
      ESTreeIRGen *irGen,
      Function *function,
      sem::FunctionInfo *semInfo);

  ~FunctionContext();

  /// The previous (outer) function context on the stack.
  FunctionContext *getPreviousContext() const {
    return oldContext_;
  }

  /// \return the associated semantic information, asserting that it is
  /// present.
  sem::FunctionInfo *getSemInfo() {
    assert(semInfo_ && "semInfo is not set");
    return semInfo_;
  }

  /// Generate a unique string that represents a temporary value. The string
  /// \p hint appears in the name.
  Identifier genAnonymousLabelName(llvh::StringRef const hint);

  /// Initialize an empty goto label. All labels in JavaScript a structured, so
  /// a label is guaranteed to be visited and initialized before it is used.
  /// Perhaps surprisingly, a label will be initialized more than once if it is
  /// defined in a finally block, since the statements inside the finally block
  /// will be visited (and compiled) more than once.
  void initLabel(
      ESTree::LabelDecorationBase *LDB,
      BasicBlock *breakTarget,
      BasicBlock *continueTarget) {
    auto &label = labels_[LDB->getLabelIndex()];
    label.breakTarget = breakTarget;
    label.continueTarget = continueTarget;
    label.surroundingTry = surroundingTry;
  }

  /// Access an already initialized label.
  const GotoLabel &label(ESTree::LabelDecorationBase *LDB) {
    auto &label = labels_[LDB->getLabelIndex()];
    assert(
        (label.breakTarget || label.continueTarget) &&
        "accessing an uninitialized label");
    return label;
  }
};

enum class ControlFlowChange { Break, Continue };

/// A link in the stack of surrounding try/catch statements we are maintaining
/// at any time.
class SurroundingTry {
  FunctionContext *const functionContext_;

 public:
  /// The record of the outer (closest surrounding) try/catch.
  SurroundingTry *const outer;
  /// The ESTree node defining the try/catch. This is usually a
  /// TryStatementNode, but in some cases that require internal try/catch
  /// statements, it can be something different.
  ESTree::Node *const node;
  /// Optional debug location to be used for the TryEnd instruction.
  SMLoc const tryEndLoc{};

  /// \param continueTarget if cfc is Continue, then the target of that
  /// continue, which is used to determine whether to emit 'finally' code in
  /// certain cases such as for-of.
  using GenFinalizerCB = std::function<
      void(ESTree::Node *, ControlFlowChange cfc, BasicBlock *continueTarget)>;

  /// An optional callback that will be invoked to generate the code for the
  /// finalizer.
  GenFinalizerCB genFinalizer{};

  SurroundingTry(
      FunctionContext *functionContext,
      ESTree::Node *node,
      SMLoc tryEndLoc = {},
      GenFinalizerCB genFinalizer = {})
      : functionContext_(functionContext),
        outer(functionContext->surroundingTry),
        node(node),
        tryEndLoc(tryEndLoc),
        genFinalizer(std::move(genFinalizer)) {
    // Push.
    functionContext->surroundingTry = this;
  }

  ~SurroundingTry() {
    // Pop.
    functionContext_->surroundingTry = outer;
  }
};

//===----------------------------------------------------------------------===//
// LReference

/// This is a utility class that's related to the ES5 Reference type without
/// the runtime semantic parts - https://es5.github.io/#x8.7.
class LReference {
 public:
  enum class Kind {
    /// Elision in destructuring pattern.
    Empty,
    /// Reference to a member expression.
    Member,
    /// Reference to a variable or a global property.
    VarOrGlobal,
    /// Invalid reference. Error has been reported, so it does nothing.
    Error,
    /// Destructuring assignment target.
    Destructuring,
  };

  LReference(
      Kind kind,
      ESTreeIRGen *irgen,
      bool declInit,
      Value *base,
      Value *property,
      SMLoc loadLoc)
      : kind_(kind), irgen_(irgen), declInit_(declInit) {
    base_ = base;
    property_ = property;
    loadLoc_ = loadLoc;
  }

  LReference(ESTreeIRGen *irgen, bool declInit, ESTree::PatternNode *target)
      : kind_(Kind::Destructuring), irgen_(irgen), declInit_(declInit) {
    destructuringTarget_ = target;
  }

  bool isEmpty() const {
    return kind_ == Kind::Empty;
  }

  Value *emitLoad();
  void emitStore(Value *value);

  /// \return true if it is known that \c emitStore() will not have any side
  ///   effects, including exceptions. This is not a sophisticated analysis,
  ///   it only checks for a local variable.
  bool canStoreWithoutSideEffects() const;

  Variable *castAsVariable() const;
  GlobalObjectProperty *castAsGlobalObjectProperty() const;

 private:
  /// Self explanatory.
  Kind kind_;

  /// The associated instance of ESTreeIRGen.
  ESTreeIRGen *irgen_;

  /// This is a declaration initialization, so the TDZ check should be skipped
  /// when storing.
  bool declInit_;

  union {
    struct {
      /// The base of the object, or the variable we load from.
      Value *base_;
      /// The name/value of the field this reference accesses, or null if this
      /// is a variable access.
      Value *property_;
    };

    /// Destructuring assignment target.
    ESTree::PatternNode *destructuringTarget_;
  };

  /// Debug position for loads. Must be outside of the union because it has a
  /// constructor.
  SMLoc loadLoc_;

  /// \return a reference to IRGen's builder.
  IRBuilder &getBuilder();
};

//===----------------------------------------------------------------------===//
// ESTreeIRGen

/// Performs lowering of the JSON ESTree down to Hermes IR.
class ESTreeIRGen {
  friend class EnterBlockScope;
  friend class FunctionContext;
  friend class LReference;

  using BasicBlockListType = llvh::SmallVector<BasicBlock *, 4>;

  /// The module we are constructing.
  Module *Mod;
  /// The IRBuilder we use to construct the module.
  IRBuilder Builder;
  /// Optional instrumentation
  IRInstrument instrumentIR_;
  /// The root of the ESTree.
  ESTree::Node *Root;
  /// This is a list of parsed global property declaration files.
  const DeclarationFileListTy &DeclarationFileList;
  /// This points to the context of the top-level function, which we
  /// occasionally need.
  FunctionContext *topLevelContext{};
  /// This points to the current function's context. It is saved and restored
  /// whenever we enter a nested function.
  FunctionContext *functionContext_{};
  /// This is the scoped hash table that saves the mapping between the declared
  /// names and an instance of Varible or GlobalObjectProperty.
  NameTableTy nameTable_{};

  /// Lexical scope chain from the runtime, used to resolve identifiers in local
  /// eval.
  SerializedScopePtr lexicalScopeChain;

  /// Identifier representing the string "eval".
  const Identifier identEval_;

  /// Identifier representing the string "let".
  const Identifier identLet_;

  /// Identifier representing the string "?default".
  const Identifier identDefaultExport_;

  /// The current scope descriptor available for IR generation. All new
  /// functions created in this scope will define a new, inner scope.
  ScopeDesc *currentIRScopeDesc_{};

  /// The current scope object available for IR generation.
  ScopeCreationInst *currentIRScope_{};

  /// Generate a unique string that represents a temporary value. The string \p
  /// hint appears in the name.
  Identifier genAnonymousLabelName(llvh::StringRef hint) {
    return curFunction()->genAnonymousLabelName(hint);
  }

  /// Generate a unique string that represents a temporary value. The string \p
  /// hint appears in the name.
  Identifier genAnonymousLabelName(Identifier hint) {
    return genAnonymousLabelName(hint.isValid() ? hint.str() : "anonymous");
  }

 public:
  explicit ESTreeIRGen(
      ESTree::Node *root,
      const DeclarationFileListTy &declFileList,
      Module *M,
      const ScopeChain &scopeChain);

  /// Perform IRGeneration for the whole module.
  void doIt();

  /// Perform IR generation for a given CJS module.
  void doCJSModule(
      Function *topLevelFunction,
      sem::FunctionInfo *semInfo,
      uint32_t segmentID,
      uint32_t id,
      llvh::StringRef filename);

  /// Perform IR generation for a lazy function.
  /// \return the newly allocated generated Function IR and lexical root
  std::pair<Function *, Function *> doLazyFunction(
      hbc::LazyCompilationData *lazyData);

  /// Emits code to raises the given native error (ES2023 20.5.5) \p id with the
  /// given \p msg. This generated instruction sequence is immune to changes to
  /// the global object. This is used in cases like
  ///
  /// \code
  /// const c = "you";
  /// globalThis.SyntaxError = function() {}
  /// c = "shall not pass!"
  /// \endcode
  ///
  /// where the assignment to c should raise a %SyntaxError% (i.e., the original
  /// SyntaxError constructor), and not a SyntaxError (i.e., the current value
  /// of the identifier SyntaxError).
  static Instruction *genRaiseNativeError(
      IRBuilder &builder,
      NativeErrorTypes id,
      llvh::StringRef msg);

  /// Generate a function which immediately throws the specified SyntaxError
  /// message.
  static Function *genSyntaxErrorFunction(
      Module *M,
      ScopeDesc *scopeDesc,
      Identifier originalName,
      SMRange sourceRange,
      llvh::StringRef error);

 private:
  /// @name statements
  /// @{

  /// \brief Perform IRGen for \p body, which can be the body of a function or
  /// the top-level of a program.
  void genBody(ESTree::NodeList &Body);

  /// Generate code for the statement \p stmt, which is a function body.
  void genFunctionBody(ESTree::Node *stmt);

  /// Generate code for the statement \p stmt, which is a catch handler.
  void genCatchHandler(ESTree::Node *stmt);

  /// Generate code for the statement \p stmt without creating a new scope if \p
  /// stmt is a BlockStatementNode.
  void genScopelessBlockOrStatement(ESTree::Node *stmt);

  enum class IsLoopBody { No, Yes };

  /// Generates code for the block statement \p BS.
  void genBlockStatement(ESTree::BlockStatementNode *BS, IsLoopBody isLoopBody);

  /// Generate code for the statement \p Stmt.
  void genStatement(ESTree::Node *stmt, IsLoopBody isLoopBody);

  /// Wrapper of genExpression. If curFunction()->globalReturnRegister is
  /// set, stores the expression value into it.
  void genExpressionWrapper(ESTree::Node *expr);

  void genVariableDeclaration(ESTree::VariableDeclarationNode *declaration);
  void genVariableDeclarator(
      ESTree::NodeLabel kind,
      ESTree::VariableDeclaratorNode *declarator);

  void genIfStatement(ESTree::IfStatementNode *IfStmt);
  void genReturnStatement(ESTree::ReturnStatementNode *RetStmt);
  void genForInStatement(ESTree::ForInStatementNode *ForInStmt);
  void genForOfStatement(ESTree::ForOfStatementNode *forOfStmt);

  /// Generate IR for for/while/do..while statements.
  /// \p loop the loop statement node
  /// \p init optional init statement for 'for' loops.
  /// \p preTest the expression that guards the loop entrance.
  /// \p postTest the expression that guards the loop.
  /// \p update optional statement for the update phase of 'for' loops.
  /// \o body the body of the loop.
  void genForWhileLoops(
      ESTree::LoopStatementNode *loop,
      ESTree::Node *init,
      ESTree::Node *preTest,
      ESTree::Node *postTest,
      ESTree::Node *update,
      ESTree::Node *body);

  void genSwitchStatement(ESTree::SwitchStatementNode *switchStmt);

  /// Check if all cases values are constant and stores them in
  /// \p casdeLiterals. The default case, if present, is stored as nullptr, to
  ///     maintain the ordering of the cases.
  /// \returns true if all cases are constant.
  bool areAllCasesConstant(
      ESTree::SwitchStatementNode *switchStmt,
      llvh::SmallVectorImpl<Literal *> &caseLiterals);

  /// Generate IR Switch statements when all case tests are constant.
  void genConstSwitchStmt(
      ESTree::SwitchStatementNode *switchStmt,
      llvh::SmallVectorImpl<Literal *> &caseLiterals);

  void genImportDeclaration(ESTree::ImportDeclarationNode *importDecl);

  void genExportNamedDeclaration(
      ESTree::ExportNamedDeclarationNode *exportDecl);
  void genExportDefaultDeclaration(
      ESTree::ExportDefaultDeclarationNode *exportDecl);
  void genExportAllDeclaration(ESTree::ExportAllDeclarationNode *exportDecl);

  /// @}

 private:
  /// @name expressions
  /// @{

  /// Generate IR for the expression \p Expr.
  /// \p nameHint is used to provide names for anonymous functions.
  /// We currently provide names for functions that are assigned to a variable,
  /// or functions that are assigned to an object key. These are a subset of
  /// ES6, but not all of it.
  Value *genExpression(ESTree::Node *expr, Identifier nameHint = Identifier{});

  /// Generate an expression and perform a conditional branch depending on
  /// whether it evaluates to true or false (or optionally, nullish).
  /// \param onNullish when not nullptr, the block to branch to if expr
  ///   is nullish (undefined or null). Note that this supersedes `onFalse` when
  ///   provided. If onNullish is nullptr, then `onFalse` will be branched to
  ///   when expr is nullish, by virtue of boolean coercion.
  void genExpressionBranch(
      ESTree::Node *expr,
      BasicBlock *onTrue,
      BasicBlock *onFalse,
      BasicBlock *onNullish);

  /// Convert the \p input into an array, spreading SpreadElements
  /// using for-or iteration semantics.
  /// Allows sharing spread code between genArrayExpr and genCallExpr.
  Value *genArrayFromElements(ESTree::NodeList &list);

  Value *genObjectExpr(ESTree::ObjectExpressionNode *Expr);
  Value *genArrayExpr(ESTree::ArrayExpressionNode *Expr);
  Value *genCallExpr(ESTree::CallExpressionNode *call);

  /// Generates a call expression in an optional chain.
  /// \param shortCircuitBB the block to jump to upon short circuiting,
  ///    when the `?.` operator is used. If null, this is the outermost
  ///    optional expression in the chain, and will create its own
  ///    shortCircuitBB.
  Value *genOptionalCallExpr(
      ESTree::OptionalCallExpressionNode *call,
      BasicBlock *shortCircuitBB);

  /// Emits the actual call for \p call, and is used as a helper function for
  /// genCallExpr and genOptionalCallExpr.
  /// \return the result value of the call.
  Value *
  emitCall(ESTree::CallExpressionLikeNode *call, Value *callee, Value *thisVal);

  struct MemberExpressionResult {
    /// Value of the looked up property.
    Value *result;
    /// Object the property is being looked up on.
    Value *base;
  };

  enum class MemberExpressionOperation {
    Load,
    Delete,
  };

  /// Generate IR for a member expression.
  /// \param op the operation to perform on the property if it exists.
  ///    If Load, the result is set to the value of the property.
  ///    If Delete, the result is the result of DeletePropertyInst.
  /// \return both the result and the base object.
  MemberExpressionResult genMemberExpression(
      ESTree::MemberExpressionNode *Mem,
      MemberExpressionOperation op);

  /// Generate IR for a member expression in the middle of an optional chain.
  /// \param shortCircuitBB the block to jump to upon short circuiting,
  ///    when the `?.` operator is used. If null, this is the outermost
  ///    optional expression in the chain, and will create its own
  ///    shortCircuitBB.
  /// \param op the operation to perform on the property if it exists.
  ///    If Load, the result is set to the value of the property.
  ///    If Delete, the result is the result of DeletePropertyInst.
  /// \return both the result and the base object.
  MemberExpressionResult genOptionalMemberExpression(
      ESTree::OptionalMemberExpressionNode *mem,
      BasicBlock *shortCircuitBB,
      MemberExpressionOperation op);

  /// Generate IR for a direct call to eval(). This is invoked from
  /// genCallExpr().
  Value *genCallEvalExpr(ESTree::CallExpressionNode *call);

  Value *genNewExpr(ESTree::NewExpressionNode *N);
  Value *genAssignmentExpr(ESTree::AssignmentExpressionNode *AE);
  Value *genLogicalAssignmentExpr(
      ESTree::AssignmentExpressionNode *AE,
      BinaryOperatorInst::OpKind AssignmentKind,
      LReference lref,
      Identifier nameHint);
  Value *genConditionalExpr(ESTree::ConditionalExpressionNode *C);
  Value *genSequenceExpr(ESTree::SequenceExpressionNode *Sq);
  Value *genYieldExpr(ESTree::YieldExpressionNode *Y);
  Value *genYieldStarExpr(ESTree::YieldExpressionNode *Y);
  Value *genAwaitExpr(ESTree::AwaitExpressionNode *A);
  Value *genBinaryExpression(ESTree::BinaryExpressionNode *bin);
  Value *genUnaryExpression(ESTree::UnaryExpressionNode *U);
  Value *genUpdateExpr(ESTree::UpdateExpressionNode *updateExpr);
  Value *genLogicalExpression(ESTree::LogicalExpressionNode *logical);

  /// A helper function to unify the largely same IRGen logic of \c genYieldExpr
  /// and \c genAwaitExpr.
  /// \param value the value operand of the will-generate SaveAndYieldInst.
  Value *genYieldOrAwaitExpr(Value *value);

  /// Generate IR for the logical expression \p logical and jump to the
  /// corresponding label depending on its value.
  void genLogicalExpressionBranch(
      ESTree::LogicalExpressionNode *logical,
      BasicBlock *onTrue,
      BasicBlock *onFalse,
      BasicBlock *onNullish);

  /// Generate IR for the identifier expression.
  /// We need to know whether it's after typeof (\p afterTypeOf),
  /// to help decide whether a load can throw.
  Value *genIdentifierExpression(
      ESTree::IdentifierNode *Iden,
      bool afterTypeOf);

  Value *genMetaProperty(ESTree::MetaPropertyNode *MP);

  /// Generate IR for a template literal expression, which in most cases is
  /// translated to a call to HermesInternal.concat().
  Value *genTemplateLiteralExpr(ESTree::TemplateLiteralNode *Expr);

  /// Generate IR for a tagged template expression, which involves converting
  /// a template literal to a template object, then invoking the tag function
  /// with the template object and substitution expressions.
  Value *genTaggedTemplateExpr(ESTree::TaggedTemplateExpressionNode *Expr);

  /// Whether or not a finally clause is needed in \c genResumeGenerator.
  enum class GenFinally { No, Yes };

  /// Generate IR for handling resuming a suspended generator.
  /// \param genFinally whether or not a finally clause need to be generated.
  ///   It's true when there is a corresponding yield/await to resume from. It's
  ///   not if this is the first resume and there is no yield/await.
  /// \param isReturn the slot indicating whether the user requested a return.
  /// \param nextBB the next BasicBlock to run if the user did not request a
  ///   return.
  /// \param received if non-null, the stack location at which to store the
  ///   value received from the user as the arg to next(), throw(), or return().
  Value *genResumeGenerator(
      GenFinally genFinally,
      AllocStackInst *isReturn,
      BasicBlock *nextBB,
      AllocStackInst *received = nullptr);

  Value *genRegExpLiteral(ESTree::RegExpLiteralNode *RE);

  /// @}

 private:
  /// @name Exception handling.
  /// @{

  /// Generate IR for try/catch/finally statement.
  void genTryStatement(ESTree::TryStatementNode *tryStmt);

  /// Emit the scaffoling for a try/catch block.
  /// \param nextBlock is the block to branch to after the body and the
  ///     finalizer. If nullptr, a new block is allocated. At the end the
  ///     insertion point is set to the that block.
  /// \param emitBody updates \c surroundingTry and emits the body of the block
  ///     guarded by the \c try. Exceptions occurring in the body will be caught
  ///     by the handler.
  /// \param emitNormalCleanup emits the cleanup after exiting the try body
  ///     normally (neither through an exception nor break/return/continue). It
  ///     executes outside of the exception scope (exceptions here will not be
  ///     caught by the handler).
  /// \param emitHandler emits the code to execute when
  ///     the exception is caught. It must emit a CatchInstu to clear the active
  ///     exception, even if it ignores it. \p nextBlock is passed as a
  ///     parameter.
  /// \return the block used as \c nextBlock
  template <typename EB, typename EF, typename EH>
  BasicBlock *emitTryCatchScaffolding(
      BasicBlock *nextBlock,
      EB emitBody,
      EF emitNormalCleanup,
      EH emitHandler);

  /// \param catchParam is not null, create the required variable binding
  ///     for the catch parameter and emit the store.
  /// \return the CatchInst.
  CatchInst *prepareCatch(ESTree::CatchClauseNode *catchHandler);

  /// When we see a control change such as return, break, continue,
  /// we need to make sure to generate code for finally block if
  /// we are under a try/catch.
  /// \param sourceTry  the try statement surrounding the AST node performing
  ///   the goto (break, continue, return)
  /// \param targetTry  the try statement surrounding the AST node of the label.
  /// \param cfc indicates whether this is "continue" or "break".
  /// \param continueTarget when cfc == Continue, the target of the branch of
  /// the continue.
  void genFinallyBeforeControlChange(
      SurroundingTry *sourceTry,
      SurroundingTry *targetTry,
      ControlFlowChange cfc,
      BasicBlock *continueTarget = nullptr);

  /// @}

 private:
  /// @name functions
  /// @{

  /// Generate IR for function declarations;
  void genFunctionDeclaration(ESTree::FunctionDeclarationNode *func);

  /// Generate IR for FunctionExpression.
  Value *genFunctionExpression(
      ESTree::FunctionExpressionNode *FE,
      Identifier nameHint);

  /// Generate IR for ArrowFunctionExpression.
  Value *genArrowFunctionExpression(
      ESTree::ArrowFunctionExpressionNode *AF,
      Identifier nameHint);

  /// Generate IR for an ES5 function (function declaration or expression).
  /// The body may optionally be lazy.
  /// \param originalName is the original non-unique name specified by the user
  ///   or inferred according to the rules of ES6.
  /// \param lazyClosureAlias an optional variable in the parent that will
  ///   contain the closure being created. It is non-null only if an alias
  ///   binding from  \c originalName to the variable was created and is
  ///   available inside the closure. Used only by lazy compilation.
  /// \param functionNode is the ESTree function node (declaration, expression,
  ///   object method).
  /// \param isGeneratorInnerFunction whether this is a GeneratorInnerFunction.
  /// \returns a new Function.
  Function *genES5Function(
      Identifier originalName,
      Variable *lazyClosureAlias,
      ESTree::FunctionLikeNode *functionNode,
      bool isGeneratorInnerFunction = false);

  /// Generate the IR for two functions: an outer GeneratorFunction and an inner
  /// GeneratorInnerFunction. The outer function runs CreateGenerator on the
  /// inner function and returns the result.
  /// \param originalName is the original non-unique name specified by the user
  ///   or inferred according to the rules of ES6.
  /// \param lazyClosureAlias an optional variable in the parent that will
  ///   contain the closure being created. It is non-null only if an alias
  ///   binding from  \c originalName to the variable was created and is
  ///   available inside the closure. Used only by lazy compilation.
  /// \param functionNode is the ESTree function node (declaration, expression,
  ///   object method).
  /// \return the outer Function.
  Function *genGeneratorFunction(
      Identifier originalName,
      Variable *lazyClosureAlias,
      ESTree::FunctionLikeNode *functionNode);

  /// Set the current scope to the lazy scope on \p function
  /// and assigns the proper source range and information in order to
  /// continue later in lazy compilation.
  /// \param bodyBlock the body of the function, must be a lazy function body.
  void setupLazyScope(
      ESTree::FunctionLikeNode *functionNode,
      Function *function,
      ESTree::BlockStatementNode *bodyBlock);

  /// Generate the IR for an async function: it desugars async function to a
  /// generator function wrapped in a call to the JS builtin `spawnAsync` and
  /// return the result. To visualize it,
  ///   async function<name>(<args>)<body>
  /// is effectively lowered to
  ///   function<name>(){return spawnAsync(function*()<body>, this, arguments)}
  /// \param originalName is the original non-unique name specified by the user
  ///   or inferred according to the rules of ES6.
  /// \param lazyClosureAlias an optional variable in the parent that will
  ///   contain the closure being created. It is non-null only if an alias
  ///   binding from  \c originalName to the variable was created and is
  ///   available inside the closure. Used only by lazy compilation.
  /// \param functionNode is the ESTree function node (declaration, expression,
  ///   object method).
  /// \return the async Function.
  Function *genAsyncFunction(
      Identifier originalName,
      Variable *lazyClosureAlias,
      ESTree::FunctionLikeNode *functionNode);

  //===----------------------------------------------------------------------===//
  // Function prologue emission
  //
  // Function prologue emission has 3 steps
  //  - preamble
  //  - es5-state capture
  //  - top-level declarations
  //
  // It used to be a single function that was becoming too complex for its own
  // good, so it has been split into the functions in this section. It is
  // expected that all functions (including global()) need the code generated by
  // emitFunctionPreamble(), but the other 2 steps are optional.

  /// Emit the function preamble for the current function, consisting of:
  ///  - the top-level CreateScopeInst creating the function's top-level scope
  ///  - sets the current source-level scope to the function's top-level scope
  ///  - creates the arguments array (which is removed during the epilogue if
  ///    unused)
  ///  - the %this parameter.
  ///
  /// \param entry the unpopulated entry block for the function
  void emitFunctionPreamble(BasicBlock *entry);

  /// In the beginning of an ES5 function, initialize the special captured
  /// variables needed by arrow functions, constructors and methods. Should be
  /// called after \c emitFunctionPreamble in function that need this (i.e., all
  /// functions but arrow function expressions).
  void initCaptureStateInES5Function();

  /// Controls whether parameters should be emitted. \c No means only the
  /// .length property of the resulting function will be populated. \c Yes means
  /// the parameters are emitted, as well as the code to initialize them.
  /// \c YesMultiScopes instructs emitTopLevelDeclarations to emit separate
  /// scopes for parameters, their initializer, and the function's top-level
  /// declarations, as mandated by ES2023 10.2.11.
  enum class DoEmitParameters { No, Yes, YesMultiScopes };

  /// Emits top-level declarations for \p funcNode. These include:
  ///
  ///  - parameters (and their initialization, if they have default values)
  ///  - var-declared variables
  ///  - let/const-declared variables
  ///  - imports
  ///  - any functions defined in \p funcNode's top level scope; and
  ///  - any hoisted functions (as per ES2023 Annex B.3.2.1 and B.3.2.2).
  ///
  /// \param body the AST node for the function body; or the Program node when
  ///     generating the global function
  void emitTopLevelDeclarations(
      ESTree::FunctionLikeNode *funcNode,
      ESTree::Node *body,
      DoEmitParameters doEmitParameters);

  //
  //===----------------------------------------------------------------------===//

  /// Creates a new binding \p id of kind \p kind in the given \p scopeDesc.
  /// \p needsInitializer indicates whether newly created bindings should be
  /// default-initialized.
  void createNewBinding(
      ScopeDesc *scopeDesc,
      VarDecl::Kind kind,
      ESTree::Node *id,
      bool needsInitializer,
      Value *init = nullptr);

  /// Creates all bindings (variables and functions) for \p containingNode in
  /// \p scopeDesc.
  void createScopeBindings(ScopeDesc *scopeDesc, ESTree::Node *containingNode);

  /// Generates all closures declares in \p containingNode in the current IR
  /// ScopeDesc.
  void genFunctionDeclarations(ESTree::Node *containingNode);

  /// Emit the loading and initialization of parameters in the function
  /// prologue.
  /// \param hasParamExpressions indicates that \p funcNode has parameter
  ///   experssions; if this is true, parameters are lowered to tdz locals to
  ///   prevent using parameters before they are initialized, e.g.
  ///   \code
  ///   function f(a = b, b = 10) { ... }
  ///   \endcode
  void emitParameters(
      ESTree::FunctionLikeNode *funcNode,
      bool hasParamExpressions);

  /// Count number of expected arguments, including "this".
  /// Only parameters up to the first parameter with an initializer are counted.
  /// See ES14.1.7 for details.
  uint32_t countExpectedArgumentsIncludingThis(
      ESTree::FunctionLikeNode *funcNode);

  /// Optionally emit a return value and perform cleanup after emission of the
  /// current function is finished. Specifically it attempts to merge the entry
  /// and the next block in order to create less "noise".
  /// \param returnValue if non-nullptr, a return instruction for it is emitted
  ///   with debug location at the end of the source range.
  void emitFunctionEpilogue(Value *returnValue);

  /// Generate a body for a dummy function so that it doesn't crash the
  /// backend when encountered.
  static void genDummyFunction(Function *dummy);

  /// @}

 private:
  inline FunctionContext *curFunction() {
    assert(functionContext_ && "No active function context");
    return functionContext_;
  }

  /// Declare a variable or a global propery depending in function \p inFunc,
  /// depending on whether it is the global scope. Do nothing if the variable
  /// or property is already declared in that scope.
  /// \return A pair. pair.first is the variable, and pair.second is set to true
  ///   if it was declared, false if it already existed.
  std::pair<Value *, bool> declareVariableOrGlobalProperty(
      ScopeDesc *inScope,
      VarDecl::Kind declKind,
      Identifier name);

  /// Declare a new ambient global property, if not already declared.
  GlobalObjectProperty *declareAmbientGlobalProperty(Identifier name);

  /// Scan all the global declarations in the supplied declaration file and
  /// declare them as global properties.
  void processDeclarationFile(ESTree::ProgramNode *programNode);

  /// This method ensures that a variable with the name \p name exists in the
  /// current scope. The method reports an error if the variable does not exist
  /// and creates a new variable with this name in the current scope in an
  /// attempt to recover from the error.
  /// \return the existing variable or global property, or a freshly created
  ///   ambient global property in case of error.
  Value *ensureVariableExists(ESTree::IdentifierNode *id);

  /// Generate the IR for the property field of the MemberExpressionNode.
  /// The property field may be a string literal or some computed expression.
  Value *genMemberExpressionProperty(ESTree::MemberExpressionLikeNode *Mem);

  /// Check whether we know that an LReference to the specified AST node can
  /// be created without any side effects, including throwing. This is not
  /// supposed to perform a sophisticated analysis, just catch the obvious
  /// cases. For now it only recognizes local non-const variables.
  bool canCreateLRefWithoutSideEffects(ESTree::Node *target);

  /// Generates a left hand side reference from valid estree nodes
  /// that can be translated to lref.
  /// \param declInit indicates whether this reference is a declaration
  ///     initialization, in which case the TDZ check on store should be
  ///     omitted.
  LReference createLRef(ESTree::Node *node, bool declInit);

  /// Generate a call to a method of HermesInternal with the specified name \p
  /// name.
  Value *genHermesInternalCall(
      llvh::StringRef name,
      Value *thisValue,
      ArrayRef<Value *> args);

  /// Generate a builtin call.
  Value *genBuiltinCall(
      BuiltinMethod::Enum builtinIndex,
      ArrayRef<Value *> args);

  /// Generate code to ensure that \p value is an object and it it isn't, throw
  /// a type error with the specified message.
  void emitEnsureObject(Value *value, llvh::StringRef message);

  /// \return the internal value @@iterator
  Value *emitIteratorSymbol();

  /// IteratorRecord as defined in ES2018 7.4.1 GetIterator
  struct IteratorRecordSlow {
    Value *iterator;
    Value *nextMethod;
  };

  /// ES2018 7.4.1 GetIterator
  /// https://www.ecma-international.org/ecma-262/9.0/index.html#sec-getiterator
  ///
  /// Call obj[@@iterator], which should return an iterator, and return the
  /// iterator itself and its \c next() method.
  ///
  /// NOTE: This API is slow and should only be used if it is necessary to
  /// provide a value to the `next()` method on the iterator.
  ///
  /// \return (iterator, nextMethod)
  IteratorRecordSlow emitGetIteratorSlow(Value *obj);

  /// ES2018 7.4.2 IteratorNext
  /// https://www.ecma-international.org/ecma-262/9.0/index.html#sec-iteratornext
  ///
  /// Call the next method and throw an exception if the result is not an
  /// object.
  ///
  /// \return the result object.
  Value *emitIteratorNextSlow(IteratorRecordSlow iteratorRecord);

  /// ES2018 7.4.3 IteratorComplete
  /// https://www.ecma-international.org/ecma-262/9.0/index.html#sec-iteratorcomplete
  ///
  /// \return \c iterResult.done
  Value *emitIteratorCompleteSlow(Value *iterResult);

  /// ES2018 7.4.4 IteratorValue
  /// https://www.ecma-international.org/ecma-262/9.0/index.html#sec-iteratorvalue
  ///
  /// \return \c iterResult.value
  Value *emitIteratorValueSlow(Value *iterResult);

  /// ES2018 7.4.6 IteratorClose
  /// https://www.ecma-international.org/ecma-262/9.0/index.html#sec-iteratorclose
  ///
  /// \param ignoreInnerException if set, exceptions thrown by the \c
  ///     iterator.return() method will be ignored and its result will not be
  ///     checked whether it is an object.
  void emitIteratorCloseSlow(
      IteratorRecordSlow iteratorRecord,
      bool ignoreInnerException);

  /// Used as the stack storages needed for fast iteration.
  struct IteratorRecord {
    /// Storage for the iterator or the index.
    /// Set to undefined if iteration is complete.
    AllocStackInst *iterStorage;
    /// If this is an array being iterated by index, stores the source.
    /// Otherwise, stores the `iterator.next` method.
    AllocStackInst *sourceOrNext;
  };

  /// Emit the IteratorBeginInst for \p obj, which allows us to iterate arrays
  /// efficiently.
  IteratorRecord emitGetIterator(Value *obj);

  /// Emit the IteratorNextInst for \p iteratorRecord, which may alter the
  /// iteratorRecord to indicate completion.
  Value *emitIteratorNext(IteratorRecord iteratorRecord) {
    return Builder.createIteratorNextInst(
        iteratorRecord.iterStorage, iteratorRecord.sourceOrNext);
  }

  /// If iteratorRecord.iterStorage is undefined, then iteration is complete
  /// and the user of the iterator may want to branch to an exit code path.
  /// \return iteratorRecord.iterStorage === undefined
  Value *emitIteratorComplete(IteratorRecord iteratorRecord) {
    return Builder.createBinaryOperatorInst(
        Builder.createLoadStackInst(iteratorRecord.iterStorage),
        Builder.getLiteralUndefined(),
        BinaryOperatorInst::OpKind::StrictlyEqualKind);
  }

  /// Emit the IteratorCloseInst, which will close the iterator if it was
  /// opened by IteratorBeginInst.
  /// \param ignoreInnerException ignore any exceptions thrown by `.return()`.
  Value *emitIteratorClose(
      IteratorRecord iteratorRecord,
      bool ignoreInnerException) {
    return Builder.createIteratorCloseInst(
        iteratorRecord.iterStorage, ignoreInnerException);
  }

  /// Generate code for destructuring assignment to ArrayPattern or
  /// ObjectPattern.
  void emitDestructuringAssignment(
      bool declInit,
      ESTree::PatternNode *target,
      Value *source);

  /// Generate code for destructuring assignment to ArrayPattern.
  void emitDestructuringArray(
      bool declInit,
      ESTree::ArrayPatternNode *targetPat,
      Value *source);

  /// A record used by to describe a shared exception handler. Every individual
  /// stores the caught exception in the specified location and branches to the
  /// specified block.
  struct SharedExceptionHandler {
    /// Stack location to store the exception if it occurs.
    AllocStackInst *exc = nullptr;
    /// Branch to this block if an exception occurs.
    BasicBlock *exceptionBlock = nullptr;
    /// Set to true if at least one try/catch block was needed and emitted.
    bool emittedTry = false;
  };

  void emitRestElement(
      bool declInit,
      ESTree::RestElementNode *rest,
      IteratorRecord iteratorRecord,
      AllocStackInst *iteratorDone,
      SharedExceptionHandler *handler);

  /// Emit an operation guarded by try with a shared exception handler.
  template <typename EB>
  void emitTryWithSharedHandler(SharedExceptionHandler *handler, EB emitBody);

  /// Generate code for destructuring assignment to ObjectPattern.
  void emitDestructuringObject(
      bool declInit,
      ESTree::ObjectPatternNode *target,
      Value *source);

  /// Generate code for assigning to the "rest" property in an object
  /// destructuring pattern. \p excludedItems is a list of the keys that have
  /// been destructured so far, so they can be excluded from the rest property.
  void emitRestProperty(
      bool declInit,
      ESTree::RestElementNode *rest,
      const llvh::SmallVectorImpl<Value *> &excludedItems,
      Value *source);

  /// If the initializer \p init is nullptr, just return \p value.
  /// Otherwise emit code to check whether \p value equals \c undefined, and
  /// evaluate and return the initializer in that case.
  /// \param nameHint used to provide names for anonymous functions.
  Value *emitOptionalInitialization(
      Value *value,
      ESTree::Node *init,
      Identifier nameHint);

 private:
  /// "Converts" a ScopeChain into a SerializedScopePtr chain by resolving the
  /// identifiers.
  SerializedScopePtr resolveScopeIdentifiers(const ScopeChain &chain);

  /// Save all variables currently in scope, for lazy compilation.
  SerializedScopePtr saveScopeChain(ScopeDesc *S) {
    return serializeScope(S, true);
  }

  /// Recursively serialize scopes. The global scope is serialized
  /// if and only if it's the first scope and includeGlobal is true.
  SerializedScopePtr serializeScope(ScopeDesc *S, bool includeGlobal);

  ScopeDesc *newScopeDesc() {
    return currentIRScopeDesc_->createInnerScope();
  }

  /// Generate a synthetic name if \p name is invalid and the code generation
  /// settings specify unnamed functions must have a name.
  Identifier genAnonymousFunctionNameIfNeeded(Identifier name) {
    if (LLVM_LIKELY(
            name.isValid() ||
            !Mod->getContext()
                 .getCodeGenerationSettings()
                 .generateNameForUnnamedFunctions)) {
      return name;
    }

    // Use the current function's name to generate a globally-unique name.
    return genAnonymousLabelName(
        std::string("anonFunc@") +
        curFunction()->function->getOriginalOrInferredName().c_str());
  }

  enum class AlreadyEmitted { No, Yes };
  llvh::DenseMap<
      ESTree::FunctionDeclarationNode *,
      std::pair<Function *, AlreadyEmitted>>
      functionForDecl;

  /// Emits a CreateFunction instruction for \p func in the current location if
  /// \p func hasn't been created yet (it may have been created due to function
  /// hoisting, in which case this method won't do anything).
  void emitCreateFunction(ESTree::FunctionDeclarationNode *func);

  /// Emits CreateFunction instructions for all functions defined immediately
  /// within \p containingNode. This implements the semantics of
  /// ES2023 14.2.2 Runtime Semantics: Evaluation
  ///   [...]
  ///   Block : { StatementList }
  ///     [...]
  ///     3. Perform BlockDeclarationInstantiation(StatementList, blockEnv).
  ///
  /// Specifically, this implements 14.2.3 BlockDeclarationInstantiation,
  /// 14.2.3.3.b.i, 14.2.3.3.b.ii, and 14.2.3.3.b.iii.
  void hoistCreateFunctions(ESTree::Node *containingNode);

  /// Emit an instruction to load a value from a specified location.
  /// \param from location to load from, either a Variable or
  ///     GlobalObjectProperty.
  /// \param inhibitThrow  if true, do not throw when loading from missing
  ///     global properties.
  /// \return the instruction performing the load.
  Instruction *emitLoad(Value *from, bool inhibitThrow = false);

  /// Emit an instruction to a store a value into the specified location.
  /// \param storedValue value to store
  /// \param ptr location to store into, either a Variable or
  ///     GlobalObjectProperty.
  /// \param declInit whether this is a declaration initializer, so the TDZ
  ///     check should be skipped.
  /// \return the instruction performing the store.
  Instruction *emitStore(Value *storedValue, Value *ptr, bool declInit);

  /// Creates a new, empty declarative environment, and make it the current
  /// environment for IRGen. Caller is supposed to have creates an appropriate
  /// EnterBlockScope object. This implements
  /// ES2023 9.1.2.2 NewDeclarativeEnvironment ( E ),
  /// with E being currentIRScope_.
  void newDeclarativeEnvironment();

  /// Sets up a new scope by
  ///
  /// 1. Creating a new declarative environment;
  /// 2. Creates all let/const declarations in \p containingNode;
  /// 3. Codegens all functions definitions in \p containingNode; and
  /// 4. Emit all CreateFunctions for function definitions in \p containingNode.
  ///
  /// 2. through 4. implement
  /// ES2023 14.2.3 BlockDeclarationInstantiation ( code, env ).
  void blockDeclarationInstantiation(ESTree::Node *containingNode);
};

template <typename EB, typename EF, typename EH>
BasicBlock *ESTreeIRGen::emitTryCatchScaffolding(
    BasicBlock *nextBlock,
    EB emitBody,
    EF emitNormalCleanup,
    EH emitHandler) {
  auto *function = Builder.getFunction();

  auto *catchBlock = Builder.createBasicBlock(function);
  if (!nextBlock)
    nextBlock = Builder.createBasicBlock(function);
  auto *tryBodyBlock = Builder.createBasicBlock(function);

  // Start with a TryStartInst, and transition to try body.
  Builder.createTryStartInst(tryBodyBlock, catchBlock);
  Builder.setInsertionBlock(tryBodyBlock);

  // Generate IR for the body of Try
  emitBody();

  // Emit TryEnd in a new block.
  auto *tryEndBlock = Builder.createBasicBlock(function);
  Builder.createBranchInst(tryEndBlock);
  Builder.setInsertionBlock(tryEndBlock);
  Builder.createTryEndInst();

  emitNormalCleanup();

  Builder.createBranchInst(nextBlock);

  // Generate the catch/finally block.
  Builder.setInsertionBlock(catchBlock);

  emitHandler(nextBlock);

  return nextBlock;
}

template <typename EB>
void ESTreeIRGen::emitTryWithSharedHandler(
    hermes::irgen::ESTreeIRGen::SharedExceptionHandler *handler,
    EB emitBody) {
  emitTryCatchScaffolding(
      nullptr,
      // emitBody.
      emitBody,
      // emitNormalCleanup.
      []() {},
      // emitHandler.
      [this, handler](BasicBlock *nextBlock) {
        Builder.createStoreStackInst(Builder.createCatchInst(), handler->exc);
        Builder.createBranchInst(handler->exceptionBlock);
        Builder.setInsertionBlock(nextBlock);
      });
  handler->emittedTry = true;
}

} // namespace irgen
} // namespace hermes

#endif // HERMES_IRGEN_ESTREEIRGEN_H
