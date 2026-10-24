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
// limitations under the License

#include "libdredd/util.h"

#include <algorithm>
#include <cassert>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclBase.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/Expr.h"
#include "clang/AST/NestedNameSpecifier.h"
#include "clang/AST/PrettyPrinter.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Basic/SourceLocation.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Lex/Lexer.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/Rewrite/Core/Rewriter.h"
#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Config/llvm-config.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/raw_ostream.h"

namespace dredd {

namespace {

clang::SourceLocation getSourceLocationInMainFileForExtremeOfRange(
    const clang::SourceLocation& extreme_of_range,
    const clang::Preprocessor& preprocessor, bool is_start_of_range) {
  const clang::SourceManager& source_manager = preprocessor.getSourceManager();
  auto file_id = source_manager.getFileID(extreme_of_range);
  if (file_id == source_manager.getMainFileID()) {
    // This is the easy case: the source location occurs directly in the main
    // file.
    return extreme_of_range;
  }
  // The source location's file ID is not the main file's ID. This means it is
  // either part of a macro expansion, or it is in some other file.
  if (!extreme_of_range.isMacroID()) {
    // The source location is in some other file.
    return {};
  }
  // The source location is part of a macro expansion. If it is part of a
  // "paste" operation in a macro, which uses the ## preprocessor operator, then
  // it is hard to trace this back accurately to a non-macro source location,
  // so we give up. Testing for this is done by checking whether source location
  // is written into "scratch" space, which is the case for pasted macro
  // expansions.
  if (preprocessor.getSourceManager().isWrittenInScratchSpace(
          source_manager.getSpellingLoc(extreme_of_range))) {
    return {};
  }
  clang::SourceLocation macro_expansion_location;
  // We now check whether the macro expansion location is at the start/end of
  // the expansion, depending on whether the source location corresponds to the
  // start/end of a range.
  if (is_start_of_range) {
    if (!preprocessor.isAtStartOfMacroExpansion(extreme_of_range,
                                                &macro_expansion_location)) {
      // The source location is somewhere in the middle of a macro expansion -
      // it cannot be traced back to the main file.
      return {};
    }
  } else {
    if (!preprocessor.isAtEndOfMacroExpansion(extreme_of_range,
                                              &macro_expansion_location)) {
      // Similar.
      return {};
    }
  }
  // The source location does correspond to the extreme of a macro expansion.
  // Finally, we check whether the expansion location is in the main file or
  // some other file.
  if (source_manager.getFileID(macro_expansion_location) ==
      source_manager.getMainFileID()) {
    // The expansion location is in the main file, so can be returned.
    return macro_expansion_location;
  }
  // Tne expansion location is in some file other than the main file.
  return {};
}
}  // namespace

// Utility method used to avoid spaces when types, such as 'unsigned int', are
// used in mutation function names.
std::string SpaceToUnderscore(const std::string& input) {
  std::string result(input);
  std::replace(result.begin(), result.end(), ' ', '_');
  return result;
}

bool SourceRangeConsistencyCheck(clang::SourceRange source_range,
                                 const clang::ASTContext& ast_context) {
  const auto& source_manager = ast_context.getSourceManager();
  auto char_source_range = clang::CharSourceRange::getTokenRange(source_range);
  assert(char_source_range.isTokenRange() && "Expected a token range.");
  (void)char_source_range;  // Keep release-mode compilers happy.
  const unsigned int final_token_length = clang::Lexer::MeasureTokenLength(
      source_range.getEnd(), source_manager, ast_context.getLangOpts());

  const unsigned int start_line =
      source_manager.getSpellingLineNumber(source_range.getBegin());
  const unsigned int start_column =
      source_manager.getSpellingColumnNumber(source_range.getBegin());
  const unsigned int end_line =
      source_manager.getSpellingLineNumber(source_range.getEnd());
  const unsigned int end_column =
      source_manager.getSpellingColumnNumber(source_range.getEnd()) +
      final_token_length;

  return start_line < end_line || start_column < end_column;
}

InfoForSourceRange::InfoForSourceRange(clang::SourceRange source_range,
                                       const clang::ASTContext& ast_context) {
  const auto& source_manager = ast_context.getSourceManager();
  auto char_source_range = clang::CharSourceRange::getTokenRange(source_range);
  assert(char_source_range.isTokenRange() && "Expected a token range.");
  (void)char_source_range;  // Keep release-mode compilers happy.
  const unsigned int final_token_length = clang::Lexer::MeasureTokenLength(
      source_range.getEnd(), source_manager, ast_context.getLangOpts());

  auto start_loc_decomposed =
      source_manager.getDecomposedLoc(source_range.getBegin());
  auto end_loc_decomposed =
      source_manager.getDecomposedLoc(source_range.getEnd());
  auto buffer_data = source_manager.getBufferData(start_loc_decomposed.first);

  start_line_ = source_manager.getSpellingLineNumber(source_range.getBegin());
  start_column_ =
      source_manager.getSpellingColumnNumber(source_range.getBegin());
  end_line_ = source_manager.getSpellingLineNumber(source_range.getEnd());
  end_column_ = source_manager.getSpellingColumnNumber(source_range.getEnd()) +
                final_token_length;

  assert((start_line_ < end_line_ || start_column_ < end_column_) &&
         "Bad source range.");

  const unsigned int length = end_loc_decomposed.second -
                              start_loc_decomposed.second + final_token_length;

  const std::string kSnipText(" ... [snip] ... ");
  const unsigned int kSnippetLengthEachSide = 10;
  const unsigned int kMinSnippedLength =
      static_cast<unsigned int>(kSnipText.size()) + 2 * kSnippetLengthEachSide;
  if (length <= kMinSnippedLength) {
    snippet_ = buffer_data.substr(start_loc_decomposed.second, length).str();
  } else {
    snippet_ =
        buffer_data.substr(start_loc_decomposed.second, kSnippetLengthEachSide)
            .str() +
        kSnipText +
        buffer_data
            .substr(
                start_loc_decomposed.second + length - kSnippetLengthEachSide,
                kSnippetLengthEachSide)
            .str();
  }
}

bool EvaluateAsBooleanCondition(const clang::Expr& expr,
                                const clang::ASTContext& ast_context,
                                bool& result) {
  return !expr.isValueDependent() &&
         expr.EvaluateAsBooleanCondition(result, ast_context);
}

bool EvaluateAsInt(const clang::Expr& expr,
                   const clang::ASTContext& ast_context,
                   clang::Expr::EvalResult& result) {
  return !expr.isValueDependent() && expr.EvaluateAsInt(result, ast_context);
}

bool EvaluateAsFloat(const clang::Expr& expr,
                     const clang::ASTContext& ast_context,
                     llvm::APFloat& result) {
  return !expr.isValueDependent() && expr.EvaluateAsFloat(result, ast_context);
}

bool IsCxx11ConstantExpr(const clang::Expr& expr,
                         const clang::ASTContext& ast_context) {
  return !expr.isValueDependent() && expr.isCXX11ConstantExpr(ast_context);
}

[[nodiscard]] clang::SourceRange GetSourceRangeInMainFile(
    const clang::Preprocessor& preprocessor,
    const clang::SourceRange& source_range) {
  // Try to get the beginning of the source range in th emain file.
  const clang::SourceLocation begin_loc_in_main_file =
      getSourceLocationInMainFileForExtremeOfRange(source_range.getBegin(),
                                                   preprocessor, true);
  if (begin_loc_in_main_file.isInvalid()) {
    // The beginning of the source range could not be traced to the main file,
    // so the entire range cannot.
    return {};
  }
  // Try to get the end of the source range in th emain file.
  const clang::SourceLocation end_loc_in_main_file =
      getSourceLocationInMainFileForExtremeOfRange(source_range.getEnd(),
                                                   preprocessor, false);
  if (end_loc_in_main_file.isInvalid()) {
    // The end of the source range could not be traced to the main file, so the
    // entire range cannot.
    return {};
  }
  return {begin_loc_in_main_file, end_loc_in_main_file};
}

namespace {

bool IsMicrosoftSuper(clang::NestedNameSpecifierLoc qualifier_loc) {
  if (!qualifier_loc) {
    return false;
  }
#if LLVM_VERSION_MAJOR >= 22
  return qualifier_loc.getNestedNameSpecifier().getKind() ==
         clang::NestedNameSpecifier::Kind::MicrosoftSuper;
#else
  return qualifier_loc.getNestedNameSpecifier()->getKind() ==
         clang::NestedNameSpecifier::Super;
#endif
}

// Finds each `__super` qualifier in an expression, and the class of the member
// that the expression names through it.
class SuperQualifierFinder
    : public clang::RecursiveASTVisitor<SuperQualifierFinder> {
 public:
  bool TraverseNestedNameSpecifierLoc(
      clang::NestedNameSpecifierLoc qualifier_loc) {
    if (IsMicrosoftSuper(qualifier_loc)) {
      super_locations_.push_back(qualifier_loc.getLocalBeginLoc());
    }
    return RecursiveASTVisitor::TraverseNestedNameSpecifierLoc(qualifier_loc);
  }

  bool VisitDeclRefExpr(clang::DeclRefExpr* decl_ref_expr) {
    RecordClassOfMember(decl_ref_expr->getQualifierLoc(),
                        *decl_ref_expr->getFoundDecl());
    return true;
  }

  bool VisitMemberExpr(clang::MemberExpr* member_expr) {
    RecordClassOfMember(member_expr->getQualifierLoc(),
                        *member_expr->getFoundDecl().getDecl());
    return true;
  }

  [[nodiscard]] const std::vector<clang::SourceLocation>& GetSuperLocations()
      const {
    return super_locations_;
  }

  // Returns the class for the `__super` at |super_location|, or nullptr if
  // that `__super` is not the whole qualifier of a member, as in
  // `__super::Inner::f()`, or if the member is not known, as in a template.
  [[nodiscard]] const clang::CXXRecordDecl* GetClass(
      clang::SourceLocation super_location) const {
    const auto found = classes_.find(super_location);
    return found == classes_.end() ? nullptr : found->second;
  }

 private:
  // The class to name is the class that holds the declaration that the lookup
  // found. For a member that a using-declaration brings into a class, that is
  // the class of the using-declaration, so that the access stays the same.
  void RecordClassOfMember(clang::NestedNameSpecifierLoc qualifier_loc,
                           const clang::NamedDecl& found_decl) {
    if (!IsMicrosoftSuper(qualifier_loc)) {
      return;
    }
    // The enumerators of an unscoped enumeration are members of the class
    // that holds the enumeration.
    if (const auto* record = llvm::dyn_cast<clang::CXXRecordDecl>(
            found_decl.getDeclContext()->getRedeclContext())) {
      classes_[qualifier_loc.getLocalBeginLoc()] = record;
    }
  }

  std::vector<clang::SourceLocation> super_locations_;
  std::map<clang::SourceLocation, const clang::CXXRecordDecl*> classes_;
};

// Returns the name of |record| from the global scope, with the template
// arguments of a specialization. Returns no value if a qualified name cannot
// name |record|: for a dependent class, a class in a function, or a class in a
// class without a name.
std::optional<std::string> GetNameFromGlobalScope(
    const clang::CXXRecordDecl& record, const clang::ASTContext& ast_context) {
  if (record.isDependentContext()) {
    return std::nullopt;
  }
  for (const clang::DeclContext* context = &record;
       !context->isTranslationUnit(); context = context->getParent()) {
    if (llvm::isa<clang::NamespaceDecl, clang::LinkageSpecDecl>(context)) {
      continue;
    }
    const auto* enclosing_record =
        llvm::dyn_cast<clang::CXXRecordDecl>(context);
    if (enclosing_record == nullptr ||
        enclosing_record->getIdentifier() == nullptr) {
      return std::nullopt;
    }
  }
  clang::PrintingPolicy printing_policy = ast_context.getPrintingPolicy();
  // A qualified name cannot name an unnamed namespace, but its lookup finds the
  // members of an unnamed namespace in the enclosing namespace.
  printing_policy.SuppressUnwrittenScope = true;
  std::string name;
  llvm::raw_string_ostream stream(name);
  record.getNameForDiagnostic(stream, printing_policy, /*Qualified=*/true);
  return "::" + stream.str();
}

}  // namespace

std::optional<std::vector<SuperQualifierReplacement>>
GetSuperQualifierReplacements(const clang::Expr& expr,
                              const clang::ASTContext& ast_context) {
  std::vector<SuperQualifierReplacement> replacements;
  if (!ast_context.getLangOpts().MicrosoftExt) {
    // Only the Microsoft extensions have the keyword `__super`.
    return replacements;
  }
  SuperQualifierFinder finder;
  // RecursiveASTVisitor takes a non-const statement, and has no const form in
  // every supported LLVM version. The finder only reads the expression.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
  finder.TraverseStmt(const_cast<clang::Expr*>(&expr));
  const clang::SourceManager& source_manager = ast_context.getSourceManager();
  for (const auto& super_location : finder.GetSuperLocations()) {
    if (!super_location.isFileID() ||
        !source_manager.isInMainFile(super_location)) {
      // A macro writes the keyword, so it cannot be replaced.
      return std::nullopt;
    }
    const clang::CXXRecordDecl* record = finder.GetClass(super_location);
    if (record == nullptr) {
      return std::nullopt;
    }
    std::optional<std::string> class_name =
        GetNameFromGlobalScope(*record, ast_context);
    if (!class_name.has_value()) {
      return std::nullopt;
    }
    replacements.push_back({.location = super_location,
                            .class_name = std::move(class_name.value())});
  }
  return replacements;
}

void ReplaceSuperQualifiers(
    const std::vector<SuperQualifierReplacement>& replacements,
    clang::Rewriter& rewriter) {
  // The text that other mutations insert before and after the keyword is not
  // part of the text to replace.
  clang::Rewriter::RewriteOptions without_insertions;
  without_insertions.IncludeInsertsAtBeginOfRange = false;
  without_insertions.IncludeInsertsAtEndOfRange = false;
  for (const auto& replacement : replacements) {
    const unsigned int keyword_length = clang::Lexer::MeasureTokenLength(
        replacement.location, rewriter.getSourceMgr(), rewriter.getLangOpts());
    // The text in place of the keyword is the keyword, or the class if the
    // lambda of an operand in this operand replaced the keyword already.
    const int current_length = rewriter.getRangeSize(
        clang::CharSourceRange::getCharRange(
            replacement.location,
            replacement.location.getLocWithOffset(
                static_cast<clang::SourceLocation::IntTy>(keyword_length))),
        without_insertions);
    assert(current_length >= 0 && "The keyword must be rewritable.");
    const bool rewriter_result = rewriter.ReplaceText(
        replacement.location, static_cast<unsigned int>(current_length),
        replacement.class_name);
    assert(!rewriter_result && "Rewrite failed.\n");
    (void)rewriter_result;  // Keep release-mode compilers happy.
  }
}

}  // namespace dredd
