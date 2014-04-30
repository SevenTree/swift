//===--- Constraint.cpp - Constraint in the Type Checker --------*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2015 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// This file implements the \c Constraint class and its related types,
// which is used by the constraint-based type checker to describe a
// constraint that must be solved.
//
//===----------------------------------------------------------------------===//
#include "Constraint.h"
#include "ConstraintSystem.h"
#include "swift/AST/Types.h"
#include "swift/Basic/Fallthrough.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>

using namespace swift;
using namespace constraints;

Constraint::Constraint(ConstraintKind kind, ArrayRef<Constraint *> constraints,
                       ConstraintLocator *locator, 
                       ArrayRef<TypeVariableType *> typeVars)
  : Kind(kind), HasRestriction(false), HasFix(false), IsActive(false),
    RememberChoice(false), NumTypeVariables(typeVars.size()),
    Nested(constraints), Locator(locator)
{
  assert(kind == ConstraintKind::Conjunction ||
         kind == ConstraintKind::Disjunction);
  std::copy(typeVars.begin(), typeVars.end(), getTypeVariablesBuffer().begin());
}

Constraint::Constraint(ConstraintKind Kind, Type First, Type Second, 
                       DeclName Member, ConstraintLocator *locator,
                       ArrayRef<TypeVariableType *> typeVars)
  : Kind(Kind), HasRestriction(false), HasFix(false), IsActive(false),
    RememberChoice(false), NumTypeVariables(typeVars.size()),
    Types { First, Second, Member }, Locator(locator)
{
  switch (Kind) {
  case ConstraintKind::Bind:
  case ConstraintKind::Equal:
  case ConstraintKind::Subtype:
  case ConstraintKind::Conversion:
  case ConstraintKind::ArgumentTupleConversion:
  case ConstraintKind::OperatorConversion:
  case ConstraintKind::Construction:
  case ConstraintKind::ConformsTo:
  case ConstraintKind::CheckedCast:
  case ConstraintKind::SelfObjectOfProtocol:
  case ConstraintKind::DynamicTypeOf:
    assert(!First.isNull());
    assert(!Second.isNull());
    assert(!Member && "Relational constraint cannot have a member");
    break;
  case ConstraintKind::ApplicableFunction:
    assert(First->is<FunctionType>()
           && "The left-hand side type should be a function type");
    assert(!Member && "Relational constraint cannot have a member");
    break;

  case ConstraintKind::TypeMember:
  case ConstraintKind::ValueMember:
    assert(Member && "Member constraint has no member");
    break;

  case ConstraintKind::Archetype:
  case ConstraintKind::Class:
  case ConstraintKind::DynamicLookupValue:
    assert(!Member && "Type property cannot have a member");
    assert(Second.isNull() && "Type property with second type");
    break;

  case ConstraintKind::BindOverload:
    llvm_unreachable("Wrong constructor for overload binding constraint");

  case ConstraintKind::Conjunction:
    llvm_unreachable("Conjunction constraints should use create()");

  case ConstraintKind::Disjunction:
    llvm_unreachable("Disjunction constraints should use create()");
  }

  std::copy(typeVars.begin(), typeVars.end(), getTypeVariablesBuffer().begin());
}

Constraint::Constraint(Type type, OverloadChoice choice, 
                       ConstraintLocator *locator,
                       ArrayRef<TypeVariableType *> typeVars)
  : Kind(ConstraintKind::BindOverload),
    HasRestriction(false), HasFix(false), IsActive(false),
    RememberChoice(false), NumTypeVariables(typeVars.size()),
    Overload{type, choice}, Locator(locator)
{ 
  std::copy(typeVars.begin(), typeVars.end(), getTypeVariablesBuffer().begin());
}

Constraint::Constraint(ConstraintKind kind, 
                       ConversionRestrictionKind restriction,
                       Type first, Type second, ConstraintLocator *locator,
                       ArrayRef<TypeVariableType *> typeVars)
    : Kind(kind), Restriction(restriction),
      HasRestriction(true), HasFix(false), IsActive(false),
      RememberChoice(false), NumTypeVariables(typeVars.size()),
      Types{ first, second, Identifier() }, Locator(locator)
{
  assert(!first.isNull());
  assert(!second.isNull());
  std::copy(typeVars.begin(), typeVars.end(), getTypeVariablesBuffer().begin());
}

