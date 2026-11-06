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

#include "libdredd/mutate_visitor.h"

#include <llvm/ADT/ArrayRef.h>

#include <cassert>
#include <cstddef>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "clang/AST/Attrs.inc"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclBase.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/LambdaCapture.h"
#include "clang/AST/OperationKinds.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/Stmt.h"
#include "clang/AST/StmtCXX.h"
#include "clang/AST/TemplateBase.h"
#include "clang/AST/Type.h"
#include "clang/AST/TypeLoc.h"
#include "clang/Basic/Builtins.h"
#include "clang/Basic/LangOptions.h"
#include "clang/Basic/SourceLocation.h"
#include "clang/Basic/TypeTraits.h"
#include "clang/Frontend/CompilerInstance.h"
#include "libdredd/mutation.h"
#include "libdredd/mutation_remove_stmt.h"
#include "libdredd/mutation_replace_binary_operator.h"
#include "libdredd/mutation_replace_expr.h"
#include "libdredd/mutation_replace_unary_operator.h"
#include "libdredd/options.h"
#include "libdredd/util.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/iterator.h"
#include "llvm/Support/Casting.h"

namespace dredd {

namespace {

// Determines whether |cast_expr| converts a value, or reads an l-value that
// is not volatile. A mutant of the l-value itself (++ or --) changes a
// variable, and is kept, but which l-value is read, as decided by the
// condition of "c ? x : y", only changes the value.
bool IsConversionOfValue(const clang::CastExpr& cast_expr) {
  const clang::Expr* sub_expr = cast_expr.getSubExpr();
  if (cast_expr.getCastKind() == clang::CK_LValueToRValue) {
    return !sub_expr->getType().isVolatileQualified();
  }
  return sub_expr->isPRValue() && sub_expr->getType()->isScalarType() &&
         cast_expr.getCastKind() != clang::CK_UserDefinedConversion &&
         cast_expr.getCastKind() != clang::CK_ConstructorConversion &&
         cast_expr.getCastKind() != clang::CK_Dynamic;
}

std::vector<const clang::Expr*> GetValueOperandsOfBinaryOperator(
    const clang::BinaryOperator& binary_operator,
    const clang::ASTContext& ast_context) {
  if (binary_operator.isAssignmentOp() || binary_operator.isPtrMemOp() ||
      binary_operator.getOpcode() == clang::BO_Cmp) {
    return {};
  }
  if (binary_operator.isCommaOp()) {
    // The value of the left operand is not the value of the expression.
    return {binary_operator.getRHS()};
  }
  if (binary_operator.isLogicalOp() &&
      binary_operator.getRHS()->HasSideEffects(
          ast_context, /*IncludePossibleEffects=*/true)) {
    // The value of the left operand decides whether the right operand is
    // evaluated.
    return {binary_operator.getRHS()};
  }
  return {binary_operator.getLHS(), binary_operator.getRHS()};
}

std::vector<const clang::Expr*> GetValueOperandsOfConditionalOperator(
    const clang::ConditionalOperator& conditional_operator,
    const clang::ASTContext& ast_context) {
  const clang::Expr* true_expr = conditional_operator.getTrueExpr();
  const clang::Expr* false_expr = conditional_operator.getFalseExpr();
  if (true_expr->HasSideEffects(ast_context,
                                /*IncludePossibleEffects=*/true) ||
      false_expr->HasSideEffects(ast_context,
                                 /*IncludePossibleEffects=*/true)) {
    // The value of the condition decides which side effects happen.
    return {true_expr, false_expr};
  }
  return {conditional_operator.getCond(), true_expr, false_expr};
}

std::vector<const clang::Expr*> GetValueOperandsOfInitList(
    const clang::InitListExpr& init_list_expr) {
  // A scalar in braces, as in "int x{y};". Both the syntactic and the semantic
  // form of the list hold the value.
  std::vector<const clang::Expr*> result;
  if (!init_list_expr.getType()->isScalarType()) {
    return result;
  }
  const clang::InitListExpr* other_form =
      init_list_expr.isSemanticForm() ? init_list_expr.getSyntacticForm()
                                      : init_list_expr.getSemanticForm();
  for (const clang::InitListExpr* form : {&init_list_expr, other_form}) {
    if (form != nullptr && form->getNumInits() == 1) {
      result.push_back(form->getInit(0));
    }
  }
  return result;
}

// Gives the subexpressions of |expr| whose value only reaches the value of
// |expr|: another value for such a subexpression changes the value of |expr|,
// and no side effect.
std::vector<const clang::Expr*> GetValueOperands(
    const clang::Expr& expr, const clang::ASTContext& ast_context) {
  if (const auto* paren_expr = llvm::dyn_cast<clang::ParenExpr>(&expr)) {
    return {paren_expr->getSubExpr()};
  }
  if (const auto* expr_with_cleanups =
          llvm::dyn_cast<clang::ExprWithCleanups>(&expr)) {
    return {expr_with_cleanups->getSubExpr()};
  }
  if (const auto* cast_expr = llvm::dyn_cast<clang::CastExpr>(&expr)) {
    if (IsConversionOfValue(*cast_expr)) {
      return {cast_expr->getSubExpr()};
    }
    return {};
  }
  if (const auto* unary_operator =
          llvm::dyn_cast<clang::UnaryOperator>(&expr)) {
    switch (unary_operator->getOpcode()) {
      case clang::UO_Plus:
      case clang::UO_Minus:
      case clang::UO_Not:
      case clang::UO_LNot:
      case clang::UO_Extension:
        return {unary_operator->getSubExpr()};
      default:
        return {};
    }
  }
  if (const auto* binary_operator =
          llvm::dyn_cast<clang::BinaryOperator>(&expr)) {
    return GetValueOperandsOfBinaryOperator(*binary_operator, ast_context);
  }
  if (const auto* conditional_operator =
          llvm::dyn_cast<clang::ConditionalOperator>(&expr)) {
    return GetValueOperandsOfConditionalOperator(*conditional_operator,
                                                 ast_context);
  }
  if (const auto* init_list_expr = llvm::dyn_cast<clang::InitListExpr>(&expr)) {
    return GetValueOperandsOfInitList(*init_list_expr);
  }
  return {};
}

}  // namespace

MutateVisitor::MutateVisitor(const clang::CompilerInstance& compiler_instance,
                             const Options& options)
    : compiler_instance_(&compiler_instance),
      options_(&options),
      mutation_tree_root_() {
  mutation_tree_path_.push_back(&mutation_tree_root_);
}

bool MutateVisitor::IsTypeSupported(const clang::QualType qual_type) {
  if (qual_type.isNull()) {
    return false;
  }
  const auto* builtin_type = qual_type->getAs<clang::BuiltinType>();
  return builtin_type != nullptr &&
         (builtin_type->isInteger() || builtin_type->isFloatingPoint());
}

bool MutateVisitor::IsInFunction() {
  // Walk up the next of enclosing declarations
  for (int index = static_cast<int>(enclosing_decls_.size()) - 1; index >= 0;
       index--) {
    const auto* decl = enclosing_decls_[static_cast<size_t>(index)];
    if (llvm::dyn_cast<clang::FunctionDecl>(decl) != nullptr) {
      // A function declaration has been found, either directly or only by going
      // via variable declarations. Thus the point of visitation is in a
      // function without any other intervening constructs.
      return true;
    }
    // It is OK if the visitation point is inside a variable declaration, as
    // long as that declaration turns out to be inside a function.
    if (llvm::dyn_cast<clang::VarDecl>(decl) == nullptr) {
      // The visitation point is inside some other declaration (e.g. a class).
      return false;
    }
  }
  // Global scope was reached without hitting a function, so the declaration is
  // not in a function.
  return false;
}

bool MutateVisitor::TraverseDecl(clang::Decl* decl) {
  if (decl == nullptr) {
    // A Clang AST can feature nodes with null children; e.g.
    // "catch (...)" leads to a null catch expression.
    return true;
  }
  if (llvm::dyn_cast<clang::TranslationUnitDecl>(decl) != nullptr) {
    // This is the top-level translation unit declaration, so descend into it.
    const bool result = RecursiveASTVisitor::TraverseDecl(decl);
    // At this point the translation unit has been fully visited, so the
    // mutation tree that has been built can be made simpler, in preparation for
    // later turning it into a JSON summary.
    mutation_tree_root_.TidyUp();
    return result;
  }
  auto source_range_in_main_file =
      GetSourceRangeInMainFile(compiler_instance_->getPreprocessor(), *decl);
  if (source_range_in_main_file.isInvalid()) {
    // This declaration is not wholly contained in the main file, so do not
    // consider it for mutation.
    return true;
  }
  NoteNodeForIgnoreScopes(source_range_in_main_file);
  if (const auto* static_assert_decl =
          llvm::dyn_cast<clang::StaticAssertDecl>(decl)) {
    // It does not make sense to mutate static assertions, as (a) this will
    // very likely lead to compile-time failures due to the assertion not
    // holding, (b) if compilation succeeds then the assertion is not actually
    // present at runtime so there is no notion of killing the mutant, and (c)
    // dynamic instrumentation of the mutation operator will break the rules
    // associated with static assertions anyway.

    // However, the expression under a static assertion must be a compile-time
    // constant. To allow the constant declarations that occur inside a static
    // assertion to have their initial value mutated (because they may be used
    // elsewhere) and to avoid compilation errors, we record static assertions
    // so that their argument expressions can be replaced with `1`.
    static_assertions_to_rewrite_.push_back(static_assert_decl);
    return true;
  }
  if (const auto* function_decl = llvm::dyn_cast<clang::FunctionDecl>(decl)) {
    if (function_decl->getNameAsString() == "__dredd_prelude_start" &&
        !HasDreddPreludeStartLocation()) {
      // This is a special function provided by the user to record where the
      // Dredd prelude should be inserted.
      dredd_prelude_start_location_ = function_decl->getBeginLoc();
    }
    if (function_decl->isConstexpr()) {
      // Because Dredd's mutations occur dynamically, they cannot be applied to
      // C++ constexpr functions, which require compile-time evaluation.
      return true;
    }
    if (function_decl->doesThisDeclarationHaveABody()) {
      RecordTailStatements(*function_decl);
    }
  }

  // Besides variable declaration, constant-sized C++ arrays can occur inside a
  // struct as a field. As in the case of variable declarations, we record the
  // arrays in question so that later their size expressions can be rewritten.
  if (const auto* field_decl = llvm::dyn_cast<clang::FieldDecl>(decl)) {
    if (compiler_instance_->getLangOpts().CPlusPlus &&
        field_decl->getType()->isConstantArrayType()) {
      constant_sized_arrays_to_rewrite_.push_back(field_decl);
    }
  }

  enclosing_decls_.push_back(decl);
  // Consider the declaration for mutation.
  RecursiveASTVisitor::TraverseDecl(decl);
  enclosing_decls_.pop_back();

  return true;
}

bool MutateVisitor::TraverseVarDecl(clang::VarDecl* var_decl) {
  if (const auto* template_specialization_type =
          var_decl->getType()->getAs<clang::TemplateSpecializationType>()) {
    // We have a template specialization type where the arguments could be
    // constant expressions. Compilers may issue a compilation error if a
    // template argument is not a compile-time constant. To allow the constant
    // expressions used as template arguments to have their initial values
    // modified, we record the template argument expression so that it can be
    // replaced with the value it would normally evaluate to. This has to be
    // done recursively to handle nested template specializations.
    SaveConstantTemplateArgumentForRewrite(template_specialization_type);
  }
  if (compiler_instance_->getLangOpts().CPlusPlus &&
      var_decl->getType()->isConstantArrayType()) {
    // We have a constant-sized C++ array. Some C++ compilers complain if
    // components of the size expression are not compile-time constants. Other
    // compilers are OK with this unless the array is also initialized. To
    // allow the constant declarations that occur in a size expression to have
    // their initial values mutated (because they may be used elsewhere), but
    // to avoid compilation errors, we record the arrays in question so that
    // later their size expressions can be replaced with the value to which
    // the size expression would normally evaluate.
    constant_sized_arrays_to_rewrite_.push_back(var_decl);
  }
  if (var_decl->isConstexpr() || var_decl->hasAttr<clang::ConstInitAttr>()) {
    // Because Dredd's mutations occur dynamically, they cannot be applied to
    // C++ constexprs or variables that require constant initialization as
    // these both require compile-time evaluation.
    return true;
  }
  if (!compiler_instance_->getLangOpts().CPlusPlus &&
      var_decl->isStaticLocal()) {
    // In C, static local variables can only be initialized using constant
    // expressions, which require compile-time evaluation.
    return true;
  }
  if (options_->GetOptimisations().GetDoNotMutateUnreadLocalValues() &&
      var_decl->getInit() != nullptr && IsUnreadLocalScalar(*var_decl)) {
    // Nothing reads the variable, so a mutant whose only effect is another
    // value for it is equivalent to the original program.
    NoteUnreadValue(*var_decl->getInit());
  }

  enclosing_decls_.push_back(var_decl);
  // Consider the declaration for mutation.
  RecursiveASTVisitor::TraverseVarDecl(var_decl);
  enclosing_decls_.pop_back();

  return true;
}

bool MutateVisitor::TraverseStmt(clang::Stmt* stmt) {
  if (stmt == nullptr) {
    return true;
  }

  if (ignore_scopes_ != nullptr && ignore_scopes_->HasPendingOnce()) {
    NoteNodeForIgnoreScopes(
        GetSourceRangeInMainFile(compiler_instance_->getPreprocessor(), *stmt));
  }

  // Do not mutate user defined literals.
  // TODO(https://github.com/mc-imperial/dredd/issues/223): Consider supporting
  // them.
  if (llvm::dyn_cast<clang::UserDefinedLiteral>(stmt) != nullptr) {
    return true;
  }

  // Do not mutate under a constant expression, since mutation logic is
  // inherently non-constant.
  if (llvm::dyn_cast<clang::ConstantExpr>(stmt) != nullptr) {
    return true;
  }

  // Do not mutate under a 'noexcept', since it requires a constexpr argument
  // and mutation logic is inherently non-constant.
  if (llvm::dyn_cast<clang::CXXNoexceptExpr>(stmt) != nullptr) {
    return true;
  }

  if (options_->GetOptimisations().DoNotMutateSizeofAndAlignof()) {
    // Do not mutate under 'sizeof' or 'alignof', as this is guaranteed to yield
    // equivalent mutants. Note that we *do* want to mutate above these
    // expressions, hence we ignore children of such expressions, rather than
    // ignoring the expressions themselves.
    if (const auto* unary_expr_or_type_trait_parent =
            GetFirstParentOfType<clang::UnaryExprOrTypeTraitExpr>(
                *stmt, compiler_instance_->getASTContext())) {
      const auto kind = unary_expr_or_type_trait_parent->getKind();
      if (kind == clang::UETT_SizeOf || kind == clang::UETT_AlignOf) {
        return true;
      }
    }
  }

  // Do not mutate the condition of a constexpr if statement.
  if (const auto* if_stmt = GetFirstParentOfType<clang::IfStmt>(
          *stmt, compiler_instance_->getASTContext())) {
    if (if_stmt->isConstexpr() && if_stmt->getCond() == stmt) {
      return true;
    }
  }

  // Do not mutate the array size expression of C++'s NewExpr.
  // For instance, we do not want to mutate `2` in new `a[2]{3, 4}`,
  // as doing so requires type `a` to have zero-argument constructor.
  if (const auto* cxx_new_expr = GetFirstParentOfType<clang::CXXNewExpr>(
          *stmt, compiler_instance_->getASTContext())) {
    if (cxx_new_expr->getArraySize() == stmt) {
      return true;
    }
  }

  // Some functions, e.g., __builtin_frame_address(), require their arguments to
  // be constant integers. Attempting to apply a mutator function to such an
  // argument would result in a compile-time error. Thus, we record these
  // arguments' expressions so that they can be rewritten with the values to
  // which the expressions would normally evaluate.
  if (const auto* call_expr = llvm::dyn_cast<clang::CallExpr>(stmt)) {
    if (call_expr->getBuiltinCallee() ==
        clang::Builtin::BI__builtin_frame_address) {
      // callee is `__builtin_frame_address()`
      constant_arguments_to_rewrite_.push_back(call_expr->getArg(0));
      return true;
    }
  }

  // Do not mutate expression under `decltype`, as Dredd doesn't change the type
  // of underlying expression.
  if (const auto* cast_expr = llvm::dyn_cast<clang::CastExpr>(stmt)) {
    if (cast_expr->getType()->getAs<clang::DecltypeType>() != nullptr) {
      return true;
    }
  }

  // Add a node to the mutation tree to capture any mutations beneath this
  // statement.
  const PushMutationTreeRAII push_mutation_tree(*this);
  return RecursiveASTVisitor::TraverseStmt(stmt);
}

bool MutateVisitor::TraverseCaseStmt(clang::CaseStmt* case_stmt) {
  // Do not traverse the expression associated with this switch case: switch
  // case expressions need to be constant, which rules out the kinds of
  // mutations that Dredd performs.
  return TraverseStmt(case_stmt->getSubStmt());
}

bool MutateVisitor::TraverseConstantArrayTypeLoc(
    clang::ConstantArrayTypeLoc constant_array_type_loc,
    bool traverse_qualifier) {
  // Prevent compilers complaining that this method could be made static, and
  // that it ignores its parameters.
  (void)this;
  (void)constant_array_type_loc;
  (void)traverse_qualifier;
  // Changing a constant-sized array to a non-constant-sized array is
  // problematic in C if the array has an initializer, and in C++ lambdas cannot
  // be used in array size expressions. For simplicity, don't try to mutate
  // constant array sizes.
  return true;
}

bool MutateVisitor::TraverseVariableArrayTypeLoc(
    clang::VariableArrayTypeLoc variable_array_type_loc,
    bool traverse_qualifier) {
  if (compiler_instance_->getLangOpts().CPlusPlus) {
    // In C++, lambdas cannot appear in array sizes, so avoid mutating here.
    return true;
  }
#if LLVM_VERSION_MAJOR >= 22
  return RecursiveASTVisitor::TraverseVariableArrayTypeLoc(
      variable_array_type_loc, traverse_qualifier);
#else
  (void)traverse_qualifier;
  return RecursiveASTVisitor::TraverseVariableArrayTypeLoc(
      variable_array_type_loc);
#endif
}

bool MutateVisitor::TraverseDependentSizedArrayTypeLoc(
    clang::DependentSizedArrayTypeLoc dependent_sized_array_type_loc,
    bool traverse_qualifier) {
  // Prevent compilers complaining that this method could be made static, and
  // that it ignores its parameters.
  (void)this;
  (void)dependent_sized_array_type_loc;
  (void)traverse_qualifier;
  // Changing an array whose size is derived from template parameters is
  // problematic because after template instantiation these lead to either
  // constant or variable-sized arrays, neither of which can be mutated in C++.
  return true;
}

bool MutateVisitor::TraverseTemplateArgumentLoc(
    clang::TemplateArgumentLoc template_argument_loc) {
  // Prevent compilers complaining that this method could be made static, and
  // that it ignores its parameter.
  (void)this;
  (void)template_argument_loc;
  // C++ template arguments typically need to be compile-time constants, and so
  // should not be mutated.
  return true;
}

bool MutateVisitor::TraverseLambdaCapture(
    clang::LambdaExpr* lambda_expr, const clang::LambdaCapture* lambda_capture,
    clang::Expr* init) {
  // Prevent compilers complaining that this method could be made static, and
  // that it ignores its parameters.
  (void)this;
  (void)lambda_expr;
  (void)lambda_capture;
  (void)init;
  return true;
}

bool MutateVisitor::TraverseParmVarDecl(clang::ParmVarDecl* parm_var_decl) {
  // Prevent compilers complaining that this method could be made static, and
  // that it ignores its parameter.
  (void)this;
  (void)parm_var_decl;
  return true;
}

void MutateVisitor::HandleUnaryOperator(clang::UnaryOperator* unary_operator) {
  // Check that the argument to the unary expression has a source ranges that is
  // part of the main file. In particular, this avoids mutating expressions that
  // directly involve the use of macros (though it is OK if sub-expressions of
  // arguments use macros).
  if (GetSourceRangeInMainFile(compiler_instance_->getPreprocessor(),
                               *unary_operator->getSubExpr())
          .isInvalid()) {
    return;
  }

  // Don't mutate unary plus as this is unlikely to lead to a mutation that
  // differs from inserting a unary operator
  if (unary_operator->getOpcode() == clang::UnaryOperatorKind::UO_Plus) {
    return;
  }

  // Check that the argument type is supported
  if (!IsTypeSupported(unary_operator->getSubExpr()->getType())) {
    return;
  }

  // Mutation functions for ++ and -- operators require their argument to be
  // passed by reference. It is not possible to pass bit-fields by reference,
  // this mutation of these operators when applied to bit-fields is not
  // supported.
  if (unary_operator->isIncrementDecrementOp() &&
      unary_operator->getSubExpr()->refersToBitField()) {
    return;
  }

  if (options_->GetOptimisations().GetLeverageConstantFolding()) {
    if (unary_operator->getOpcode() == clang::UO_Minus &&
        (MutationReplaceExpr::ExprIsEquivalentToInt(
             *unary_operator->getSubExpr(), 1,
             compiler_instance_->getASTContext()) ||
         MutationReplaceExpr::ExprIsEquivalentToFloat(
             *unary_operator->getSubExpr(), 1.0,
             compiler_instance_->getASTContext()))) {
      return;
    }

    if (unary_operator->getOpcode() == clang::UO_Not &&
        (MutationReplaceExpr::ExprIsEquivalentToInt(
             *unary_operator->getSubExpr(), 0,
             compiler_instance_->getASTContext()) ||
         MutationReplaceExpr::ExprIsEquivalentToFloat(
             *unary_operator->getSubExpr(), 0.0,
             compiler_instance_->getASTContext()) ||
         MutationReplaceExpr::ExprIsEquivalentToInt(
             *unary_operator->getSubExpr(), 1,
             compiler_instance_->getASTContext()) ||
         MutationReplaceExpr::ExprIsEquivalentToFloat(
             *unary_operator->getSubExpr(), 1.0,
             compiler_instance_->getASTContext()))) {
      return;
    }
  }

  if (IsValueUnread(*unary_operator) &&
      !unary_operator->isIncrementDecrementOp()) {
    // Each replacement of this operator still evaluates the operand, and only
    // changes a value that nothing reads.
    return;
  }

  AddMutation(std::make_unique<MutationReplaceUnaryOperator>(
      *unary_operator, compiler_instance_->getPreprocessor(),
      compiler_instance_->getASTContext()));
}

void MutateVisitor::HandleBinaryOperator(
    clang::BinaryOperator* binary_operator) {
  // Check that arguments of the binary expression have source ranges that are
  // part of the main file. In particular, this avoids mutating expressions that
  // directly involve the use of macros (though it is OK if sub-expressions of
  // arguments use macros).
  if (GetSourceRangeInMainFile(compiler_instance_->getPreprocessor(),
                               *binary_operator->getLHS())
          .isInvalid() ||
      GetSourceRangeInMainFile(compiler_instance_->getPreprocessor(),
                               *binary_operator->getRHS())
          .isInvalid()) {
    return;
  }

  // We only want to change operators for binary operations on basic types.
  // Check that the argument types are supported.
  if (!IsTypeSupported(binary_operator->getLHS()->getType())) {
    return;
  }
  if (!IsTypeSupported(binary_operator->getRHS()->getType())) {
    return;
  }

  // Mutation functions for assignment operators (for example +=) require their
  // first argument to be passed by reference. It is not possible to pass
  // bit-fields by reference, this mutation of these operators when applied to a
  // bit-field first argument is not supported.
  if (binary_operator->isAssignmentOp() &&
      binary_operator->getLHS()->refersToBitField()) {
    return;
  }

  if (binary_operator->isCommaOp()) {
    // The comma operator is so versatile that it does not make a great deal of
    // sense to try to rewrite it.
    return;
  }

  // There is no useful way to mutate this expression since it is equivalent to
  // replacement with a constant in all cases.
  if (options_->GetOptimisations().GetLeverageConstantFolding() &&
      (MutationReplaceExpr::ExprIsEquivalentToInt(
           *binary_operator->getLHS(), 0,
           compiler_instance_->getASTContext()) ||
       MutationReplaceExpr::ExprIsEquivalentToFloat(
           *binary_operator->getLHS(), 0.0,
           compiler_instance_->getASTContext())) &&
      (MutationReplaceExpr::ExprIsEquivalentToInt(
           *binary_operator->getRHS(), 1,
           compiler_instance_->getASTContext()) ||
       MutationReplaceExpr::ExprIsEquivalentToFloat(
           *binary_operator->getRHS(), 1.0,
           compiler_instance_->getASTContext()))) {
    return;
  }

  // An assignment changes a variable other than the one that nothing reads,
  // so each mutant of an assignment is kept.
  const bool value_is_unread =
      IsValueUnread(*binary_operator) && !binary_operator->isAssignmentOp();
  if (value_is_unread &&
      !binary_operator->getLHS()->HasSideEffects(
          compiler_instance_->getASTContext(),
          /*IncludePossibleEffects=*/true) &&
      !binary_operator->getRHS()->HasSideEffects(
          compiler_instance_->getASTContext(),
          /*IncludePossibleEffects=*/true)) {
    // Whichever operands a mutant evaluates, it has no side effect, and only
    // changes a value that nothing reads.
    return;
  }

  AddMutation(std::make_unique<MutationReplaceBinaryOperator>(
      *binary_operator, compiler_instance_->getPreprocessor(),
      compiler_instance_->getASTContext(), value_is_unread));
}

void MutateVisitor::HandleExpr(clang::Expr* expr) {
  // L-values are only mutated by inserting the prefix operators ++ and --, and
  // only under specific circumstances as documented by
  // MutationReplaceExpr::CanMutateLValue.
  if (expr->isLValue() && !MutationReplaceExpr::CanMutateLValue(
                              compiler_instance_->getASTContext(), *expr)) {
    return;
  }

  // It is incorrect to attempt to mutate braced initializer lists.
  if (llvm::dyn_cast<clang::InitListExpr>(expr) != nullptr) {
    return;
  }

  // Avoid mutating null pointer assignments, such as int* x = 0, as mutating
  // these expressions in C++ is either not safe or not useful. This mutation
  // is acceptable in C, but we avoid the mutation for consistency.
  //
  // The expression may be value-dependent, e.g. 'sizeof(T)' in a template.
  // Such an expression is not a null pointer constant in C++11, and
  // NPC_ValueDependentIsNotNull says so explicitly. The default,
  // NPC_NeverValueDependent, makes Clang reach llvm_unreachable for a
  // value-dependent expression under -fms-compatibility (the default for MSVC
  // targets) and before C++11, which crashes release builds of Dredd.
  if (expr->isNullPointerConstant(compiler_instance_->getASTContext(),
                                  clang::Expr::NPC_ValueDependentIsNotNull) !=
      clang::Expr::NPCK_NotNull) {
    if (const auto* cast_parent = GetFirstParentOfType<clang::CastExpr>(
            *expr, compiler_instance_->getASTContext())) {
      if (cast_parent->getType()->isAnyPointerType()) {
        return;
      }
    }
  }

  if (IsConversionOfEnumToConstructor(*expr)) {
    return;
  }

  // Avoid mutation on cast when its underlying value is a bitfield
  // l-value that is subsequently passed by reference. Any expression that is
  // passed by reference is a child of MaterializeTemporaryExpr, which
  // represents a prvalue temporary that is written into memory so that a
  // reference can bind to it.
  if (const auto* cast_expr = llvm::dyn_cast<clang::CastExpr>(expr)) {
    if (cast_expr->getSubExpr()->refersToBitField() &&
        GetFirstParentOfType<clang::MaterializeTemporaryExpr>(
            *expr, compiler_instance_->getASTContext()) != nullptr) {
      return;
    }
  }

  if (options_->GetOptimisations()
          .GetDoNotMutateCastsCleanupsAndParentheses()) {
    // If an expression is the direct child of a cast expression, do not mutate
    // it unless the cast is an l-value to r-value cast. In an l-value to
    // r-value cast it is worth mutating the expression before and after casting
    // since different mutations will arise. In a cast that does not change l/r-
    // value status, it is highly likely that mutations on the inner cast will
    // have the same effect as on the outer cast. The possible differences
    // arising due to a change of type are unlikely to be all that interesting,
    // and r-value to r-value implicit casts are very common, e.g. occurring
    // whenever a signed literal, such as `1`, is used in an unsigned context.
    if (const auto* cast_parent = GetFirstParentOfType<clang::CastExpr>(
            *expr, compiler_instance_->getASTContext())) {
      if (cast_parent->isLValue() == expr->isLValue() &&
          GetFirstParentOfType<clang::InitListExpr>(
              *expr, compiler_instance_->getASTContext()) == nullptr) {
        // However, this optimization shouldn't be performed on expressions
        // under Initializer List. This is because: (1) Dredd won't act on the
        // outer implicit cast under Initializer List. (2)  Bypassing this
        // would skip dredd-introduced static_cast, causing issues when the
        // expression is implicitly cast into a narrower type and passed into
        // a C++ std::initializer_list.
        return;
      }
    }

    // If an expression is the direct child of a compound statement or switch
    // case then don't mutate it, because:
    // - replacing it with a constant has the same effect as deleting it;
    // - if it is an r-value, inserting a unary operator has no effect;
    // - if it is an l-value, inserting an increment operator is fairly well
    // captured by inserting an increment before a future use of the l-value (it
    // is not exactly the same, because the future use could be dynamically
    // reached in different manners compared with this statement).
    if (GetFirstParentOfType<clang::CompoundStmt>(
            *expr, compiler_instance_->getASTContext()) != nullptr ||
        GetFirstParentOfType<clang::SwitchCase>(
            *expr, compiler_instance_->getASTContext()) != nullptr) {
      return;
    }
  }

  AddMutationReplaceExpr(*expr);
}

void MutateVisitor::AddMutationReplaceExpr(const clang::Expr& expr) {
  const bool value_is_unread = IsValueUnread(expr);
  if (value_is_unread && !MutationReplaceExpr::ExprRequiresLambda(
                             expr, compiler_instance_->getASTContext())) {
    // Only a constant replacement can skip the evaluation of the expression,
    // when the mutator function takes the expression in a lambda. Every other
    // mutant only changes a value that nothing reads.
    return;
  }
  AddMutation(std::make_unique<MutationReplaceExpr>(
      expr, compiler_instance_->getPreprocessor(),
      compiler_instance_->getASTContext(), value_is_unread));
}

bool MutateVisitor::VisitExpr(clang::Expr* expr) {
  if (options_->GetOptimisations()
          .GetDoNotMutateCastsCleanupsAndParentheses() &&
      llvm::dyn_cast<clang::ParenExpr>(expr) != nullptr) {
    // There is no value in mutating a parentheses expression.
    return true;
  }

  if (options_->GetOptimisations()
          .GetDoNotMutateCastsCleanupsAndParentheses() &&
      llvm::dyn_cast<clang::ExprWithCleanups>(expr) != nullptr) {
    // This special AST node represents an expression with certain associated
    // cleanup actions. The node's subexpression captures the actual syntactic
    // expression, and the ExprWithCleanups has the same type as its
    // subexpression, so there is no value in mutating both of them.
    return true;
  }

  if (!IsInFunction()) {
    // Only consider mutating expressions that occur inside functions.
    return true;
  }

  if (var_decl_source_locations_.contains(expr->getBeginLoc())) {
    // The start of the expression coincides with the source location of a
    // variable declaration. This happens when the expression has the form:
    // "auto v = ...", e.g. occurring in "if (auto v = ...)". Here, the source
    // location for "v" is associated with both the declaration of "v" and the
    // condition expression for the if statement. It must not be mutated,
    // because this would lead to invalid code of the form:
    // "if (auto __dredd_fun(v) = ...)".
    return true;
  }

  if (GetSourceRangeInMainFile(compiler_instance_->getPreprocessor(), *expr)
          .isInvalid()) {
    return true;
  }

  // Introduced to work around what seems like a bug in Clang, where a source
  // range can end earlier than it starts. See "structured_binding.cc" under
  // single file tests. If the Clang issue is indeed a bug and gets fixed, this
  // check (and the associated function) should be removed.
  if (!SourceRangeConsistencyCheck(expr->getSourceRange(),
                                   compiler_instance_->getASTContext())) {
    return true;
  }

  // Check that the result type is supported
  if (!IsTypeSupported(expr->getType())) {
    return true;
  }

  if (MutatingMayAffectArgumentDependentLookup(*expr)) {
    // Do not mutate this expression, because doing so might change the outcome
    // of argument-dependent lookup, which might cause compilation problems.
    return true;
  }

  if (MayDependOnLifetimeOfMaterializedTemporaryStorage(*expr)) {
    // Do not mutate a call expression if there is a risk that the value
    // returned by the call might be a reference to temporary storage arising
    // from a materialized temporary expression.
    return true;
  }

  if (auto* unary_operator = llvm::dyn_cast<clang::UnaryOperator>(expr)) {
    HandleUnaryOperator(unary_operator);
  }

  if (auto* binary_operator = llvm::dyn_cast<clang::BinaryOperator>(expr)) {
    HandleBinaryOperator(binary_operator);
  }

  HandleExpr(expr);

  return true;
}

bool MutateVisitor::TraverseCompoundStmt(clang::CompoundStmt* compound_stmt) {
  for (auto* stmt : compound_stmt->body()) {
    // A switch case (a 'case' or 'default') appears as a component of a
    // compound statement, but it is the statement *after* the 'case' or
    // 'default' label that should be considered for removal. This is because
    // the 'if' check for statement removal must occur after the label, not
    // before it, otherwise a branch to the label would branch straight into the
    // body of the 'if' statement that simulates statement removal. Furthermore,
    // if there are many 'case' or 'default' labels in a row, they are
    // arranged hierarchically in the AST. We therefore traverse any such
    // hierarchy until we reach a statement that is not a switch case, and it
    // is this statement that is considered for removal.
    clang::Stmt* target_stmt = stmt;
    while (auto* switch_case = llvm::dyn_cast<clang::SwitchCase>(target_stmt)) {
      target_stmt = switch_case->getSubStmt();
    }
    if (options_->GetOptimisations()
            .GetDoNotRemoveSideEffectFreeExpressionStatements()) {
      if (const auto* expr = llvm::dyn_cast<clang::Expr>(target_stmt)) {
        if (!expr->HasSideEffects(compiler_instance_->getASTContext())) {
          // There is no point mutating a side-effect free expression statement.
          continue;
        }
      }
    }

    // To ensure that each sub-statement of a compound statement has its
    // mutations recorded in sibling subtrees of the mutation tree, a mutation
    // tree node is pushed per sub-statement.
    const PushMutationTreeRAII push_mutation_tree(*this);
    TraverseStmt(target_stmt);

    assert(llvm::dyn_cast<clang::SwitchCase>(target_stmt) == nullptr &&
           "target_stmt isn't a SwitchCase due to previous AST traversal.");
    if (GetSourceRangeInMainFile(compiler_instance_->getPreprocessor(),
                                 *target_stmt)
            .isInvalid() ||
        llvm::dyn_cast<clang::NullStmt>(target_stmt) != nullptr ||
        llvm::dyn_cast<clang::DeclStmt>(target_stmt) != nullptr ||
        llvm::dyn_cast<clang::LabelStmt>(target_stmt) != nullptr) {
      // Wrapping labels and null statements in conditional code
      // has no effect. Declarations cannot be wrapped in conditional code
      // without risking breaking compilation.
      continue;
    }
    if (tail_statements_.contains(target_stmt)) {
      // Without this statement, control would flow off the end of a function
      // that must return a value.
      continue;
    }
    if (options_->GetOptimisations().GetDoNotRemoveCompoundStatements() &&
        llvm::dyn_cast<clang::CompoundStmt>(target_stmt) != nullptr) {
      // It is likely redundant to remove a compound statement since each of its
      // sub-statements will be considered for removal anyway.
      continue;
    }
    assert(!enclosing_decls_.empty() &&
           "Statements can only be removed if they are nested in some "
           "declaration.");
    AddMutation(std::make_unique<MutationRemoveStmt>(
        *target_stmt, compiler_instance_->getPreprocessor(),
        compiler_instance_->getASTContext()));
  }
  return true;
}

bool MutateVisitor::TraverseLambdaExpr(clang::LambdaExpr* lambda_expr) {
  RecordTailStatements(*lambda_expr->getCallOperator());
  return RecursiveASTVisitor::TraverseLambdaExpr(lambda_expr);
}

namespace {

// Determines whether |stmt| is or contains a return statement with an operand
// that returns from the function that |stmt| is in.
bool ContainsReturnWithOperand(const clang::Stmt& stmt) {
  if (const auto* return_stmt = llvm::dyn_cast<clang::ReturnStmt>(&stmt)) {
    return return_stmt->getRetValue() != nullptr;
  }
  if (llvm::isa<clang::LambdaExpr>(stmt)) {
    // A return statement in the body of a lambda returns from the lambda.
    return false;
  }
  for (const auto* child : stmt.children()) {
    if (child != nullptr && ContainsReturnWithOperand(*child)) {
      return true;
    }
  }
  return false;
}

// Yields |stmt| without the labels and attributes in front of it.
const clang::Stmt* SkipLabelsAndAttributes(const clang::Stmt* stmt) {
  while (true) {
    if (const auto* switch_case = llvm::dyn_cast<clang::SwitchCase>(stmt)) {
      stmt = switch_case->getSubStmt();
    } else if (const auto* label_stmt =
                   llvm::dyn_cast<clang::LabelStmt>(stmt)) {
      stmt = label_stmt->getSubStmt();
    } else if (const auto* attributed_stmt =
                   llvm::dyn_cast<clang::AttributedStmt>(stmt)) {
      stmt = attributed_stmt->getSubStmt();
    } else {
      return stmt;
    }
  }
}

}  // namespace

bool MutateVisitor::MustReturnValue(const clang::FunctionDecl& function_decl,
                                    const clang::Stmt& body) {
  if (function_decl.isMain()) {
    return false;
  }
  const clang::QualType return_type = function_decl.getReturnType();
  if (const auto* deduced_type = return_type->getContainedDeducedType();
      deduced_type != nullptr &&
      (deduced_type->getDeducedType().isNull() ||
       deduced_type->getDeducedType()->isDependentType())) {
    // In a template, a placeholder such as 'auto' stays undeduced (and counts
    // as dependent rather than as void).
    return ContainsReturnWithOperand(body);
  }
  return !return_type->isVoidType();
}

void MutateVisitor::RecordTailStatements(
    const clang::FunctionDecl& function_decl) {
  const clang::Stmt* body = function_decl.getBody();
  if (body == nullptr || llvm::isa<clang::CoroutineBodyStmt>(body) ||
      !MustReturnValue(function_decl, *body)) {
    return;
  }
  MarkTailStatements(body);
}

void MutateVisitor::MarkTailStatements(const clang::Stmt* stmt) {
  if (stmt == nullptr) {
    return;
  }
  tail_statements_.insert(stmt);
  if (const auto* compound_stmt = llvm::dyn_cast<clang::CompoundStmt>(stmt)) {
    for (const auto* sub_stmt : llvm::reverse(compound_stmt->body())) {
      MarkTailStatements(sub_stmt);
      if (!llvm::isa<clang::NullStmt>(SkipLabelsAndAttributes(sub_stmt))) {
        break;
      }
    }
  } else if (const auto* if_stmt = llvm::dyn_cast<clang::IfStmt>(stmt)) {
    MarkTailStatements(if_stmt->getThen());
    MarkTailStatements(if_stmt->getElse());
  } else if (const auto* switch_stmt =
                 llvm::dyn_cast<clang::SwitchStmt>(stmt)) {
    MarkTailStatements(switch_stmt->getBody());
  } else if (const auto* switch_case =
                 llvm::dyn_cast<clang::SwitchCase>(stmt)) {
    MarkTailStatements(switch_case->getSubStmt());
  } else if (const auto* label_stmt = llvm::dyn_cast<clang::LabelStmt>(stmt)) {
    MarkTailStatements(label_stmt->getSubStmt());
  } else if (const auto* attributed_stmt =
                 llvm::dyn_cast<clang::AttributedStmt>(stmt)) {
    MarkTailStatements(attributed_stmt->getSubStmt());
  } else if (const auto* try_stmt = llvm::dyn_cast<clang::CXXTryStmt>(stmt)) {
    MarkTailStatements(try_stmt->getTryBlock());
    for (unsigned int index = 0; index < try_stmt->getNumHandlers(); index++) {
      MarkTailStatements(try_stmt->getHandler(index)->getHandlerBlock());
    }
  }
}

void MutateVisitor::AddMutation(std::unique_ptr<Mutation> mutation) {
  mutation_tree_path_.back()->AddMutation(std::move(mutation));
}

void MutateVisitor::NoteNodeForIgnoreScopes(clang::SourceRange range) {
  if (ignore_scopes_ != nullptr && ignore_scopes_->HasPendingOnce()) {
    ignore_scopes_->NoteNode(range, compiler_instance_->getASTContext());
  }
}

bool MutateVisitor::VisitVarDecl(clang::VarDecl* var_decl) {
  var_decl_source_locations_.insert(var_decl->getLocation());
  return true;
}

bool MutateVisitor::IsUnreadLocalScalar(const clang::VarDecl& var_decl) {
  // A variable of class type is left alone: its initializer can give the
  // arguments of a constructor, such as that of a guard, whose side effects
  // matter. A reference is not of scalar type.
  return var_decl.isLocalVarDecl() && !var_decl.isImplicit() &&
         !var_decl.isReferenced() && var_decl.getType()->isScalarType() &&
         !var_decl.getType().isVolatileQualified() &&
         !var_decl.hasAttr<clang::CleanupAttr>();
}

void MutateVisitor::NoteUnreadValue(const clang::Expr& expr) {
  unread_values_.insert(&expr);
  for (const clang::Expr* operand :
       GetValueOperands(expr, compiler_instance_->getASTContext())) {
    NoteUnreadValue(*operand);
  }
}

bool MutateVisitor::IsConversionOfEnumToConstructor(
    const clang::Expr& expr) const {
  // This avoids a special case identified by
  // https://github.com/mc-imperial/dredd/issues/264. The issue arises when a
  // ternary expression has the form "c ? Foo(...) : enum_constant", where Foo
  // is a class/struct that can be implicitly constructed from an int, and that
  // also has the int operator overloaded.
  // In this case, the enum_constant is implicitly cast to an integer, which
  // then implicitly constructs a Foo instance. If the implicit cast is mutated,
  // the mutated code will lead to the conditional operator's third argument
  // having type int. It is then ambiguous whether the second argument,
  // Foo(...), should have type Foo, or type int, because Foo has the int
  // operator overloaded.

  // Check whether this expression is an implicit cast.
  const auto* implicit_cast_expr =
      llvm::dyn_cast<clang::ImplicitCastExpr>(&expr);
  if (implicit_cast_expr == nullptr) {
    return false;
  }
  // Check whether the parent expression is a C++ constructor.
  if (GetFirstParentOfType<clang::CXXConstructExpr>(
          expr, compiler_instance_->getASTContext()) == nullptr) {
    return false;
  }
  // Check whether there is an enum constant under the implicit cast.
  const auto* decl_ref_expr =
      llvm::dyn_cast<clang::DeclRefExpr>(implicit_cast_expr->getSubExpr());
  if (decl_ref_expr == nullptr) {
    return false;
  }
  return llvm::dyn_cast<clang::EnumConstantDecl>(decl_ref_expr->getDecl()) !=
         nullptr;
}
bool MutateVisitor::IsArgumentToArgumentDependentLookupCall(
    const clang::Expr& expr) const {
  const auto* call_expr = GetFirstParentOfType<clang::CallExpr>(
      expr, compiler_instance_->getASTContext());
  return call_expr != nullptr && call_expr->usesADL();
}

bool MutateVisitor::MutatingMayAffectArgumentDependentLookup(
    const clang::Expr& expr) const {
  // We consider two scenarios where mutation should be avoided because it might
  // spoil argument-dependent lookup.
  //
  // The first is where a function expects an
  // integer argument and an integer-compatible enumeration from another
  // namespace is supplied. Argument-dependent lookup brings said namespace into
  // scope. Replacing the argument with a mutator function with integer return
  // type breaks this.
  //
  // The second is similar, but where the integer is obtained
  // from an overloaded operator that allows an instance of a class to be
  // converted to a primitive value. This brings the class's namespace into
  // scope for argument-dependent lookup, and mutation must not prohibit this.

  // If an implicit cast either converts from a type that would not normally be
  // supported, or converts from the result of a C++ member call, and if this is
  // directly under an ADL call, then inserting a mutator function may affect
  // the results of ADL, and is therefore avoided. This is conservative,
  // as this particular argument might not contribute to the result of ADL.
  if (const auto* implicit_cast =
          llvm::dyn_cast<clang::ImplicitCastExpr>(&expr)) {
    if (!IsTypeSupported(implicit_cast->getSubExpr()->getType()) ||
        llvm::dyn_cast<clang::CXXMemberCallExpr>(implicit_cast->getSubExpr()) !=
            nullptr) {
      if (IsArgumentToArgumentDependentLookupCall(expr)) {
        return true;
      }
    }
  }

  // A C++ member call expression that is directly under an ADL call (possibly
  // with an intervening implicit cast) may affect the outcome of ADL. Mutation
  // of such an expression should therefore be avoided. This is conservative, as
  // this particular argument might not contribute to the result of ADL.
  if (const auto* member_call_expr =
          llvm::dyn_cast<clang::CXXMemberCallExpr>(&expr)) {
    const clang::Expr* possible_adl_call_argument = member_call_expr;
    if (const auto* implicit_cast =
            GetFirstParentOfType<clang::ImplicitCastExpr>(
                *possible_adl_call_argument,
                compiler_instance_->getASTContext())) {
      possible_adl_call_argument = implicit_cast;
    }
    if (IsArgumentToArgumentDependentLookupCall(*possible_adl_call_argument)) {
      return true;
    }
  }

  return false;
}

bool MutateVisitor::MayDependOnLifetimeOfMaterializedTemporaryStorage(
    const clang::Expr& expr) {
  if (llvm::dyn_cast<clang::MaterializeTemporaryExpr>(&expr) != nullptr) {
    // Base case: this is a materialized temporary.
    return true;
  }

  if (const auto* member_expr = llvm::dyn_cast<clang::MemberExpr>(&expr)) {
    // Consider a member expression whose base expression may depend on the
    // lifetime of a materialized temporary. If the member expression yields
    // an lvalue or a pointer, then the member expression itself may depend on
    // the lifetime of the materialized temporary.
    if (member_expr->isLValue() || member_expr->getType()->isPointerType()) {
      return MayDependOnLifetimeOfMaterializedTemporaryStorage(
          *member_expr->getBase());
    }
    return false;
  }

  if (const auto* call_expr = llvm::dyn_cast<clang::CallExpr>(&expr)) {
    // Consider a calll expression that returns an lvalue or pointer. If some
    // argument to the call depends on the lifetime of a materialized temporary
    // then the overall call expression may do so too. In practice this occurs
    // in the context of the [] operator on a std::vector. When the target
    // vector is a temporary, the [] operator may yield a reference into
    // storage owned by the temporary.
    if (call_expr->isLValue() || call_expr->getType()->isPointerType()) {
      for (const auto* arg : call_expr->arguments()) {
        if (MayDependOnLifetimeOfMaterializedTemporaryStorage(*arg)) {
          return true;
        }
      }
    }
    return false;
  }

  return false;
}

void MutateVisitor::SaveConstantTemplateArgumentForRewrite(
    const clang::TemplateSpecializationType* template_specialization_type) {
  for (auto template_argument :
       template_specialization_type->template_arguments()) {
    switch (template_argument.getKind()) {
      case clang::TemplateArgument::Expression:
        // Template Argument is Expression
        constant_arguments_to_rewrite_.push_back(template_argument.getAsExpr());
        break;
      case clang::TemplateArgument::Type:
        // Template Argument is Type, which could be a template specialisation.
        if (const auto* nested_template_specialization_type =
                template_argument.getAsType()
                    ->getAs<clang::TemplateSpecializationType>()) {
          SaveConstantTemplateArgumentForRewrite(
              nested_template_specialization_type);
        }
        break;
      default:
        // We do not check for `clang::TemplateArgument::Integral`, as it is
        // always represented as `TemplateArgument::Expression` in
        // `TemplateSpecializationType`,
        break;
    }
  }
}

}  // namespace dredd
