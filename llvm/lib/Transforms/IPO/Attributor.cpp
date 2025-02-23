//===- Attributor.cpp - Module-wide attribute deduction -------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements an interprocedural pass that deduces and/or propagates
// attributes. This is done in an abstract interpretation style fixpoint
// iteration. See the Attributor.h file comment and the class descriptions in
// that file for more information.
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/IPO/Attributor.h"

#include "llvm/ADT/Statistic.h"
#include "llvm/Analysis/LazyValueInfo.h"
#include "llvm/Analysis/MustExecute.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/NoFolder.h"
#include "llvm/IR/Verifier.h"
#include "llvm/InitializePasses.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/Local.h"

#include <cassert>

using namespace llvm;

#define DEBUG_TYPE "attributor"

STATISTIC(NumFnDeleted, "Number of function deleted");
STATISTIC(NumFnWithExactDefinition,
          "Number of functions with exact definitions");
STATISTIC(NumFnWithoutExactDefinition,
          "Number of functions without exact definitions");
STATISTIC(NumFnShallowWrapperCreated, "Number of shallow wrappers created");
STATISTIC(NumAttributesTimedOut,
          "Number of abstract attributes timed out before fixpoint");
STATISTIC(NumAttributesValidFixpoint,
          "Number of abstract attributes in a valid fixpoint state");
STATISTIC(NumAttributesManifested,
          "Number of abstract attributes manifested in IR");
STATISTIC(NumAttributesFixedDueToRequiredDependences,
          "Number of abstract attributes fixed due to required dependences");

// TODO: Determine a good default value.
//
// In the LLVM-TS and SPEC2006, 32 seems to not induce compile time overheads
// (when run with the first 5 abstract attributes). The results also indicate
// that we never reach 32 iterations but always find a fixpoint sooner.
//
// This will become more evolved once we perform two interleaved fixpoint
// iterations: bottom-up and top-down.
static cl::opt<unsigned>
    MaxFixpointIterations("attributor-max-iterations", cl::Hidden,
                          cl::desc("Maximal number of fixpoint iterations."),
                          cl::init(32));
static cl::opt<bool> VerifyMaxFixpointIterations(
    "attributor-max-iterations-verify", cl::Hidden,
    cl::desc("Verify that max-iterations is a tight bound for a fixpoint"),
    cl::init(false));

static cl::opt<bool> AnnotateDeclarationCallSites(
    "attributor-annotate-decl-cs", cl::Hidden,
    cl::desc("Annotate call sites of function declarations."), cl::init(false));

static cl::opt<unsigned> DepRecInterval(
    "attributor-dependence-recompute-interval", cl::Hidden,
    cl::desc("Number of iterations until dependences are recomputed."),
    cl::init(4));

static cl::opt<bool> EnableHeapToStack("enable-heap-to-stack-conversion",
                                       cl::init(true), cl::Hidden);

static cl::opt<bool>
    AllowShallowWrappers("attributor-allow-shallow-wrappers", cl::Hidden,
                         cl::desc("Allow the Attributor to create shallow "
                                  "wrappers for non-exact definitions."),
                         cl::init(false));

/// Logic operators for the change status enum class.
///
///{
ChangeStatus llvm::operator|(ChangeStatus l, ChangeStatus r) {
  return l == ChangeStatus::CHANGED ? l : r;
}
ChangeStatus llvm::operator&(ChangeStatus l, ChangeStatus r) {
  return l == ChangeStatus::UNCHANGED ? l : r;
}
///}

/// Return true if \p New is equal or worse than \p Old.
static bool isEqualOrWorse(const Attribute &New, const Attribute &Old) {
  if (!Old.isIntAttribute())
    return true;

  return Old.getValueAsInt() >= New.getValueAsInt();
}

/// Return true if the information provided by \p Attr was added to the
/// attribute list \p Attrs. This is only the case if it was not already present
/// in \p Attrs at the position describe by \p PK and \p AttrIdx.
static bool addIfNotExistent(LLVMContext &Ctx, const Attribute &Attr,
                             AttributeList &Attrs, int AttrIdx) {

  if (Attr.isEnumAttribute()) {
    Attribute::AttrKind Kind = Attr.getKindAsEnum();
    if (Attrs.hasAttribute(AttrIdx, Kind))
      if (isEqualOrWorse(Attr, Attrs.getAttribute(AttrIdx, Kind)))
        return false;
    Attrs = Attrs.addAttribute(Ctx, AttrIdx, Attr);
    return true;
  }
  if (Attr.isStringAttribute()) {
    StringRef Kind = Attr.getKindAsString();
    if (Attrs.hasAttribute(AttrIdx, Kind))
      if (isEqualOrWorse(Attr, Attrs.getAttribute(AttrIdx, Kind)))
        return false;
    Attrs = Attrs.addAttribute(Ctx, AttrIdx, Attr);
    return true;
  }
  if (Attr.isIntAttribute()) {
    Attribute::AttrKind Kind = Attr.getKindAsEnum();
    if (Attrs.hasAttribute(AttrIdx, Kind))
      if (isEqualOrWorse(Attr, Attrs.getAttribute(AttrIdx, Kind)))
        return false;
    Attrs = Attrs.removeAttribute(Ctx, AttrIdx, Kind);
    Attrs = Attrs.addAttribute(Ctx, AttrIdx, Attr);
    return true;
  }

  llvm_unreachable("Expected enum or string attribute!");
}

Argument *IRPosition::getAssociatedArgument() const {
  if (getPositionKind() == IRP_ARGUMENT)
    return cast<Argument>(&getAnchorValue());

  // Not an Argument and no argument number means this is not a call site
  // argument, thus we cannot find a callback argument to return.
  int ArgNo = getArgNo();
  if (ArgNo < 0)
    return nullptr;

  // Use abstract call sites to make the connection between the call site
  // values and the ones in callbacks. If a callback was found that makes use
  // of the underlying call site operand, we want the corresponding callback
  // callee argument and not the direct callee argument.
  Optional<Argument *> CBCandidateArg;
  SmallVector<const Use *, 4> CallbackUses;
  const auto &CB = cast<CallBase>(getAnchorValue());
  AbstractCallSite::getCallbackUses(CB, CallbackUses);
  for (const Use *U : CallbackUses) {
    AbstractCallSite ACS(U);
    assert(ACS && ACS.isCallbackCall());
    if (!ACS.getCalledFunction())
      continue;

    for (unsigned u = 0, e = ACS.getNumArgOperands(); u < e; u++) {

      // Test if the underlying call site operand is argument number u of the
      // callback callee.
      if (ACS.getCallArgOperandNo(u) != ArgNo)
        continue;

      assert(ACS.getCalledFunction()->arg_size() > u &&
             "ACS mapped into var-args arguments!");
      if (CBCandidateArg.hasValue()) {
        CBCandidateArg = nullptr;
        break;
      }
      CBCandidateArg = ACS.getCalledFunction()->getArg(u);
    }
  }

  // If we found a unique callback candidate argument, return it.
  if (CBCandidateArg.hasValue() && CBCandidateArg.getValue())
    return CBCandidateArg.getValue();

  // If no callbacks were found, or none used the underlying call site operand
  // exclusively, use the direct callee argument if available.
  const Function *Callee = CB.getCalledFunction();
  if (Callee && Callee->arg_size() > unsigned(ArgNo))
    return Callee->getArg(ArgNo);

  return nullptr;
}

ChangeStatus AbstractAttribute::update(Attributor &A) {
  ChangeStatus HasChanged = ChangeStatus::UNCHANGED;
  if (getState().isAtFixpoint())
    return HasChanged;

  LLVM_DEBUG(dbgs() << "[Attributor] Update: " << *this << "\n");

  HasChanged = updateImpl(A);

  LLVM_DEBUG(dbgs() << "[Attributor] Update " << HasChanged << " " << *this
                    << "\n");

  return HasChanged;
}

ChangeStatus
IRAttributeManifest::manifestAttrs(Attributor &A, const IRPosition &IRP,
                                   const ArrayRef<Attribute> &DeducedAttrs) {
  Function *ScopeFn = IRP.getAnchorScope();
  IRPosition::Kind PK = IRP.getPositionKind();

  // In the following some generic code that will manifest attributes in
  // DeducedAttrs if they improve the current IR. Due to the different
  // annotation positions we use the underlying AttributeList interface.

  AttributeList Attrs;
  switch (PK) {
  case IRPosition::IRP_INVALID:
  case IRPosition::IRP_FLOAT:
    return ChangeStatus::UNCHANGED;
  case IRPosition::IRP_ARGUMENT:
  case IRPosition::IRP_FUNCTION:
  case IRPosition::IRP_RETURNED:
    Attrs = ScopeFn->getAttributes();
    break;
  case IRPosition::IRP_CALL_SITE:
  case IRPosition::IRP_CALL_SITE_RETURNED:
  case IRPosition::IRP_CALL_SITE_ARGUMENT:
    Attrs = ImmutableCallSite(&IRP.getAnchorValue()).getAttributes();
    break;
  }

  ChangeStatus HasChanged = ChangeStatus::UNCHANGED;
  LLVMContext &Ctx = IRP.getAnchorValue().getContext();
  for (const Attribute &Attr : DeducedAttrs) {
    if (!addIfNotExistent(Ctx, Attr, Attrs, IRP.getAttrIdx()))
      continue;

    HasChanged = ChangeStatus::CHANGED;
  }

  if (HasChanged == ChangeStatus::UNCHANGED)
    return HasChanged;

  switch (PK) {
  case IRPosition::IRP_ARGUMENT:
  case IRPosition::IRP_FUNCTION:
  case IRPosition::IRP_RETURNED:
    ScopeFn->setAttributes(Attrs);
    break;
  case IRPosition::IRP_CALL_SITE:
  case IRPosition::IRP_CALL_SITE_RETURNED:
  case IRPosition::IRP_CALL_SITE_ARGUMENT:
    CallSite(&IRP.getAnchorValue()).setAttributes(Attrs);
    break;
  case IRPosition::IRP_INVALID:
  case IRPosition::IRP_FLOAT:
    break;
  }

  return HasChanged;
}

const IRPosition IRPosition::EmptyKey(255);
const IRPosition IRPosition::TombstoneKey(256);