Constraint::Constraint(ConstraintKind kind, Fix fix,
                       Type first, Type second, ConstraintLocator *locator,
                       ArrayRef<TypeVariableType *> typeVars)
  : Kind(kind), TheFix(fix.getKind()), HasRestriction(false), HasFix(true),
    IsActive(false), RememberChoice(false), NumTypeVariables(typeVars.size()),
    Types{ first, second, Identifier() }, Locator(locator)
{
  assert(!first.isNull());
  assert(!second.isNull());
  std::copy(typeVars.begin(), typeVars.end(), getTypeVariablesBuffer().begin());
}

ProtocolDecl *Constraint::getProtocol() const {
  assert((Kind == ConstraintKind::ConformsTo ||
          Kind == ConstraintKind::SelfObjectOfProtocol)
          && "Not a conformance constraint");
  return Types.Second->castTo<ProtocolType>()->getDecl();
}

void Constraint::print(llvm::raw_ostream &Out, SourceManager *sm) const {
  if (Kind == ConstraintKind::Conjunction ||
      Kind == ConstraintKind::Disjunction) {
    bool isConjunction = (Kind == ConstraintKind::Conjunction);
    if (isConjunction) {
      Out << "conjunction";
    } else {
      Out << "disjunction";
      if (shouldRememberChoice())
        Out << " (remembered)";
    }
    if (Locator) {
      Out << " [[";
      Locator->dump(sm, Out);
      Out << "]]";
    }
    Out << ":";

    bool first = true;
    for (auto constraint : getNestedConstraints()) {
      if (first)
        first = false;
      else if (isConjunction)
        Out << " and ";
      else
        Out << " or ";

      constraint->print(Out, sm);
    }

    return;
  }

  Types.First->print(Out);

  bool skipSecond = false;

  switch (Kind) {
  case ConstraintKind::Bind: Out << " := "; break;
  case ConstraintKind::Equal: Out << " == "; break;
  case ConstraintKind::Subtype: Out << " < "; break;
  case ConstraintKind::Conversion: Out << " <c "; break;
  case ConstraintKind::ArgumentTupleConversion: Out << " <ac "; break;
  case ConstraintKind::OperatorConversion: Out << " <oc "; break;
  case ConstraintKind::Construction: Out << " <C "; break;
  case ConstraintKind::ConformsTo: Out << " conforms to "; break;
  case ConstraintKind::CheckedCast: Out << " checked cast to "; break;
  case ConstraintKind::SelfObjectOfProtocol: Out << " Self type of "; break;
  case ConstraintKind::ApplicableFunction: Out << " ==Fn "; break;
  case ConstraintKind::DynamicTypeOf: Out << " dynamicType type of "; break;
  case ConstraintKind::BindOverload: {
    Out << " bound to ";
    auto overload = getOverloadChoice();
    auto printDecl = [&] {
      auto decl = overload.getDecl();
      decl->print(Out);
      if (!sm || !decl->getLoc().isValid()) return;
      Out << " at ";
      decl->getLoc().print(Out, *sm);
    };

    switch (overload.getKind()) {
    case OverloadChoiceKind::Decl:
      Out << "decl ";
      printDecl();
      break;
    case OverloadChoiceKind::TypeDecl:
      Out << "type decl ";
      printDecl();
      break;
    case OverloadChoiceKind::DeclViaDynamic:
      Out << "decl-via-dynamic ";
      printDecl();
      break;
    case OverloadChoiceKind::BaseType:
      Out << "base type";
      break;
    case OverloadChoiceKind::TupleIndex:
      Out << "tuple index " << overload.getTupleIndex();
      break;
    }

    skipSecond = true;
    break;
  }

  case ConstraintKind::ValueMember:
    Out << "[." << Types.Member << ": value] == ";
    break;
  case ConstraintKind::TypeMember:
    Out << "[." << Types.Member << ": type] == ";
    break;
  case ConstraintKind::Archetype:
    Out << " is an archetype";
    skipSecond = true;
    break;
  case ConstraintKind::Class:
    Out << " is a class";
    skipSecond = true;
    break;
  case ConstraintKind::DynamicLookupValue:
    Out << " is an AnyObject value";
    skipSecond = true;
    break;
  case ConstraintKind::Conjunction:
  case ConstraintKind::Disjunction:
    llvm_unreachable("Conjunction/disjunction handled above");
  }

  if (!skipSecond)
    Types.Second->print(Out);

  if (auto restriction = getRestriction()) {
    Out << ' ' << getName(*restriction);
  }

  if (auto fix = getFix()) {
    Out << ' ';
    fix->print(Out);
  }

  if (Locator) {
    Out << " [[";
    Locator->dump(sm, Out);
    Out << "]];";
  }
}

