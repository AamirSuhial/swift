//===--- ASTScopeSourceRange.cpp - Swift Object-Oriented AST Scope --------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// This file implements the source range queries for the ASTScopeImpl ontology.
//
//===----------------------------------------------------------------------===//
#include "swift/AST/ASTScope.h"

#include "swift/AST/ASTContext.h"
#include "swift/AST/ASTWalker.h"
#include "swift/AST/Decl.h"
#include "swift/AST/Expr.h"
#include "swift/AST/Initializer.h"
#include "swift/AST/LazyResolver.h"
#include "swift/AST/Module.h"
#include "swift/AST/ParameterList.h"
#include "swift/AST/Pattern.h"
#include "swift/AST/Stmt.h"
#include "swift/AST/TypeRepr.h"
#include "swift/Basic/STLExtras.h"
#include "llvm/Support/Compiler.h"
#include <algorithm>

using namespace swift;
using namespace ast_scope;

static SourceLoc getStartOfFirstParam(ClosureExpr *closure);

SourceRange ASTScopeImpl::getSourceRange(bool forDebugging) const {
  if (forDebugging && !cachedSourceRange)
    return SourceRange();
  assert(cachedSourceRange && "should have been cached after last expandMe");
  return *cachedSourceRange;
}

SourceRange ASTScopeImpl::getUncachedSourceRange(bool forDebugging) const {
  auto childlessRange = getChildlessSourceRange();
  if (sourceRangeOfIgnoredASTNodes.isValid())
    childlessRange.widen(sourceRangeOfIgnoredASTNodes);
  if (getChildren().empty()) {
    assert(childlessRange.Start.isValid());
    return childlessRange;
  }
  const auto childStart =
      getChildren().front()->getSourceRange(forDebugging).Start;
  assert(forDebugging || childStart.isValid());
  const auto childEnd = getChildren().back()->getSourceRange(forDebugging).End;
  auto childRange = SourceRange(childStart, childEnd);
  if (childlessRange.Start.isInvalid())
    return childRange;
  auto r = childRange;
  r.widen(childlessRange);
  return r;
}

#pragma mark validation

bool ASTScopeImpl::verifySourceRange() const {
  return verifyThatChildrenAreContained() &&
         verifyThatThisNodeComeAfterItsPriorSibling();
}

bool ASTScopeImpl::hasValidSourceRange() const {
  const auto sourceRange = getSourceRange();
  return sourceRange.Start.isValid() && sourceRange.End.isValid() &&
         !getSourceManager().isBeforeInBuffer(sourceRange.End,
                                              sourceRange.Start);
}

bool ASTScopeImpl::precedesInSource(const ASTScopeImpl *next) const {
  if (!hasValidSourceRange() || !next->hasValidSourceRange())
    return false;
  return !getSourceManager().isBeforeInBuffer(next->getSourceRange().Start,
                                              getSourceRange().End);
}

bool ASTScopeImpl::verifyThatChildrenAreContained() const {
  // assumes children are already in order
  if (getChildren().empty())
    return true;
  const SourceRange rangeOfChildren =
      SourceRange(getChildren().front()->getSourceRange().Start,
                  getChildren().back()->getSourceRange().End);
  if (getSourceManager().rangeContains(getSourceRange(), rangeOfChildren))
    return true;
  auto &out = verificationError() << "children not contained in its parent\n";
  if (getChildren().size() == 1) {
    out << "\n***Only Child node***\n";
    getChildren().front()->print(out);
  } else {
    out << "\n***First Child node***\n";
    getChildren().front()->print(out);
    out << "\n***Last Child node***\n";
    getChildren().back()->print(out);
  }
  out << "\n***Parent node***\n";
  this->print(out);
  abort();
}

bool ASTScopeImpl::verifyThatThisNodeComeAfterItsPriorSibling() const {
  auto priorSibling = getPriorSibling();
  if (!priorSibling)
    return true;
  if (priorSibling.get()->precedesInSource(this))
    return true;
  auto &out = verificationError() << "unexpected out-of-order nodes\n";
  out << "\n***Penultimate child node***\n";
  priorSibling.get()->print(out);
  out << "\n***Last Child node***\n";
  print(out);
  out << "\n***Parent node***\n";
  getParent().get()->print(out);
  abort();
}

NullablePtr<ASTScopeImpl> ASTScopeImpl::getPriorSibling() const {
  auto parent = getParent();
  if (!parent)
    return nullptr;
  auto const &siblingsAndMe = parent.get()->getChildren();
  // find myIndex, which is probably the last one
  int myIndex = -1;
  for (int i = siblingsAndMe.size() - 1; i >= 0; --i) {
    if (siblingsAndMe[i] == this) {
      myIndex = i;
      break;
    }
  }
  assert(myIndex != -1 && "I have been disowned!");
  if (myIndex == 0)
    return nullptr;
  return siblingsAndMe[myIndex - 1];
}