SubsumingPositionIterator::SubsumingPositionIterator(const IRPosition &IRP) {
  IRPositions.emplace_back(IRP);

  ImmutableCallSite ICS(&IRP.getAnchorValue());
  switch (IRP.getPositionKind()) {
  case IRPosition::IRP_INVALID:
  case IRPosition::IRP_FLOAT:
  case IRPosition::IRP_FUNCTION:
    return;
  case IRPosition::IRP_ARGUMENT:
  case IRPosition::IRP_RETURNED:
    IRPositions.emplace_back(IRPosition::function(*IRP.getAnchorScope()));
    return;
  case IRPosition::IRP_CALL_SITE:
    assert(ICS && "Expected call site!");
    // TODO: We need to look at the operand bundles similar to the redirection
    //       in CallBase.
    if (!ICS.hasOperandBundles())
      if (const Function *Callee = ICS.getCalledFunction())
        IRPositions.emplace_back(IRPosition::function(*Callee));
    return;
  case IRPosition::IRP_CALL_SITE_RETURNED:
    assert(ICS && "Expected call site!");
    // TODO: We need to look at the operand bundles similar to the redirection
    //       in CallBase.
    if (!ICS.hasOperandBundles()) {
      if (const Function *Callee = ICS.getCalledFunction()) {
        IRPositions.emplace_back(IRPosition::returned(*Callee));
        IRPositions.emplace_back(IRPosition::function(*Callee));
        for (const Argument &Arg : Callee->args())
          if (Arg.hasReturnedAttr()) {
            IRPositions.emplace_back(
                IRPosition::callsite_argument(ICS, Arg.getArgNo()));
            IRPositions.emplace_back(
                IRPosition::value(*ICS.getArgOperand(Arg.getArgNo())));
            IRPositions.emplace_back(IRPosition::argument(Arg));
          }
      }
    }
    IRPositions.emplace_back(
        IRPosition::callsite_function(cast<CallBase>(*ICS.getInstruction())));
    return;
  case IRPosition::IRP_CALL_SITE_ARGUMENT: {
    int ArgNo = IRP.getArgNo();
    assert(ICS && ArgNo >= 0 && "Expected call site!");
    // TODO: We need to look at the operand bundles similar to the redirection
    //       in CallBase.
    if (!ICS.hasOperandBundles()) {
      const Function *Callee = ICS.getCalledFunction();
      if (Callee && Callee->arg_size() > unsigned(ArgNo))
        IRPositions.emplace_back(IRPosition::argument(*Callee->getArg(ArgNo)));
      if (Callee)
        IRPositions.emplace_back(IRPosition::function(*Callee));
    }
    IRPositions.emplace_back(IRPosition::value(IRP.getAssociatedValue()));
    return;
  }
  }
}

bool IRPosition::hasAttr(ArrayRef<Attribute::AttrKind> AKs,
                         bool IgnoreSubsumingPositions, Attributor *A) const {
  SmallVector<Attribute, 4> Attrs;
  for (const IRPosition &EquivIRP : SubsumingPositionIterator(*this)) {
    for (Attribute::AttrKind AK : AKs)
      if (EquivIRP.getAttrsFromIRAttr(AK, Attrs))
        return true;
    // The first position returned by the SubsumingPositionIterator is
    // always the position itself. If we ignore subsuming positions we
    // are done after the first iteration.
    if (IgnoreSubsumingPositions)
      break;
  }
  if (A)
    for (Attribute::AttrKind AK : AKs)
      if (getAttrsFromAssumes(AK, Attrs, *A))
        return true;
  return false;
}

void IRPosition::getAttrs(ArrayRef<Attribute::AttrKind> AKs,
                          SmallVectorImpl<Attribute> &Attrs,
                          bool IgnoreSubsumingPositions, Attributor *A) const {
  for (const IRPosition &EquivIRP : SubsumingPositionIterator(*this)) {
    for (Attribute::AttrKind AK : AKs)
      EquivIRP.getAttrsFromIRAttr(AK, Attrs);
    // The first position returned by the SubsumingPositionIterator is
    // always the position itself. If we ignore subsuming positions we
    // are done after the first iteration.
    if (IgnoreSubsumingPositions)
      break;
  }
  if (A)
    for (Attribute::AttrKind AK : AKs)
      getAttrsFromAssumes(AK, Attrs, *A);
}

bool IRPosition::getAttrsFromIRAttr(Attribute::AttrKind AK,
                                    SmallVectorImpl<Attribute> &Attrs) const {
  if (getPositionKind() == IRP_INVALID || getPositionKind() == IRP_FLOAT)
    return false;

  AttributeList AttrList;
  if (ImmutableCallSite ICS = ImmutableCallSite(&getAnchorValue()))
    AttrList = ICS.getAttributes();
  else
    AttrList = getAssociatedFunction()->getAttributes();

  bool HasAttr = AttrList.hasAttribute(getAttrIdx(), AK);
  if (HasAttr)
    Attrs.push_back(AttrList.getAttribute(getAttrIdx(), AK));
  return HasAttr;
}

bool IRPosition::getAttrsFromAssumes(Attribute::AttrKind AK,
                                     SmallVectorImpl<Attribute> &Attrs,
                                     Attributor &A) const {
  assert(getPositionKind() != IRP_INVALID && "Did expect a valid position!");
  Value &AssociatedValue = getAssociatedValue();

  const Assume2KnowledgeMap &A2K =
      A.getInfoCache().getKnowledgeMap().lookup({&AssociatedValue, AK});

  // Check if we found any potential assume use, if not we don't need to create
  // explorer iterators.
  if (A2K.empty())
    return false;

  LLVMContext &Ctx = AssociatedValue.getContext();
  unsigned AttrsSize = Attrs.size();
  MustBeExecutedContextExplorer &Explorer =
      A.getInfoCache().getMustBeExecutedContextExplorer();
  auto EIt = Explorer.begin(getCtxI()), EEnd = Explorer.end(getCtxI());
  for (auto &It : A2K)
    if (Explorer.findInContextOf(It.first, EIt, EEnd))
      Attrs.push_back(Attribute::get(Ctx, AK, It.second.Max));
  return AttrsSize != Attrs.size();
}

void IRPosition::verify() {
  switch (KindOrArgNo) {
  default:
    assert(KindOrArgNo >= 0 && "Expected argument or call site argument!");
    assert((isa<CallBase>(AnchorVal) || isa<Argument>(AnchorVal)) &&
           "Expected call base or argument for positive attribute index!");
    if (isa<Argument>(AnchorVal)) {
      assert(cast<Argument>(AnchorVal)->getArgNo() == unsigned(getArgNo()) &&
             "Argument number mismatch!");
      assert(cast<Argument>(AnchorVal) == &getAssociatedValue() &&
             "Associated value mismatch!");
    } else {
      assert(cast<CallBase>(*AnchorVal).arg_size() > unsigned(getArgNo()) &&
             "Call site argument number mismatch!");
      assert(cast<CallBase>(*AnchorVal).getArgOperand(getArgNo()) ==
                 &getAssociatedValue() &&
             "Associated value mismatch!");
    }
    break;
  case IRP_INVALID:
    assert(!AnchorVal && "Expected no value for an invalid position!");
    break;
  case IRP_FLOAT:
    assert((!isa<CallBase>(&getAssociatedValue()) &&
            !isa<Argument>(&getAssociatedValue())) &&
           "Expected specialized kind for call base and argument values!");
    break;
  case IRP_RETURNED:
    assert(isa<Function>(AnchorVal) &&
           "Expected function for a 'returned' position!");
    assert(AnchorVal == &getAssociatedValue() && "Associated value mismatch!");
    break;
  case IRP_CALL_SITE_RETURNED:
    assert((isa<CallBase>(AnchorVal)) &&
           "Expected call base for 'call site returned' position!");
    assert(AnchorVal == &getAssociatedValue() && "Associated value mismatch!");
    break;
  case IRP_CALL_SITE:
    assert((isa<CallBase>(AnchorVal)) &&
           "Expected call base for 'call site function' position!");
    assert(AnchorVal == &getAssociatedValue() && "Associated value mismatch!");
    break;
  case IRP_FUNCTION:
    assert(isa<Function>(AnchorVal) &&
           "Expected function for a 'function' position!");
    assert(AnchorVal == &getAssociatedValue() && "Associated value mismatch!");
    break;
  }
}

Optional<Constant *>
Attributor::getAssumedConstant(const Value &V, const AbstractAttribute &AA,
                               bool &UsedAssumedInformation) {
  const auto &ValueSimplifyAA = getAAFor<AAValueSimplify>(
      AA, IRPosition::value(V), /* TrackDependence */ false);
  Optional<Value *> SimplifiedV =
      ValueSimplifyAA.getAssumedSimplifiedValue(*this);
  bool IsKnown = ValueSimplifyAA.isKnown();
  UsedAssumedInformation |= !IsKnown;
  if (!SimplifiedV.hasValue()) {
    recordDependence(ValueSimplifyAA, AA, DepClassTy::OPTIONAL);
    return llvm::None;
  }
  if (isa_and_nonnull<UndefValue>(SimplifiedV.getValue())) {
    recordDependence(ValueSimplifyAA, AA, DepClassTy::OPTIONAL);
    return llvm::None;
  }
  Constant *CI = dyn_cast_or_null<Constant>(SimplifiedV.getValue());
  if (CI && CI->getType() != V.getType()) {
    // TODO: Check for a save conversion.
    return nullptr;
  }
  if (CI)
    recordDependence(ValueSimplifyAA, AA, DepClassTy::OPTIONAL);
  return CI;
}

Attributor::~Attributor() {
  // The abstract attributes are allocated via the BumpPtrAllocator Allocator,
  // thus we cannot delete them. We can, and want to, destruct them though.
  for (AbstractAttribute *AA : AllAbstractAttributes)
    AA->~AbstractAttribute();

  for (auto &It : ArgumentReplacementMap)
    DeleteContainerPointers(It.second);
}

bool Attributor::isAssumedDead(const AbstractAttribute &AA,
                               const AAIsDead *FnLivenessAA,
                               bool CheckBBLivenessOnly, DepClassTy DepClass) {
  const IRPosition &IRP = AA.getIRPosition();
  if (!Functions.count(IRP.getAnchorScope()))
    return false;
  return isAssumedDead(IRP, &AA, FnLivenessAA, CheckBBLivenessOnly, DepClass);
}

bool Attributor::isAssumedDead(const Use &U,
                               const AbstractAttribute *QueryingAA,
                               const AAIsDead *FnLivenessAA,
                               bool CheckBBLivenessOnly, DepClassTy DepClass) {
  Instruction *UserI = dyn_cast<Instruction>(U.getUser());
  if (!UserI)
    return isAssumedDead(IRPosition::value(*U.get()), QueryingAA, FnLivenessAA,
                         CheckBBLivenessOnly, DepClass);

  if (CallSite CS = CallSite(UserI)) {
    // For call site argument uses we can check if the argument is
    // unused/dead.
    if (CS.isArgOperand(&U)) {
      const IRPosition &CSArgPos =
          IRPosition::callsite_argument(CS, CS.getArgumentNo(&U));
      return isAssumedDead(CSArgPos, QueryingAA, FnLivenessAA,
                           CheckBBLivenessOnly, DepClass);
    }
  } else if (ReturnInst *RI = dyn_cast<ReturnInst>(UserI)) {
    const IRPosition &RetPos = IRPosition::returned(*RI->getFunction());
    return isAssumedDead(RetPos, QueryingAA, FnLivenessAA, CheckBBLivenessOnly,
                         DepClass);
  } else if (PHINode *PHI = dyn_cast<PHINode>(UserI)) {
    BasicBlock *IncomingBB = PHI->getIncomingBlock(U);
    return isAssumedDead(*IncomingBB->getTerminator(), QueryingAA, FnLivenessAA,
                         CheckBBLivenessOnly, DepClass);
  }

  return isAssumedDead(IRPosition::value(*UserI), QueryingAA, FnLivenessAA,
                       CheckBBLivenessOnly, DepClass);
}