void Constraint::dump(SourceManager *sm) const {
  print(llvm::errs(), sm);
}

StringRef swift::constraints::getName(ConversionRestrictionKind kind) {
  switch (kind) {
  case ConversionRestrictionKind::TupleToTuple:
    return "[tuple-to-tuple]";
  case ConversionRestrictionKind::ScalarToTuple:
    return "[scalar-to-tuple]";
  case ConversionRestrictionKind::TupleToScalar:
    return "[tuple-to-scalar]";
  case ConversionRestrictionKind::DeepEquality:
    return "[deep equality]";
  case ConversionRestrictionKind::Superclass:
    return "[superclass]";
  case ConversionRestrictionKind::LValueToRValue:
    return "[lvalue-to-rvalue]";
  case ConversionRestrictionKind::Existential:
    return "[existential]";
  case ConversionRestrictionKind::ValueToOptional:
    return "[value-to-optional]";
  case ConversionRestrictionKind::OptionalToOptional:
    return "[optional-to-optional]";
  case ConversionRestrictionKind::UncheckedOptionalToOptional:
    return "[unchecked-optional-to-optional]";
  case ConversionRestrictionKind::OptionalToUncheckedOptional:
    return "[optional-to-unchecked-optional]";
  case ConversionRestrictionKind::ForceUnchecked:
    return "[force-unchecked]";
  case ConversionRestrictionKind::ArrayUpcast:
    return "[array-upcast]";
  case ConversionRestrictionKind::User:
    return "[user]";
  }
  llvm_unreachable("bad conversion restriction kind");
}

StringRef Fix::getName(FixKind kind) {
  switch (kind) {
  case FixKind::None:
    return "[prevent fixes]";
  case FixKind::NullaryCall:
    return "[fix: add nullary call]";
  case FixKind::ForceOptional:
    return "[fix: force optional]";
  case FixKind::ForceDowncast:
    return "[fix: force downcast]";
  case FixKind::AddressOf:
    return "[fix: add address-of]";
  case FixKind::RemoveNullaryCall:
    return "[fix: remove nullary call]";
  }
}

void Fix::print(llvm::raw_ostream &Out) const {
  Out << getName(getKind());
}

void Fix::dump() const {
  print(llvm::errs());
  llvm::errs() << "\n";
}

/// Recursively gather the set of type variables referenced by this constraint.
static void
gatherReferencedTypeVars(Constraint *constraint,
                         SmallVectorImpl<TypeVariableType *> &typeVars) {
  switch (constraint->getKind()) {
  case ConstraintKind::Conjunction:
  case ConstraintKind::Disjunction:
    for (auto nested : constraint->getNestedConstraints())
      gatherReferencedTypeVars(nested, typeVars);
    return;

  case ConstraintKind::ApplicableFunction:
  case ConstraintKind::Bind:
  case ConstraintKind::Construction:
  case ConstraintKind::Conversion:
  case ConstraintKind::ArgumentTupleConversion:
  case ConstraintKind::OperatorConversion:
  case ConstraintKind::CheckedCast:
  case ConstraintKind::Equal:
  case ConstraintKind::Subtype:
  case ConstraintKind::TypeMember:
  case ConstraintKind::ValueMember:
  case ConstraintKind::DynamicTypeOf:
    constraint->getSecondType()->getTypeVariables(typeVars);
    SWIFT_FALLTHROUGH;

  case ConstraintKind::Archetype:
  case ConstraintKind::BindOverload:
  case ConstraintKind::Class:
  case ConstraintKind::ConformsTo:
  case ConstraintKind::DynamicLookupValue:
  case ConstraintKind::SelfObjectOfProtocol:
    constraint->getFirstType()->getTypeVariables(typeVars);

    // Special case: the base type of an overloading binding.
    if (constraint->getKind() == ConstraintKind::BindOverload) {
      if (auto baseType = constraint->getOverloadChoice().getBaseType()) {
        baseType->getTypeVariables(typeVars);
      }
    }

    break;
  }
}