#pragma mark getChildlessSourceRange


SourceRange SpecializeAttributeScope::getChildlessSourceRange() const {
  return specializeAttr->getRange();
}

SourceRange AbstractFunctionBodyScope::getChildlessSourceRange() const {
  return decl->getBodySourceRange();
}

SourceRange
StatementConditionElementPatternScope::getChildlessSourceRange() const {
  return pattern->getSourceRange();
}

SourceRange TopLevelCodeScope::getChildlessSourceRange() const {
  return decl->getSourceRange();
}

SourceRange SubscriptDeclScope::getChildlessSourceRange() const {
  return decl->getSourceRange();
}

SourceRange WholeClosureScope::getChildlessSourceRange() const {
  return closureExpr->getSourceRange();
}

SourceRange AbstractStmtScope::getChildlessSourceRange() const {
  return getStmt()->getSourceRange();
}

SourceRange DefaultArgumentInitializerScope::getChildlessSourceRange() const {
  return decl->getDefaultValue()->getSourceRange();
}

SourceRange PatternEntryDeclScope::getChildlessSourceRange() const {
  return getPatternEntry().getSourceRange();
}

SourceRange PatternEntryInitializerScope::getChildlessSourceRange() const {
  return getPatternEntry().getInitAsWritten()->getSourceRange();
}

SourceRange PatternEntryUseScope::getChildlessSourceRange() const {
  auto range =
      SourceRange(getPatternEntry().getSourceRange(/*omitAccessors*/ true).End,
                  getPatternEntry().getSourceRange().End);
  if (initializerEnd.isValid()) {
    // Sigh... If there's a correspinding initializer scope, its range may be
    // wider than the pattern decl indicates if it ends in an interpolated
    // string literal or editor placeholder.
    range.widen(SourceRange(initializerEnd));
    range.Start = initializerEnd;
  }
  return range;
}

SourceRange VarDeclScope::getChildlessSourceRange() const {
  return decl->getBracesRange();
}

SourceRange GenericParamScope::getChildlessSourceRange() const {
  auto nOrE = holder;
  // A protocol's generic parameter list is not written in source, and
  // is visible from the start of the body.
  if (auto *protoDecl = dyn_cast<ProtocolDecl>(nOrE))
    return SourceRange(protoDecl->getBraces().Start, protoDecl->getEndLoc());
  // Explicitly-written generic parameters are in scope *following* their
  // definition and through the end of the body.
  // ensure that this isn't an extension where there is no end loc
  auto startLoc = paramList->getParams()[index]->getEndLoc();
  if (startLoc.isInvalid())
    startLoc = holder->getStartLoc();
  return SourceRange(startLoc, holder->getEndLoc());
}

SourceRange ASTSourceFileScope::getChildlessSourceRange() const {
  if (auto bufferID = SF->getBufferID()) {
    auto charRange = getSourceManager().getRangeForBuffer(*bufferID);
    return SourceRange(charRange.getStart(), charRange.getEnd());
  }

  if (SF->Decls.empty())
    return SourceRange();

  // Use the source ranges of the declarations in the file.
  return SourceRange(SF->Decls.front()->getStartLoc(),
                     SF->Decls.back()->getEndLoc());
}

SourceRange GTXScope::getChildlessSourceRange() const {
  return portion->getChildlessSourceRangeOf(this);
}

SourceRange
GTXWholePortion::getChildlessSourceRangeOf(const GTXScope *scope) const {
  auto *d = scope->getDecl().get();
  auto r = d->getSourceRangeIncludingAttrs();
  if (r.Start.isValid()) {
    assert(r.End.isValid());
    return r;
  }
  return d->getSourceRange();
}

SourceRange
GTXWherePortion::getChildlessSourceRangeOf(const GTXScope *scope) const {
  return scope->getGenericContext()->getTrailingWhereClause()->getSourceRange();
}

SourceRange IterableTypeBodyPortion::getChildlessSourceRangeOf(
    const GTXScope *scope) const {
  auto *d = scope->getDecl().get();
  if (auto *nt = dyn_cast<NominalTypeDecl>(d))
    return nt->getBraces();
  if (auto *e = dyn_cast<ExtensionDecl>(d))
    return e->getBraces();
  llvm_unreachable("No body!");
}

SourceRange AbstractFunctionDeclScope::getChildlessSourceRange() const {
  // For a get/put accessor	 all of the parameters are implicit, so start
  // them at the start location of the accessor.
  auto r = decl->getSourceRangeIncludingAttrs();
  if (r.Start.isValid()) {
    assert(r.End.isValid());
    return r;
  }
  return decl->getBody()->getSourceRange();
}