bool Attributor::isAssumedDead(const Instruction &I,
                               const AbstractAttribute *QueryingAA,
                               const AAIsDead *FnLivenessAA,
                               bool CheckBBLivenessOnly, DepClassTy DepClass) {
  if (!FnLivenessAA)
    FnLivenessAA = lookupAAFor<AAIsDead>(IRPosition::function(*I.getFunction()),
                                         QueryingAA,
                                         /* TrackDependence */ false);

  // If we have a context instruction and a liveness AA we use it.
  if (FnLivenessAA &&
      FnLivenessAA->getIRPosition().getAnchorScope() == I.getFunction() &&
      FnLivenessAA->isAssumedDead(&I)) {
    if (QueryingAA)
      recordDependence(*FnLivenessAA, *QueryingAA, DepClass);
    return true;
  }

  if (CheckBBLivenessOnly)
    return false;

  const AAIsDead &IsDeadAA = getOrCreateAAFor<AAIsDead>(
      IRPosition::value(I), QueryingAA, /* TrackDependence */ false);
  // Don't check liveness for AAIsDead.
  if (QueryingAA == &IsDeadAA)
    return false;

  if (IsDeadAA.isAssumedDead()) {
    if (QueryingAA)
      recordDependence(IsDeadAA, *QueryingAA, DepClass);
    return true;
  }

  return false;
}

bool Attributor::isAssumedDead(const IRPosition &IRP,
                               const AbstractAttribute *QueryingAA,
                               const AAIsDead *FnLivenessAA,
                               bool CheckBBLivenessOnly, DepClassTy DepClass) {
  Instruction *CtxI = IRP.getCtxI();
  if (CtxI &&
      isAssumedDead(*CtxI, QueryingAA, FnLivenessAA,
                    /* CheckBBLivenessOnly */ true,
                    CheckBBLivenessOnly ? DepClass : DepClassTy::OPTIONAL))
    return true;

  if (CheckBBLivenessOnly)
    return false;

  // If we haven't succeeded we query the specific liveness info for the IRP.
  const AAIsDead *IsDeadAA;
  if (IRP.getPositionKind() == IRPosition::IRP_CALL_SITE)
    IsDeadAA = &getOrCreateAAFor<AAIsDead>(
        IRPosition::callsite_returned(cast<CallBase>(IRP.getAssociatedValue())),
        QueryingAA, /* TrackDependence */ false);
  else
    IsDeadAA = &getOrCreateAAFor<AAIsDead>(IRP, QueryingAA,
                                           /* TrackDependence */ false);
  // Don't check liveness for AAIsDead.
  if (QueryingAA == IsDeadAA)
    return false;

  if (IsDeadAA->isAssumedDead()) {
    if (QueryingAA)
      recordDependence(*IsDeadAA, *QueryingAA, DepClass);
    return true;
  }

  return false;
}

bool Attributor::checkForAllUses(function_ref<bool(const Use &, bool &)> Pred,
                                 const AbstractAttribute &QueryingAA,
                                 const Value &V, DepClassTy LivenessDepClass) {

  // Check the trivial case first as it catches void values.
  if (V.use_empty())
    return true;

  // If the value is replaced by another one, for now a constant, we do not have
  // uses. Note that this requires users of `checkForAllUses` to not recurse but
  // instead use the `follow` callback argument to look at transitive users,
  // however, that should be clear from the presence of the argument.
  bool UsedAssumedInformation = false;
  Optional<Constant *> C =
      getAssumedConstant(V, QueryingAA, UsedAssumedInformation);
  if (C.hasValue() && C.getValue()) {
    LLVM_DEBUG(dbgs() << "[Attributor] Value is simplified, uses skipped: " << V
                      << " -> " << *C.getValue() << "\n");
    return true;
  }

  const IRPosition &IRP = QueryingAA.getIRPosition();
  SmallVector<const Use *, 16> Worklist;
  SmallPtrSet<const Use *, 16> Visited;

  for (const Use &U : V.uses())
    Worklist.push_back(&U);

  LLVM_DEBUG(dbgs() << "[Attributor] Got " << Worklist.size()
                    << " initial uses to check\n");

  const Function *ScopeFn = IRP.getAnchorScope();
  const auto *LivenessAA =
      ScopeFn ? &getAAFor<AAIsDead>(QueryingAA, IRPosition::function(*ScopeFn),
                                    /* TrackDependence */ false)
              : nullptr;

  while (!Worklist.empty()) {
    const Use *U = Worklist.pop_back_val();
    if (!Visited.insert(U).second)
      continue;
    LLVM_DEBUG(dbgs() << "[Attributor] Check use: " << **U << " in "
                      << *U->getUser() << "\n");
    if (isAssumedDead(*U, &QueryingAA, LivenessAA,
                      /* CheckBBLivenessOnly */ false, LivenessDepClass)) {
      LLVM_DEBUG(dbgs() << "[Attributor] Dead use, skip!\n");
      continue;
    }
    if (U->getUser()->isDroppable()) {
      LLVM_DEBUG(dbgs() << "[Attributor] Droppable user, skip!\n");
      continue;
    }

    bool Follow = false;
    if (!Pred(*U, Follow))
      return false;
    if (!Follow)
      continue;
    for (const Use &UU : U->getUser()->uses())
      Worklist.push_back(&UU);
  }

  return true;
}

bool Attributor::checkForAllCallSites(function_ref<bool(AbstractCallSite)> Pred,
                                      const AbstractAttribute &QueryingAA,
                                      bool RequireAllCallSites,
                                      bool &AllCallSitesKnown) {
  // We can try to determine information from
  // the call sites. However, this is only possible all call sites are known,
  // hence the function has internal linkage.
  const IRPosition &IRP = QueryingAA.getIRPosition();
  const Function *AssociatedFunction = IRP.getAssociatedFunction();
  if (!AssociatedFunction) {
    LLVM_DEBUG(dbgs() << "[Attributor] No function associated with " << IRP
                      << "\n");
    AllCallSitesKnown = false;
    return false;
  }

  return checkForAllCallSites(Pred, *AssociatedFunction, RequireAllCallSites,
                              &QueryingAA, AllCallSitesKnown);
}

bool Attributor::checkForAllCallSites(function_ref<bool(AbstractCallSite)> Pred,
                                      const Function &Fn,
                                      bool RequireAllCallSites,
                                      const AbstractAttribute *QueryingAA,
                                      bool &AllCallSitesKnown) {
  if (RequireAllCallSites && !Fn.hasLocalLinkage()) {
    LLVM_DEBUG(
        dbgs()
        << "[Attributor] Function " << Fn.getName()
        << " has no internal linkage, hence not all call sites are known\n");
    AllCallSitesKnown = false;
    return false;
  }

  // If we do not require all call sites we might not see all.
  AllCallSitesKnown = RequireAllCallSites;

  SmallVector<const Use *, 8> Uses(make_pointer_range(Fn.uses()));
  for (unsigned u = 0; u < Uses.size(); ++u) {
    const Use &U = *Uses[u];
    LLVM_DEBUG(dbgs() << "[Attributor] Check use: " << *U << " in "
                      << *U.getUser() << "\n");
    if (isAssumedDead(U, QueryingAA, nullptr, /* CheckBBLivenessOnly */ true)) {
      LLVM_DEBUG(dbgs() << "[Attributor] Dead use, skip!\n");
      continue;
    }
    if (ConstantExpr *CE = dyn_cast<ConstantExpr>(U.getUser())) {
      if (CE->isCast() && CE->getType()->isPointerTy() &&
          CE->getType()->getPointerElementType()->isFunctionTy()) {
        for (const Use &CEU : CE->uses())
          Uses.push_back(&CEU);
        continue;
      }
    }

    AbstractCallSite ACS(&U);
    if (!ACS) {
      LLVM_DEBUG(dbgs() << "[Attributor] Function " << Fn.getName()
                        << " has non call site use " << *U.get() << " in "
                        << *U.getUser() << "\n");
      // BlockAddress users are allowed.
      if (isa<BlockAddress>(U.getUser()))
        continue;
      return false;
    }

    const Use *EffectiveUse =
        ACS.isCallbackCall() ? &ACS.getCalleeUseForCallback() : &U;
    if (!ACS.isCallee(EffectiveUse)) {
      if (!RequireAllCallSites)
        continue;
      LLVM_DEBUG(dbgs() << "[Attributor] User " << EffectiveUse->getUser()
                        << " is an invalid use of " << Fn.getName() << "\n");
      return false;
    }

    // Make sure the arguments that can be matched between the call site and the
    // callee argee on their type. It is unlikely they do not and it doesn't
    // make sense for all attributes to know/care about this.
    assert(&Fn == ACS.getCalledFunction() && "Expected known callee");
    unsigned MinArgsParams =
        std::min(size_t(ACS.getNumArgOperands()), Fn.arg_size());
    for (unsigned u = 0; u < MinArgsParams; ++u) {
      Value *CSArgOp = ACS.getCallArgOperand(u);
      if (CSArgOp && Fn.getArg(u)->getType() != CSArgOp->getType()) {
        LLVM_DEBUG(
            dbgs() << "[Attributor] Call site / callee argument type mismatch ["
                   << u << "@" << Fn.getName() << ": "
                   << *Fn.getArg(u)->getType() << " vs. "
                   << *ACS.getCallArgOperand(u)->getType() << "\n");
        return false;
      }
    }

    if (Pred(ACS))
      continue;

    LLVM_DEBUG(dbgs() << "[Attributor] Call site callback failed for "
                      << *ACS.getInstruction() << "\n");
    return false;
  }

  return true;
}

bool Attributor::checkForAllReturnedValuesAndReturnInsts(
    function_ref<bool(Value &, const SmallSetVector<ReturnInst *, 4> &)> Pred,
    const AbstractAttribute &QueryingAA) {

  const IRPosition &IRP = QueryingAA.getIRPosition();
  // Since we need to provide return instructions we have to have an exact
  // definition.
  const Function *AssociatedFunction = IRP.getAssociatedFunction();
  if (!AssociatedFunction)
    return false;

  // If this is a call site query we use the call site specific return values
  // and liveness information.
  // TODO: use the function scope once we have call site AAReturnedValues.
  const IRPosition &QueryIRP = IRPosition::function(*AssociatedFunction);
  const auto &AARetVal = getAAFor<AAReturnedValues>(QueryingAA, QueryIRP);
  if (!AARetVal.getState().isValidState())
    return false;

  return AARetVal.checkForAllReturnedValuesAndReturnInsts(Pred);
}

