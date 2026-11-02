/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "IRInstrument.h" // @manual

namespace hermes {

#ifdef HERMES_ENABLE_IR_INSTRUMENTATION
IRInstrument::IRInstrument(hermes::Module *M, hermes::IRBuilder &builder)
    : builder_(builder),
      globalName_(builder.getLiteralString("__instrument")),
      M_(M),
      enabled_(M->getContext().getCodeGenerationSettings().instrumentIR) {}

IRInstrument::~IRInstrument() = default;

Value *IRInstrument::getIID(ESTree::Node *node) {
  auto start = node->getStartLoc();
  if (!start.isValid())
    return builder_.getLiteralUndefined();

  auto &sem = M_->getContext().getSourceErrorManager();
  auto *buffer = sem.findBufferForLoc(start);
  uint64_t bufferId = sem.findBufferIdForLoc(start);
  uint64_t offset = (uint64_t)((uintptr_t)start.getPointer() -
                               (uintptr_t)buffer->getBufferStart());

  double id = (double)((bufferId << 32) | offset);
  return builder_.getLiteralNumber(id);
}

Value *IRInstrument::invokeHook(
    llvh::StringRef name,
    llvh::ArrayRef<Value *> args) {
  TryLoadGlobalPropertyInst *instrument =
      builder_.createTryLoadGlobalPropertyInst(globalName_);
  auto *hook = builder_.createLoadPropertyInst(instrument, name);
  return builder_.createCallInst(
      CallInst::kNoTextifiedCallee, hook, instrument, args);
}

Value *IRInstrument::preBinaryExpression(
    ESTree::BinaryExpressionNode *node,
    Value *left,
    Value *right) {
  if (!enabled_)
    return nullptr;

  auto *opStr = builder_.getLiteralString(node->_operator->str());
  return invokeHook("preBinary", {getIID(node), opStr, left, right});
}

Value *IRInstrument::postBinaryExpression(
    ESTree::BinaryExpressionNode *node,
    Value *cookie,
    Value *result,
    Value *left,
    Value *right) {
  if (!enabled_)
    return result;

  auto *opStr = builder_.getLiteralString(node->_operator->str());
  return invokeHook(
      "postBinary",
      {getIID(node), undefinedIfNull(cookie), opStr, result, left, right});
}

Value *IRInstrument::preUnaryExpression(
    ESTree::UnaryExpressionNode *node,
    Value *operand) {
  if (!enabled_)
    return nullptr;

  auto *opStr = builder_.getLiteralString(node->_operator->str());
  return invokeHook("preUnary", {getIID(node), opStr, operand});
}
Value *IRInstrument::postUnaryExpression(
    ESTree::UnaryExpressionNode *node,
    Value *cookie,
    Value *result,
    Value *operand) {
  if (!enabled_)
    return result;

  auto *opStr = builder_.getLiteralString(node->_operator->str());
  return invokeHook(
      "postUnary",
      {getIID(node), undefinedIfNull(cookie), opStr, result, operand});
}

Value *IRInstrument::preAssignment(
    ESTree::AssignmentExpressionNode *node,
    Value *left,
    Value *right) {
  if (!enabled_)
    return nullptr;

  auto *opStr = builder_.getLiteralString(node->_operator->str());
  return invokeHook(
      "preAssignment", {getIID(node), opStr, undefinedIfNull(left), right});
}

Value *IRInstrument::postAssignment(
    ESTree::AssignmentExpressionNode *node,
    Value *cookie,
    Value *result,
    Value *left,
    Value *right) {
  if (!enabled_)
    return result;

  auto *opStr = builder_.getLiteralString(node->_operator->str());
  return invokeHook(
      "postAssignment",
      {getIID(node),
       undefinedIfNull(cookie),
       opStr,
       result,
       undefinedIfNull(left),
       right});
}
#endif // HERMES_ENABLE_IR_INSTRUMENTATION

} // namespace hermes