SourceRange AbstractFunctionParamsScope::getChildlessSourceRange() const {
  auto *fn = getEnclosingAbstractFunctionOrSubscriptDecl();
  SourceLoc endLoc = fn->getEndLoc();

  // FIXME: Why oh why don't deinitializers have a parameter list?

  // clang-format off
  SourceLoc startLoc =
  isa<AccessorDecl>(fn)         ? fn->getLoc() :
  isa<DestructorDecl>(fn)       ? dyn_cast<DestructorDecl>(fn)->getNameLoc() :
  isa<SubscriptDecl>(fn)        ? dyn_cast<SubscriptDecl>(fn)->getIndices()->getLParenLoc() :
  isa<AbstractFunctionDecl>(fn) ? dyn_cast<AbstractFunctionDecl>(fn)->getParameters()->getLParenLoc() :
                                    SourceLoc();
  // clang-format on

  assert(startLoc.isValid());
  return SourceRange(startLoc, endLoc);
}


SourceRange ForEachPatternScope::getChildlessSourceRange() const {
  // The scope of the pattern extends from the 'where' expression (if present)
  // until the end of the body.
  if (stmt->getWhere())
    return SourceRange(stmt->getWhere()->getStartLoc(),
                       stmt->getBody()->getEndLoc());

  // Otherwise, scope of the pattern covers the body.
  return stmt->getBody()->getSourceRange();
}

SourceRange CatchStmtScope::getChildlessSourceRange() const {
  // The scope of the pattern extends from the 'where' (if present)
  // to the end of the body.
  if (stmt->getGuardExpr())
    return SourceRange(stmt->getWhereLoc(), stmt->getBody()->getEndLoc());

  // Otherwise, the scope of the pattern encompasses the body.
  return stmt->getBody()->getSourceRange();
}
SourceRange CaseStmtScope::getChildlessSourceRange() const {
  // The scope of the case statement begins at the first guard expression,
  // if there is one, and extends to the end of the body.
  // FIXME: Figure out what to do about multiple pattern bindings. We might
  // want a more restrictive rule in those cases.
  for (const auto &caseItem : stmt->getCaseLabelItems()) {
    if (auto guardExpr = caseItem.getGuardExpr())
      return SourceRange(guardExpr->getStartLoc(),
                         stmt->getBody()->getEndLoc());
  }

  // Otherwise, it covers the body.
  return stmt->getBody()
      ->getSourceRange(); // The scope of the case statement begins
}

SourceRange BraceStmtScope::getChildlessSourceRange() const {
  // The brace statements that represent closures start their scope at the
  // 'in' keyword, when present.
  if (auto closure = parentClosureIfAny()) {
    if (closure.get()->getInLoc().isValid())
      return SourceRange(closure.get()->getInLoc(), stmt->getEndLoc());
  }
  return stmt->getSourceRange();
}

SourceLoc ConditionalClauseScope::startLocAccordingToCondition() const {
  // Determine the start location if the condition can provide it.
  auto conditionals = getContainingStatement()->getCond();
  const StmtConditionElement cond = conditionals[index];
  switch (cond.getKind()) {
  case StmtConditionElement::CK_Boolean:
  case StmtConditionElement::CK_Availability:
    return cond.getStartLoc();
  case StmtConditionElement::CK_PatternBinding:
    return index + 1 < conditionals.size()
               ? conditionals[index + 1].getStartLoc()
               : SourceLoc();
  }
}

SourceRange WhileConditionalClauseScope::getChildlessSourceRange() const {
  // For 'while' statements, the conditional clause covers the body.
  // If we didn't have a conditional clause to start the new scope, use
  // the beginning of the body.
  auto startLoc = startLocAccordingToCondition();
  if (startLoc.isInvalid())
    startLoc = stmt->getBody()->getStartLoc();
  return SourceRange(startLoc, stmt->getBody()->getEndLoc());
}
SourceRange IfConditionalClauseScope::getChildlessSourceRange() const {
  // For 'if' statements, the conditional clause covers the 'then' branch.
  // If we didn't have a conditional clause to start the new scope, use
  // the beginning of the 'then' clause.
  auto startLoc = startLocAccordingToCondition();
  if (startLoc.isInvalid())
    startLoc = stmt->getThenStmt()->getStartLoc();
  return SourceRange(startLoc, stmt->getThenStmt()->getEndLoc());
}
SourceRange GuardConditionalClauseScope::getChildlessSourceRange() const {
  // For 'guard' statements, the conditional clause.
  // If we didn't have a condition clause to start the new scope, use the
  // end of the guard statement itself.
  auto startLoc = startLocAccordingToCondition();
  if (startLoc.isInvalid())
    startLoc = stmt->getBody()->getStartLoc();
  return SourceRange(startLoc, stmt->getBody()->getStartLoc());
}

