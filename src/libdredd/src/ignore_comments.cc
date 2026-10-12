// Copyright 2026 The Dredd Project Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "libdredd/ignore_comments.h"

#include <cstddef>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "clang/AST/ASTContext.h"
#include "clang/Basic/Diagnostic.h"
#include "clang/Basic/SourceLocation.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Basic/TokenKinds.h"
#include "clang/Lex/Lexer.h"
#include "clang/Lex/Token.h"
#include "libdredd/protobufs/dredd_protobufs.h"
#include "libdredd/util.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringRef.h"

namespace dredd {
namespace {

// The lower-case names of the mutation kinds and of the mutation actions, as
// the mutation info file gives them.
std::set<std::string> BuildKnownNames() {
  std::set<std::string> result = {"removestmt", "replaceexpr",
                                  "replacebinaryoperator",
                                  "replaceunaryoperator"};
  for (const auto* descriptor :
       {protobufs::MutationReplaceExprAction_descriptor(),
        protobufs::MutationReplaceBinaryOperatorAction_descriptor(),
        protobufs::MutationReplaceUnaryOperatorAction_descriptor()}) {
    for (int index = 0; index < descriptor->value_count(); index++) {
      result.insert(
          llvm::StringRef(std::string(descriptor->value(index)->name()))
              .lower());
    }
  }
  return result;
}

const std::set<std::string>& KnownNames() {
  static const std::set<std::string> known_names = BuildKnownNames();
  return known_names;
}

}  // namespace

std::optional<IgnoreDirective> ParseIgnoreDirective(llvm::StringRef comment,
                                                    std::string& error) {
  error.clear();
  if (!comment.consume_front("//")) {
    return std::nullopt;
  }
  llvm::StringRef rest = comment.ltrim();
  const llvm::StringRef tool = rest.take_while(llvm::isAlnum);
  if (!tool.equals_insensitive("dredd")) {
    return std::nullopt;
  }
  rest = rest.drop_front(tool.size());
  if (rest.empty() || !llvm::isSpace(rest.front())) {
    return std::nullopt;
  }
  rest = rest.ltrim();
  const llvm::StringRef verb = rest.take_while(llvm::isAlnum);
  IgnoreDirective directive;
  if (verb.equals_insensitive("disable")) {
    directive.disable = true;
  } else if (verb.equals_insensitive("restore")) {
    directive.disable = false;
  } else {
    return std::nullopt;
  }
  rest = rest.drop_front(verb.size());
  if (!rest.empty() && !llvm::isSpace(rest.front()) && rest.front() != ':') {
    return std::nullopt;
  }

  // The comment is a directive from here on, so each problem is an error.
  rest = rest.ltrim();
  const llvm::StringRef next = rest.take_while(llvm::isAlnum);
  if (next.equals_insensitive("once")) {
    if (!directive.disable) {
      error = "a Dredd restore directive takes no 'once'";
      return std::nullopt;
    }
    directive.once = true;
    rest = rest.drop_front(next.size());
  }
  const auto [names_text, reason_text] = rest.split(':');
  llvm::SmallVector<llvm::StringRef> parts;
  names_text.split(parts, ',');
  for (llvm::StringRef part : parts) {
    part = part.trim();
    if (part.empty()) {
      error =
          "a Dredd directive needs 'all' or a list of mutation kinds and "
          "actions, separated by commas";
      return std::nullopt;
    }
    const std::string name = part.lower();
    if (name == "all") {
      directive.all = true;
      continue;
    }
    if (!KnownNames().contains(name)) {
      error = "unknown mutation kind or action '" + part.str() +
              "' in a Dredd directive";
      return std::nullopt;
    }
    directive.names.insert(name);
  }
  if (directive.all && !directive.names.empty()) {
    error = "a Dredd directive names 'all' together with other names";
    return std::nullopt;
  }
  directive.reason = reason_text.trim().str();
  return directive;
}

bool IgnoreScopes::Collect(clang::ASTContext& ast_context) {
  const clang::SourceManager& source_manager = ast_context.getSourceManager();
  const clang::FileID main_file = source_manager.getMainFileID();
  const llvm::StringRef buffer = source_manager.getBufferData(main_file);
  clang::Lexer lexer(source_manager.getLocForStartOfFile(main_file),
                     ast_context.getLangOpts(), buffer.begin(), buffer.begin(),
                     buffer.end());
  lexer.SetCommentRetentionState(true);
  clang::DiagnosticsEngine& diagnostics = ast_context.getDiagnostics();
  const unsigned error_id =
      diagnostics.getCustomDiagID(clang::DiagnosticsEngine::Error, "%0");

  bool valid = true;
  // The scopes of the "disable" directives that no "restore" has closed yet.
  std::vector<Scope> open;
  // The "disable once" directives that wait for the next token.
  std::vector<size_t> waiting;
  clang::Token token;
  while (true) {
    lexer.LexFromRawLexer(token);
    if (token.is(clang::tok::eof)) {
      break;
    }
    const clang::SourceLocation location = token.getLocation();
    if (!token.is(clang::tok::comment)) {
      for (const size_t index : waiting) {
        pending_[index].target_offset = source_manager.getFileOffset(location);
        pending_count_++;
      }
      waiting.clear();
      continue;
    }
    const llvm::StringRef text(
        buffer.data() + source_manager.getFileOffset(location),
        token.getLength());
    std::string error;
    const std::optional<IgnoreDirective> directive =
        ParseIgnoreDirective(text, error);
    if (!error.empty()) {
      diagnostics.Report(location, error_id).AddString(error);
      valid = false;
      continue;
    }
    if (!directive.has_value()) {
      continue;
    }
    const Position position = {
        source_manager.getSpellingLineNumber(location),
        source_manager.getSpellingColumnNumber(location)};
    if (directive->once) {
      pending_.push_back({0, *directive, position, false});
      waiting.push_back(pending_.size() - 1);
      continue;
    }
    if (directive->disable) {
      Scope scope;
      scope.begin = {position.first, position.second + token.getLength()};
      scope.all = directive->all;
      scope.names = directive->names;
      scope.reason = directive->reason;
      scope.comment = position;
      open.push_back(scope);
      continue;
    }
    // A "restore" ends each open scope that holds a name that it lists.
    // "restore all" ends each open scope.
    std::vector<Scope> still_open;
    for (Scope& scope : open) {
      if (directive->all) {
        scope.end = position;
        scopes_.push_back(scope);
        continue;
      }
      if (scope.all) {
        still_open.push_back(scope);
        continue;
      }
      Scope closed = scope;
      closed.names.clear();
      for (const auto& name : directive->names) {
        if (scope.names.erase(name) > 0) {
          closed.names.insert(name);
        }
      }
      if (!closed.names.empty()) {
        closed.end = position;
        scopes_.push_back(closed);
      }
      if (!scope.names.empty()) {
        still_open.push_back(scope);
      }
    }
    open = still_open;
  }
  for (Scope& scope : open) {
    scope.end = {std::numeric_limits<unsigned>::max(),
                 std::numeric_limits<unsigned>::max()};
    scopes_.push_back(scope);
  }
  return valid;
}

void IgnoreScopes::NoteNode(clang::SourceRange range,
                            const clang::ASTContext& ast_context) {
  if (range.isInvalid()) {
    return;
  }
  const clang::SourceManager& source_manager = ast_context.getSourceManager();
  const unsigned offset =
      source_manager.getFileOffset(source_manager.getFileLoc(range.getBegin()));
  for (PendingOnce& pending : pending_) {
    if (pending.resolved || pending.target_offset != offset) {
      continue;
    }
    const InfoForSourceRange info(range, ast_context);
    Scope scope;
    scope.begin = {info.GetStartLine(), info.GetStartColumn()};
    scope.end = {info.GetEndLine(), info.GetEndColumn()};
    scope.all = pending.directive.all;
    scope.names = pending.directive.names;
    scope.reason = pending.directive.reason;
    scope.comment = pending.comment;
    scopes_.push_back(scope);
    pending.resolved = true;
    pending_count_--;
  }
}

const IgnoreScopes::Scope* IgnoreScopes::FindScope(
    Position begin, Position end, const std::string& kind,
    const std::string& action) const {
  const Scope* result = nullptr;
  for (const Scope& scope : scopes_) {
    if (begin < scope.begin || scope.end < end) {
      continue;
    }
    if (!scope.all && !scope.names.contains(kind) &&
        !scope.names.contains(action)) {
      continue;
    }
    if (result == nullptr || result->begin < scope.begin) {
      result = &scope;
    }
  }
  return result;
}

void IgnoreScopes::AddIgnoredMutations(
    const protobufs::MutationGroup& group,
    protobufs::MutationInfoForFile& info_for_file) const {
  if (scopes_.empty()) {
    return;
  }
  const auto add = [this, &info_for_file](
                       int mutation_id, const protobufs::SourceLocation& start,
                       const protobufs::SourceLocation& end,
                       const std::string& kind, const std::string& action) {
    const Scope* scope = FindScope({start.line(), start.column()},
                                   {end.line(), end.column()}, kind, action);
    if (scope == nullptr) {
      return;
    }
    protobufs::IgnoredMutation* ignored = info_for_file.add_ignored_mutations();
    ignored->set_mutation_id(mutation_id);
    ignored->set_reason(scope->reason);
    ignored->mutable_comment()->set_line(scope->comment.first);
    ignored->mutable_comment()->set_column(scope->comment.second);
  };
  switch (group.group_case()) {
    case protobufs::MutationGroup::kRemoveStmt: {
      const auto& remove_stmt = group.remove_stmt();
      add(remove_stmt.mutation_id(), remove_stmt.start(), remove_stmt.end(),
          "removestmt", "");
      break;
    }
    case protobufs::MutationGroup::kReplaceExpr: {
      const auto& replace_expr = group.replace_expr();
      for (const auto& instance : replace_expr.instances()) {
        add(instance.mutation_id(), replace_expr.start(), replace_expr.end(),
            "replaceexpr",
            llvm::StringRef(
                std::string(protobufs::MutationReplaceExprAction_Name(
                    instance.action())))
                .lower());
      }
      break;
    }
    case protobufs::MutationGroup::kReplaceBinaryOperator: {
      const auto& replace = group.replace_binary_operator();
      for (const auto& instance : replace.instances()) {
        add(instance.mutation_id(), replace.expr_start(), replace.expr_end(),
            "replacebinaryoperator",
            llvm::StringRef(
                std::string(protobufs::MutationReplaceBinaryOperatorAction_Name(
                    instance.action())))
                .lower());
      }
      break;
    }
    case protobufs::MutationGroup::kReplaceUnaryOperator: {
      const auto& replace = group.replace_unary_operator();
      for (const auto& instance : replace.instances()) {
        add(instance.mutation_id(), replace.expr_start(), replace.expr_end(),
            "replaceunaryoperator",
            llvm::StringRef(
                std::string(protobufs::MutationReplaceUnaryOperatorAction_Name(
                    instance.action())))
                .lower());
      }
      break;
    }
    default:
      break;
  }
}

}  // namespace dredd
