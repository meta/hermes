/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes/Parser/JSParser.h"

#include "JSParserImpl.h" // @manual

namespace hermes {
namespace parser {
JSParser::JSParser(Context &context, std::unique_ptr<llvh::MemoryBuffer> input)
    : impl_(std::make_shared<detail::JSParserImpl>(context, std::move(input))) {
}

JSParser::JSParser(Context &context, uint32_t bufferId, ParserPass pass)
    : impl_(std::make_shared<detail::JSParserImpl>(context, bufferId, pass)) {}

JSParser::~JSParser() = default;

Context &JSParser::getContext() {
  return impl_->getContext();
}

bool JSParser::isStrictMode() const {
  return impl_->isStrictMode();
}

void JSParser::setStrictMode(bool mode) {
  return impl_->setStrictMode(mode);
}

/// \return the source URL from the magic comment, or an empty string if there
/// was no magic comment.
llvh::StringRef JSParser::getSourceURL() const {
  return impl_->getLexer().getSourceURL();
}

/// \return the source mapping URL from the magic comment, or an empty string
/// if there was no magic comment.
llvh::StringRef JSParser::getSourceMappingURL() const {
  return impl_->getLexer().getSourceMappingURL();
}

void JSParser::registerMagicURLs(MCFlag::Type flags) const {
#ifdef STATIC_HERMES
  uint32_t bufId = impl_->getLexer().getBufferId();
  SourceErrorManager &sm = impl_->getContext().getSourceErrorManager();
  if (flags & MCFlag::SourceURL)
    sm.setSourceUrl(bufId, getSourceURL());
  if (flags & MCFlag::SourceMappingURL)
    sm.setSourceMappingUrl(bufId, getSourceMappingURL());
#endif
}

llvh::ArrayRef<StoredComment> JSParser::getStoredComments() const {
  return impl_->getLexer().getStoredComments();
}

std::vector<StoredComment> JSParser::moveStoredComments() const {
  return impl_->getLexer().moveStoredComments();
}

llvh::ArrayRef<StoredToken> JSParser::getStoredTokens() const {
  return impl_->getLexer().getStoredTokens();
}

void JSParser::setStoreComments(bool storeComments) {
  impl_->getLexer().setStoreComments(storeComments);
}

void JSParser::setStoreTokens(bool storeTokens) {
  impl_->getLexer().setStoreTokens(storeTokens);
}

bool JSParser::getUseStaticBuiltin() const {
  return impl_->getUseStaticBuiltin();
}

llvh::Optional<ESTree::ProgramNode *> JSParser::parse() {
  return impl_->parse();
}

void JSParser::seek(SMLoc startPos) {
  return impl_->seek(startPos);
}

std::unique_ptr<JSParser> JSParser::preParseBuffer(
    Context &context,
    uint32_t bufferId) {
  if (auto preParser =
          detail::JSParserImpl::preParseBuffer(context, bufferId)) {
    return std::unique_ptr<JSParser>(new JSParser(std::move(preParser)));
  } else {
    return nullptr;
  }
}

llvh::Optional<ESTree::NodePtr> JSParser::parseLazyFunction(
    ESTree::NodeKind kind,
    bool paramYield,
    bool paramAwait,
    SMLoc start) {
  return impl_->parseLazyFunction(kind, paramYield, paramAwait, start);
}
} // namespace parser
} // namespace hermes
