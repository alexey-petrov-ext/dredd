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

#include <optional>
#include <set>
#include <string>

#include "libdreddtest/gtest.h"

namespace dredd {
namespace {

TEST(IgnoreCommentsTest, ParsesDisableOnceWithKindsActionsAndReason) {
  std::string error;
  const std::optional<IgnoreDirective> directive = ParseIgnoreDirective(
      "// Dredd disable once replaceExpr, ReplaceWithLHS: The value goes to a "
      "trace.",
      error);
  ASSERT_TRUE(directive.has_value());
  ASSERT_TRUE(error.empty());
  ASSERT_TRUE(directive->disable);
  ASSERT_TRUE(directive->once);
  ASSERT_FALSE(directive->all);
  const std::set<std::string> expected_names = {"replaceexpr",
                                                "replacewithlhs"};
  ASSERT_EQ(expected_names, directive->names);
  ASSERT_EQ("The value goes to a trace.", directive->reason);
}

TEST(IgnoreCommentsTest, ParsesDisableAndRestoreAll) {
  std::string error;
  const std::optional<IgnoreDirective> disable = ParseIgnoreDirective(
      "// Dredd disable all: No caller reads the trace.", error);
  ASSERT_TRUE(disable.has_value());
  ASSERT_TRUE(disable->disable);
  ASSERT_FALSE(disable->once);
  ASSERT_TRUE(disable->all);
  ASSERT_TRUE(disable->names.empty());
  ASSERT_EQ("No caller reads the trace.", disable->reason);

  const std::optional<IgnoreDirective> restore =
      ParseIgnoreDirective("// Dredd restore all", error);
  ASSERT_TRUE(restore.has_value());
  ASSERT_TRUE(error.empty());
  ASSERT_FALSE(restore->disable);
  ASSERT_TRUE(restore->all);
  ASSERT_TRUE(restore->reason.empty());
}

TEST(IgnoreCommentsTest, IgnoresCaseOfKeywordsAndNames) {
  std::string error;
  const std::optional<IgnoreDirective> directive =
      ParseIgnoreDirective("//dredd DISABLE ONCE REMOVESTMT: x", error);
  ASSERT_TRUE(directive.has_value());
  ASSERT_TRUE(directive->once);
  const std::set<std::string> expected_names = {"removestmt"};
  ASSERT_EQ(expected_names, directive->names);
}

TEST(IgnoreCommentsTest, ReasonIsOptional) {
  std::string error;
  const std::optional<IgnoreDirective> directive =
      ParseIgnoreDirective("// Dredd disable once all", error);
  ASSERT_TRUE(directive.has_value());
  ASSERT_TRUE(directive->reason.empty());
}

TEST(IgnoreCommentsTest, ReasonCanHoldAColon) {
  std::string error;
  const std::optional<IgnoreDirective> directive =
      ParseIgnoreDirective("// Dredd disable all: a: b", error);
  ASSERT_TRUE(directive.has_value());
  ASSERT_EQ("a: b", directive->reason);
}

TEST(IgnoreCommentsTest, RejectsUnknownName) {
  std::string error;
  ASSERT_FALSE(
      ParseIgnoreDirective("// Dredd disable once replaceExpression: x", error)
          .has_value());
  ASSERT_NE(std::string::npos, error.find("replaceExpression"));
}

TEST(IgnoreCommentsTest, RejectsDirectiveWithoutNames) {
  std::string error;
  ASSERT_FALSE(ParseIgnoreDirective("// Dredd disable: x", error).has_value());
  ASSERT_FALSE(error.empty());
}

TEST(IgnoreCommentsTest, RejectsAllTogetherWithOtherNames) {
  std::string error;
  ASSERT_FALSE(
      ParseIgnoreDirective("// Dredd disable all, removeStmt: x", error)
          .has_value());
  ASSERT_FALSE(error.empty());
}

TEST(IgnoreCommentsTest, RejectsOnceForRestore) {
  std::string error;
  ASSERT_FALSE(
      ParseIgnoreDirective("// Dredd restore once all", error).has_value());
  ASSERT_FALSE(error.empty());
}

TEST(IgnoreCommentsTest, IgnoresCommentsThatAreNotDirectives) {
  for (const auto* comment :
       {"// Dredd removes this statement.", "// see Dredd disable",
        "/* Dredd disable all: x */", "// Dredd disabled all: x",
        "// Dreddish disable all: x", "// disable once all: x"}) {
    std::string error;
    ASSERT_FALSE(ParseIgnoreDirective(comment, error).has_value()) << comment;
    ASSERT_TRUE(error.empty()) << comment;
  }
}

}  // namespace
}  // namespace dredd
