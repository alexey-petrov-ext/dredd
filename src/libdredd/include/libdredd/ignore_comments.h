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

#ifndef LIBDREDD_IGNORE_COMMENTS_H
#define LIBDREDD_IGNORE_COMMENTS_H

#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "clang/AST/ASTContext.h"
#include "clang/Basic/SourceLocation.h"
#include "libdredd/protobufs/dredd_protobufs.h"
#include "llvm/ADT/StringRef.h"

namespace dredd {

// A line comment that keeps mutants out of the mutation testing, so that a
// tool that reads the mutation info file can report them as ignored. Dredd
// still applies such a mutant, so a directive changes no mutant id and no
// mutated code.
//
//   // Dredd disable once <names>[: <reason>]
//   // Dredd disable <names>[: <reason>]
//   // Dredd restore <names>
//
// <names> is "all", or a comma-separated list of mutation kinds (such as
// "replaceExpr") and mutation actions (such as "ReplaceWithLHS"), as the
// mutation info file names them. The keywords and the names ignore case.
//
// "disable once" holds the outermost statement or declaration that starts at
// the first token after the comment. "disable" holds each of its names until a
// "restore" that names it, or "restore all", or the end of the file. A
// directive holds a mutant only when the whole source range of the mutant lies
// in its scope. Where scopes overlap, the scope that starts last gives the
// reason.
struct IgnoreDirective {
  // False for "restore".
  bool disable = true;
  bool once = false;
  bool all = false;
  // The lower-case names, when 'all' is false.
  std::set<std::string> names;
  std::string reason;
};

// Parses the text of one comment, with its comment markers. Gives std::nullopt
// and an empty |error| when the comment is no Dredd directive. Gives
// std::nullopt and sets |error| when the comment starts with "Dredd disable" or
// "Dredd restore", but is not a valid directive.
std::optional<IgnoreDirective> ParseIgnoreDirective(llvm::StringRef comment,
                                                    std::string& error);

// The scopes of the directives of one main file.
class IgnoreScopes {
 public:
  // Reads each line comment of the main file with a raw lexer, so that text in
  // a string literal or in a block comment is never a directive. Reports each
  // invalid directive as an error at its location, and gives false when there
  // is one.
  bool Collect(clang::ASTContext& ast_context);

  // True while a "disable once" directive waits for its statement or
  // declaration.
  [[nodiscard]] bool HasPendingOnce() const { return pending_count_ > 0; }

  // Gives a "disable once" directive its scope when |range|, the source range
  // in the main file of a node that the visitor enters, starts at the token
  // after the directive. The visitor enters an outer node first, so the first
  // node that starts there gives the scope.
  void NoteNode(clang::SourceRange range, const clang::ASTContext& ast_context);

  // Adds an ignored mutation to |info_for_file| for each mutant of |group|
  // that a scope holds.
  void AddIgnoredMutations(const protobufs::MutationGroup& group,
                           protobufs::MutationInfoForFile& info_for_file) const;

 private:
  // A line and a column, as the mutation info file gives them.
  using Position = std::pair<unsigned, unsigned>;

  struct Scope {
    Position begin;
    // The position after the last character.
    Position end;
    bool all = false;
    std::set<std::string> names;
    std::string reason;
    Position comment;
  };

  struct PendingOnce {
    unsigned target_offset = 0;
    IgnoreDirective directive;
    Position comment;
    bool resolved = false;
  };

  [[nodiscard]] const Scope* FindScope(Position begin, Position end,
                                       const std::string& kind,
                                       const std::string& action) const;

  std::vector<Scope> scopes_;
  std::vector<PendingOnce> pending_;
  size_t pending_count_ = 0;
};

}  // namespace dredd

#endif  // LIBDREDD_IGNORE_COMMENTS_H