bool Attributor::checkForAllReturnedValues(
    function_ref<bool(Value &)> Pred, const AbstractAttribute &QueryingAA) {

  const IRPosition &IRP = QueryingAA.getIRPosition();
  const Function *AssociatedFunction = IRP.getAssociatedFunction();
  if (!AssociatedFunction)
    return false;

  // TODO: use the function scope once we have call site AAReturnedValues.
  const IRPosition &QueryIRP = IRPosition::function(*AssociatedFunction);
  const auto &AARetVal = getAAFor<AAReturnedValues>(QueryingAA, QueryIRP);
  if (!AARetVal.getState().isValidState())
    return false;

  return AARetVal.checkForAllReturnedValuesAndReturnInsts(
      [&](Value &RV, const SmallSetVector<ReturnInst *, 4> &) {
        return Pred(RV);
      });
}

static bool checkForAllInstructionsImpl(
    Attributor *A, InformationCache::OpcodeInstMapTy &OpcodeInstMap,
    function_ref<bool(Instruction &)> Pred, const AbstractAttribute *QueryingAA,
    const AAIsDead *LivenessAA, const ArrayRef<unsigned> &Opcodes,
    bool CheckBBLivenessOnly = false) {
  for (unsigned Opcode : Opcodes) {
    for (Instruction *I : OpcodeInstMap[Opcode]) {
      // Skip dead instructions.
      if (A && A->isAssumedDead(IRPosition::value(*I), QueryingAA, LivenessAA,
                                CheckBBLivenessOnly))
        continue;

      if (!Pred(*I))
        return false;
    }
  }
  return true;
}

bool Attributor::checkForAllInstructions(function_ref<bool(Instruction &)> Pred,
                                         const AbstractAttribute &QueryingAA,
                                         const ArrayRef<unsigned> &Opcodes,
                                         bool CheckBBLivenessOnly) {

  const IRPosition &IRP = QueryingAA.getIRPosition();
  // Since we need to provide instructions we have to have an exact definition.
  const Function *AssociatedFunction = IRP.getAssociatedFunction();
  if (!AssociatedFunction)
    return false;

  // TODO: use the function scope once we have call site AAReturnedValues.
  const IRPosition &QueryIRP = IRPosition::function(*AssociatedFunction);
  const auto &LivenessAA =
      getAAFor<AAIsDead>(QueryingAA, QueryIRP, /* TrackDependence */ false);

  auto &OpcodeInstMap =
      InfoCache.getOpcodeInstMapForFunction(*AssociatedFunction);
  if (!checkForAllInstructionsImpl(this, OpcodeInstMap, Pred, &QueryingAA,
                                   &LivenessAA, Opcodes, CheckBBLivenessOnly))
    return false;

  return true;
}

bool Attributor::checkForAllReadWriteInstructions(
    function_ref<bool(Instruction &)> Pred, AbstractAttribute &QueryingAA) {

  const Function *AssociatedFunction =
      QueryingAA.getIRPosition().getAssociatedFunction();
  if (!AssociatedFunction)
    return false;

  // TODO: use the function scope once we have call site AAReturnedValues.
  const IRPosition &QueryIRP = IRPosition::function(*AssociatedFunction);
  const auto &LivenessAA =
      getAAFor<AAIsDead>(QueryingAA, QueryIRP, /* TrackDependence */ false);

  for (Instruction *I :
       InfoCache.getReadOrWriteInstsForFunction(*AssociatedFunction)) {
    // Skip dead instructions.
    if (isAssumedDead(IRPosition::value(*I), &QueryingAA, &LivenessAA))
      continue;

    if (!Pred(*I))
      return false;
  }

  return true;
}