/// Unique the given set of type variables.
static void uniqueTypeVariables(SmallVectorImpl<TypeVariableType *> &typeVars) {
  // Remove any duplicate type variables.
  llvm::SmallPtrSet<TypeVariableType *, 4> knownTypeVars;
  typeVars.erase(std::remove_if(typeVars.begin(), typeVars.end(),
                                [&](TypeVariableType *typeVar) {
                                  return !knownTypeVars.insert(typeVar);
                                }),
                 typeVars.end());
}

Constraint *Constraint::create(ConstraintSystem &cs, ConstraintKind kind, 
                               Type first, Type second, DeclName member,
                               ConstraintLocator *locator) {
  // Collect type variables.
  SmallVector<TypeVariableType *, 4> typeVars;
  if (first->hasTypeVariable())
    first->getTypeVariables(typeVars);
  if (second && second->hasTypeVariable())
    second->getTypeVariables(typeVars);
  uniqueTypeVariables(typeVars);

  // Create the constraint.
  unsigned size = sizeof(Constraint) 
                + typeVars.size() * sizeof(TypeVariableType*);
  void *mem = cs.getAllocator().Allocate(size, alignof(Constraint));
  return new (mem) Constraint(kind, first, second, member, locator, typeVars);
}

Constraint *Constraint::createBindOverload(ConstraintSystem &cs, Type type, 
                                           OverloadChoice choice, 
                                           ConstraintLocator *locator) {
  // Collect type variables.
  SmallVector<TypeVariableType *, 4> typeVars;
  if (type->hasTypeVariable())
    type->getTypeVariables(typeVars);
  if (auto baseType = choice.getBaseType()) {
    baseType->getTypeVariables(typeVars);
  }

  // Create the constraint.
  unsigned size = sizeof(Constraint) 
                + typeVars.size() * sizeof(TypeVariableType*);
  void *mem = cs.getAllocator().Allocate(size, alignof(Constraint));
  return new (mem) Constraint(type, choice, locator, typeVars);
}

Constraint *Constraint::createRestricted(ConstraintSystem &cs, 
                                         ConstraintKind kind, 
                                         ConversionRestrictionKind restriction,
                                         Type first, Type second, 
                                         ConstraintLocator *locator) {
  // Collect type variables.
  SmallVector<TypeVariableType *, 4> typeVars;
  if (first->hasTypeVariable())
    first->getTypeVariables(typeVars);
  if (second->hasTypeVariable())
    second->getTypeVariables(typeVars);
  uniqueTypeVariables(typeVars);

  // Create the constraint.
  unsigned size = sizeof(Constraint) 
                + typeVars.size() * sizeof(TypeVariableType*);
  void *mem = cs.getAllocator().Allocate(size, alignof(Constraint));
  return new (mem) Constraint(kind, restriction, first, second, locator,
                              typeVars);
}

Constraint *Constraint::createFixed(ConstraintSystem &cs, ConstraintKind kind,
                                    Fix fix,
                                    Type first, Type second,
                                    ConstraintLocator *locator) {
  // Collect type variables.
  SmallVector<TypeVariableType *, 4> typeVars;
  if (first->hasTypeVariable())
    first->getTypeVariables(typeVars);
  if (second->hasTypeVariable())
    second->getTypeVariables(typeVars);
  uniqueTypeVariables(typeVars);

  // Create the constraint.
  unsigned size = sizeof(Constraint)
  + typeVars.size() * sizeof(TypeVariableType*);
  void *mem = cs.getAllocator().Allocate(size, alignof(Constraint));
  return new (mem) Constraint(kind, fix, first, second, locator, typeVars);
}

