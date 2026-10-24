// Copyright 2022 The Dredd Project Authors
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

#include "libdredd/mutation_remove_stmt.h"

#include <cassert>
#include <sstream>
#include <string>
#include <unordered_set>

#include "clang/AST/ASTContext.h"
#include "clang/AST/Expr.h"
#include "clang/AST/Stmt.h"
#include "clang/Basic/SourceLocation.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Basic/TokenKinds.h"
#include "clang/Lex/Lexer.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/Lex/Token.h"
#include "clang/Rewrite/Core/Rewriter.h"
#include "clang/Tooling/Transformer/SourceCode.h"
#include "libdredd/options.h"
#include "libdredd/util.h"
#include "llvm/Support/Casting.h"

namespace dredd {

namespace {

// Determines whether |stmt| ends with the closing brace of a compound
// statement, such as the body of a loop, and not with the last token of an
// expression, which can be a closing brace too.
bool EndsWithCompoundStmt(const clang::Stmt& stmt) {
  const clang::Stmt* current = &stmt;
  while (llvm::dyn_cast<clang::CompoundStmt>(current) == nullptr) {
    if (llvm::dyn_cast<clang::Expr>(current) != nullptr) {
      return false;
    }
    // Descend into the last child that ends where |current| ends.
    const clang::Stmt* last_child = nullptr;
    for (const clang::Stmt* child : current->children()) {
      if (child != nullptr && child->getEndLoc() == current->getEndLoc()) {
        last_child = child;
      }
    }
    if (last_child == nullptr) {
      return false;
    }
    current = last_child;
  }
  return true;
}

}  // namespace

MutationRemoveStmt::MutationRemoveStmt(const clang::Stmt& stmt,
                                       const clang::Preprocessor& preprocessor,
                                       const clang::ASTContext& ast_context)
    : stmt_(&stmt),
      info_for_source_range_(GetSourceRangeInMainFile(preprocessor, stmt),
                             ast_context) {}

protobufs::MutationGroup MutationRemoveStmt::Apply(
    clang::ASTContext& ast_context, const clang::Preprocessor& preprocessor,
    const Options& options, int first_mutation_id_in_file, int& mutation_id,
    clang::Rewriter& rewriter,
    std::unordered_set<std::string>& dredd_declarations) const {
  (void)dredd_declarations;  // Unused.

  // The protobuf object for the mutation, which will be wrapped in a
  // MutationGroup.
  protobufs::MutationRemoveStmt inner_result;
  inner_result.set_mutation_id(mutation_id);
  inner_result.mutable_start()->set_line(info_for_source_range_.GetStartLine());
  inner_result.mutable_start()->set_column(
      info_for_source_range_.GetStartColumn());
  inner_result.mutable_end()->set_line(info_for_source_range_.GetEndLine());
  inner_result.mutable_end()->set_column(info_for_source_range_.GetEndColumn());
  *inner_result.mutable_snippet() = info_for_source_range_.GetSnippet();

  clang::CharSourceRange source_range = clang::CharSourceRange::getTokenRange(
      GetSourceRangeInMainFile(preprocessor, *stmt_));

  // If the statement is followed immediately by a semicolon, possibly with
  // intervening comments, that semicolon should be part of the code that is
  // wrapped in an 'if'.

  // First, skip over any intervening comments. It doesn't matter whether they
  // end up getting wrapped in the 'if'.
  bool is_extended_with_comment = false;
  while (true) {
    // Extend the source range in the case that the next token is a comment.
    const clang::CharSourceRange source_range_extended_with_comment =
        clang::tooling::maybeExtendRange(
            source_range, clang::tok::TokenKind::comment, ast_context);
    if (source_range.getAsRange() ==
        source_range_extended_with_comment.getAsRange()) {
      // The source range wasn't extended, so there aren't any more comments
      // before the next non-comment token.
      break;
    }
    // Update the source range to its extended form and note that a comment was
    // skipped.
    is_extended_with_comment = true;
    source_range = source_range_extended_with_comment;
  }

  // Now try to extend the source range further to include the next token, if it
  // is a semicolon.
  const clang::CharSourceRange source_range_extended_with_semi =
      clang::tooling::maybeExtendRange(
          source_range, clang::tok::TokenKind::semi, ast_context);
  const bool is_extended_with_semi =
      source_range.getAsRange() != source_range_extended_with_semi.getAsRange();
  if (is_extended_with_semi) {
    source_range = source_range_extended_with_semi;
  }

  // Subtracting |first_mutation_id_in_file| turns the global mutation id,
  // |mutation_id|, into a file-local mutation id.
  const int local_mutation_id = mutation_id - first_mutation_id_in_file;

  std::string ast_node_type_comment;
  if (options.GetShowAstNodeTypes()) {
    std::stringstream stringstream;
    stringstream << stmt_;
    ast_node_type_comment = "/*" + std::string(stmt_->getStmtClassName()) +
                            " " + stringstream.str() + "*/";
  }

  if (options.GetOnlyTrackMutantCoverage()) {
    const bool rewriter_result = rewriter.InsertTextBefore(
        source_range.getBegin(),
        "__dredd_record_covered_mutants" + ast_node_type_comment + "(" +
            std::to_string(local_mutation_id) + ", 1); ");
    assert(!rewriter_result && "Rewrite failed.\n");
    (void)rewriter_result;  // Keep release-mode compilers happy.
  } else {
    bool rewriter_result = rewriter.InsertTextBefore(
        source_range.getBegin(),
        "if (!__dredd_enabled_mutation" + ast_node_type_comment + "(" +
            std::to_string(local_mutation_id) + ")) { ");
    assert(!rewriter_result && "Rewrite failed.\n");
    std::string to_insert = " }";
    if (!is_extended_with_semi && IsNextTokenHash(source_range, preprocessor)) {
      // If we did not extend the range to capture an immediately-following
      // semicolon, then we could have a problem if the statement is separated
      // from its semicolon by a preprocessor directive. For example:
      //
      // ...
      // x = 1
      // #endif
      // ;
      //
      // If we find a hash token - indicative of a preprocessor directive -
      // having not extended the range with a semicolon, we conservatively add
      // a semicolon to ensure that the code in the conditional that is
      // introduced is well-formed.
      to_insert = "; " + to_insert;
    }
    if (is_extended_with_comment && !is_extended_with_semi) {
      // If the source range was extended with a comment but not with a
      // semicolon, it is possible that the end of the source range is on
      // the same line as a single-line comment, in which case it's
      // important to take a new line (otherwise the closing brace will form
      // part of the comment). It would be possible to always take a new
      // line, but this would make mutated files harder to read.
      to_insert = "\n" + to_insert;
    }
    // The next statement can start where this one ends, as in
    // 'else f(x);y = 1;'. A mutation that is applied later then inserts the
    // text that opens its mutant at the location where the closing text of
    // this 'if' goes. The rewriter puts text that is inserted before a
    // location in front of the text that was inserted there earlier, so the
    // closing text would come after the start of the next statement. If the
    // last token is the semicolon or the comment that extends the range, or
    // the closing brace of a compound statement that ends this statement, it
    // is therefore replaced by itself followed by the closing text: text that
    // is inserted later at the next location then goes after the closing text.
    // Any other last token, a closing brace too as in 'x = int{2}', can end an
    // expression of this statement, after which a mutation of that expression
    // has already inserted text that must stay before the closing text, so the
    // closing text is inserted after it.
    const clang::SourceManager& source_manager =
        preprocessor.getSourceManager();
    clang::Token last_token{};
    if ((is_extended_with_semi || is_extended_with_comment ||
         EndsWithCompoundStmt(*stmt_)) &&
        !clang::Lexer::getRawToken(source_range.getEnd(), last_token,
                                   source_manager, preprocessor.getLangOpts(),
                                   /*IgnoreWhiteSpace=*/false) &&
        last_token.isOneOf(clang::tok::TokenKind::semi,
                           clang::tok::TokenKind::r_brace,
                           clang::tok::TokenKind::comment)) {
      const std::string last_token_text(
          source_manager.getCharacterData(source_range.getEnd()),
          last_token.getLength());
      rewriter_result =
          rewriter.ReplaceText(source_range.getEnd(), last_token.getLength(),
                               last_token_text + to_insert);
    } else {
      rewriter_result =
          rewriter.InsertTextAfterToken(source_range.getEnd(), to_insert);
    }
    assert(!rewriter_result && "Rewrite failed.\n");
    (void)rewriter_result;  // Keep release-mode compilers happy.
  }

  mutation_id++;

  protobufs::MutationGroup result;
  *result.mutable_remove_stmt() = inner_result;
  return result;
}

bool MutationRemoveStmt::IsNextTokenHash(
    const clang::CharSourceRange& source_range,
    const clang::Preprocessor& preprocessor) {
  const clang::CharSourceRange source_range_as_char_range =
      clang::Lexer::getAsCharRange(source_range,
                                   preprocessor.getSourceManager(),
                                   preprocessor.getLangOpts());
  clang::Token maybe_hash{};
  return !clang::Lexer::getRawToken(source_range_as_char_range.getEnd(),
                                    maybe_hash, preprocessor.getSourceManager(),
                                    preprocessor.getLangOpts(),
                                    /*IgnoreWhiteSpace=*/true) &&
         maybe_hash.is(clang::tok::TokenKind::hash);
}

}  // namespace dredd