ChangeStatus Attributor::run() {
  LLVM_DEBUG(dbgs() << "[Attributor] Identified and initialized "
                    << AllAbstractAttributes.size()
                    << " abstract attributes.\n");

  // Now that all abstract attributes are collected and initialized we start
  // the abstract analysis.

  unsigned IterationCounter = 1;

  SmallVector<AbstractAttribute *, 32> ChangedAAs;
  SetVector<AbstractAttribute *> Worklist, InvalidAAs;
  Worklist.insert(AllAbstractAttributes.begin(), AllAbstractAttributes.end());

  bool RecomputeDependences = false;

  do {
    // Remember the size to determine new attributes.
    size_t NumAAs = AllAbstractAttributes.size();
    LLVM_DEBUG(dbgs() << "\n\n[Attributor] #Iteration: " << IterationCounter
                      << ", Worklist size: " << Worklist.size() << "\n");

    // For invalid AAs we can fix dependent AAs that have a required dependence,
    // thereby folding long dependence chains in a single step without the need
    // to run updates.
    for (unsigned u = 0; u < InvalidAAs.size(); ++u) {
      AbstractAttribute *InvalidAA = InvalidAAs[u];
      auto &QuerriedAAs = QueryMap[InvalidAA];
      LLVM_DEBUG(dbgs() << "[Attributor] InvalidAA: " << *InvalidAA << " has "
                        << QuerriedAAs.RequiredAAs.size() << "/"
                        << QuerriedAAs.OptionalAAs.size()
                        << " required/optional dependences\n");
      for (AbstractAttribute *DepOnInvalidAA : QuerriedAAs.RequiredAAs) {
        AbstractState &DOIAAState = DepOnInvalidAA->getState();
        DOIAAState.indicatePessimisticFixpoint();
        ++NumAttributesFixedDueToRequiredDependences;
        assert(DOIAAState.isAtFixpoint() && "Expected fixpoint state!");
        if (!DOIAAState.isValidState())
          InvalidAAs.insert(DepOnInvalidAA);
        else
          ChangedAAs.push_back(DepOnInvalidAA);
      }
      if (!RecomputeDependences)
        Worklist.insert(QuerriedAAs.OptionalAAs.begin(),
                        QuerriedAAs.OptionalAAs.end());
    }

    // If dependences (=QueryMap) are recomputed we have to look at all abstract
    // attributes again, regardless of what changed in the last iteration.
    if (RecomputeDependences) {
      LLVM_DEBUG(
          dbgs() << "[Attributor] Run all AAs to recompute dependences\n");
      QueryMap.clear();
      ChangedAAs.clear();
      Worklist.insert(AllAbstractAttributes.begin(),
                      AllAbstractAttributes.end());
    }

    // Add all abstract attributes that are potentially dependent on one that
    // changed to the work list.
    for (AbstractAttribute *ChangedAA : ChangedAAs) {
      auto &QuerriedAAs = QueryMap[ChangedAA];
      Worklist.insert(QuerriedAAs.OptionalAAs.begin(),
                      QuerriedAAs.OptionalAAs.end());
      Worklist.insert(QuerriedAAs.RequiredAAs.begin(),
                      QuerriedAAs.RequiredAAs.end());
    }

    LLVM_DEBUG(dbgs() << "[Attributor] #Iteration: " << IterationCounter
                      << ", Worklist+Dependent size: " << Worklist.size()
                      << "\n");

    // Reset the changed and invalid set.
    ChangedAAs.clear();
    InvalidAAs.clear();

    // Update all abstract attribute in the work list and record the ones that
    // changed.
    for (AbstractAttribute *AA : Worklist)
      if (!AA->getState().isAtFixpoint() &&
          !isAssumedDead(*AA, nullptr, /* CheckBBLivenessOnly */ true)) {
        QueriedNonFixAA = false;
        if (AA->update(*this) == ChangeStatus::CHANGED) {
          ChangedAAs.push_back(AA);
          if (!AA->getState().isValidState())
            InvalidAAs.insert(AA);
        } else if (!QueriedNonFixAA) {
          // If the attribute did not query any non-fix information, the state
          // will not change and we can indicate that right away.
          AA->getState().indicateOptimisticFixpoint();
        }
      }

    // Check if we recompute the dependences in the next iteration.
    RecomputeDependences = (DepRecomputeInterval > 0 &&
                            IterationCounter % DepRecomputeInterval == 0);

    // Add attributes to the changed set if they have been created in the last
    // iteration.
    ChangedAAs.append(AllAbstractAttributes.begin() + NumAAs,
                      AllAbstractAttributes.end());

    // Reset the work list and repopulate with the changed abstract attributes.
    // Note that dependent ones are added above.
    Worklist.clear();
    Worklist.insert(ChangedAAs.begin(), ChangedAAs.end());

  } while (!Worklist.empty() && (IterationCounter++ < MaxFixpointIterations ||
                                 VerifyMaxFixpointIterations));

  LLVM_DEBUG(dbgs() << "\n[Attributor] Fixpoint iteration done after: "
                    << IterationCounter << "/" << MaxFixpointIterations
                    << " iterations\n");

  size_t NumFinalAAs = AllAbstractAttributes.size();

  // Reset abstract arguments not settled in a sound fixpoint by now. This
  // happens when we stopped the fixpoint iteration early. Note that only the
  // ones marked as "changed" *and* the ones transitively depending on them
  // need to be reverted to a pessimistic state. Others might not be in a
  // fixpoint state but we can use the optimistic results for them anyway.
  SmallPtrSet<AbstractAttribute *, 32> Visited;
  for (unsigned u = 0; u < ChangedAAs.size(); u++) {
    AbstractAttribute *ChangedAA = ChangedAAs[u];
    if (!Visited.insert(ChangedAA).second)
      continue;

    AbstractState &State = ChangedAA->getState();
    if (!State.isAtFixpoint()) {
      State.indicatePessimisticFixpoint();

      NumAttributesTimedOut++;
    }

    auto &QuerriedAAs = QueryMap[ChangedAA];
    ChangedAAs.append(QuerriedAAs.OptionalAAs.begin(),
                      QuerriedAAs.OptionalAAs.end());
    ChangedAAs.append(QuerriedAAs.RequiredAAs.begin(),
                      QuerriedAAs.RequiredAAs.end());
  }

  LLVM_DEBUG({
    if (!Visited.empty())
      dbgs() << "\n[Attributor] Finalized " << Visited.size()
             << " abstract attributes.\n";
  });

  unsigned NumManifested = 0;
  unsigned NumAtFixpoint = 0;
  ChangeStatus ManifestChange = ChangeStatus::UNCHANGED;
  for (AbstractAttribute *AA : AllAbstractAttributes) {
    AbstractState &State = AA->getState();

    // If there is not already a fixpoint reached, we can now take the
    // optimistic state. This is correct because we enforced a pessimistic one
    // on abstract attributes that were transitively dependent on a changed one
    // already above.
    if (!State.isAtFixpoint())
      State.indicateOptimisticFixpoint();

    // If the state is invalid, we do not try to manifest it.
    if (!State.isValidState())
      continue;

    // Skip dead code.
    if (isAssumedDead(*AA, nullptr, /* CheckBBLivenessOnly */ true))
      continue;
    // Manifest the state and record if we changed the IR.
    ChangeStatus LocalChange = AA->manifest(*this);
    if (LocalChange == ChangeStatus::CHANGED && AreStatisticsEnabled())
      AA->trackStatistics();
    LLVM_DEBUG(dbgs() << "[Attributor] Manifest " << LocalChange << " : " << *AA
                      << "\n");

    ManifestChange = ManifestChange | LocalChange;

    NumAtFixpoint++;
    NumManifested += (LocalChange == ChangeStatus::CHANGED);
  }

  (void)NumManifested;
  (void)NumAtFixpoint;
  LLVM_DEBUG(dbgs() << "\n[Attributor] Manifested " << NumManifested
                    << " arguments while " << NumAtFixpoint
                    << " were in a valid fixpoint state\n");

  NumAttributesManifested += NumManifested;
  NumAttributesValidFixpoint += NumAtFixpoint;

  (void)NumFinalAAs;
  if (NumFinalAAs != AllAbstractAttributes.size()) {
    for (unsigned u = NumFinalAAs; u < AllAbstractAttributes.size(); ++u)
      errs() << "Unexpected abstract attribute: " << *AllAbstractAttributes[u]
             << " :: "
             << AllAbstractAttributes[u]->getIRPosition().getAssociatedValue()
             << "\n";
    llvm_unreachable("Expected the final number of abstract attributes to "
                     "remain unchanged!");
  }

  // Delete stuff at the end to avoid invalid references and a nice order.
  {
    LLVM_DEBUG(dbgs() << "\n[Attributor] Delete at least "
                      << ToBeDeletedFunctions.size() << " functions and "
                      << ToBeDeletedBlocks.size() << " blocks and "
                      << ToBeDeletedInsts.size() << " instructions and "
                      << ToBeChangedUses.size() << " uses\n");

    SmallVector<WeakTrackingVH, 32> DeadInsts;
    SmallVector<Instruction *, 32> TerminatorsToFold;

    for (auto &It : ToBeChangedUses) {
      Use *U = It.first;
      Value *NewV = It.second;
      Value *OldV = U->get();

      // Do not replace uses in returns if the value is a must-tail call we will
      // not delete.
      if (isa<ReturnInst>(U->getUser()))
        if (auto *CI = dyn_cast<CallInst>(OldV->stripPointerCasts()))
          if (CI->isMustTailCall() && !ToBeDeletedInsts.count(CI))
            continue;

      LLVM_DEBUG(dbgs() << "Use " << *NewV << " in " << *U->getUser()
                        << " instead of " << *OldV << "\n");
      U->set(NewV);
      // Do not modify call instructions outside the SCC.
      if (auto *CB = dyn_cast<CallBase>(OldV))
        if (!Functions.count(CB->getCaller()))
          continue;
      if (Instruction *I = dyn_cast<Instruction>(OldV)) {
        CGModifiedFunctions.insert(I->getFunction());
        if (!isa<PHINode>(I) && !ToBeDeletedInsts.count(I) &&
            isInstructionTriviallyDead(I))
          DeadInsts.push_back(I);
      }
      if (isa<Constant>(NewV) && isa<BranchInst>(U->getUser())) {
        Instruction *UserI = cast<Instruction>(U->getUser());
        if (isa<UndefValue>(NewV)) {
          ToBeChangedToUnreachableInsts.insert(UserI);
        } else {
          TerminatorsToFold.push_back(UserI);
        }
      }
    }
    for (auto &V : InvokeWithDeadSuccessor)
      if (InvokeInst *II = dyn_cast_or_null<InvokeInst>(V)) {
        bool UnwindBBIsDead = II->hasFnAttr(Attribute::NoUnwind);
        bool NormalBBIsDead = II->hasFnAttr(Attribute::NoReturn);
        bool Invoke2CallAllowed =
            !AAIsDead::mayCatchAsynchronousExceptions(*II->getFunction());
        assert((UnwindBBIsDead || NormalBBIsDead) &&
               "Invoke does not have dead successors!");
        BasicBlock *BB = II->getParent();
        BasicBlock *NormalDestBB = II->getNormalDest();
        if (UnwindBBIsDead) {
          Instruction *NormalNextIP = &NormalDestBB->front();
          if (Invoke2CallAllowed) {
            changeToCall(II);
            NormalNextIP = BB->getTerminator();
          }
          if (NormalBBIsDead)
            ToBeChangedToUnreachableInsts.insert(NormalNextIP);
        } else {
          assert(NormalBBIsDead && "Broken invariant!");
          if (!NormalDestBB->getUniquePredecessor())
            NormalDestBB = SplitBlockPredecessors(NormalDestBB, {BB}, ".dead");
          ToBeChangedToUnreachableInsts.insert(&NormalDestBB->front());
        }
      }
    for (Instruction *I : TerminatorsToFold) {
      CGModifiedFunctions.insert(I->getFunction());
      ConstantFoldTerminator(I->getParent());
    }
    for (auto &V : ToBeChangedToUnreachableInsts)
      if (Instruction *I = dyn_cast_or_null<Instruction>(V)) {
        CGModifiedFunctions.insert(I->getFunction());
        changeToUnreachable(I, /* UseLLVMTrap */ false);
      }

    for (auto &V : ToBeDeletedInsts) {
      if (Instruction *I = dyn_cast_or_null<Instruction>(V)) {
        I->dropDroppableUses();
        CGModifiedFunctions.insert(I->getFunction());
        if (!I->getType()->isVoidTy())
          I->replaceAllUsesWith(UndefValue::get(I->getType()));
        if (!isa<PHINode>(I) && isInstructionTriviallyDead(I))
          DeadInsts.push_back(I);
        else
          I->eraseFromParent();
      }
    }

    RecursivelyDeleteTriviallyDeadInstructions(DeadInsts);

    if (unsigned NumDeadBlocks = ToBeDeletedBlocks.size()) {
      SmallVector<BasicBlock *, 8> ToBeDeletedBBs;
      ToBeDeletedBBs.reserve(NumDeadBlocks);
      for (BasicBlock *BB : ToBeDeletedBlocks) {
        CGModifiedFunctions.insert(BB->getParent());
        ToBeDeletedBBs.push_back(BB);
      }
      // Actually we do not delete the blocks but squash them into a single
      // unreachable but untangling branches that jump here is something we need
      // to do in a more generic way.
      DetatchDeadBlocks(ToBeDeletedBBs, nullptr);
    }

    // Identify dead internal functions and delete them. This happens outside
    // the other fixpoint analysis as we might treat potentially dead functions
    // as live to lower the number of iterations. If they happen to be dead, the
    // below fixpoint loop will identify and eliminate them.
    SmallVector<Function *, 8> InternalFns;
    for (Function *F : Functions)
      if (F->hasLocalLinkage())
        InternalFns.push_back(F);

    bool FoundDeadFn = true;
    while (FoundDeadFn) {
      FoundDeadFn = false;
      for (unsigned u = 0, e = InternalFns.size(); u < e; ++u) {
        Function *F = InternalFns[u];
        if (!F)
          continue;

        bool AllCallSitesKnown;
        if (!checkForAllCallSites(
                [this](AbstractCallSite ACS) {
                  return ToBeDeletedFunctions.count(
                      ACS.getInstruction()->getFunction());
                },
                *F, true, nullptr, AllCallSitesKnown))
          continue;

        ToBeDeletedFunctions.insert(F);
        InternalFns[u] = nullptr;
        FoundDeadFn = true;
      }
    }
  }

  // Rewrite the functions as requested during manifest.
  ManifestChange =
      ManifestChange | rewriteFunctionSignatures(CGModifiedFunctions);

  for (Function *Fn : CGModifiedFunctions)
    CGUpdater.reanalyzeFunction(*Fn);

  for (Function *Fn : ToBeDeletedFunctions)
    CGUpdater.removeFunction(*Fn);

  NumFnDeleted += ToBeDeletedFunctions.size();

  if (VerifyMaxFixpointIterations &&
      IterationCounter != MaxFixpointIterations) {
    errs() << "\n[Attributor] Fixpoint iteration done after: "
           << IterationCounter << "/" << MaxFixpointIterations
           << " iterations\n";
    llvm_unreachable("The fixpoint was not reached with exactly the number of "
                     "specified iterations!");
  }

#ifdef EXPENSIVE_CHECKS
  for (Function *F : Functions) {
    if (ToBeDeletedFunctions.count(F))
      continue;
    assert(!verifyFunction(*F, &errs()) && "Module verification failed!");
  }
#endif

  return ManifestChange;
}

/// Create a shallow wrapper for \p F such that \p F has internal linkage
/// afterwards. It also sets the original \p F 's name to anonymous
///
/// A wrapper is a function with the same type (and attributes) as \p F
/// that will only call \p F and return the result, if any.
///
/// Assuming the declaration of looks like:
///   rty F(aty0 arg0, ..., atyN argN);
///
/// The wrapper will then look as follows:
///   rty wrapper(aty0 arg0, ..., atyN argN) {
///     return F(arg0, ..., argN);
///   }
///
static void createShallowWrapper(Function &F) {
  assert(AllowShallowWrappers &&
         "Cannot create a wrapper if it is not allowed!");
  assert(!F.isDeclaration() && "Cannot create a wrapper around a declaration!");

  Module &M = *F.getParent();
  LLVMContext &Ctx = M.getContext();
  FunctionType *FnTy = F.getFunctionType();

  Function *Wrapper =
      Function::Create(FnTy, F.getLinkage(), F.getAddressSpace(), F.getName());
  F.setName(""); // set the inside function anonymous
  M.getFunctionList().insert(F.getIterator(), Wrapper);

  F.setLinkage(GlobalValue::InternalLinkage);

  F.replaceAllUsesWith(Wrapper);
  assert(F.getNumUses() == 0 && "Uses remained after wrapper was created!");

  // Move the COMDAT section to the wrapper.
  // TODO: Check if we need to keep it for F as well.
  Wrapper->setComdat(F.getComdat());
  F.setComdat(nullptr);

  // Copy all metadata and attributes but keep them on F as well.
  SmallVector<std::pair<unsigned, MDNode *>, 1> MDs;
  F.getAllMetadata(MDs);
  for (auto MDIt : MDs)
    Wrapper->addMetadata(MDIt.first, *MDIt.second);
  Wrapper->setAttributes(F.getAttributes());

  // Create the call in the wrapper.
  BasicBlock *EntryBB = BasicBlock::Create(Ctx, "entry", Wrapper);

  SmallVector<Value *, 8> Args;
  auto FArgIt = F.arg_begin();
  for (Argument &Arg : Wrapper->args()) {
    Args.push_back(&Arg);
    Arg.setName((FArgIt++)->getName());
  }

  CallInst *CI = CallInst::Create(&F, Args, "", EntryBB);
  CI->setTailCall(true);
  CI->addAttribute(AttributeList::FunctionIndex, Attribute::NoInline);
  ReturnInst::Create(Ctx, CI->getType()->isVoidTy() ? nullptr : CI, EntryBB);

  NumFnShallowWrapperCreated++;
}