Constraint *Constraint::createConjunction(ConstraintSystem &cs,
                                          ArrayRef<Constraint *> constraints,
                                          ConstraintLocator *locator) {
  // Unwrap any conjunctions inside the conjunction constraint.
  SmallVector<TypeVariableType *, 4> typeVars;
  bool unwrappedAny = false;
  SmallVector<Constraint *, 1> unwrapped;
  unsigned index = 0;
  for (auto constraint : constraints) {
    // Gather type variables from this constraint.
    gatherReferencedTypeVars(constraint, typeVars);

    // If we have a nested conjunction, unwrap it.
    if (constraint->getKind() == ConstraintKind::Conjunction) {
      // If we haven't unwrapped anything before, copy all of the constraints
      // we skipped.
      if (!unwrappedAny) {
        unwrapped.append(constraints.begin(), constraints.begin() + index);
        unwrappedAny = true;
      }

      // Add all of the constraints in the conjunction.
      unwrapped.append(constraint->getNestedConstraints().begin(),
                       constraint->getNestedConstraints().end());
    } else if (unwrappedAny) {
      // Since we unwrapped constraints before, add this constraint.
      unwrapped.push_back(constraint);
    }

    // FIXME: If we find any disjunctions in here, should we distribute them?

    ++index;
  }

  // If we unwrapped anything, our list of constraints is the unwrapped list.
  if (unwrappedAny)
    constraints = unwrapped;

  assert(!constraints.empty() && "Empty conjunction constraint");

  // If there is a single constraint, this isn't a disjunction at all.
  if (constraints.size() == 1)
    return constraints.front();

  // Create the conjunction constraint.
  uniqueTypeVariables(typeVars);
  unsigned size = sizeof(Constraint) 
                + typeVars.size() * sizeof(TypeVariableType*);
  void *mem = cs.getAllocator().Allocate(size, alignof(Constraint));
  return new (mem) Constraint(ConstraintKind::Conjunction,
                              cs.allocateCopy(constraints), locator, typeVars);

}

Constraint *Constraint::createDisjunction(ConstraintSystem &cs,
                                          ArrayRef<Constraint *> constraints,
                                          ConstraintLocator *locator,
                                          RememberChoice_t rememberChoice) {
  // Unwrap any disjunctions inside the disjunction constraint; we only allow
  // disjunctions at the top level.
  SmallVector<TypeVariableType *, 4> typeVars;
  bool unwrappedAny = false;
  SmallVector<Constraint *, 1> unwrapped;
  unsigned index = 0;
  for (auto constraint : constraints) {
    // Gather type variables from this constraint.
    gatherReferencedTypeVars(constraint, typeVars);

    // If we have a nested disjunction, unwrap it.
    if (constraint->getKind() == ConstraintKind::Disjunction) {
      // If we haven't unwrapped anything before, copy all of the constraints
      // we skipped.
      if (!unwrappedAny) {
        unwrapped.append(constraints.begin(), constraints.begin() + index);
        unwrappedAny = true;
      }

      // Add all of the constraints in the disjunction.
      unwrapped.append(constraint->getNestedConstraints().begin(),
                       constraint->getNestedConstraints().end());
    } else if (unwrappedAny) {
      // Since we unwrapped constraints before, add this constraint.
      unwrapped.push_back(constraint);
    }
    ++index;
  }

  // If we unwrapped anything, our list of constraints is the unwrapped list.
  if (unwrappedAny)
    constraints = unwrapped;

  assert(!constraints.empty() && "Empty disjunction constraint");

  // If there is a single constraint, this isn't a disjunction at all.
  if (constraints.size() == 1) {
    assert(!rememberChoice && "simplified an important disjunction?");
    return constraints.front();
  }

  // Create the disjunction constraint.
  uniqueTypeVariables(typeVars);
  unsigned size = sizeof(Constraint) 
                + typeVars.size() * sizeof(TypeVariableType*);
  void *mem = cs.getAllocator().Allocate(size, alignof(Constraint));
  auto disjunction =  new (mem) Constraint(ConstraintKind::Disjunction,
                              cs.allocateCopy(constraints), locator, typeVars);
  disjunction->RememberChoice = (bool) rememberChoice;
  return disjunction;
}

void *Constraint::operator new(size_t bytes, ConstraintSystem& cs,
                               size_t alignment) {
  return ::operator new (bytes, cs, alignment);
}
