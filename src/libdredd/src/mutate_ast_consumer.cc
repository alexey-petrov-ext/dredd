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

#include "libdredd/mutate_ast_consumer.h"

#include <cassert>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/Expr.h"
#include "clang/AST/Type.h"
#include "clang/AST/TypeLoc.h"
#include "clang/Basic/Diagnostic.h"
#include "clang/Basic/FileEntry.h"
#include "clang/Basic/LangOptions.h"
#include "clang/Basic/SourceLocation.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Rewrite/Core/Rewriter.h"
#include "libdredd/mutation.h"
#include "libdredd/util.h"
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/APSInt.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/raw_ostream.h"

namespace dredd {

namespace {
const char* const kDreddPreludeStartComment =
    "// DREDD PRELUDE START\n"
    "// If this has been inserted at an inappropriate place in a source file,\n"
    "// declare a placeholder function with the following signature to\n"
    "// mandate where the prelude should be placed:\n"
    "//\n"
    "// void __dredd_prelude_start();\n"
    "//\n";

const char* const kDreddPreludeHeadersCpp =
    "#include <atomic>\n"
    "#include <cinttypes>\n"
    "#include <cstddef>\n"
    "#include <cstdlib>\n"
    "#include <fstream>\n"
    "#include <sstream>\n\n";

// The mutant tracking prelude, like the regular prelude, calls no function of
// the C++ standard library, so that a program that links without that library,
// or with another variant of it, still links when it is mutated. It therefore
// writes the tracking file with the C library.
const char* const kDreddPreludeMutantTrackingHeadersCpp =
    "#include <cinttypes>\n"
    "#include <cstddef>\n"
    "#include <cstdio>\n"
    "#include <cstdlib>\n\n";

// The C and C++ mutant tracking preludes synchronize the threads of a program
// with this code. The atomic exchange is a compiler intrinsic. The relaxed
// atomic load is a volatile read with MSVC, and a compiler intrinsic elsewhere.
// Neither calls a library function. With MSVC, std::atomic and the C11 atomic
// operations both check the memory order through the C runtime, and in a debug
// build through the debug C runtime.
//
// A lock serializes the appends to the tracking file, because two threads that
// append at the same time can overwrite each other's lines: the C runtime of
// Windows emulates append mode by moving to the end of the file, then writing.
// The lock is one variable for the whole program, so that all mutated files
// use it, in C and in C++ alike. It has C linkage, so that C and C++
// translation units name the same variable. Each translation unit defines it,
// and the linker keeps one of the definitions: they are selectany, a COMDAT, in
// the object files of Windows, and weak symbols elsewhere.
//
// The tracking file is open once for the whole program, in a variable of the
// same kind that the lock guards. A program that reaches thousands of mutation
// groups used to open and close the file for each group. On Windows, the virus
// scanner can examine the file at each close, and the appends then took many
// times longer than the program itself. Each group flushes its lines, so that
// the operating system holds them when the program crashes. The C runtime
// closes the file when the program ends. The variable is a void pointer, so
// that C and C++ translation units declare it without a header of their own.
const char* const kDreddPreludeMutantTrackingLock =
    "#ifdef _MSC_VER\n"
    "#include <intrin.h>\n"
    "#endif\n"
    "\n"
    "static char __dredd_atomic_exchange(volatile char* target, char value) {\n"
    "#ifdef _MSC_VER\n"
    "  return _InterlockedExchange8(target, value);\n"
    "#else\n"
    "  return __atomic_exchange_n(target, value, __ATOMIC_SEQ_CST);\n"
    "#endif\n"
    "}\n"
    "\n"
    "static char __dredd_atomic_load(volatile char* target) {\n"
    "#ifdef _MSC_VER\n"
    "  return *target;\n"
    "#else\n"
    "  return __atomic_load_n(target, __ATOMIC_RELAXED);\n"
    "#endif\n"
    "}\n"
    "\n"
    "#ifdef __cplusplus\n"
    "extern \"C\" {\n"
    "#endif\n"
    "#ifdef _WIN32\n"
    "__declspec(selectany)\n"
    "#else\n"
    "__attribute__((weak))\n"
    "#endif\n"
    "char __dredd_tracking_file_lock = 0;\n"
    "#ifdef _WIN32\n"
    "__declspec(selectany)\n"
    "#else\n"
    "__attribute__((weak))\n"
    "#endif\n"
    "void* __dredd_tracking_file = 0;\n"
    "#ifdef __cplusplus\n"
    "}\n"
    "#endif\n"
    "\n";

const char* const kDreddPreludeThreadLocalCpp =
    "#ifdef _MSC_VER\n"
    "#define __dredd_thread_local __declspec(thread)\n"
    "#elif __APPLE__\n"
    "#define __dredd_thread_local __thread\n"
    "#else\n"
    "#define __dredd_thread_local thread_local\n"
    "#endif\n"
    "\n";

// A mutator function takes an operand that must be evaluated lazily, such as
// the right operand of && or an operand with side effects, as a lambda. This
// type refers to the lambda without owning it. That is safe, because the lambda
// is a temporary of the full-expression that calls the mutator function. Unlike
// std::function, it needs no function of the C++ standard library.
const char* const kDreddPreludeLazyCpp =
    "namespace {\n"
    "template <typename Result>\n"
    "class __dredd_lazy {\n"
    " public:\n"
    "  template <typename Callable>\n"
    "  __dredd_lazy(const Callable& callable)\n"
    "      : callable_(&callable),\n"
    "        invoke_([](const void* target) -> Result {\n"
    "          return (*static_cast<const Callable*>(target))();\n"
    "        }) {}\n"
    "  Result operator()() const { return invoke_(callable_); }\n"
    "\n"
    " private:\n"
    "  const void* callable_;\n"
    "  Result (*invoke_)(const void*);\n"
    "};\n"
    "}  // namespace\n\n";

const char* const kDreddPreludeHeadersC =
    "#include <inttypes.h>\n"
    "#include <stdatomic.h>\n"
    "#include <stdbool.h>\n"
    "#include <stdio.h>\n"
    "#include <stdlib.h>\n"
    "#include <string.h>\n";
}  // namespace

void MutateAstConsumer::HandleTranslationUnit(clang::ASTContext& ast_context) {
  const std::string filename =
      ast_context.getSourceManager()
          .getFileEntryRefForID(ast_context.getSourceManager().getMainFileID())
          ->getName()
          .str();

  llvm::errs() << "Processing " << filename << "\n";

  if (ast_context.getDiagnostics().hasErrorOccurred()) {
    llvm::errs() << "Skipping due to errors\n";
    return;
  }

  // An invalid "Dredd disable" directive is an error of the file, so the file
  // is not mutated.
  if (!ignore_scopes_.Collect(ast_context)) {
    llvm::errs() << "Skipping due to errors\n";
    return;
  }

  if (options_->GetDumpAsts()) {
    llvm::errs() << "AST:\n";
    ast_context.getTranslationUnitDecl()->dump();
    llvm::errs() << "\n";
  }
  visitor_->TraverseDecl(ast_context.getTranslationUnitDecl());

  rewriter_.setSourceMgr(compiler_instance_->getSourceManager(),
                         compiler_instance_->getLangOpts());

  // Recording this makes it possible to keep track of how many mutations have
  // been applied to an individual source file, which allows mutations within
  // that source file to be tracked using a file-local mutation id that starts
  // from zero. Adding this value to the file-local id gives the global mutation
  // id.
  const int initial_mutation_id = *mutation_id_;

  // This is used to collect the various declarations that are introduced by
  // mutations in a manner that avoids duplicates, after which they can be added
  // to the start of the source file. As lots of duplicates are expected, an
  // unordered set is used to facilitate efficient lookup. Later, this is
  // converted to an ordered set so that declarations can be added to the source
  // file in a deterministic order.
  std::unordered_set<std::string> dredd_declarations;

  protobufs::MutationInfoForFile mutation_info_for_file;

  protobufs::MutationTreeNode* root_protobuf_mutation_tree_node =
      mutation_info_for_file.add_mutation_tree();
  ApplyMutations(visitor_->GetMutations(), initial_mutation_id, ast_context,
                 mutation_info_for_file, *root_protobuf_mutation_tree_node,
                 dredd_declarations, mutation_info_->has_value());

  if (initial_mutation_id == *mutation_id_) {
    // No possibilities for mutation were found; nothing else to do.
    return;
  }

  RewriteExpressionsInMainFile();

  if (mutation_info_->has_value()) {
    mutation_info_for_file.set_filename(
        ast_context.getSourceManager()
            .getFileEntryRefForID(
                ast_context.getSourceManager().getMainFileID())
            ->getName()
            .str());
    *mutation_info_->value().add_info_for_files() = mutation_info_for_file;
  }

  auto& source_manager = ast_context.getSourceManager();
  clang::SourceLocation start_of_source_file =
      source_manager.translateLineCol(source_manager.getMainFileID(), 1, 1);
  // Keep a UTF-8 byte order mark at the start of the file: the prelude goes
  // after it, otherwise compilers such as MSVC no longer detect the encoding.
  if (source_manager.getBufferData(source_manager.getMainFileID())
          .starts_with("\xEF\xBB\xBF")) {
    start_of_source_file = start_of_source_file.getLocWithOffset(3);
  }
  const clang::SourceLocation dredd_prelude_start_location =
      visitor_->HasDreddPreludeStartLocation()
          ? visitor_->GetDreddPreludeStartLocation()
          : start_of_source_file;
  assert(dredd_prelude_start_location.isValid() &&
         "There is at least one mutation, therefore there must be at least one "
         "function.");

  // Convert the unordered set Dredd declarations into an ordered set and add
  // them to the source file before the first declaration.
  std::set<std::string> sorted_dredd_declarations;
  sorted_dredd_declarations.insert(dredd_declarations.begin(),
                                   dredd_declarations.end());
  for (const auto& decl : sorted_dredd_declarations) {
    const bool rewriter_result =
        rewriter_.InsertTextBefore(dredd_prelude_start_location, decl);
    (void)rewriter_result;  // Keep release-mode compilers happy.
    assert(!rewriter_result && "Rewrite failed.\n");
  }

  const std::string dredd_prelude =
      compiler_instance_->getLangOpts().CPlusPlus
          ? GetDreddPreludeCpp(initial_mutation_id)
          : GetDreddPreludeC(initial_mutation_id);

  bool rewriter_result =
      rewriter_.InsertTextBefore(dredd_prelude_start_location, dredd_prelude);
  (void)rewriter_result;  // Keep release-mode compilers happy.
  assert(!rewriter_result && "Rewrite failed.\n");

  rewriter_result = rewriter_.overwriteChangedFiles();
  (void)rewriter_result;  // Keep release mode compilers happy
  assert(!rewriter_result && "Something went wrong emitting rewritten files.");
}

void MutateAstConsumer::RewriteExpressionsInMainFile() {
  // Rewrite the size expressions of constant-sized arrays as needed.
  for (const auto& constant_sized_array_decl :
       visitor_->GetConstantSizedArraysToRewrite()) {
    assert(compiler_instance_->getLangOpts().CPlusPlus &&
           constant_sized_array_decl->getType()->isConstantArrayType());
    auto typeloc = constant_sized_array_decl->getTypeSourceInfo()->getTypeLoc();

    // Note: in some cases a declaration with constant array type does not have
    // an associated ConstantArrayTypeLoc object. This happens, for example,
    // when structured bindings are used. For example, consider:
    //
    //     auto [x, y] = a;
    //
    // This yields a constant-sized array, but there is no explicit array type
    // declaration. In such cases, no action is required.
    //
    // We use a loop to descend into nests of arrays.
    while (
        auto constant_array_typeloc =
            typeloc.getUnqualifiedLoc().getAs<clang::ConstantArrayTypeLoc>()) {
      RewriteExpressionInMainFileToIntegerConstant(
          constant_array_typeloc.getSizeExpr(),
          llvm::dyn_cast<clang::ConstantArrayType>(
              constant_array_typeloc.getType()
                  ->getAsArrayTypeUnsafe()
                  ->getAsArrayTypeUnsafe())
              ->getSize()
              .getLimitedValue());
      typeloc = constant_array_typeloc.getElementLoc();
    }
  }

  // Rewrite the argument of static assertion as needed to `1`.
  // There's no need to evaluate the actual expression, as any value other than
  // 1 would have caused the front-end used by Dredd to fail.
  for (const auto& static_assert_decl :
       visitor_->GetStaticAssertionsToRewrite()) {
    RewriteExpressionInMainFileToIntegerConstant(
        static_assert_decl->getAssertExpr(), 1);
  }

  // Rewrite the constant integer arguments of builtin functions and templates.
  std::unordered_set<const clang::Expr*> already_rewritten;
  for (const auto* constant_argument_expresion :
       visitor_->GetConstantArgumentsToRewrite()) {
    if (already_rewritten.contains(constant_argument_expresion)) {
      continue;
    }
    already_rewritten.insert(constant_argument_expresion);
    if (const auto integer_const_expr =
            constant_argument_expresion->getIntegerConstantExpr(
                compiler_instance_->getASTContext())) {
      // Argument is an integer constant
      RewriteExpressionInMainFileToIntegerConstant(
          constant_argument_expresion, integer_const_expr->getLimitedValue());
    }
  }
}

bool MutateAstConsumer::RewriteExpressionInMainFileToIntegerConstant(
    const clang::Expr* expr, uint64_t integer_constant) {
  auto source_range_in_main_file =
      GetSourceRangeInMainFile(compiler_instance_->getPreprocessor(), *expr);

  // We only consider the rewriting if the source range for the size
  // expression is in the main source file.
  if (source_range_in_main_file.isValid()) {
    std::stringstream stringstream;
    stringstream << integer_constant;
    rewriter_.ReplaceText(source_range_in_main_file, stringstream.str());
    return true;
  }
  return false;
}

std::string MutateAstConsumer::GetRegularDreddPreludeCpp(
    int initial_mutation_id) const {
  // The number of mutations applied to this file is known.
  const int num_mutations = *mutation_id_ - initial_mutation_id;

  // Whether mutants are enabled or not will be tracked using a bitset,
  // represented as an array of 64-bit integers. First, work out how large this
  // array will need to be, as ceiling(num_mutations / 64).
  const int kWordSize = 64;
  const int num_64_bit_words_required =
      (num_mutations + kWordSize - 1) / kWordSize;

  std::stringstream result;
  result << kDreddPreludeHeadersCpp;
  result << "\n";
  result << kDreddPreludeLazyCpp;

  if (options_->GetEnablednessCheckingMode() ==
      Options::EnablednessCheckingMode::ALWAYS_READ_ENVIRONMENT_VARIABLE) {
    result << "static bool __dredd_enabled_mutation(int local_mutation_id) {\n";
    result << "  const char* dredd_environment_variable = "
              "std::getenv(\"DREDD_ENABLED_MUTATION\");\n";
    result << "  if (dredd_environment_variable != nullptr) {\n";
    result << "    const char* token = dredd_environment_variable;\n";
    result << "    while (*token != '\\0') {\n";
    result << "      if (*token == ',') {\n";
    result << "        token++;\n";
    result << "        continue;\n";
    result << "      }\n";
    result << "      char* end = nullptr;\n";
    result << "      long value = std::strtol(token, &end, 10);\n";
    result << "      if (end == token) {\n";
    result << "        std::abort();\n";
    result << "      }\n";
    result << "      if (value == local_mutation_id + " << initial_mutation_id
           << ") {\n";
    result << "        return true;\n";
    result << "      }\n";
    result << "      token = end;\n";
    result << "      while (*token != ',' && *token != '\\0') {\n";
    result << "        token++;\n";
    result << "      }\n";
    result << "    }\n";
    result << "  }\n";
    result << "  return false;\n";
    result << "}\n";
  } else {
    result << kDreddPreludeThreadLocalCpp;
    if (options_->GetEnablednessCheckingMode() ==
        Options::EnablednessCheckingMode::STANDARD) {
      // This allows for fast checking that at least *some* mutation in the file
      // is enabled. It is set to true initially so that
      // __dredd_enabled_mutation gets invoked the first time enabledness is
      // queried. At that point it will get set to false if no mutations are
      // actually enabled.
      result << "static __dredd_thread_local bool "
                "__dredd_some_mutation_enabled = true;\n";
    }
    result << "static bool __dredd_enabled_mutation(int local_mutation_id) {\n";
    result << "  static __dredd_thread_local bool initialized = false;\n";
    // Array of booleans, one per mutation in this file, determining whether
    // they are enabled.
    result << "  static __dredd_thread_local uint64_t enabled_bitset["
           << num_64_bit_words_required << "];\n";
    result << "  if (!initialized) {\n";
    // Record locally whether some mutation is enabled.
    if (options_->GetEnablednessCheckingMode() ==
        Options::EnablednessCheckingMode::STANDARD) {
      result << "    bool some_mutation_enabled = false;\n";
    }
    result << "    const char* dredd_environment_variable = "
              "std::getenv(\"DREDD_ENABLED_MUTATION\");\n";
    result << "    if (dredd_environment_variable != nullptr) {\n";
    // The environment variable for mutations is set, so process the contents of
    // this environment variable as a comma-separated list of integers. The
    // parsing uses only the C library, so that the mutated code does not need
    // the C++ standard library.
    result << "      const char* token = dredd_environment_variable;\n";
    result << "      while (*token != '\\0') {\n";
    // Skip an empty token: this allows for a trailing comma at the end of the
    // string.
    result << "        if (*token == ',') {\n";
    result << "          token++;\n";
    result << "          continue;\n";
    result << "        }\n";
    // Parse the token as an integer. The program stops if the token does not
    // start with a number, which is OK: it is expected that the user has set
    // the environment variable to a legitimate value.
    result << "        char* end = nullptr;\n";
    result << "        long value = std::strtol(token, &end, 10);\n";
    result << "        if (end == token) {\n";
    result << "          std::abort();\n";
    result << "        }\n";
    // Check whether the mutant id actually corresponds to a mutant in this
    // file; skip it if it does not.
    result << "        if (value >= " << initial_mutation_id << " && value < "
           << (initial_mutation_id + num_mutations) << ") {\n";
    result << "          int local_value = static_cast<int>(value - "
           << initial_mutation_id << ");\n";
    // `local_value / 64` gives the element in the bitset array corresponding to
    // this mutant. Then `local_value % 64` determines which bit of that element
    // needs to be set in order to enable the mutant, and a bitwise operation is
    // used to set the correct bit.
    result << "          enabled_bitset[local_value / 64] |= "
              "(static_cast<uint64_t>(1) << "
              "(local_value % 64));\n";
    if (options_->GetEnablednessCheckingMode() ==
        Options::EnablednessCheckingMode::STANDARD) {
      // Note that at least one enabled mutation has been encountered.
      result << "          some_mutation_enabled = true;\n";
    }
    result << "        }\n";
    // Skip the rest of the token, so that the next iteration starts at the
    // next comma or at the end of the string.
    result << "        token = end;\n";
    result << "        while (*token != ',' && *token != '\\0') {\n";
    result << "          token++;\n";
    result << "        }\n";
    result << "      }\n";
    result << "    }\n";
    // Initialisation is now complete, and whether at least one mutation is
    // enabled is known.
    result << "    initialized = true;\n";
    if (options_->GetEnablednessCheckingMode() ==
        Options::EnablednessCheckingMode::STANDARD) {
      result << "    __dredd_some_mutation_enabled = some_mutation_enabled;\n";
    }
    result << "  }\n";
    // Similar to the above, a combination of division, modulo and bit-shifting
    // is used to look up whether this mutant is enabled in the bitset.
    result << "  return (enabled_bitset[local_mutation_id / 64] & "
              "(static_cast<uint64_t>(1) << "
              "(local_mutation_id % 64))) != 0;\n";
    result << "}\n\n";
  }
  return result.str();
}

std::string MutateAstConsumer::GetMutantTrackingDreddPreludeCpp(
    int initial_mutation_id) const {
  // The number of mutations applied to this file is known.
  const int num_mutations = *mutation_id_ - initial_mutation_id;

  std::stringstream result;
  result << kDreddPreludeMutantTrackingHeadersCpp;
  result << "\n";
  result << kDreddPreludeLazyCpp;
  result << kDreddPreludeMutantTrackingLock;
  result << "static void __dredd_record_covered_mutants(int local_mutation_id, "
            "int num_mutations) {\n";
  // Record the mutants of a mutation group once for each process: the thread
  // that sets the flag of the group first appends the lines of the group, and
  // no other thread does, so the file names each mutant that a run reaches
  // once. The flag is read before the exchange, because nearly every call
  // finds it set: an exchange writes the cache line of the flags, and threads
  // that run the same code would move that line between their cores on every
  // call. A stale zero only costs one more exchange.
  result << "  static char already_recorded[" << num_mutations << "];\n";
  result << "  if (__dredd_atomic_load(&already_recorded[local_mutation_id])) "
            "return;\n";
  result << "  if (__dredd_atomic_exchange("
            "&already_recorded[local_mutation_id], 1)) return;\n";
  result << "  const char* dredd_tracking_environment_variable = "
            "std::getenv(\"DREDD_MUTANT_TRACKING_FILE\");\n";
  result << "  if (dredd_tracking_environment_variable == nullptr) return;\n";
  // Append to the file with the C library, so that the mutated code does not
  // need the C++ standard library. The lock is held while the file is opened
  // and written. A thread that waits for the lock reads it until it is free,
  // and only then tries the exchange again. The first group opens the file,
  // and the later groups write to it too. If the file cannot be opened, the
  // mutants of the group are not recorded, and the next group tries again.
  result << "  while (__dredd_atomic_exchange(&__dredd_tracking_file_lock, 1)) "
            "{\n";
  result << "    while (__dredd_atomic_load(&__dredd_tracking_file_lock)) {\n";
  result << "    }\n";
  result << "  }\n";
  result << "  std::FILE* output_file = "
            "static_cast<std::FILE*>(__dredd_tracking_file);\n";
  result << "  if (output_file == nullptr) {\n";
  result << "    output_file = "
            "std::fopen(dredd_tracking_environment_variable, \"a\");\n";
  result << "    __dredd_tracking_file = output_file;\n";
  result << "  }\n";
  result << "  if (output_file != nullptr) {\n";
  result << "    for (int i = 0; i < num_mutations; i++) {\n";
  result << R"(      std::fprintf(output_file, "%d\n", )" << initial_mutation_id
         << " + local_mutation_id + i);\n";
  result << "    }\n";
  result << "    std::fflush(output_file);\n";
  result << "  }\n";
  result << "  __dredd_atomic_exchange(&__dredd_tracking_file_lock, 0);\n";
  result << "}\n\n";
  return result.str();
}

std::string MutateAstConsumer::GetDreddPreludeCpp(
    int initial_mutation_id) const {
  return kDreddPreludeStartComment +
         (options_->GetOnlyTrackMutantCoverage()
              ? GetMutantTrackingDreddPreludeCpp(initial_mutation_id)
              : GetRegularDreddPreludeCpp(initial_mutation_id));
}

std::string MutateAstConsumer::GetRegularDreddPreludeC(
    int initial_mutation_id) const {
  // See comments in GetRegularDreddPreludeCpp - this C version is a
  // straightforward port.
  const int num_mutations = *mutation_id_ - initial_mutation_id;
  const int kWordSize = 64;
  const int num_64_bit_words_required =
      (num_mutations + kWordSize - 1) / kWordSize;

  std::stringstream result;
  result << kDreddPreludeHeadersC;
  result << "\n";

  if (options_->GetEnablednessCheckingMode() ==
      Options::EnablednessCheckingMode::ALWAYS_READ_ENVIRONMENT_VARIABLE) {
    result << "static bool __dredd_enabled_mutation(int local_mutation_id) {\n";
    result << "  const char* dredd_environment_variable = "
              "getenv(\"DREDD_ENABLED_MUTATION\");\n";
    result << "  if (dredd_environment_variable) {\n";
    result
        << "    char* temp = malloc(strlen(dredd_environment_variable) + 1);\n";
    result << "    strcpy(temp, dredd_environment_variable);\n";
    result << "    char* token;\n";
    result << "    token = strtok(temp, \",\");\n";
    result << "    while(token) {\n";
    result << "      int value = atoi(token);\n";
    result << "      if (value - " << initial_mutation_id
           << " == local_mutation_id) {\n";
    result << "        free(temp);\n";
    result << "        return true;\n";
    result << "      }\n";
    result << "    token = strtok(NULL, \",\");\n";
    result << "  }\n";
    result << "  free(temp);\n";
    result << "  }\n";
    result << "  return false;\n";
    result << "}\n";
  } else {
    result << "#ifdef _MSC_VER\n";
    result << "#define __dredd_thread_local __declspec(thread)\n";
    result << "#elif __APPLE__\n";
    result << "#define __dredd_thread_local __thread\n";
    result << "#else\n";
    result << "#define __dredd_thread_local _Thread_local\n";
    result << "#endif\n";
    result << "\n";
    if (options_->GetEnablednessCheckingMode() ==
        Options::EnablednessCheckingMode::STANDARD) {
      result << "static __dredd_thread_local int __dredd_some_mutation_enabled "
                "= 1;\n";
    }
    result << "static bool __dredd_enabled_mutation(int local_mutation_id) {\n";
    result << "  static __dredd_thread_local int initialized = 0;\n";
    result << "  static __dredd_thread_local uint64_t enabled_bitset["
           << num_64_bit_words_required << "];\n";
    result << "  if (!initialized) {\n";
    if (options_->GetEnablednessCheckingMode() ==
        Options::EnablednessCheckingMode::STANDARD) {
      result << "    int some_mutation_enabled = 0;\n";
    }
    result << "    const char* dredd_environment_variable = "
              "getenv(\"DREDD_ENABLED_MUTATION\");\n";
    result << "    if (dredd_environment_variable) {\n";
    result << "      char* temp = malloc(strlen(dredd_environment_variable) + "
              "1);\n";
    result << "      strcpy(temp, dredd_environment_variable);\n";
    result << "      char* token;\n";
    result << "      token = strtok(temp, \",\");\n";
    result << "      while(token) {\n";
    result << "        int value = atoi(token);\n";
    result << "        int local_value = value - " << initial_mutation_id
           << ";\n";
    result << "        if (local_value >= 0 && local_value < " << num_mutations
           << ") {\n";
    result << "          enabled_bitset[local_value / 64] |= ((uint64_t) 1 << "
              "(local_value % 64));\n";
    if (options_->GetEnablednessCheckingMode() ==
        Options::EnablednessCheckingMode::STANDARD) {
      result << "          some_mutation_enabled = 1;\n";
    }
    result << "        }\n";
    result << "        token = strtok(NULL, \",\");\n";
    result << "      }\n";
    result << "      free(temp);\n";
    result << "    }\n";
    result << "    initialized = 1;\n";
    if (options_->GetEnablednessCheckingMode() ==
        Options::EnablednessCheckingMode::STANDARD) {
      result << "    __dredd_some_mutation_enabled = some_mutation_enabled;\n";
    }
    result << "  }\n";
    result
        << "  return enabled_bitset[local_mutation_id / 64] & ((uint64_t) 1 << "
           "(local_mutation_id % 64));\n";
    result << "}\n";
  }
  result << "\n";
  return result.str();
}

std::string MutateAstConsumer::GetMutantTrackingDreddPreludeC(
    int initial_mutation_id) const {
  // See comments in GetMutantTrackingDreddPreludeCpp; this is a straightforward
  // port to C.
  const int num_mutations = *mutation_id_ - initial_mutation_id;
  std::stringstream result;
  result << kDreddPreludeHeadersC;
  result << "\n";
  result << kDreddPreludeMutantTrackingLock;
  result << "static void __dredd_record_covered_mutants(int local_mutation_id, "
            "int num_mutations) {\n";
  result << "  static char already_recorded[" << num_mutations << "];\n";
  result << "  if (__dredd_atomic_load(&already_recorded[local_mutation_id])) "
            "return;\n";
  result << "  if (__dredd_atomic_exchange("
            "&already_recorded[local_mutation_id], 1)) return;\n";
  result << "  const char* dredd_tracking_environment_variable = "
            "getenv(\"DREDD_MUTANT_TRACKING_FILE\");\n";
  result << "  if (!dredd_tracking_environment_variable) return;\n";
  result << "  while (__dredd_atomic_exchange(&__dredd_tracking_file_lock, 1)) "
            "{\n";
  result << "    while (__dredd_atomic_load(&__dredd_tracking_file_lock)) {\n";
  result << "    }\n";
  result << "  }\n";
  result << "  FILE* fp = __dredd_tracking_file;\n";
  result << "  if (!fp) {\n";
  result << "    fp = fopen(dredd_tracking_environment_variable, \"a\");\n";
  result << "    __dredd_tracking_file = fp;\n";
  result << "  }\n";
  result << "  if (fp) {\n";
  result << "    for (int i = 0; i < num_mutations; i++) {\n";
  result << R"(      fprintf(fp, "%d\n", )" +
                std::to_string(initial_mutation_id)
         << " + local_mutation_id + i);\n";
  result << "    }\n";
  result << "    fflush(fp);\n";
  result << "  }\n";
  result << "  __dredd_atomic_exchange(&__dredd_tracking_file_lock, 0);\n";
  result << "}\n\n";
  return result.str();
}

std::string MutateAstConsumer::GetDreddPreludeC(int initial_mutation_id) const {
  return kDreddPreludeStartComment +
         (options_->GetOnlyTrackMutantCoverage()
              ? GetMutantTrackingDreddPreludeC(initial_mutation_id)
              : GetRegularDreddPreludeC(initial_mutation_id));
}

void MutateAstConsumer::ApplyMutations(
    const MutationTreeNode& dredd_mutation_tree_node, int initial_mutation_id,
    clang::ASTContext& context,
    protobufs::MutationInfoForFile& protobufs_mutation_info_for_file,
    protobufs::MutationTreeNode& protobufs_mutation_tree_node,
    std::unordered_set<std::string>& dredd_declarations, bool build_tree) {
  assert(!(dredd_mutation_tree_node.IsEmpty() &&
           dredd_mutation_tree_node.GetChildren().size() == 1) &&
         "The mutation tree should already be compressed.");
  for (const auto& child : dredd_mutation_tree_node.GetChildren()) {
    assert(!child->IsEmpty() &&
           "The mutation tree should not have empty subtrees.");
    protobufs_mutation_tree_node.add_children(static_cast<uint32_t>(
        protobufs_mutation_info_for_file.mutation_tree_size()));
    protobufs::MutationTreeNode* new_protobufs_mutation_tree_node =
        protobufs_mutation_info_for_file.add_mutation_tree();
    ApplyMutations(
        *child, initial_mutation_id, context, protobufs_mutation_info_for_file,
        *new_protobufs_mutation_tree_node, dredd_declarations, build_tree);
  }

  for (const auto& mutation : dredd_mutation_tree_node.GetMutations()) {
    const int mutation_id_old = *mutation_id_;
    const auto mutation_group = mutation->Apply(
        context, compiler_instance_->getPreprocessor(), *options_,
        initial_mutation_id, *mutation_id_, rewriter_, dredd_declarations);
    if (build_tree && *mutation_id_ > mutation_id_old) {
      // Only add the result of applying the mutation if it had an effect.
      *protobufs_mutation_tree_node.add_mutation_groups() = mutation_group;
      ignore_scopes_.AddIgnoredMutations(mutation_group,
                                         protobufs_mutation_info_for_file);
    }
  }
}

}  // namespace dredd