bool Attributor::isValidFunctionSignatureRewrite(
    Argument &Arg, ArrayRef<Type *> ReplacementTypes) {

  auto CallSiteCanBeChanged = [](AbstractCallSite ACS) {
    // Forbid must-tail calls for now.
    return !ACS.isCallbackCall() && !ACS.getInstruction()->isMustTailCall();
  };

  Function *Fn = Arg.getParent();
  // Avoid var-arg functions for now.
  if (Fn->isVarArg()) {
    LLVM_DEBUG(dbgs() << "[Attributor] Cannot rewrite var-args functions\n");
    return false;
  }

  // Avoid functions with complicated argument passing semantics.
  AttributeList FnAttributeList = Fn->getAttributes();
  if (FnAttributeList.hasAttrSomewhere(Attribute::Nest) ||
      FnAttributeList.hasAttrSomewhere(Attribute::StructRet) ||
      FnAttributeList.hasAttrSomewhere(Attribute::InAlloca)) {
    LLVM_DEBUG(
        dbgs() << "[Attributor] Cannot rewrite due to complex attribute\n");
    return false;
  }

  // Avoid callbacks for now.
  bool AllCallSitesKnown;
  if (!checkForAllCallSites(CallSiteCanBeChanged, *Fn, true, nullptr,
                            AllCallSitesKnown)) {
    LLVM_DEBUG(dbgs() << "[Attributor] Cannot rewrite all call sites\n");
    return false;
  }

  auto InstPred = [](Instruction &I) {
    if (auto *CI = dyn_cast<CallInst>(&I))
      return !CI->isMustTailCall();
    return true;
  };

  // Forbid must-tail calls for now.
  // TODO:
  auto &OpcodeInstMap = InfoCache.getOpcodeInstMapForFunction(*Fn);
  if (!checkForAllInstructionsImpl(nullptr, OpcodeInstMap, InstPred, nullptr,
                                   nullptr, {Instruction::Call})) {
    LLVM_DEBUG(dbgs() << "[Attributor] Cannot rewrite due to instructions\n");
    return false;
  }

  return true;
}

bool Attributor::registerFunctionSignatureRewrite(
    Argument &Arg, ArrayRef<Type *> ReplacementTypes,
    ArgumentReplacementInfo::CalleeRepairCBTy &&CalleeRepairCB,
    ArgumentReplacementInfo::ACSRepairCBTy &&ACSRepairCB) {
  LLVM_DEBUG(dbgs() << "[Attributor] Register new rewrite of " << Arg << " in "
                    << Arg.getParent()->getName() << " with "
                    << ReplacementTypes.size() << " replacements\n");
  assert(isValidFunctionSignatureRewrite(Arg, ReplacementTypes) &&
         "Cannot register an invalid rewrite");

  Function *Fn = Arg.getParent();
  SmallVectorImpl<ArgumentReplacementInfo *> &ARIs = ArgumentReplacementMap[Fn];
  if (ARIs.empty())
    ARIs.resize(Fn->arg_size());

  // If we have a replacement already with less than or equal new arguments,
  // ignore this request.
  ArgumentReplacementInfo *&ARI = ARIs[Arg.getArgNo()];
  if (ARI && ARI->getNumReplacementArgs() <= ReplacementTypes.size()) {
    LLVM_DEBUG(dbgs() << "[Attributor] Existing rewrite is preferred\n");
    return false;
  }

  // If we have a replacement already but we like the new one better, delete
  // the old.
  if (ARI)
    delete ARI;

  LLVM_DEBUG(dbgs() << "[Attributor] Register new rewrite of " << Arg << " in "
                    << Arg.getParent()->getName() << " with "
                    << ReplacementTypes.size() << " replacements\n");

  // Remember the replacement.
  ARI = new ArgumentReplacementInfo(*this, Arg, ReplacementTypes,
                                    std::move(CalleeRepairCB),
                                    std::move(ACSRepairCB));

  return true;
}

ChangeStatus Attributor::rewriteFunctionSignatures(
    SmallPtrSetImpl<Function *> &ModifiedFns) {
  ChangeStatus Changed = ChangeStatus::UNCHANGED;

  for (auto &It : ArgumentReplacementMap) {
    Function *OldFn = It.getFirst();

    // Deleted functions do not require rewrites.
    if (ToBeDeletedFunctions.count(OldFn))
      continue;

    const SmallVectorImpl<ArgumentReplacementInfo *> &ARIs = It.getSecond();
    assert(ARIs.size() == OldFn->arg_size() && "Inconsistent state!");

    SmallVector<Type *, 16> NewArgumentTypes;
    SmallVector<AttributeSet, 16> NewArgumentAttributes;

    // Collect replacement argument types and copy over existing attributes.
    AttributeList OldFnAttributeList = OldFn->getAttributes();
    for (Argument &Arg : OldFn->args()) {
      if (ArgumentReplacementInfo *ARI = ARIs[Arg.getArgNo()]) {
        NewArgumentTypes.append(ARI->ReplacementTypes.begin(),
                                ARI->ReplacementTypes.end());
        NewArgumentAttributes.append(ARI->getNumReplacementArgs(),
                                     AttributeSet());
      } else {
        NewArgumentTypes.push_back(Arg.getType());
        NewArgumentAttributes.push_back(
            OldFnAttributeList.getParamAttributes(Arg.getArgNo()));
      }
    }

    FunctionType *OldFnTy = OldFn->getFunctionType();
    Type *RetTy = OldFnTy->getReturnType();

    // Construct the new function type using the new arguments types.
    FunctionType *NewFnTy =
        FunctionType::get(RetTy, NewArgumentTypes, OldFnTy->isVarArg());

    LLVM_DEBUG(dbgs() << "[Attributor] Function rewrite '" << OldFn->getName()
                      << "' from " << *OldFn->getFunctionType() << " to "
                      << *NewFnTy << "\n");

    // Create the new function body and insert it into the module.
    Function *NewFn = Function::Create(NewFnTy, OldFn->getLinkage(),
                                       OldFn->getAddressSpace(), "");
    OldFn->getParent()->getFunctionList().insert(OldFn->getIterator(), NewFn);
    NewFn->takeName(OldFn);
    NewFn->copyAttributesFrom(OldFn);

    // Patch the pointer to LLVM function in debug info descriptor.
    NewFn->setSubprogram(OldFn->getSubprogram());
    OldFn->setSubprogram(nullptr);

    // Recompute the parameter attributes list based on the new arguments for
    // the function.
    LLVMContext &Ctx = OldFn->getContext();
    NewFn->setAttributes(AttributeList::get(
        Ctx, OldFnAttributeList.getFnAttributes(),
        OldFnAttributeList.getRetAttributes(), NewArgumentAttributes));

    // Since we have now created the new function, splice the body of the old
    // function right into the new function, leaving the old rotting hulk of the
    // function empty.
    NewFn->getBasicBlockList().splice(NewFn->begin(),
                                      OldFn->getBasicBlockList());

    // Set of all "call-like" instructions that invoke the old function mapped
    // to their new replacements.
    SmallVector<std::pair<CallBase *, CallBase *>, 8> CallSitePairs;

    // Callback to create a new "call-like" instruction for a given one.
    auto CallSiteReplacementCreator = [&](AbstractCallSite ACS) {
      CallBase *OldCB = cast<CallBase>(ACS.getInstruction());
      const AttributeList &OldCallAttributeList = OldCB->getAttributes();

      // Collect the new argument operands for the replacement call site.
      SmallVector<Value *, 16> NewArgOperands;
      SmallVector<AttributeSet, 16> NewArgOperandAttributes;
      for (unsigned OldArgNum = 0; OldArgNum < ARIs.size(); ++OldArgNum) {
        unsigned NewFirstArgNum = NewArgOperands.size();
        (void)NewFirstArgNum; // only used inside assert.
        if (ArgumentReplacementInfo *ARI = ARIs[OldArgNum]) {
          if (ARI->ACSRepairCB)
            ARI->ACSRepairCB(*ARI, ACS, NewArgOperands);
          assert(ARI->getNumReplacementArgs() + NewFirstArgNum ==
                     NewArgOperands.size() &&
                 "ACS repair callback did not provide as many operand as new "
                 "types were registered!");
          // TODO: Exose the attribute set to the ACS repair callback
          NewArgOperandAttributes.append(ARI->ReplacementTypes.size(),
                                         AttributeSet());
        } else {
          NewArgOperands.push_back(ACS.getCallArgOperand(OldArgNum));
          NewArgOperandAttributes.push_back(
              OldCallAttributeList.getParamAttributes(OldArgNum));
        }
      }

      assert(NewArgOperands.size() == NewArgOperandAttributes.size() &&
             "Mismatch # argument operands vs. # argument operand attributes!");
      assert(NewArgOperands.size() == NewFn->arg_size() &&
             "Mismatch # argument operands vs. # function arguments!");

      SmallVector<OperandBundleDef, 4> OperandBundleDefs;
      OldCB->getOperandBundlesAsDefs(OperandBundleDefs);

      // Create a new call or invoke instruction to replace the old one.
      CallBase *NewCB;
      if (InvokeInst *II = dyn_cast<InvokeInst>(OldCB)) {
        NewCB =
            InvokeInst::Create(NewFn, II->getNormalDest(), II->getUnwindDest(),
                               NewArgOperands, OperandBundleDefs, "", OldCB);
      } else {
        auto *NewCI = CallInst::Create(NewFn, NewArgOperands, OperandBundleDefs,
                                       "", OldCB);
        NewCI->setTailCallKind(cast<CallInst>(OldCB)->getTailCallKind());
        NewCB = NewCI;
      }

      // Copy over various properties and the new attributes.
      uint64_t W;
      if (OldCB->extractProfTotalWeight(W))
        NewCB->setProfWeight(W);
      NewCB->setCallingConv(OldCB->getCallingConv());
      NewCB->setDebugLoc(OldCB->getDebugLoc());
      NewCB->takeName(OldCB);
      NewCB->setAttributes(AttributeList::get(
          Ctx, OldCallAttributeList.getFnAttributes(),
          OldCallAttributeList.getRetAttributes(), NewArgOperandAttributes));

      CallSitePairs.push_back({OldCB, NewCB});
      return true;
    };

    // Use the CallSiteReplacementCreator to create replacement call sites.
    bool AllCallSitesKnown;
    bool Success = checkForAllCallSites(CallSiteReplacementCreator, *OldFn,
                                        true, nullptr, AllCallSitesKnown);
    (void)Success;
    assert(Success && "Assumed call site replacement to succeed!");

    // Rewire the arguments.
    auto OldFnArgIt = OldFn->arg_begin();
    auto NewFnArgIt = NewFn->arg_begin();
    for (unsigned OldArgNum = 0; OldArgNum < ARIs.size();
         ++OldArgNum, ++OldFnArgIt) {
      if (ArgumentReplacementInfo *ARI = ARIs[OldArgNum]) {
        if (ARI->CalleeRepairCB)
          ARI->CalleeRepairCB(*ARI, *NewFn, NewFnArgIt);
        NewFnArgIt += ARI->ReplacementTypes.size();
      } else {
        NewFnArgIt->takeName(&*OldFnArgIt);
        OldFnArgIt->replaceAllUsesWith(&*NewFnArgIt);
        ++NewFnArgIt;
      }
    }

    // Eliminate the instructions *after* we visited all of them.
    for (auto &CallSitePair : CallSitePairs) {
      CallBase &OldCB = *CallSitePair.first;
      CallBase &NewCB = *CallSitePair.second;
      ModifiedFns.insert(OldCB.getFunction());
      CGUpdater.replaceCallSite(OldCB, NewCB);
      OldCB.replaceAllUsesWith(&NewCB);
      OldCB.eraseFromParent();
    }

    // Replace the function in the call graph (if any).
    CGUpdater.replaceFunctionWith(*OldFn, *NewFn);

    // If the old function was modified and needed to be reanalyzed, the new one
    // does now.
    if (ModifiedFns.erase(OldFn))
      ModifiedFns.insert(NewFn);

    Changed = ChangeStatus::CHANGED;
  }

  return Changed;
}

