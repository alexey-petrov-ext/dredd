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

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "clang/AST/ASTContext.h"
#include "clang/AST/Expr.h"
#include "clang/AST/Stmt.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/ASTMatchers/ASTMatchers.h"
#include "clang/ASTMatchers/ASTMatchersInternal.h"
#include "clang/Basic/Diagnostic.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Frontend/ASTUnit.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/Rewrite/Core/RewriteBuffer.h"
#include "clang/Rewrite/Core/Rewriter.h"
#include "clang/Tooling/Tooling.h"
#include "libdreddtest/gtest.h"
#include "llvm/ADT/SmallVector.h"

namespace dredd {
namespace {

void TestRemoval(const std::string& original, const std::string& expected,
                 std::function<MutationRemoveStmt(const clang::Preprocessor&,
                                                  clang::ASTContext&)>
                     mutation_supplier) {
  auto ast_unit = clang::tooling::buildASTFromCodeWithArgs(original, {"-w"});
  ASSERT_FALSE(ast_unit->getDiagnostics().hasErrorOccurred());
  clang::Rewriter rewriter(ast_unit->getSourceManager(),
                           ast_unit->getLangOpts());
  int mutation_id = 0;
  std::unordered_set<std::string> dredd_declarations;
  mutation_supplier(ast_unit->getPreprocessor(), ast_unit->getASTContext())
      .Apply(ast_unit->getASTContext(), ast_unit->getPreprocessor(), Options(),
             0, mutation_id, rewriter, dredd_declarations);
  ASSERT_EQ(1, mutation_id);
  ASSERT_EQ(0, dredd_declarations.size());
  const clang::RewriteBuffer* rewrite_buffer = rewriter.getRewriteBufferFor(
      ast_unit->getSourceManager().getMainFileID());
  const std::string rewritten_text(rewrite_buffer->begin(),
                                   rewrite_buffer->end());
  ASSERT_EQ(expected, rewritten_text);
}

TEST(MutationRemoveStmtTest, BasicTest) {
  const std::string original = "void foo() { 1 + 2; }";
  const std::string expected =
      R"(void foo() { if (!__dredd_enabled_mutation(0)) { 1 + 2; } })";
  const std::function<MutationRemoveStmt(const clang::Preprocessor&,
                                         clang::ASTContext&)>
      mutation_supplier =
          [](const clang::Preprocessor& preprocessor,
             clang::ASTContext& ast_context) -> MutationRemoveStmt {
    auto statement = clang::ast_matchers::match(
        clang::ast_matchers::binaryOperator().bind("op"), ast_context);
    EXPECT_EQ(1, statement.size());
    return {*statement[0].getNodeAs<clang::BinaryOperator>("op"), preprocessor,
            ast_context};
  };
  TestRemoval(original, expected, mutation_supplier);
}

TEST(MutationRemoveStmtTest, RemoveIfStatement) {
  const std::string original = "void foo() { if (true) { } }";
  const std::string expected =
      R"(void foo() { if (!__dredd_enabled_mutation(0)) { if (true) { } } })";
  const std::function<MutationRemoveStmt(const clang::Preprocessor&,
                                         clang::ASTContext&)>
      mutation_supplier =
          [](const clang::Preprocessor& preprocessor,
             clang::ASTContext& ast_context) -> MutationRemoveStmt {
    auto statement = clang::ast_matchers::match(
        clang::ast_matchers::ifStmt().bind("if"), ast_context);
    EXPECT_EQ(1, statement.size());
    return {*statement[0].getNodeAs<clang::IfStmt>("if"), preprocessor,
            ast_context};
  };
  TestRemoval(original, expected, mutation_supplier);
}

TEST(MutationRemoveStmtTest, RemoveIfStatementWithTrailingSemi) {
  const std::string original = "void foo() { if (true) { }; }";
  const std::string expected =
      R"(void foo() { if (!__dredd_enabled_mutation(0)) { if (true) { }; } })";
  const std::function<MutationRemoveStmt(const clang::Preprocessor&,
                                         clang::ASTContext&)>
      mutation_supplier =
          [](const clang::Preprocessor& preprocessor,
             clang::ASTContext& ast_context) -> MutationRemoveStmt {
    auto statement = clang::ast_matchers::match(
        clang::ast_matchers::ifStmt().bind("if"), ast_context);
    EXPECT_EQ(1, statement.size());
    return {*statement[0].getNodeAs<clang::IfStmt>("if"), preprocessor,
            ast_context};
  };
  TestRemoval(original, expected, mutation_supplier);
}

TEST(MutationRemoveStmtTest, RemoveIfStatementWithTrailingSemis) {
  const std::string original = "void foo() { if (true) { };; }";
  const std::string expected =
      R"(void foo() { if (!__dredd_enabled_mutation(0)) { if (true) { }; }; })";
  const std::function<MutationRemoveStmt(const clang::Preprocessor&,
                                         clang::ASTContext&)>
      mutation_supplier =
          [](const clang::Preprocessor& preprocessor,
             clang::ASTContext& ast_context) -> MutationRemoveStmt {
    auto statement = clang::ast_matchers::match(
        clang::ast_matchers::ifStmt().bind("if"), ast_context);
    EXPECT_EQ(1, statement.size());
    return {*statement[0].getNodeAs<clang::IfStmt>("if"), preprocessor,
            ast_context};
  };
  TestRemoval(original, expected, mutation_supplier);
}

TEST(MutationRemoveStmtTest, RemoveIfStatementWithoutBraces) {
  const std::string original = "void foo() { if (true) return; }";
  const std::string expected =
      R"(void foo() { if (!__dredd_enabled_mutation(0)) { if (true) return; } })";
  const std::function<MutationRemoveStmt(const clang::Preprocessor&,
                                         clang::ASTContext&)>
      mutation_supplier =
          [](const clang::Preprocessor& preprocessor,
             clang::ASTContext& ast_context) -> MutationRemoveStmt {
    auto statement = clang::ast_matchers::match(
        clang::ast_matchers::ifStmt().bind("if"), ast_context);
    EXPECT_EQ(1, statement.size());
    return {*statement[0].getNodeAs<clang::IfStmt>("if"), preprocessor,
            ast_context};
  };
  TestRemoval(original, expected, mutation_supplier);
}

TEST(MutationRemoveStmtTest, RemoveReturnStmt) {
  const std::string original = "void foo() { return; }";
  const std::string expected =
      R"(void foo() { if (!__dredd_enabled_mutation(0)) { return; } })";
  const std::function<MutationRemoveStmt(const clang::Preprocessor&,
                                         clang::ASTContext&)>
      mutation_supplier =
          [](const clang::Preprocessor& preprocessor,
             clang::ASTContext& ast_context) -> MutationRemoveStmt {
    auto statement = clang::ast_matchers::match(
        clang::ast_matchers::returnStmt().bind("return"), ast_context);
    EXPECT_EQ(1, statement.size());
    return {*statement[0].getNodeAs<clang::ReturnStmt>("return"), preprocessor,
            ast_context};
  };
  TestRemoval(original, expected, mutation_supplier);
}

TEST(MutationRemoveStmtTest, RemoveBreakStmt) {
  const std::string original = "void foo() { while (true) { break; } }";
  const std::string expected =
      R"(void foo() { while (true) { if (!__dredd_enabled_mutation(0)) { break; } } })";
  const std::function<MutationRemoveStmt(const clang::Preprocessor&,
                                         clang::ASTContext&)>
      mutation_supplier =
          [](const clang::Preprocessor& preprocessor,
             clang::ASTContext& ast_context) -> MutationRemoveStmt {
    auto statement = clang::ast_matchers::match(
        clang::ast_matchers::breakStmt().bind("break"), ast_context);
    EXPECT_EQ(1, statement.size());
    return {*statement[0].getNodeAs<clang::BreakStmt>("break"), preprocessor,
            ast_context};
  };
  TestRemoval(original, expected, mutation_supplier);
}

TEST(MutationRemoveStmtTest, RemoveMacroStmt) {
  const std::string original =
      "#define ASSIGN(A, B) A = B\n"
      "void foo() { int x; ASSIGN(x, 1); }";
  const std::string expected =
      R"(#define ASSIGN(A, B) A = B
void foo() { int x; if (!__dredd_enabled_mutation(0)) { ASSIGN(x, 1); } })";
  const std::function<MutationRemoveStmt(const clang::Preprocessor&,
                                         clang::ASTContext&)>
      mutation_supplier =
          [](const clang::Preprocessor& preprocessor,
             clang::ASTContext& ast_context) -> MutationRemoveStmt {
    auto statement = clang::ast_matchers::match(
        clang::ast_matchers::binaryOperator().bind("assign"), ast_context);
    EXPECT_EQ(1, statement.size());
    return {*statement[0].getNodeAs<clang::BinaryOperator>("assign"),
            preprocessor, ast_context};
  };
  TestRemoval(original, expected, mutation_supplier);
}

// Applies to one rewriter the removal of the statement that |first_matcher|
// matches, and then the removal of the statement that |second_matcher| matches,
// as Dredd applies the removals of two adjacent statements of a block.
void TestAdjacentRemovals(
    const std::string& original, const std::string& expected,
    const clang::ast_matchers::StatementMatcher& first_matcher,
    const clang::ast_matchers::StatementMatcher& second_matcher) {
  auto ast_unit = clang::tooling::buildASTFromCodeWithArgs(original, {"-w"});
  ASSERT_FALSE(ast_unit->getDiagnostics().hasErrorOccurred());
  clang::Rewriter rewriter(ast_unit->getSourceManager(),
                           ast_unit->getLangOpts());
  int mutation_id = 0;
  std::unordered_set<std::string> dredd_declarations;
  const auto apply_removal =
      [&](const clang::ast_matchers::StatementMatcher& matcher) -> void {
    auto statement = clang::ast_matchers::match(
        clang::ast_matchers::stmt(matcher).bind("stmt"),
        ast_unit->getASTContext());
    ASSERT_EQ(1, statement.size());
    MutationRemoveStmt(*statement[0].getNodeAs<clang::Stmt>("stmt"),
                       ast_unit->getPreprocessor(), ast_unit->getASTContext())
        .Apply(ast_unit->getASTContext(), ast_unit->getPreprocessor(),
               Options(), 0, mutation_id, rewriter, dredd_declarations);
  };
  apply_removal(first_matcher);
  apply_removal(second_matcher);
  ASSERT_EQ(2, mutation_id);
  const auto* rewrite_buffer = rewriter.getRewriteBufferFor(
      ast_unit->getSourceManager().getMainFileID());
  const std::string rewritten_text(rewrite_buffer->begin(),
                                   rewrite_buffer->end());
  ASSERT_EQ(expected, rewritten_text);
}

TEST(MutationRemoveStmtTest, RemoveStatementAfterSemi) {
  const std::string original = "void foo() { int x; x = 1;x = 2; }";
  const std::string expected =
      R"(void foo() { int x; if (!__dredd_enabled_mutation(0)) { x = 1; }if (!__dredd_enabled_mutation(1)) { x = 2; } })";
  TestAdjacentRemovals(
      original, expected,
      clang::ast_matchers::binaryOperator(clang::ast_matchers::hasRHS(
          clang::ast_matchers::integerLiteral(clang::ast_matchers::equals(1)))),
      clang::ast_matchers::binaryOperator(
          clang::ast_matchers::hasRHS(clang::ast_matchers::integerLiteral(
              clang::ast_matchers::equals(2)))));
}

TEST(MutationRemoveStmtTest, RemoveStatementAfterElseWithoutBraces) {
  const std::string original =
      "void foo(int x) { if (x) x = 1; else x = 2;x = 3; }";
  const std::string expected =
      R"(void foo(int x) { if (!__dredd_enabled_mutation(0)) { if (x) x = 1; else x = 2; }if (!__dredd_enabled_mutation(1)) { x = 3; } })";
  TestAdjacentRemovals(
      original, expected, clang::ast_matchers::ifStmt(),
      clang::ast_matchers::binaryOperator(
          clang::ast_matchers::hasRHS(clang::ast_matchers::integerLiteral(
              clang::ast_matchers::equals(3)))));
}

TEST(MutationRemoveStmtTest, RemoveStatementAfterBrace) {
  const std::string original = "void foo() { if (true) { }return; }";
  const std::string expected =
      R"(void foo() { if (!__dredd_enabled_mutation(0)) { if (true) { } }if (!__dredd_enabled_mutation(1)) { return; } })";
  TestAdjacentRemovals(original, expected, clang::ast_matchers::ifStmt(),
                       clang::ast_matchers::returnStmt());
}

TEST(MutationRemoveStmtTest, RemoveStatementAfterComment) {
  const std::string original = "void foo() { if (true) { }/* c */return; }";
  const std::string expected =
      "void foo() { if (!__dredd_enabled_mutation(0)) { if (true) { }/* c */\n"
      " }if (!__dredd_enabled_mutation(1)) { return; } }";
  TestAdjacentRemovals(original, expected, clang::ast_matchers::ifStmt(),
                       clang::ast_matchers::returnStmt());
}

TEST(MutationRemoveStmtTest, RemoveStatementEndingInBraceBeforeDirective) {
  // The statement ends with the '}' of its own expression, and a directive
  // comes before its semicolon. A mutation of that expression inserts its
  // closing text after the '}' before the removal is applied; the closing text
  // of the removal must come after it.
  const std::string original =
      "void foo(int& x) {\n  x = int{2}\n#if 1\n  ;\n#endif\n}";
  const std::string expected =
      "void foo(int& x) {\n"
      "  if (!__dredd_enabled_mutation(0)) { x = int{2}/* e */;  }\n"
      "#if 1\n  ;\n#endif\n}";
  auto ast_unit = clang::tooling::buildASTFromCodeWithArgs(original, {"-w"});
  ASSERT_FALSE(ast_unit->getDiagnostics().hasErrorOccurred());
  clang::Rewriter rewriter(ast_unit->getSourceManager(),
                           ast_unit->getLangOpts());
  auto statement = clang::ast_matchers::match(
      clang::ast_matchers::binaryOperator().bind("assign"),
      ast_unit->getASTContext());
  ASSERT_EQ(1, statement.size());
  const auto* assign = statement[0].getNodeAs<clang::BinaryOperator>("assign");
  ASSERT_FALSE(
      rewriter.InsertTextAfterToken(assign->getRHS()->getEndLoc(), "/* e */"));
  int mutation_id = 0;
  std::unordered_set<std::string> dredd_declarations;
  MutationRemoveStmt(*assign, ast_unit->getPreprocessor(),
                     ast_unit->getASTContext())
      .Apply(ast_unit->getASTContext(), ast_unit->getPreprocessor(), Options(),
             0, mutation_id, rewriter, dredd_declarations);
  ASSERT_EQ(1, mutation_id);
  const auto* rewrite_buffer = rewriter.getRewriteBufferFor(
      ast_unit->getSourceManager().getMainFileID());
  const std::string rewritten_text(rewrite_buffer->begin(),
                                   rewrite_buffer->end());
  ASSERT_EQ(expected, rewritten_text);
}

}  // namespace
}  // namespace dredd