SourceRange GuardContinuationScope::getChildlessSourceRange() const {
  // For a guard continuation, the scope extends from the end of the 'else'
  // to the end of the continuation.
  return SourceRange(stmt->getBody()->getEndLoc());
}

SourceRange CaptureListScope::getChildlessSourceRange() const {
  auto *const closure = expr->getClosureBody();
  return SourceRange(expr->getStartLoc(), getStartOfFirstParam(closure));
}

SourceRange ClosureParametersScope::getChildlessSourceRange() const {
  assert(closureExpr->getInLoc().isValid() &&
         "We don't create these if no in loc");
  return SourceRange(getStartOfFirstParam(closureExpr),
                     closureExpr->getInLoc());
}

SourceRange ClosureBodyScope::getChildlessSourceRange() const {
  if (closureExpr->getInLoc().isValid())
    return SourceRange(closureExpr->getInLoc(), closureExpr->getEndLoc());

  return closureExpr->getSourceRange();
}

SourceRange AttachedPropertyDelegateScope::getChildlessSourceRange() const {
  return getCustomAttributesSourceRange(decl);
}

#pragma mark source range caching

void ASTScopeImpl::cacheSourceRange() {
  cachedSourceRange = getUncachedSourceRange();
  verifySourceRange();
}

void ASTScopeImpl::clearSourceRangeCache() { cachedSourceRange = SourceLoc(); }
void ASTScopeImpl::cacheSourceRangesOfAncestors() {
  cacheSourceRange();
  if (auto p = getParent())
    p.get()->cacheSourceRangesOfAncestors();
}
void ASTScopeImpl::clearCachedSourceRangesOfAncestors() {
  clearSourceRangeCache();
  if (auto p = getParent())
    p.get()->clearCachedSourceRangesOfAncestors();
}

#pragma mark ignored nodes and compensating for InterpolatedStringLiteralExprs and EditorPlaceHolders

namespace {
class EffectiveEndFinder : public ASTWalker {
  SourceLoc end;
  const SourceManager &SM;

public:
  EffectiveEndFinder(const SourceManager &SM) : SM(SM) {}

  std::pair<bool, Expr *> walkToExprPre(Expr *E) {
    if (!E)
      return {true, E};
    if (auto *isl = dyn_cast<InterpolatedStringLiteralExpr>(E)) {
      if (end.isInvalid() ||
          SM.isBeforeInBuffer(end, isl->getTrailingQuoteLoc()))
        end = isl->getTrailingQuoteLoc();
    } else if (auto *epl = dyn_cast<EditorPlaceholderExpr>(E)) {
      if (end.isInvalid() ||
          SM.isBeforeInBuffer(end, epl->getTrailingAngleBracketLoc()))
        end = epl->getTrailingAngleBracketLoc();
    }
    return ASTWalker::walkToExprPre(E);
  }
  SourceLoc getTrailingQuoteLoc() const { return end; }
};
} // namespace

SourceRange ASTScopeImpl::getEffectiveSourceRange(const ASTNode n) const {
  if (const auto *d = n.dyn_cast<Decl *>())
    return d->getSourceRange();
  if (const auto *s = n.dyn_cast<Stmt *>())
    return s->getSourceRange();
  auto *e = n.dyn_cast<Expr *>();
  assert(e);
  EffectiveEndFinder finder(getSourceManager());
  e->walk(finder);
  return SourceRange(e->getLoc(), finder.getTrailingQuoteLoc().isValid()
                                      ? finder.getTrailingQuoteLoc()
                                      : e->getEndLoc());
}

void ASTScopeImpl::widenSourceRangeForIgnoredASTNode(const ASTNode n) {
  // The pattern scopes will include the source ranges for VarDecls.
  // Doing the default here would cause a pattern initializer scope's range
  // to overlap the pattern use scope's range.

  if (AbstractPatternEntryScope::isCreatedDirectly(n))
    return;

  SourceRange r = getEffectiveSourceRange(n);
  if (r.isInvalid())
    return;
  if (sourceRangeOfIgnoredASTNodes.isInvalid())
    sourceRangeOfIgnoredASTNodes = r;
  else
    sourceRangeOfIgnoredASTNodes.widen(r);
}

static SourceLoc getStartOfFirstParam(ClosureExpr *closure) {
  if (auto *parms = closure->getParameters()) {
    if (parms->size())
      return parms->get(0)->getStartLoc();
  }
  if (closure->getInLoc().isValid())
    return closure->getInLoc();
  if (closure->getBody())
    return closure->getBody()->getLBraceLoc();
  return closure->getStartLoc();
}