void InformationCache::initializeInformationCache(const Function &CF,
                                                  FunctionInfo &FI) {
  // As we do not modify the function here we can remove the const
  // withouth breaking implicit assumptions. At the end of the day, we could
  // initialize the cache eagerly which would look the same to the users.
  Function &F = const_cast<Function &>(CF);

  // Walk all instructions to find interesting instructions that might be
  // queried by abstract attributes during their initialization or update.
  // This has to happen before we create attributes.

  for (Instruction &I : instructions(&F)) {
    bool IsInterestingOpcode = false;

    // To allow easy access to all instructions in a function with a given
    // opcode we store them in the InfoCache. As not all opcodes are interesting
    // to concrete attributes we only cache the ones that are as identified in
    // the following switch.
    // Note: There are no concrete attributes now so this is initially empty.
    switch (I.getOpcode()) {
    default:
      assert((!ImmutableCallSite(&I)) && (!isa<CallBase>(&I)) &&
             "New call site/base instruction type needs to be known in the "
             "Attributor.");
      break;
    case Instruction::Call:
      // Calls are interesting on their own, additionally:
      // For `llvm.assume` calls we also fill the KnowledgeMap as we find them.
      // For `must-tail` calls we remember the caller and callee.
      if (IntrinsicInst *Assume = dyn_cast<IntrinsicInst>(&I)) {
        if (Assume->getIntrinsicID() == Intrinsic::assume)
          fillMapFromAssume(*Assume, KnowledgeMap);
      } else if (cast<CallInst>(I).isMustTailCall()) {
        FI.ContainsMustTailCall = true;
        if (const Function *Callee = cast<CallInst>(I).getCalledFunction())
          getFunctionInfo(*Callee).CalledViaMustTail = true;
      }
      LLVM_FALLTHROUGH;
    case Instruction::CallBr:
    case Instruction::Invoke:
    case Instruction::CleanupRet:
    case Instruction::CatchSwitch:
    case Instruction::AtomicRMW:
    case Instruction::AtomicCmpXchg:
    case Instruction::Br:
    case Instruction::Resume:
    case Instruction::Ret:
    case Instruction::Load:
      // The alignment of a pointer is interesting for loads.
    case Instruction::Store:
      // The alignment of a pointer is interesting for stores.
      IsInterestingOpcode = true;
    }
    if (IsInterestingOpcode)
      FI.OpcodeInstMap[I.getOpcode()].push_back(&I);
    if (I.mayReadOrWriteMemory())
      FI.RWInsts.push_back(&I);
  }

  if (F.hasFnAttribute(Attribute::AlwaysInline) &&
      isInlineViable(F).isSuccess())
    InlineableFunctions.insert(&F);
}

void Attributor::recordDependence(const AbstractAttribute &FromAA,
                                  const AbstractAttribute &ToAA,
                                  DepClassTy DepClass) {
  if (FromAA.getState().isAtFixpoint())
    return;

  if (DepClass == DepClassTy::REQUIRED)
    QueryMap[&FromAA].RequiredAAs.insert(
        const_cast<AbstractAttribute *>(&ToAA));
  else
    QueryMap[&FromAA].OptionalAAs.insert(
        const_cast<AbstractAttribute *>(&ToAA));
  QueriedNonFixAA = true;
}

void Attributor::identifyDefaultAbstractAttributes(Function &F) {
  if (!VisitedFunctions.insert(&F).second)
    return;
  if (F.isDeclaration())
    return;

  // In non-module runs we need to look at the call sites of a function to
  // determine if it is part of a must-tail call edge. This will influence what
  // attributes we can derive.
  InformationCache::FunctionInfo &FI = InfoCache.getFunctionInfo(F);
  if (!isModulePass() && !FI.CalledViaMustTail) {
    for (const Use &U : F.uses())
      if (ImmutableCallSite ICS = ImmutableCallSite(U.getUser()))
        if (ICS.isCallee(&U) && ICS.isMustTailCall())
          FI.CalledViaMustTail = true;
  }

  IRPosition FPos = IRPosition::function(F);

  // Check for dead BasicBlocks in every function.
  // We need dead instruction detection because we do not want to deal with
  // broken IR in which SSA rules do not apply.
  getOrCreateAAFor<AAIsDead>(FPos);

  // Every function might be "will-return".
  getOrCreateAAFor<AAWillReturn>(FPos);

  // Every function might contain instructions that cause "undefined behavior".
  getOrCreateAAFor<AAUndefinedBehavior>(FPos);

  // Every function can be nounwind.
  getOrCreateAAFor<AANoUnwind>(FPos);

  // Every function might be marked "nosync"
  getOrCreateAAFor<AANoSync>(FPos);

  // Every function might be "no-free".
  getOrCreateAAFor<AANoFree>(FPos);

  // Every function might be "no-return".
  getOrCreateAAFor<AANoReturn>(FPos);

  // Every function might be "no-recurse".
  getOrCreateAAFor<AANoRecurse>(FPos);

  // Every function might be "readnone/readonly/writeonly/...".
  getOrCreateAAFor<AAMemoryBehavior>(FPos);

  // Every function can be "readnone/argmemonly/inaccessiblememonly/...".
  getOrCreateAAFor<AAMemoryLocation>(FPos);

  // Every function might be applicable for Heap-To-Stack conversion.
  if (EnableHeapToStack)
    getOrCreateAAFor<AAHeapToStack>(FPos);

  // Return attributes are only appropriate if the return type is non void.
  Type *ReturnType = F.getReturnType();
  if (!ReturnType->isVoidTy()) {
    // Argument attribute "returned" --- Create only one per function even
    // though it is an argument attribute.
    getOrCreateAAFor<AAReturnedValues>(FPos);

    IRPosition RetPos = IRPosition::returned(F);

    // Every returned value might be dead.
    getOrCreateAAFor<AAIsDead>(RetPos);

    // Every function might be simplified.
    getOrCreateAAFor<AAValueSimplify>(RetPos);

    if (ReturnType->isPointerTy()) {

      // Every function with pointer return type might be marked align.
      getOrCreateAAFor<AAAlign>(RetPos);

      // Every function with pointer return type might be marked nonnull.
      getOrCreateAAFor<AANonNull>(RetPos);

      // Every function with pointer return type might be marked noalias.
      getOrCreateAAFor<AANoAlias>(RetPos);

      // Every function with pointer return type might be marked
      // dereferenceable.
      getOrCreateAAFor<AADereferenceable>(RetPos);
    }
  }

  for (Argument &Arg : F.args()) {
    IRPosition ArgPos = IRPosition::argument(Arg);

    // Every argument might be simplified.
    getOrCreateAAFor<AAValueSimplify>(ArgPos);

    // Every argument might be dead.
    getOrCreateAAFor<AAIsDead>(ArgPos);

    if (Arg.getType()->isPointerTy()) {
      // Every argument with pointer type might be marked nonnull.
      getOrCreateAAFor<AANonNull>(ArgPos);

      // Every argument with pointer type might be marked noalias.
      getOrCreateAAFor<AANoAlias>(ArgPos);

      // Every argument with pointer type might be marked dereferenceable.
      getOrCreateAAFor<AADereferenceable>(ArgPos);

      // Every argument with pointer type might be marked align.
      getOrCreateAAFor<AAAlign>(ArgPos);

      // Every argument with pointer type might be marked nocapture.
      getOrCreateAAFor<AANoCapture>(ArgPos);

      // Every argument with pointer type might be marked
      // "readnone/readonly/writeonly/..."
      getOrCreateAAFor<AAMemoryBehavior>(ArgPos);

      // Every argument with pointer type might be marked nofree.
      getOrCreateAAFor<AANoFree>(ArgPos);

      // Every argument with pointer type might be privatizable (or promotable)
      getOrCreateAAFor<AAPrivatizablePtr>(ArgPos);
    }
  }

  auto CallSitePred = [&](Instruction &I) -> bool {
    CallSite CS(&I);
    IRPosition CSRetPos = IRPosition::callsite_returned(CS);

    // Call sites might be dead if they do not have side effects and no live
    // users. The return value might be dead if there are no live users.
    getOrCreateAAFor<AAIsDead>(CSRetPos);

    Function *Callee = CS.getCalledFunction();
    // TODO: Even if the callee is not known now we might be able to simplify
    //       the call/callee.
    if (!Callee)
      return true;

    // Skip declarations except if annotations on their call sites were
    // explicitly requested.
    if (!AnnotateDeclarationCallSites && Callee->isDeclaration() &&
        !Callee->hasMetadata(LLVMContext::MD_callback))
      return true;

    if (!Callee->getReturnType()->isVoidTy() && !CS->use_empty()) {

      IRPosition CSRetPos = IRPosition::callsite_returned(CS);

      // Call site return integer values might be limited by a constant range.
      if (Callee->getReturnType()->isIntegerTy())
        getOrCreateAAFor<AAValueConstantRange>(CSRetPos);
    }

    for (int i = 0, e = CS.getNumArgOperands(); i < e; i++) {

      IRPosition CSArgPos = IRPosition::callsite_argument(CS, i);

      // Every call site argument might be dead.
      getOrCreateAAFor<AAIsDead>(CSArgPos);

      // Call site argument might be simplified.
      getOrCreateAAFor<AAValueSimplify>(CSArgPos);

      if (!CS.getArgument(i)->getType()->isPointerTy())
        continue;

      // Call site argument attribute "non-null".
      getOrCreateAAFor<AANonNull>(CSArgPos);

      // Call site argument attribute "no-alias".
      getOrCreateAAFor<AANoAlias>(CSArgPos);

      // Call site argument attribute "dereferenceable".
      getOrCreateAAFor<AADereferenceable>(CSArgPos);

      // Call site argument attribute "align".
      getOrCreateAAFor<AAAlign>(CSArgPos);

      // Call site argument attribute
      // "readnone/readonly/writeonly/..."
      getOrCreateAAFor<AAMemoryBehavior>(CSArgPos);

      // Call site argument attribute "nofree".
      getOrCreateAAFor<AANoFree>(CSArgPos);
    }
    return true;
  };

  auto &OpcodeInstMap = InfoCache.getOpcodeInstMapForFunction(F);
  bool Success;
  Success = checkForAllInstructionsImpl(
      nullptr, OpcodeInstMap, CallSitePred, nullptr, nullptr,
      {(unsigned)Instruction::Invoke, (unsigned)Instruction::CallBr,
       (unsigned)Instruction::Call});
  (void)Success;
  assert(Success && "Expected the check call to be successful!");

  auto LoadStorePred = [&](Instruction &I) -> bool {
    if (isa<LoadInst>(I))
      getOrCreateAAFor<AAAlign>(
          IRPosition::value(*cast<LoadInst>(I).getPointerOperand()));
    else
      getOrCreateAAFor<AAAlign>(
          IRPosition::value(*cast<StoreInst>(I).getPointerOperand()));
    return true;
  };
  Success = checkForAllInstructionsImpl(
      nullptr, OpcodeInstMap, LoadStorePred, nullptr, nullptr,
      {(unsigned)Instruction::Load, (unsigned)Instruction::Store});
  (void)Success;
  assert(Success && "Expected the check call to be successful!");
}

/// Helpers to ease debugging through output streams and print calls.
///
///{
raw_ostream &llvm::operator<<(raw_ostream &OS, ChangeStatus S) {
  return OS << (S == ChangeStatus::CHANGED ? "changed" : "unchanged");
}

raw_ostream &llvm::operator<<(raw_ostream &OS, IRPosition::Kind AP) {
  switch (AP) {
  case IRPosition::IRP_INVALID:
    return OS << "inv";
  case IRPosition::IRP_FLOAT:
    return OS << "flt";
  case IRPosition::IRP_RETURNED:
    return OS << "fn_ret";
  case IRPosition::IRP_CALL_SITE_RETURNED:
    return OS << "cs_ret";
  case IRPosition::IRP_FUNCTION:
    return OS << "fn";
  case IRPosition::IRP_CALL_SITE:
    return OS << "cs";
  case IRPosition::IRP_ARGUMENT:
    return OS << "arg";
  case IRPosition::IRP_CALL_SITE_ARGUMENT:
    return OS << "cs_arg";
  }
  llvm_unreachable("Unknown attribute position!");
}

raw_ostream &llvm::operator<<(raw_ostream &OS, const IRPosition &Pos) {
  const Value &AV = Pos.getAssociatedValue();
  return OS << "{" << Pos.getPositionKind() << ":" << AV.getName() << " ["
            << Pos.getAnchorValue().getName() << "@" << Pos.getArgNo() << "]}";
}

raw_ostream &llvm::operator<<(raw_ostream &OS, const IntegerRangeState &S) {
  OS << "range-state(" << S.getBitWidth() << ")<";
  S.getKnown().print(OS);
  OS << " / ";
  S.getAssumed().print(OS);
  OS << ">";

  return OS << static_cast<const AbstractState &>(S);
}

raw_ostream &llvm::operator<<(raw_ostream &OS, const AbstractState &S) {
  return OS << (!S.isValidState() ? "top" : (S.isAtFixpoint() ? "fix" : ""));
}

raw_ostream &llvm::operator<<(raw_ostream &OS, const AbstractAttribute &AA) {
  AA.print(OS);
  return OS;
}

void AbstractAttribute::print(raw_ostream &OS) const {
  OS << "[P: " << getIRPosition() << "][" << getAsStr() << "][S: " << getState()
     << "]";
}
///}

/// ----------------------------------------------------------------------------
///                       Pass (Manager) Boilerplate
/// ----------------------------------------------------------------------------

static bool runAttributorOnFunctions(InformationCache &InfoCache,
                                     SetVector<Function *> &Functions,
                                     AnalysisGetter &AG,
                                     CallGraphUpdater &CGUpdater) {
  if (Functions.empty())
    return false;

  LLVM_DEBUG(dbgs() << "[Attributor] Run on module with " << Functions.size()
                    << " functions.\n");

  // Create an Attributor and initially empty information cache that is filled
  // while we identify default attribute opportunities.
  Attributor A(Functions, InfoCache, CGUpdater, DepRecInterval);

  // Create shallow wrappers for all functions that are not IPO amendable
  if (AllowShallowWrappers)
    for (Function *F : Functions)
      if (!A.isFunctionIPOAmendable(*F))
        createShallowWrapper(*F);

  for (Function *F : Functions) {
    if (F->hasExactDefinition())
      NumFnWithExactDefinition++;
    else
      NumFnWithoutExactDefinition++;

    // We look at internal functions only on-demand but if any use is not a
    // direct call or outside the current set of analyzed functions, we have to
    // do it eagerly.
    if (F->hasLocalLinkage()) {
      if (llvm::all_of(F->uses(), [&Functions](const Use &U) {
            ImmutableCallSite ICS(U.getUser());
            return ICS && ICS.isCallee(&U) &&
                   Functions.count(const_cast<Function *>(ICS.getCaller()));
          }))
        continue;
    }

    // Populate the Attributor with abstract attribute opportunities in the
    // function and the information cache with IR information.
    A.identifyDefaultAbstractAttributes(*F);
  }

  ChangeStatus Changed = A.run();
  LLVM_DEBUG(dbgs() << "[Attributor] Done with " << Functions.size()
                    << " functions, result: " << Changed << ".\n");
  return Changed == ChangeStatus::CHANGED;
}

PreservedAnalyses AttributorPass::run(Module &M, ModuleAnalysisManager &AM) {
  FunctionAnalysisManager &FAM =
      AM.getResult<FunctionAnalysisManagerModuleProxy>(M).getManager();
  AnalysisGetter AG(FAM);

  SetVector<Function *> Functions;
  for (Function &F : M)
    Functions.insert(&F);

  CallGraphUpdater CGUpdater;
  InformationCache InfoCache(M, AG, /* CGSCC */ nullptr);
  if (runAttributorOnFunctions(InfoCache, Functions, AG, CGUpdater)) {
    // FIXME: Think about passes we will preserve and add them here.
    return PreservedAnalyses::none();
  }
  return PreservedAnalyses::all();
}

PreservedAnalyses AttributorCGSCCPass::run(LazyCallGraph::SCC &C,
                                           CGSCCAnalysisManager &AM,
                                           LazyCallGraph &CG,
                                           CGSCCUpdateResult &UR) {
  FunctionAnalysisManager &FAM =
      AM.getResult<FunctionAnalysisManagerCGSCCProxy>(C, CG).getManager();
  AnalysisGetter AG(FAM);

  SetVector<Function *> Functions;
  for (LazyCallGraph::Node &N : C)
    Functions.insert(&N.getFunction());

  if (Functions.empty())
    return PreservedAnalyses::all();

  Module &M = *Functions.back()->getParent();
  CallGraphUpdater CGUpdater;
  CGUpdater.initialize(CG, C, AM, UR);
  InformationCache InfoCache(M, AG, /* CGSCC */ &Functions);
  if (runAttributorOnFunctions(InfoCache, Functions, AG, CGUpdater)) {
    // FIXME: Think about passes we will preserve and add them here.
    return PreservedAnalyses::none();
  }
  return PreservedAnalyses::all();
}

namespace {

struct AttributorLegacyPass : public ModulePass {
  static char ID;

  AttributorLegacyPass() : ModulePass(ID) {
    initializeAttributorLegacyPassPass(*PassRegistry::getPassRegistry());
  }

  bool runOnModule(Module &M) override {
    if (skipModule(M))
      return false;

    AnalysisGetter AG;
    SetVector<Function *> Functions;
    for (Function &F : M)
      Functions.insert(&F);

    CallGraphUpdater CGUpdater;
    InformationCache InfoCache(M, AG, /* CGSCC */ nullptr);
    return runAttributorOnFunctions(InfoCache, Functions, AG, CGUpdater);
  }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    // FIXME: Think about passes we will preserve and add them here.
    AU.addRequired<TargetLibraryInfoWrapperPass>();
  }
};

struct AttributorCGSCCLegacyPass : public CallGraphSCCPass {
  CallGraphUpdater CGUpdater;
  static char ID;

  AttributorCGSCCLegacyPass() : CallGraphSCCPass(ID) {
    initializeAttributorCGSCCLegacyPassPass(*PassRegistry::getPassRegistry());
  }

  bool runOnSCC(CallGraphSCC &SCC) override {
    if (skipSCC(SCC))
      return false;

    SetVector<Function *> Functions;
    for (CallGraphNode *CGN : SCC)
      if (Function *Fn = CGN->getFunction())
        if (!Fn->isDeclaration())
          Functions.insert(Fn);

    if (Functions.empty())
      return false;

    AnalysisGetter AG;
    CallGraph &CG = const_cast<CallGraph &>(SCC.getCallGraph());
    CGUpdater.initialize(CG, SCC);
    Module &M = *Functions.back()->getParent();
    InformationCache InfoCache(M, AG, /* CGSCC */ &Functions);
    return runAttributorOnFunctions(InfoCache, Functions, AG, CGUpdater);
  }

  bool doFinalization(CallGraph &CG) override { return CGUpdater.finalize(); }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    // FIXME: Think about passes we will preserve and add them here.
    AU.addRequired<TargetLibraryInfoWrapperPass>();
    CallGraphSCCPass::getAnalysisUsage(AU);
  }
};

} // end anonymous namespace

Pass *llvm::createAttributorLegacyPass() { return new AttributorLegacyPass(); }
Pass *llvm::createAttributorCGSCCLegacyPass() {
  return new AttributorCGSCCLegacyPass();
}

char AttributorLegacyPass::ID = 0;
char AttributorCGSCCLegacyPass::ID = 0;

INITIALIZE_PASS_BEGIN(AttributorLegacyPass, "attributor",
                      "Deduce and propagate attributes", false, false)
INITIALIZE_PASS_DEPENDENCY(TargetLibraryInfoWrapperPass)
INITIALIZE_PASS_END(AttributorLegacyPass, "attributor",
                    "Deduce and propagate attributes", false, false)
INITIALIZE_PASS_BEGIN(AttributorCGSCCLegacyPass, "attributor-cgscc",
                      "Deduce and propagate attributes (CGSCC pass)", false,
                      false)
INITIALIZE_PASS_DEPENDENCY(TargetLibraryInfoWrapperPass)
INITIALIZE_PASS_DEPENDENCY(CallGraphWrapperPass)
INITIALIZE_PASS_END(AttributorCGSCCLegacyPass, "attributor-cgscc",
                    "Deduce and propagate attributes (CGSCC pass)", false,
                    false)
