//===--- MetadataRequest.cpp - IR generation for metadata requests --------===//
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
//  This file implements IR generation for accessing metadata.
//
//===----------------------------------------------------------------------===//

#include "MetadataRequest.h"

#include "ConstantBuilder.h"
#include "Explosion.h"
#include "FixedTypeInfo.h"
#include "GenericRequirement.h"
#include "GenArchetype.h"
#include "GenClass.h"
#include "GenMeta.h"
#include "GenProto.h"
#include "GenType.h"
#include "IRGenDebugInfo.h"
#include "IRGenFunction.h"
#include "IRGenMangler.h"
#include "IRGenModule.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/ExistentialLayout.h"
#include "swift/AST/IRGenOptions.h"
#include "swift/AST/SubstitutionMap.h"
#include "swift/ClangImporter/ClangModule.h"
#include "swift/IRGen/Linking.h"
#include "swift/SIL/FormalLinkage.h"
#include "swift/SIL/TypeLowering.h"

using namespace swift;
using namespace irgen;

llvm::Value *DynamicMetadataRequest::get(IRGenFunction &IGF) const {
  if (isStatic()) {
    return IGF.IGM.getSize(Size(StaticRequest.getOpaqueValue()));
  } else {
    return DynamicRequest;
  }
}

llvm::Value *DynamicMetadataRequest::getRequiredState(IRGenFunction &IGF) const{
  if (isStatic()) {
    return IGF.IGM.getSize(Size(size_t(StaticRequest.getState())));
  }

  auto request = DynamicRequest;

  static_assert(MetadataRequest::State_bit == 0,
                "code below is not doing any shifts");

  uint32_t mask =
    ((uint32_t(1) << MetadataRequest::State_width) - 1);
  auto requiredState =
    IGF.Builder.CreateAnd(request,
                          llvm::ConstantInt::get(IGF.IGM.SizeTy, mask));
  return requiredState;
}

MetadataResponse MetadataResponse::getUndef(IRGenFunction &IGF) {
  return forComplete(llvm::UndefValue::get(IGF.IGM.TypeMetadataPtrTy));
}

MetadataResponse
MetadataResponse::handle(IRGenFunction &IGF, DynamicMetadataRequest request,
                         llvm::Value *pair) {
  assert(pair->getType() == IGF.IGM.TypeMetadataResponseTy);

  // If the request is statically known to produce a complete result,
  // we never even need to extract the status value.
  if (request.isStaticallyBlockingComplete()) {
    auto value = IGF.Builder.CreateExtractValue(pair, 0);
    return MetadataResponse::forComplete(value);
  }

  // Otherwise, split the response.
  auto split = IGF.Builder.CreateSplit<2>(pair);

  // If the request has a collector installed, check the dependency now.
  if (auto collector = request.getDependencyCollector()) {
    collector->checkDependency(IGF, request, split[0], split[1]);
  }

  // Compute the static lower bound on the metadata's dynamic state.
  // This will include any refinements from having branched for the
  // dependency collector.
  auto staticBound = request.getStaticLowerBoundOnResponseState();

  auto response = MetadataResponse(split[0], split[1], staticBound);
  return response;
}

llvm::Value *MetadataResponse::combine(IRGenFunction &IGF) const {
  assert(isValid());
  assert(hasDynamicState() && "cannot combine response without dynamic state");
  return IGF.Builder.CreateCombine(IGF.IGM.TypeMetadataResponseTy,
                                   {Metadata, getDynamicState()});
}

void MetadataResponse::ensureDynamicState(IRGenFunction &IGF) & {
  assert(isValid());

  // If we already have a dynamic state, bail out.
  if (hasDynamicState()) return;

  // If we're statically known complete, we can just fill in
  // MetadataState::Complete.
  if (isStaticallyKnownComplete()) {
    DynamicState = getCompletedState(IGF.IGM);
    return;
  }

  // Otherwise, we need to check the state dynamically.  Do a non-blocking
  // request for complete metadata.
  auto request = MetadataRequest(MetadataState::Complete,
                                 /*non-blocking*/ true);
  *this = emitGetTypeMetadataDynamicState(IGF, request, Metadata);
}

llvm::Constant *MetadataResponse::getCompletedState(IRGenModule &IGM) {
  return IGM.getSize(Size(size_t(MetadataState::Complete)));
}

llvm::Value *MetadataDependency::combine(IRGenFunction &IGF) const {
  if (isTrivial()) {
    return getTrivialCombinedDependency(IGF.IGM);
  }

  return IGF.Builder.CreateCombine(IGF.IGM.TypeMetadataDependencyTy,
                                   {RequiredMetadata, RequiredState});
}

llvm::Constant *
MetadataDependency::getTrivialCombinedDependency(IRGenModule &IGM) {
  return llvm::ConstantAggregateZero::get(IGM.TypeMetadataDependencyTy);
}

void MetadataDependencyCollector::checkDependency(IRGenFunction &IGF,
                                              DynamicMetadataRequest request,
                                                  llvm::Value *metadata,
                                                  llvm::Value *metadataState) {
  // Having either both or neither of the PHIs is normal.
  // Having just RequiredState means that we already finalized this collector
  // and shouldn't be using it anymore.
  assert((!RequiredMetadata || RequiredState) &&
         "checking dependencies on a finished collector");

  // If the request is statically always satisfied, the operation cannot
  // have failed.
  if (request.isStaticallyAlwaysSatisfied())
    return;

  // Otherwise, we need to pull out the response state and compare it against
  // the request state.
  llvm::Value *requiredState = request.getRequiredState(IGF);

  // More advanced metadata states are lower numbers.
  static_assert(MetadataStateIsReverseOrdered,
                "relying on the ordering of MetadataState here");
  auto satisfied = IGF.Builder.CreateICmpULE(metadataState, requiredState);

  emitCheckBranch(IGF, satisfied, metadata, requiredState);
}

void MetadataDependencyCollector::collect(IRGenFunction &IGF,
                                          llvm::Value *dependency) {
  // Having either both or neither of the PHIs is normal.
  // Having just RequiredState means that we already finalized this collector
  // and shouldn't be using it anymore.
  assert((!RequiredMetadata || RequiredState) &&
         "checking dependencies on a finished collector");

  assert(dependency->getType() == IGF.IGM.TypeMetadataDependencyTy);

  // Split the dependency.
  auto metadata = IGF.Builder.CreateExtractValue(dependency, 0);
  auto requiredState = IGF.Builder.CreateExtractValue(dependency, 1);

  // We have a dependency if the metadata is non-null; otherwise we're
  // satisfied and can continue.
  auto satisfied = IGF.Builder.CreateIsNull(metadata);
  emitCheckBranch(IGF, satisfied, metadata, requiredState);
}

void MetadataDependencyCollector::emitCheckBranch(IRGenFunction &IGF,
                                                  llvm::Value *satisfied,
                                                  llvm::Value *metadata,
                                                  llvm::Value *requiredState) {
  // Lazily create the final continuation block and phis.
  if (!RequiredMetadata) {
    auto contBB = IGF.createBasicBlock("metadata-dependencies.cont");
    RequiredMetadata =
      llvm::PHINode::Create(IGF.IGM.TypeMetadataPtrTy, 4, "", contBB);
    RequiredState = llvm::PHINode::Create(IGF.IGM.SizeTy, 4, "", contBB);
  }

  // Conditionally branch to the final continuation block.
  auto satisfiedBB = IGF.createBasicBlock("dependency-satisfied");
  auto curBB = IGF.Builder.GetInsertBlock();
  RequiredMetadata->addIncoming(metadata, curBB);
  RequiredState->addIncoming(requiredState, curBB);
  IGF.Builder.CreateCondBr(satisfied, satisfiedBB,
                           RequiredMetadata->getParent());

  // Otherwise resume emitting code on the main path.
  IGF.Builder.emitBlock(satisfiedBB);
}

MetadataDependency MetadataDependencyCollector::finish(IRGenFunction &IGF) {
  assert((!RequiredMetadata || RequiredState) &&
         "finishing an already-finished collector");

  // If we never branched with a dependency, the result is trivial.
  if (RequiredMetadata == nullptr)
    return MetadataDependency();

  llvm::BasicBlock *curBB = IGF.Builder.GetInsertBlock();
  assert(curBB);
  auto contBB = RequiredMetadata->getParent();
  IGF.Builder.CreateBr(contBB);
  RequiredMetadata->addIncoming(
    llvm::ConstantPointerNull::get(IGF.IGM.TypeMetadataPtrTy),
                                curBB);
  RequiredState->addIncoming(llvm::ConstantInt::get(IGF.IGM.SizeTy, 0), curBB);

  IGF.Builder.emitBlock(contBB);

  auto result = MetadataDependency(RequiredMetadata, RequiredState);

  // Clear RequiredMetadata to tell the destructor that we finished.
  // We leave RequiredState in place so that we can detect attempts to
  // add 
  RequiredMetadata = nullptr;

  return result;
}


llvm::Constant *IRGenModule::getAddrOfStringForMetadataRef(
    StringRef symbolName,
    unsigned alignment,
    bool shouldSetLowBit,
    llvm::function_ref<ConstantInitFuture (ConstantInitBuilder &)> body) {
  // Call this to form the return value.
  auto returnValue = [&](llvm::Constant *addr) {
    if (!shouldSetLowBit)
      return addr;

    auto bitConstant = llvm::ConstantInt::get(IntPtrTy, 1);
    return llvm::ConstantExpr::getGetElementPtr(nullptr, addr, bitConstant);
  };

  // Check whether we already have an entry with this name.
  auto &entry = StringsForTypeRef[symbolName];
  if (entry.second) {
    return returnValue(entry.second);
  }

  // Construct the initializer.
  ConstantInitBuilder builder(*this);
  auto finished = body(builder);

  auto var = new llvm::GlobalVariable(Module, finished.getType(),
                                      /*constant*/ true,
                                      llvm::GlobalValue::LinkOnceODRLinkage,
                                      nullptr,
                                      symbolName);

  ApplyIRLinkage(IRLinkage::InternalLinkOnceODR).to(var);
  if (alignment)
    var->setAlignment(alignment);
  setTrueConstGlobal(var);
  var->setSection(getReflectionTypeRefSectionName());

  finished.installInGlobal(var);

  // Drill down to the i8* at the beginning of the constant.
  auto addr = llvm::ConstantExpr::getBitCast(var, Int8PtrTy);
  StringsForTypeRef[symbolName] = { var, addr };

  return returnValue(addr);
}

llvm::Constant *IRGenModule::getAddrOfStringForTypeRef(StringRef str,
                                                       MangledTypeRefRole role){
  return getAddrOfStringForTypeRef(SymbolicMangling{str, {}}, role);
}

llvm::Constant *IRGenModule::getAddrOfStringForTypeRef(
                                             const SymbolicMangling &mangling,
                                             MangledTypeRefRole role) {
  // Create a symbol name for the symbolic mangling. This is used as the
  // uniquing key both for ODR coalescing and within this TU.
  IRGenMangler mangler;
  std::string symbolName =
    mangler.mangleSymbolNameForSymbolicMangling(mangling, role);

  // See if we emitted the constant already.
  auto &entry = StringsForTypeRef[symbolName];
  if (entry.second) {
    return entry.second;
  }
  
  ConstantInitBuilder B(*this);
  auto S = B.beginStruct();
  S.setPacked(true);

  switch (role) {
  case MangledTypeRefRole::DefaultAssociatedTypeWitness:
    // The 0xFF prefix identifies a default associated type witness.
    S.addInt(Int8Ty,
             ProtocolRequirementFlags::AssociatedTypeInProtocolContextByte);
    break;

  case MangledTypeRefRole::Metadata:
  case MangledTypeRefRole::Reflection:
    break;
  }

  unsigned pos = 0;
  for (auto &symbolic : mangling.SymbolicReferences) {
    assert(symbolic.second >= pos
           && "references should be ordered");
    if (symbolic.second != pos) {
      // Emit the preceding literal chunk.
      auto literalChunk = StringRef(mangling.String.data() + pos,
                                    symbolic.second - pos);
      auto literal = llvm::ConstantDataArray::getString(getLLVMContext(),
                                                        literalChunk,
                                                        /*null*/ false);
      S.add(literal);
    }
    
    ConstantReference ref;
    unsigned char baseKind;
    if (auto ctype = symbolic.first.dyn_cast<const NominalTypeDecl*>()) {
      auto type = const_cast<NominalTypeDecl*>(ctype);
      if (auto proto = dyn_cast<ProtocolDecl>(type)) {
        // The symbolic reference is to the protocol descriptor of the
        // referenced protocol.
        ref = getAddrOfLLVMVariableOrGOTEquivalent(
          LinkEntity::forProtocolDescriptor(proto));
      } else {
        // The symbolic reference is to the type context descriptor of the
        // referenced type.
        IRGen.noteUseOfTypeContextDescriptor(type, DontRequireMetadata);
        ref = getAddrOfLLVMVariableOrGOTEquivalent(
          LinkEntity::forNominalTypeDescriptor(type));
      }
      // \1 - direct reference, \2 - indirect reference
      baseKind = 1;
    } else {
      llvm_unreachable("unhandled symbolic referent");
    }
    
    // add kind byte. indirect kinds are the direct kind + 1
    unsigned char kind = ref.isIndirect() ? baseKind + 1 : baseKind;
    S.add(llvm::ConstantInt::get(Int8Ty, kind));
    // add relative reference
    S.addRelativeAddress(ref.getValue());
    pos = symbolic.second + 5;
  }
  
  // Add the last literal bit, if any.
  if (pos != mangling.String.size()) {
    auto literalChunk = StringRef(mangling.String.data() + pos,
                                  mangling.String.size() - pos);
    auto literal = llvm::ConstantDataArray::getString(getLLVMContext(),
                                                      literalChunk,
                                                      /*null*/ false);
    S.add(literal);
  }
  
  // And a null terminator!
  S.addInt(Int8Ty, 0);
  
  auto finished = S.finishAndCreateFuture();
  auto var = new llvm::GlobalVariable(Module, finished.getType(),
                                      /*constant*/ true,
                                      llvm::GlobalValue::LinkOnceODRLinkage,
                                      nullptr,
                                      symbolName);
  ApplyIRLinkage(IRLinkage::InternalLinkOnceODR).to(var);
  var->setAlignment(2);
  setTrueConstGlobal(var);
  var->setSection(getReflectionTypeRefSectionName());
  
  finished.installInGlobal(var);
  
  // Drill down to the i8* at the beginning of the constant.
  auto addr = llvm::ConstantExpr::getBitCast(var, Int8PtrTy);
  entry = {var, addr};
  
  return addr;
}

llvm::Value *irgen::emitObjCMetadataRefForMetadata(IRGenFunction &IGF,
                                                   llvm::Value *classPtr) {
  assert(IGF.IGM.Context.LangOpts.EnableObjCInterop);
  classPtr = IGF.Builder.CreateBitCast(classPtr, IGF.IGM.ObjCClassPtrTy);
  
  // Fetch the metadata for that class.
  auto call = IGF.Builder.CreateCall(IGF.IGM.getGetObjCClassMetadataFn(),
                                     classPtr);
  call->setDoesNotThrow();
  call->setDoesNotAccessMemory();
  return call;
}

/// Emit a reference to the Swift metadata for an Objective-C class.
static llvm::Value *emitObjCMetadataRef(IRGenFunction &IGF,
                                        ClassDecl *theClass) {
  // Derive a pointer to the Objective-C class.
  auto classPtr = emitObjCHeapMetadataRef(IGF, theClass);
  
  return emitObjCMetadataRefForMetadata(IGF, classPtr);
}

namespace {
  /// A structure for collecting generic arguments for emitting a
  /// nominal metadata reference.  The structure produced here is
  /// consumed by swift_getGenericMetadata() and must correspond to
  /// the fill operations that the compiler emits for the bound decl.
  struct GenericArguments {
    /// The values to use to initialize the arguments structure.
    SmallVector<llvm::Value *, 8> Values;
    SmallVector<llvm::Type *, 8> Types;

    static unsigned getNumGenericArguments(IRGenModule &IGM,
                                           NominalTypeDecl *nominal) {
      GenericTypeRequirements requirements(IGM, nominal);
      return requirements.getNumTypeRequirements();
    }

    void collectTypes(IRGenModule &IGM, NominalTypeDecl *nominal) {
      GenericTypeRequirements requirements(IGM, nominal);
      collectTypes(IGM, requirements);
    }

    void collectTypes(IRGenModule &IGM,
                      const GenericTypeRequirements &requirements) {
      for (auto &requirement : requirements.getRequirements()) {
        if (requirement.Protocol) {
          Types.push_back(IGM.WitnessTablePtrTy);
        } else {
          Types.push_back(IGM.TypeMetadataPtrTy);
        }
      }
    }

    void collect(IRGenFunction &IGF, CanType type) {
      auto *decl = type.getNominalOrBoundGenericNominal();
      GenericTypeRequirements requirements(IGF.IGM, decl);

      auto subs =
        type->getContextSubstitutionMap(IGF.IGM.getSwiftModule(), decl);
      requirements.enumerateFulfillments(IGF.IGM, subs,
                                [&](unsigned reqtIndex, CanType type,
                                    Optional<ProtocolConformanceRef> conf) {
        if (conf) {
          Values.push_back(emitWitnessTableRef(IGF, type, *conf));
        } else {
          Values.push_back(IGF.emitAbstractTypeMetadataRef(type));
        }
      });

      collectTypes(IGF.IGM, decl);
      assert(Types.size() == Values.size());
    }
  };
} // end anonymous namespace

static bool isTypeErasedGenericClass(NominalTypeDecl *ntd) {
  // ObjC classes are type erased.
  // TODO: Unless they have magic methods...
  if (auto clas = dyn_cast<ClassDecl>(ntd))
    return clas->hasClangNode() && clas->isGenericContext();
  return false;
}

static bool isTypeErasedGenericClassType(CanType type) {
  if (auto nom = type->getAnyNominal())
    return isTypeErasedGenericClass(nom);
  return false;
}

// Get the type that exists at runtime to represent a compile-time type.
CanType
irgen::getRuntimeReifiedType(IRGenModule &IGM, CanType type) {
  return CanType(type.transform([&](Type t) -> Type {
    if (isTypeErasedGenericClassType(CanType(t))) {
      return t->getAnyNominal()->getDeclaredType()->getCanonicalType();
    }
    return t;
  }));
}

/// Attempts to return a constant heap metadata reference for a
/// class type.  This is generally only valid for specific kinds of
/// ObjC reference, like superclasses or category references.
llvm::Constant *
irgen::tryEmitConstantHeapMetadataRef(IRGenModule &IGM,
                                      CanType type,
                                      bool allowDynamicUninitialized,
                                      bool allowStub) {
  auto theDecl = type->getClassOrBoundGenericClass();
  assert(theDecl && "emitting constant heap metadata ref for non-class type?");

  switch (IGM.getClassMetadataStrategy(theDecl)) {
  case ClassMetadataStrategy::Resilient:
    if (allowStub && IGM.Context.LangOpts.EnableObjCResilientClassStubs) {
      return IGM.getAddrOfObjCResilientClassStub(theDecl, NotForDefinition,
                                            TypeMetadataAddress::AddressPoint);
    }
    return nullptr;

  case ClassMetadataStrategy::Singleton:
    if (!allowDynamicUninitialized)
      return nullptr;
    break;

  case ClassMetadataStrategy::Update:
  case ClassMetadataStrategy::FixedOrUpdate:
  case ClassMetadataStrategy::Fixed:
    break;
  }

  // For imported classes, use the ObjC class symbol.
  if (!hasKnownSwiftMetadata(IGM, theDecl))
    return IGM.getAddrOfObjCClass(theDecl, NotForDefinition);

  return IGM.getAddrOfTypeMetadata(type);
}

/// Attempts to return a constant type metadata reference for a
/// nominal type.
ConstantReference
irgen::tryEmitConstantTypeMetadataRef(IRGenModule &IGM, CanType type,
                                      SymbolReferenceKind refKind) {
  if (IGM.isStandardLibrary())
    return ConstantReference();
  if (!isTypeMetadataAccessTrivial(IGM, type))
    return ConstantReference();
  return IGM.getAddrOfTypeMetadata(type, refKind);
}

/// Emit a reference to an ObjC class.  In general, the only things
/// you're allowed to do with the address of an ObjC class symbol are
/// (1) send ObjC messages to it (in which case the message will be
/// forwarded to the real class, if one exists) or (2) put it in
/// various data sections where the ObjC runtime will properly arrange
/// things.  Therefore, we must typically force the initialization of
/// a class when emitting a reference to it.
llvm::Value *irgen::emitObjCHeapMetadataRef(IRGenFunction &IGF,
                                            ClassDecl *theClass,
                                            bool allowUninitialized) {
  // If the class is visible only through the Objective-C runtime, form the
  // appropriate runtime call.
  if (theClass->getForeignClassKind() == ClassDecl::ForeignKind::RuntimeOnly) {
    SmallString<64> scratch;
    auto className =
        IGF.IGM.getAddrOfGlobalString(theClass->getObjCRuntimeName(scratch));
    return IGF.Builder.CreateCall(IGF.IGM.getLookUpClassFn(), className);
  }

  assert(!theClass->isForeign());

  Address classRef = IGF.IGM.getAddrOfObjCClassRef(theClass);
  auto classObject = IGF.Builder.CreateLoad(classRef);
  if (allowUninitialized) return classObject;

  // TODO: memoize this the same way that we memoize Swift type metadata?
  return IGF.Builder.CreateCall(IGF.IGM.getGetInitializedObjCClassFn(),
                                classObject);
}

/// Returns a metadata reference for a nominal type.
///
/// This is only valid in a couple of special cases:
/// 1) The nominal type is generic, in which case we emit a call to the
///    generic metadata accessor function, which must be defined separately.
/// 2) The nominal type is a value type with a fixed size from this
///    resilience domain, in which case we can reference the constant
///    metadata directly.
///
/// In any other case, a metadata accessor should be called instead.
static MetadataResponse emitNominalMetadataRef(IRGenFunction &IGF,
                                               NominalTypeDecl *theDecl,
                                               CanType theType,
                                               DynamicMetadataRequest request) {
  assert(!isa<ProtocolDecl>(theDecl));

  if (!theDecl->isGenericContext()) {
    assert(!IGF.IGM.isResilient(theDecl, ResilienceExpansion::Maximal));
    // TODO: If Obj-C interop is off, we can relax this to allow referencing
    // class metadata too.
    assert(isa<StructDecl>(theDecl) || isa<EnumDecl>(theDecl));
    auto metadata = IGF.IGM.getAddrOfTypeMetadata(theType);
    return MetadataResponse::forComplete(metadata);
  }

  // We are applying generic parameters to a generic type.
  assert(theType->isSpecialized() &&
         theType->getAnyNominal() == theDecl);

  // Check to see if we've maybe got a local reference already.
  if (auto cache = IGF.tryGetLocalTypeMetadata(theType, request))
    return cache;

  // Grab the substitutions.
  GenericArguments genericArgs;
  genericArgs.collect(IGF, theType);
  assert((!genericArgs.Values.empty() ||
          theDecl->getGenericSignature()->areAllParamsConcrete()) &&
         "no generic args?!");

  // Call the generic metadata accessor function.
  llvm::Function *accessor =
      IGF.IGM.getAddrOfGenericTypeMetadataAccessFunction(theDecl,
                                                         genericArgs.Types,
                                                         NotForDefinition);

  auto response =
    IGF.emitGenericTypeMetadataAccessFunctionCall(accessor, genericArgs.Values,
                                                  request);

  IGF.setScopedLocalTypeMetadata(theType, response);
  return response;
}

/// Is it basically trivial to access the given metadata?  If so, we don't
/// need a cache variable in its accessor.
bool irgen::isTypeMetadataAccessTrivial(IRGenModule &IGM, CanType type) {
  assert(!type->hasArchetype());

  // Value type metadata only requires dynamic initialization on first
  // access if it contains a resilient type.
  if (isa<StructType>(type) || isa<EnumType>(type)) {
    auto nominalType = cast<NominalType>(type);
    auto *nominalDecl = nominalType->getDecl();

    // Imported type metadata always requires an accessor.
    if (isa<ClangModuleUnit>(nominalDecl->getModuleScopeContext()))
      return false;

    // Generic type metadata always requires an accessor.
    if (nominalDecl->isGenericContext())
      return false;

    auto expansion = ResilienceExpansion::Maximal;

    // Resiliently-sized metadata access always requires an accessor.
    return (IGM.getTypeInfoForUnlowered(type).isFixedSize(expansion));
  }

  // The empty tuple type has a singleton metadata.
  if (auto tuple = dyn_cast<TupleType>(type))
    return tuple->getNumElements() == 0;
  
  // Any and AnyObject have singleton metadata.
  if (type->isAny() || type->isAnyObject())
    return true;

  // The builtin types generally don't require metadata, but some of them
  // have nodes in the runtime anyway.
  if (isa<BuiltinType>(type))
    return true;

  // SIL box types are artificial, but for the purposes of dynamic layout,
  // we use the NativeObject metadata.
  if (isa<SILBoxType>(type))
    return true;

  // DynamicSelfType is actually local.
  if (type->hasDynamicSelfType())
    return true;

  return false;
}

/// Return the standard access strategy for getting a non-dependent
/// type metadata object.
MetadataAccessStrategy irgen::getTypeMetadataAccessStrategy(CanType type) {
  // We should not be emitting accessors for partially-substituted
  // generic types.
  assert(!type->hasArchetype());

  // Non-generic structs, enums, and classes are special cases.
  //
  // Note that while protocol types don't have a metadata pattern,
  // we still require an accessor since we actually want to get
  // the metadata for the existential type.
  //
  // This needs to kept in sync with hasRequiredTypeMetadataAccessPattern.
  auto nominal = type->getAnyNominal();
  if (nominal && !isa<ProtocolDecl>(nominal)) {
    // Metadata accessors for fully-substituted generic types are
    // emitted with shared linkage.
    if (nominal->isGenericContext() && !nominal->isObjC()) {
      if (type->isSpecialized())
        return MetadataAccessStrategy::NonUniqueAccessor;
      assert(type->hasUnboundGenericType());
    }

    if (requiresForeignTypeMetadata(nominal))
      return MetadataAccessStrategy::ForeignAccessor;

    // If the type doesn't guarantee that it has an access function,
    // we might have to use a non-unique accessor.

    // Everything else requires accessors.
    switch (getDeclLinkage(nominal)) {
    case FormalLinkage::PublicUnique:
      return MetadataAccessStrategy::PublicUniqueAccessor;
    case FormalLinkage::HiddenUnique:
      return MetadataAccessStrategy::HiddenUniqueAccessor;
    case FormalLinkage::Private:
      return MetadataAccessStrategy::PrivateAccessor;

    case FormalLinkage::PublicNonUnique:
      return MetadataAccessStrategy::NonUniqueAccessor;
    }
    llvm_unreachable("bad formal linkage");
  }

  // Everything else requires a shared accessor function.
  return MetadataAccessStrategy::NonUniqueAccessor;
}

/// Emit a string encoding the labels in the given tuple type.
static llvm::Constant *getTupleLabelsString(IRGenModule &IGM,
                                            CanTupleType type,
                                            bool useLabels) {
  // If we were asked to ignore the labels, do so.
  if (!useLabels) {
    return llvm::ConstantPointerNull::get(IGM.Int8PtrTy);
  }

  bool hasLabels = false;
  llvm::SmallString<128> buffer;
  for (auto &elt : type->getElements()) {
    if (elt.hasName()) {
      hasLabels = true;
      buffer.append(elt.getName().str());
    }

    // Each label is space-terminated.
    buffer += ' ';
  }

  // If there are no labels, use a null pointer.
  if (!hasLabels) {
    return llvm::ConstantPointerNull::get(IGM.Int8PtrTy);
  }

  // Otherwise, create a new string literal.
  // This method implicitly adds a null terminator.
  return IGM.getAddrOfGlobalString(buffer);
}

static llvm::Constant *emitEmptyTupleTypeMetadataRef(IRGenModule &IGM) {
  llvm::Constant *fullMetadata = IGM.getEmptyTupleMetadata();
  llvm::Constant *indices[] = {
    llvm::ConstantInt::get(IGM.Int32Ty, 0),
    llvm::ConstantInt::get(IGM.Int32Ty, 1)
  };
  return llvm::ConstantExpr::getInBoundsGetElementPtr(
        /*Ty=*/nullptr, fullMetadata, indices);
}

using GetElementMetadataFn =
  llvm::function_ref<MetadataResponse(CanType eltType,
                                      DynamicMetadataRequest eltRequest)>;

static MetadataResponse emitTupleTypeMetadataRef(IRGenFunction &IGF,
                                                 CanTupleType type,
                                                 DynamicMetadataRequest request,
                                                 bool useLabels,
                                    GetElementMetadataFn getMetadataRecursive) {
  auto getElementMetadata = [&](CanType type) {
    // Just request the elements to be abstract so that we can always build
    // the metadata.
    // TODO: if we have a collector, or if this is a blocking request, maybe
    // we should build a stronger request?
    return getMetadataRecursive(type, MetadataState::Abstract).getMetadata();
  };

  switch (type->getNumElements()) {
  case 0:
    return MetadataResponse::forComplete(
                                        emitEmptyTupleTypeMetadataRef(IGF.IGM));

  case 1:
    // For metadata purposes, we consider a singleton tuple to be
    // isomorphic to its element type. ???
    return getMetadataRecursive(type.getElementType(0), request);

  case 2: {
    auto elt0Metadata = getElementMetadata(type.getElementType(0));
    auto elt1Metadata = getElementMetadata(type.getElementType(1));

    llvm::Value *args[] = {
      request.get(IGF),
      elt0Metadata, elt1Metadata,
      getTupleLabelsString(IGF.IGM, type, useLabels),
      llvm::ConstantPointerNull::get(IGF.IGM.WitnessTablePtrTy) // proposed
    };

    auto call = IGF.Builder.CreateCall(IGF.IGM.getGetTupleMetadata2Fn(),
                                       args);
    call->setCallingConv(IGF.IGM.SwiftCC);
    call->setDoesNotThrow();

    return MetadataResponse::handle(IGF, request, call);
  }

  case 3: {
    auto elt0Metadata = getElementMetadata(type.getElementType(0));
    auto elt1Metadata = getElementMetadata(type.getElementType(1));
    auto elt2Metadata = getElementMetadata(type.getElementType(2));

    llvm::Value *args[] = {
      request.get(IGF),
      elt0Metadata, elt1Metadata, elt2Metadata,
      getTupleLabelsString(IGF.IGM, type, useLabels),
      llvm::ConstantPointerNull::get(IGF.IGM.WitnessTablePtrTy) // proposed
    };

    auto call = IGF.Builder.CreateCall(IGF.IGM.getGetTupleMetadata3Fn(),
                                       args);
    call->setCallingConv(IGF.IGM.SwiftCC);
    call->setDoesNotThrow();

    return MetadataResponse::handle(IGF, request, call);
  }
  default:
    // TODO: use a caching entrypoint (with all information
    // out-of-line) for non-dependent tuples.

    llvm::Value *pointerToFirst = nullptr; // appease -Wuninitialized

    auto elements = type.getElementTypes();
    auto arrayTy = llvm::ArrayType::get(IGF.IGM.TypeMetadataPtrTy,
                                        elements.size());
    Address buffer = IGF.createAlloca(arrayTy,IGF.IGM.getPointerAlignment(),
                                      "tuple-elements");
    IGF.Builder.CreateLifetimeStart(buffer,
                                IGF.IGM.getPointerSize() * elements.size());
    for (auto i : indices(elements)) {
      // Find the metadata pointer for this element.
      llvm::Value *eltMetadata = getElementMetadata(elements[i]);

      // GEP to the appropriate element and store.
      Address eltPtr = IGF.Builder.CreateStructGEP(buffer, i,
                                                 IGF.IGM.getPointerSize());
      IGF.Builder.CreateStore(eltMetadata, eltPtr);

      // Remember the GEP to the first element.
      if (i == 0) pointerToFirst = eltPtr.getAddress();
    }

    TupleTypeFlags flags =
      TupleTypeFlags().withNumElements(elements.size());
    llvm::Value *args[] = {
      request.get(IGF),
      llvm::ConstantInt::get(IGF.IGM.SizeTy, flags.getIntValue()),
      pointerToFirst,
      getTupleLabelsString(IGF.IGM, type, useLabels),
      llvm::ConstantPointerNull::get(IGF.IGM.WitnessTablePtrTy) // proposed
    };

    auto call = IGF.Builder.CreateCall(IGF.IGM.getGetTupleMetadataFn(),
                                       args);
    call->setCallingConv(IGF.IGM.SwiftCC);
    call->setDoesNotThrow();

    IGF.Builder.CreateLifetimeEnd(buffer,
                                IGF.IGM.getPointerSize() * elements.size());

    return MetadataResponse::handle(IGF, request, call);
  }
}

namespace {
  /// A visitor class for emitting a reference to a metatype object.
  /// This implements a "raw" access, useful for implementing cache
  /// functions or for implementing dependent accesses.
  ///
  /// If the access requires runtime initialization, that initialization
  /// must be dependency-ordered-before any load that carries a dependency
  /// from the resulting metadata pointer.
  class EmitTypeMetadataRef
    : public CanTypeVisitor<EmitTypeMetadataRef, MetadataResponse,
                            DynamicMetadataRequest> {
  private:
    IRGenFunction &IGF;
  public:
    EmitTypeMetadataRef(IRGenFunction &IGF) : IGF(IGF) {}

    MetadataResponse emitDirectMetadataRef(CanType type) {
      return MetadataResponse::forComplete(IGF.IGM.getAddrOfTypeMetadata(type));
    }

    /// The given type should use opaque type info.  We assume that
    /// the runtime always provides an entry for such a type.
    MetadataResponse visitBuiltinIntegerType(CanBuiltinIntegerType type,
                                             DynamicMetadataRequest request) {
      // If the size isn't a power up two, round up to the next power of two
      // and use the corresponding integer type.
      auto &opaqueTI = cast<FixedTypeInfo>(IGF.IGM.getTypeInfoForLowered(type));
      unsigned numBits = opaqueTI.getFixedSize().getValueInBits();
      if (!llvm::isPowerOf2_32(numBits)) {
        numBits = llvm::NextPowerOf2(numBits);
        type = CanBuiltinIntegerType(
                 BuiltinIntegerType::get(numBits, IGF.IGM.Context));
      }

      return emitDirectMetadataRef(type);
    }

    MetadataResponse
    visitBuiltinIntegerLiteralType(CanBuiltinIntegerLiteralType type,
                                   DynamicMetadataRequest request) {
      return emitDirectMetadataRef(type);
    }

    MetadataResponse
    visitBuiltinNativeObjectType(CanBuiltinNativeObjectType type,
                                 DynamicMetadataRequest request) {
      return emitDirectMetadataRef(type);
    }

    MetadataResponse
    visitBuiltinBridgeObjectType(CanBuiltinBridgeObjectType type,
                                 DynamicMetadataRequest request) {
      return emitDirectMetadataRef(type);
    }

    MetadataResponse
    visitBuiltinUnknownObjectType(CanBuiltinUnknownObjectType type,
                                  DynamicMetadataRequest request) {
      return emitDirectMetadataRef(type);
    }

    MetadataResponse
    visitBuiltinUnsafeValueBufferType(CanBuiltinUnsafeValueBufferType type,
                                      DynamicMetadataRequest request) {
      return emitDirectMetadataRef(type);
    }

    MetadataResponse
    visitBuiltinRawPointerType(CanBuiltinRawPointerType type,
                               DynamicMetadataRequest request) {
      return emitDirectMetadataRef(type);
    }

    MetadataResponse
    visitBuiltinFloatType(CanBuiltinFloatType type,
                          DynamicMetadataRequest request) {
      return emitDirectMetadataRef(type);
    }

    MetadataResponse
    visitBuiltinVectorType(CanBuiltinVectorType type,
                           DynamicMetadataRequest request) {
      return emitDirectMetadataRef(type);
    }

    MetadataResponse visitNominalType(CanNominalType type,
                                      DynamicMetadataRequest request) {
      assert(!type->isExistentialType());
      return emitNominalMetadataRef(IGF, type->getDecl(), type, request);
    }

    MetadataResponse visitBoundGenericType(CanBoundGenericType type,
                                           DynamicMetadataRequest request) {
      assert(!type->isExistentialType());
      return emitNominalMetadataRef(IGF, type->getDecl(), type, request);
    }

    MetadataResponse visitTupleType(CanTupleType type,
                                    DynamicMetadataRequest request) {
      if (auto cached = tryGetLocal(type, request))
        return cached;

      auto response = emitTupleTypeMetadataRef(IGF, type, request,
                                               /*labels*/ true,
          [&](CanType eltType, DynamicMetadataRequest eltRequest) {
        return IGF.emitTypeMetadataRef(eltType, eltRequest);
      });

      return setLocal(type, response);
    }

    MetadataResponse visitGenericFunctionType(CanGenericFunctionType type,
                                              DynamicMetadataRequest request) {
      IGF.unimplemented(SourceLoc(),
                        "metadata ref for generic function type");
      return MetadataResponse::getUndef(IGF);
    }

    llvm::Value *getFunctionParameterRef(AnyFunctionType::CanParam &param) {
      auto type = param.getPlainType()->getCanonicalType();
      return IGF.emitAbstractTypeMetadataRef(type);
    }

    MetadataResponse visitFunctionType(CanFunctionType type,
                                       DynamicMetadataRequest request) {
      if (auto metatype = tryGetLocal(type, request))
        return metatype;

      auto result =
        IGF.emitAbstractTypeMetadataRef(type->getResult()->getCanonicalType());

      auto params = type.getParams();
      auto numParams = params.size();

      // Retrieve the ABI parameter flags from the type-level parameter
      // flags.
      auto getABIParameterFlags = [](ParameterTypeFlags flags) {
        return ParameterFlags()
            .withValueOwnership(flags.getValueOwnership())
            .withVariadic(flags.isVariadic())
            .withAutoClosure(flags.isAutoClosure());
      };

      bool hasFlags = false;
      for (auto param : params) {
        if (!getABIParameterFlags(param.getParameterFlags()).isNone()) {
          hasFlags = true;
          break;
        }
      }

      // Map the convention to a runtime metadata value.
      FunctionMetadataConvention metadataConvention;
      bool isEscaping = false;
      switch (type->getRepresentation()) {
      case FunctionTypeRepresentation::Swift:
        metadataConvention = FunctionMetadataConvention::Swift;
        isEscaping = !type->isNoEscape();
        break;
      case FunctionTypeRepresentation::Thin:
        metadataConvention = FunctionMetadataConvention::Thin;
        break;
      case FunctionTypeRepresentation::Block:
        metadataConvention = FunctionMetadataConvention::Block;
        break;
      case FunctionTypeRepresentation::CFunctionPointer:
        metadataConvention = FunctionMetadataConvention::CFunctionPointer;
        break;
      }

      auto flagsVal = FunctionTypeFlags()
                          .withNumParameters(numParams)
                          .withConvention(metadataConvention)
                          .withThrows(type->throws())
                          .withParameterFlags(hasFlags)
                          .withEscaping(isEscaping);

      auto flags = llvm::ConstantInt::get(IGF.IGM.SizeTy,
                                          flagsVal.getIntValue());

      auto collectParameters =
          [&](llvm::function_ref<void(unsigned, llvm::Value *,
                                      ParameterFlags flags)>
                  processor) {
            for (auto index : indices(params)) {
              auto param = params[index];
              auto flags = param.getParameterFlags();

              auto parameterFlags = getABIParameterFlags(flags);
              processor(index, getFunctionParameterRef(param), parameterFlags);
            }
          };

      auto constructSimpleCall =
          [&](llvm::SmallVectorImpl<llvm::Value *> &arguments)
          -> llvm::Constant * {
        arguments.push_back(flags);

        collectParameters([&](unsigned i, llvm::Value *typeRef,
                              ParameterFlags flags) {
          arguments.push_back(typeRef);
          if (hasFlags)
            arguments.push_back(
                llvm::ConstantInt::get(IGF.IGM.Int32Ty, flags.getIntValue()));
        });

        arguments.push_back(result);

        switch (params.size()) {
        case 0:
          return IGF.IGM.getGetFunctionMetadata0Fn();

        case 1:
          return IGF.IGM.getGetFunctionMetadata1Fn();

        case 2:
          return IGF.IGM.getGetFunctionMetadata2Fn();

        case 3:
          return IGF.IGM.getGetFunctionMetadata3Fn();

        default:
          llvm_unreachable("supports only 1/2/3 parameter functions");
        }
      };

      switch (numParams) {
      case 0:
      case 1:
      case 2:
      case 3: {
        if (!hasFlags) {
          llvm::SmallVector<llvm::Value *, 8> arguments;
          auto *metadataFn = constructSimpleCall(arguments);
          auto *call = IGF.Builder.CreateCall(metadataFn, arguments);
          call->setDoesNotThrow();
          return setLocal(CanType(type), MetadataResponse::forComplete(call));
        }

        // If function type has parameter flags, let's emit
        // the most general function to retrieve them.
        LLVM_FALLTHROUGH;
      }

      default:
        assert(!params.empty() && "0 parameter case is specialized!");

        auto *const Int32Ptr = IGF.IGM.Int32Ty->getPointerTo();
        llvm::SmallVector<llvm::Value *, 8> arguments;

        arguments.push_back(flags);

        ConstantInitBuilder paramFlags(IGF.IGM);
        auto flagsArr = paramFlags.beginArray();

        auto arrayTy =
            llvm::ArrayType::get(IGF.IGM.TypeMetadataPtrTy, numParams);
        Address parameters = IGF.createAlloca(
            arrayTy, IGF.IGM.getTypeMetadataAlignment(), "function-parameters");

        IGF.Builder.CreateLifetimeStart(parameters,
                                        IGF.IGM.getPointerSize() * numParams);

        collectParameters([&](unsigned i, llvm::Value *typeRef,
                              ParameterFlags flags) {
          auto argPtr = IGF.Builder.CreateStructGEP(parameters, i,
                                                    IGF.IGM.getPointerSize());
          IGF.Builder.CreateStore(typeRef, argPtr);
          if (i == 0)
            arguments.push_back(argPtr.getAddress());

          if (hasFlags)
            flagsArr.addInt32(flags.getIntValue());
        });

        if (hasFlags) {
          auto *flagsVar = flagsArr.finishAndCreateGlobal(
              "parameter-flags", IGF.IGM.getPointerAlignment(),
              /* constant */ true);
          arguments.push_back(IGF.Builder.CreateBitCast(flagsVar, Int32Ptr));
        } else {
          flagsArr.abandon();
          arguments.push_back(llvm::ConstantPointerNull::get(Int32Ptr));
        }

        arguments.push_back(result);

        auto call = IGF.Builder.CreateCall(IGF.IGM.getGetFunctionMetadataFn(),
                                           arguments);
        call->setDoesNotThrow();

        if (parameters.isValid())
          IGF.Builder.CreateLifetimeEnd(parameters,
                                        IGF.IGM.getPointerSize() * numParams);

        return setLocal(type, MetadataResponse::forComplete(call));
      }
    }

    MetadataResponse visitAnyMetatypeType(CanAnyMetatypeType type,
                                          DynamicMetadataRequest request) {
      // FIXME: We shouldn't accept a lowered metatype here, but we need to
      // represent Optional<@objc_metatype T.Type> as an AST type for ABI
      // reasons.
      
      // assert(!type->hasRepresentation()
      //       && "should not be asking for a representation-specific metatype "
      //          "metadata");
      
      if (auto metatype = tryGetLocal(type, request))
        return metatype;

      auto instMetadata =
        IGF.emitAbstractTypeMetadataRef(type.getInstanceType());
      auto fn = isa<MetatypeType>(type)
                  ? IGF.IGM.getGetMetatypeMetadataFn()
                  : IGF.IGM.getGetExistentialMetatypeMetadataFn();
      auto call = IGF.Builder.CreateCall(fn, instMetadata);
      call->setDoesNotThrow();

      return setLocal(type, MetadataResponse::forComplete(call));
    }

    MetadataResponse visitModuleType(CanModuleType type,
                                     DynamicMetadataRequest request) {
      IGF.unimplemented(SourceLoc(), "metadata ref for module type");
      return MetadataResponse::getUndef(IGF);
    }

    MetadataResponse visitDynamicSelfType(CanDynamicSelfType type,
                                          DynamicMetadataRequest request) {
      return MetadataResponse::forComplete(IGF.getLocalSelfMetadata());
    }
      
    MetadataResponse emitExistentialTypeMetadata(CanType type,
                                          DynamicMetadataRequest request) {
      if (auto metatype = tryGetLocal(type, request))
        return metatype;

      // Any and AnyObject have singleton metadata in the runtime.
      llvm::Constant *singletonMetadata = nullptr;
      if (type->isAny())
        singletonMetadata = IGF.IGM.getAnyExistentialMetadata();
      if (type->isAnyObject())
        singletonMetadata = IGF.IGM.getAnyObjectExistentialMetadata();
      
      if (singletonMetadata) {
        llvm::Constant *indices[] = {
          llvm::ConstantInt::get(IGF.IGM.Int32Ty, 0),
          llvm::ConstantInt::get(IGF.IGM.Int32Ty, 1)
        };
        return MetadataResponse::forComplete(
          llvm::ConstantExpr::getInBoundsGetElementPtr(
            /*Ty=*/nullptr, singletonMetadata, indices));
      }

      auto layout = type.getExistentialLayout();
      
      auto protocols = layout.getProtocols();

      // Collect references to the protocol descriptors.
      auto descriptorArrayTy
        = llvm::ArrayType::get(IGF.IGM.ProtocolDescriptorRefTy,
                               protocols.size());
      Address descriptorArray = IGF.createAlloca(descriptorArrayTy,
                                                 IGF.IGM.getPointerAlignment(),
                                                 "protocols");
      IGF.Builder.CreateLifetimeStart(descriptorArray,
                                   IGF.IGM.getPointerSize() * protocols.size());
      descriptorArray = IGF.Builder.CreateBitCast(descriptorArray,
                               IGF.IGM.ProtocolDescriptorRefTy->getPointerTo());
      
      unsigned index = 0;
      for (auto *protoTy : protocols) {
        auto *protoDecl = protoTy->getDecl();
        llvm::Value *ref = emitProtocolDescriptorRef(IGF, protoDecl);

        Address slot = IGF.Builder.CreateConstArrayGEP(descriptorArray,
                                               index, IGF.IGM.getPointerSize());
        IGF.Builder.CreateStore(ref, slot);
        ++index;
      }

      // Note: ProtocolClassConstraint::Class is 0, ::Any is 1.
      auto classConstraint =
        llvm::ConstantInt::get(IGF.IGM.Int1Ty,
                               !layout.requiresClass());
      llvm::Value *superclassConstraint =
        llvm::ConstantPointerNull::get(IGF.IGM.TypeMetadataPtrTy);
      if (auto superclass = layout.explicitSuperclass) {
        superclassConstraint = IGF.emitAbstractTypeMetadataRef(
          CanType(superclass));
      }

      auto call = IGF.Builder.CreateCall(IGF.IGM.getGetExistentialMetadataFn(),
                                         {classConstraint,
                                          superclassConstraint,
                                          IGF.IGM.getSize(Size(protocols.size())),
                                          descriptorArray.getAddress()});
      call->setDoesNotThrow();
      IGF.Builder.CreateLifetimeEnd(descriptorArray,
                                   IGF.IGM.getPointerSize() * protocols.size());
      return setLocal(type, MetadataResponse::forComplete(call));
    }

    MetadataResponse visitProtocolType(CanProtocolType type,
                                       DynamicMetadataRequest request) {
      return emitExistentialTypeMetadata(type, request);
    }
      
    MetadataResponse
    visitProtocolCompositionType(CanProtocolCompositionType type,
                                 DynamicMetadataRequest request) {
      return emitExistentialTypeMetadata(type, request);
    }

    MetadataResponse visitReferenceStorageType(CanReferenceStorageType type,
                                               DynamicMetadataRequest request) {
      llvm_unreachable("reference storage type should have been converted by "
                       "SILGen");
    }
    MetadataResponse visitSILFunctionType(CanSILFunctionType type,
                                          DynamicMetadataRequest request) {
      llvm_unreachable("should not be asking for metadata of a lowered SIL "
                       "function type--SILGen should have used the AST type");
    }
    MetadataResponse visitSILTokenType(CanSILTokenType type,
                                          DynamicMetadataRequest request) {
      llvm_unreachable("should not be asking for metadata of a SILToken type");
    }

    MetadataResponse visitArchetypeType(CanArchetypeType type,
                                        DynamicMetadataRequest request) {
      return emitArchetypeTypeMetadataRef(IGF, type, request);
    }

    MetadataResponse visitGenericTypeParamType(CanGenericTypeParamType type,
                                               DynamicMetadataRequest request) {
      llvm_unreachable("dependent type should have been substituted by Sema or SILGen");
    }

    MetadataResponse visitDependentMemberType(CanDependentMemberType type,
                                              DynamicMetadataRequest request) {
      llvm_unreachable("dependent type should have been substituted by Sema or SILGen");
    }

    MetadataResponse visitLValueType(CanLValueType type,
                                     DynamicMetadataRequest request) {
      llvm_unreachable("lvalue type should have been lowered by SILGen");
    }
    MetadataResponse visitInOutType(CanInOutType type,
                                    DynamicMetadataRequest request) {
      llvm_unreachable("inout type should have been lowered by SILGen");
    }
    MetadataResponse visitErrorType(CanErrorType type,
                                    DynamicMetadataRequest request) {
      llvm_unreachable("error type should not appear in IRGen");
    }

    MetadataResponse visitSILBlockStorageType(CanSILBlockStorageType type,
                                              DynamicMetadataRequest request) {
      llvm_unreachable("cannot ask for metadata of block storage");
    }

    MetadataResponse visitSILBoxType(CanSILBoxType type,
                                     DynamicMetadataRequest request) {
      // The Builtin.NativeObject metadata can stand in for boxes.
      return emitDirectMetadataRef(type->getASTContext().TheNativeObjectType);
    }

    /// Try to find the metatype in local data.
    MetadataResponse tryGetLocal(CanType type, DynamicMetadataRequest request) {
      return IGF.tryGetLocalTypeMetadata(type, request);
    }

    /// Set the metatype in local data.
    MetadataResponse setLocal(CanType type, MetadataResponse response) {
      IGF.setScopedLocalTypeMetadata(type, response);
      return response;
    }
  };
} // end anonymous namespace

/// Emit a type metadata reference without using an accessor function.
static MetadataResponse emitDirectTypeMetadataRef(IRGenFunction &IGF,
                                                  CanType type,
                                           DynamicMetadataRequest request) {
  return EmitTypeMetadataRef(IGF).visit(type, request);
}

static bool isLoadFrom(llvm::Value *value, Address address) {
  if (auto load = dyn_cast<llvm::LoadInst>(value)) {
    return load->getOperand(0) == address.getAddress();
  }
  return false;
}

/// Emit the body of a cache accessor.
///
/// If cacheVariable is null, we perform the direct access every time.
/// This is used for metadata accessors that come about due to resilience,
/// where the direct access is completely trivial.
void irgen::emitCacheAccessFunction(IRGenModule &IGM,
                                    llvm::Function *accessor,
                                    llvm::Constant *cacheVariable,
                                    CacheStrategy cacheStrategy,
                                    CacheEmitter getValue,
                                    bool isReadNone) {
  assert((cacheStrategy == CacheStrategy::None) == (cacheVariable == nullptr));
  accessor->setDoesNotThrow();

  // This function is logically 'readnone': the caller does not need
  // to reason about any side effects or stores it might perform.
  if (isReadNone)
    accessor->setDoesNotAccessMemory();

  IRGenFunction IGF(IGM, accessor);
  if (IGM.DebugInfo)
    IGM.DebugInfo->emitArtificialFunction(IGF, accessor);

  auto parameters = IGF.collectParameters();

  bool returnsResponse =
    (accessor->getReturnType() == IGM.TypeMetadataResponseTy);

  switch (cacheStrategy) {

  // If there's no cache variable, just perform the direct access.
  case CacheStrategy::None: {
    auto response = getValue(IGF, parameters);
    llvm::Value *ret;
    if (returnsResponse) {
      response.ensureDynamicState(IGF);
      ret = response.combine(IGF);
    } else {
      assert(response.isStaticallyKnownComplete());
      ret = response.getMetadata();
    }
    IGF.Builder.CreateRet(ret);
    return;
  }

  // For in-place initialization, drill to the first element of the cache.
  case CacheStrategy::SingletonInitialization:
    cacheVariable =
      llvm::ConstantExpr::getBitCast(cacheVariable,
                                     IGM.TypeMetadataPtrTy->getPointerTo());
    break;

  case CacheStrategy::Lazy:
    break;
  }

  llvm::Constant *null =
    llvm::ConstantPointerNull::get(
      cast<llvm::PointerType>(
        cacheVariable->getType()->getPointerElementType()));

  Address cache(cacheVariable, IGM.getPointerAlignment());

  // Okay, first thing, check the cache variable.
  //
  // Conceptually, this needs to establish memory ordering with the
  // store we do later in the function: if the metadata value is
  // non-null, we must be able to see any stores performed by the
  // initialization of the metadata.  However, any attempt to read
  // from the metadata will be address-dependent on the loaded
  // metadata pointer, which is sufficient to provide adequate
  // memory ordering guarantees on all the platforms we care about:
  // ARM has special rules about address dependencies, and x86's
  // memory ordering is strong enough to guarantee the visibility
  // even without the address dependency.
  //
  // And we do not need to worry about the compiler because the
  // address dependency naturally forces an order to the memory
  // accesses.
  //
  // Therefore, we can perform a completely naked load here.
  // FIXME: Technically should be "consume", but that introduces barriers in the
  // current LLVM ARM backend.
  auto load = IGF.Builder.CreateLoad(cache);
  // Make this barrier explicit when building for TSan to avoid false positives.
  if (IGM.IRGen.Opts.Sanitizers & SanitizerKind::Thread)
    load->setOrdering(llvm::AtomicOrdering::Acquire);

  // Compare the load result against null.
  auto isNullBB = IGF.createBasicBlock("cacheIsNull");
  auto contBB = IGF.createBasicBlock("cont");
  llvm::Value *comparison = IGF.Builder.CreateICmpEQ(load, null);
  IGF.Builder.CreateCondBr(comparison, isNullBB, contBB);
  auto loadBB = IGF.Builder.GetInsertBlock();

  // If the load yielded null, emit the type metadata.
  IGF.Builder.emitBlock(isNullBB);
  MetadataResponse response = getValue(IGF, parameters);

  // Ensure that we have a dynamically-correct state value.
  llvm::Constant *completedState = nullptr;
  if (returnsResponse) {
    completedState = MetadataResponse::getCompletedState(IGM);
    response.ensureDynamicState(IGF);
  }

  auto directResult = response.getMetadata();

  // Emit a branch around the caching code if we're working with responses
  // and the fetched result is not complete.  We can avoid doing this if
  // the response is statically known to be complete, and we don't need to
  // do it if this is an in-place initiazation cache because the store
  // is done within the runtime.
  llvm::BasicBlock *completionCheckBB = nullptr;
  llvm::Value *directState = nullptr;
  if (cacheStrategy == CacheStrategy::SingletonInitialization) {
    directState = response.getDynamicState();
    completionCheckBB = IGF.Builder.GetInsertBlock();
  } else {
    if (returnsResponse &&
        !response.isStaticallyKnownComplete()) {
      completionCheckBB = IGF.Builder.GetInsertBlock();
      directState = response.getDynamicState();

      auto isCompleteBB = IGF.createBasicBlock("is_complete");
      auto isComplete =
        IGF.Builder.CreateICmpEQ(directState, completedState);

      IGF.Builder.CreateCondBr(isComplete, isCompleteBB, contBB);
      IGF.Builder.emitBlock(isCompleteBB);
    }

    // Store it back to the cache variable.  This needs to be a store-release
    // because it needs to propagate memory visibility to the other threads
    // that can access the cache: the initializing stores might be visible
    // to this thread, but they aren't transitively guaranteed to be visible
    // to other threads unless this is a store-release.
    //
    // However, we can skip this if the value was actually loaded from the
    // cache.  This is a simple, if hacky, peephole that's useful for the
    // code in emitOnceTypeMetadataAccessFunctionBody.
    if (!isLoadFrom(directResult, cache)) {
      IGF.Builder.CreateStore(directResult, cache)
        ->setAtomic(llvm::AtomicOrdering::Release);
    }
  }

  IGF.Builder.CreateBr(contBB);
  auto storeBB = IGF.Builder.GetInsertBlock();

  // Emit the continuation block.
  IGF.Builder.emitBlock(contBB);

  // Add a phi for the metadata value.
  auto phi = IGF.Builder.CreatePHI(null->getType(), 3);
  phi->addIncoming(load, loadBB);
  phi->addIncoming(directResult, storeBB);

  // Add a phi for the metadata state if we're returning a response.
  llvm::Value *stateToReturn = nullptr;
  if (directState) {
    if (storeBB != completionCheckBB)
      phi->addIncoming(directResult, completionCheckBB);

    auto completionStatePHI = IGF.Builder.CreatePHI(IGM.SizeTy, 3);
    completionStatePHI->addIncoming(completedState, loadBB);
    completionStatePHI->addIncoming(directState, completionCheckBB);
    if (storeBB != completionCheckBB)
      completionStatePHI->addIncoming(completedState, storeBB);
    stateToReturn = completionStatePHI;
  } else if (returnsResponse) {
    stateToReturn = completedState;
  }

  // Build the return value.
  llvm::Value *ret;
  if (returnsResponse) {
    ret = MetadataResponse(phi, stateToReturn, MetadataState::Abstract)
            .combine(IGF);
  } else {
    ret = phi;
  }

  IGF.Builder.CreateRet(ret);
}

MetadataResponse
IRGenFunction::emitGenericTypeMetadataAccessFunctionCall(
                                              llvm::Function *accessFunction,
                                              ArrayRef<llvm::Value *> args,
                                              DynamicMetadataRequest request) {

  SmallVector<llvm::Value *, 8> callArgs;

  // Add the metadata request argument.
  callArgs.push_back(request.get(*this));

  Address argsBuffer;
  bool allocatedArgsBuffer = false;
  if (args.size() > NumDirectGenericTypeMetadataAccessFunctionArgs) {
    // Allocate an array to pass the arguments.
    auto argsBufferTy = llvm::ArrayType::get(IGM.Int8PtrTy, args.size());
    argsBuffer = createAlloca(argsBufferTy, IGM.getPointerAlignment());

    // Mark the beginning of the array lifetime.
    Builder.CreateLifetimeStart(argsBuffer,
                                IGM.getPointerSize() * args.size());
    allocatedArgsBuffer = true;

    // Fill in the buffer.
    for (unsigned i : indices(args)) {
      Address elt = Builder.CreateStructGEP(argsBuffer, i,
                                            IGM.getPointerSize() * i);
      auto *arg =
        Builder.CreateBitCast(args[i], elt.getType()->getPointerElementType());
      Builder.CreateStore(arg, elt);
    }

    // Add the buffer to the call arguments.
    callArgs.push_back(
      Builder.CreateBitCast(argsBuffer.getAddress(), IGM.Int8PtrPtrTy));
  } else {
    callArgs.append(args.begin(), args.end());
  }

  auto call = Builder.CreateCall(accessFunction, callArgs);
  call->setDoesNotThrow();
  call->setCallingConv(IGM.SwiftCC);
  call->addAttribute(llvm::AttributeList::FunctionIndex,
                     allocatedArgsBuffer
                       ? llvm::Attribute::InaccessibleMemOrArgMemOnly
                       : llvm::Attribute::ReadNone);

  // If we allocated a buffer for the arguments, end its lifetime.
  if (allocatedArgsBuffer)
    Builder.CreateLifetimeEnd(argsBuffer, IGM.getPointerSize() * args.size());

  return MetadataResponse::handle(*this, request, call);
}

static MetadataResponse
emitGenericTypeMetadataAccessFunction(IRGenFunction &IGF,
                                      Explosion &params,
                                      NominalTypeDecl *nominal,
                                      GenericArguments &genericArgs) {
  llvm::Value *descriptor =
    IGF.IGM.getAddrOfTypeContextDescriptor(nominal, RequireMetadata);

  auto request = params.claimNext();

  auto numArguments = genericArgs.Types.size();

  bool allocatedBuffer = false;
  Address argsBuffer;
  if (numArguments > NumDirectGenericTypeMetadataAccessFunctionArgs) {
    // The caller provided a buffer with enough space for all of the arguments;
    // use that.
    argsBuffer = Address(params.claimNext(), IGF.IGM.getPointerAlignment());
  } else {
    // Allocate a buffer with enough storage for the arguments.
    auto argsBufferTy =
      llvm::StructType::get(IGF.IGM.LLVMContext, genericArgs.Types);
    argsBuffer = IGF.createAlloca(argsBufferTy,
                                  IGF.IGM.getPointerAlignment(),
                                  "generic.arguments");
    IGF.Builder.CreateLifetimeStart(argsBuffer,
                            IGF.IGM.getPointerSize() * genericArgs.Values.size());
    allocatedBuffer = true;

    // Store direct arguments into the buffer.
    for (auto i : range(numArguments)) {
      Address elt = IGF.Builder.CreateStructGEP(argsBuffer, i,
                                                IGF.IGM.getPointerSize() * i);

      auto *arg =
        IGF.Builder.CreateBitCast(params.claimNext(),
                                  elt.getType()->getPointerElementType());
      IGF.Builder.CreateStore(arg, elt);
    }
  }

  llvm::Value *arguments =
    IGF.Builder.CreateBitCast(argsBuffer.getAddress(), IGF.IGM.Int8PtrTy);

  // Make the call.
  auto result = IGF.Builder.CreateCall(IGF.IGM.getGetGenericMetadataFn(),
                                       {request, arguments, descriptor});
  result->setDoesNotThrow();
  result->setCallingConv(IGF.IGM.SwiftCC);
  result->addAttribute(llvm::AttributeList::FunctionIndex,
                       llvm::Attribute::ReadOnly);

  // If we allocated the array ourselves, end its lifetime.
  if (allocatedBuffer) {
    IGF.Builder.CreateLifetimeEnd(argsBuffer,
                          IGF.IGM.getPointerSize() * genericArgs.Values.size());
  }

  return MetadataResponse::handle(IGF, DynamicMetadataRequest(request), result);
}

static llvm::Value *
emitIdempotentClassMetadataInitialization(IRGenFunction &IGF,
                                          llvm::Value *metadata) {
  if (IGF.IGM.ObjCInterop) {
    metadata = IGF.Builder.CreateBitCast(metadata, IGF.IGM.ObjCClassPtrTy);
    metadata = IGF.Builder.CreateCall(IGF.IGM.getGetInitializedObjCClassFn(),
                                      metadata);
    metadata = IGF.Builder.CreateBitCast(metadata, IGF.IGM.TypeMetadataPtrTy);
  }

  return metadata;
}

/// Emit the body of a metadata accessor function for the given type.
///
/// This function is appropriate for ordinary situations where the
/// construction of the metadata value just involves calling idempotent
/// metadata-construction functions.  It is not used for the in-place
/// initialization of non-generic nominal type metadata.
static MetadataResponse
emitDirectTypeMetadataAccessFunctionBody(IRGenFunction &IGF,
                                         DynamicMetadataRequest request,
                                         CanType type) {
  assert(!type->hasArchetype() &&
         "cannot emit metadata accessor for context-dependent type");

  // We only take this path for non-generic nominal types.
  auto typeDecl = type->getAnyNominal();
  if (!typeDecl)
    return emitDirectTypeMetadataRef(IGF, type, request);

  if (typeDecl->isGenericContext() &&
      !(isa<ClassDecl>(typeDecl) &&
        isa<ClangModuleUnit>(typeDecl->getModuleScopeContext()))) {
    // This is a metadata accessor for a fully substituted generic type.
    return emitDirectTypeMetadataRef(IGF, type, request);
  }

  // We should never be emitting a metadata accessor for resilient nominal
  // types outside of their defining module.  We'd only do that anyway for
  // types that don't guarantee the existence of a non-unique access
  // function, and that should never be true of a resilient type with
  // external availability.
  //
  // (The type might still not have a statically-known layout.  It just
  // can't be resilient at the top level: we have to know its immediate
  // members, or we can't even begin to approach the problem of emitting
  // metadata for it.)
  assert(!IGF.IGM.isResilient(typeDecl, ResilienceExpansion::Maximal));

  // We should never be emitting a metadata accessor for foreign type
  // metadata using this function.
  assert(!requiresForeignTypeMetadata(typeDecl));

  // Classes that might not have Swift metadata use a different
  // access pattern.
  if (auto classDecl = dyn_cast<ClassDecl>(typeDecl)) {
    if (!hasKnownSwiftMetadata(IGF.IGM, classDecl)) {
      return MetadataResponse::forComplete(emitObjCMetadataRef(IGF, classDecl));
    }

    llvm::Constant *metadata = IGF.IGM.getAddrOfTypeMetadata(type);
    return MetadataResponse::forComplete(
      emitIdempotentClassMetadataInitialization(IGF, metadata));
  }

  // We should not be doing more serious work along this path.
  assert(isTypeMetadataAccessTrivial(IGF.IGM, type));

  // Okay, everything else is built from a Swift metadata object.
  llvm::Constant *metadata = IGF.IGM.getAddrOfTypeMetadata(type);

  return MetadataResponse::forComplete(metadata);
}

static llvm::Function *getAccessFunctionPrototype(IRGenModule &IGM,
                                                  CanType type,
                                               ForDefinition_t forDefinition) {
  assert(!type->hasArchetype());
  // Type should be bound unless it's type erased.
  assert(isTypeErasedGenericClassType(type)
           ? !isa<BoundGenericType>(type)
           : !isa<UnboundGenericType>(type));

  return IGM.getAddrOfTypeMetadataAccessFunction(type, forDefinition);
}

llvm::Function *
irgen::getOtherwiseDefinedTypeMetadataAccessFunction(IRGenModule &IGM,
                                                     CanType type) {
  return getAccessFunctionPrototype(IGM, type, NotForDefinition);
}

/// Get or create an accessor function to the given non-dependent type.
llvm::Function *
irgen::createTypeMetadataAccessFunction(IRGenModule &IGM, CanType type,
                                        CacheStrategy cacheStrategy,
                                        MetadataAccessGenerator generator,
                                        bool allowExistingDefinition) {
  // Get the prototype.
  auto accessor = getAccessFunctionPrototype(IGM, type, ForDefinition);

  // If we're not supposed to define the accessor, or if we already
  // have defined it, just return the pointer.
  if (!accessor->empty()) {
    assert(allowExistingDefinition &&
           "repeat definition of access function!");
    return accessor;
  }

  // Okay, define the accessor.
  llvm::Constant *cacheVariable = nullptr;

  // If our preferred access method is to go via an accessor, it means
  // there is some non-trivial computation that needs to be cached.
  if (isTypeMetadataAccessTrivial(IGM, type)) {
    cacheStrategy = CacheStrategy::None;
  } else {
    switch (cacheStrategy) {
    // Nothing to do.
    case CacheStrategy::None:
      break;

    // For lazy initialization, the cache variable is just a pointer.
    case CacheStrategy::Lazy:
      cacheVariable =
        IGM.getAddrOfTypeMetadataLazyCacheVariable(type, ForDefinition);
      break;

    // For in-place initialization, drill down to the first element.
    case CacheStrategy::SingletonInitialization:
      cacheVariable = IGM.getAddrOfTypeMetadataSingletonInitializationCache(
                                          type->getAnyNominal(), ForDefinition);
      break;
    }

    if (IGM.getOptions().optimizeForSize())
      accessor->addFnAttr(llvm::Attribute::NoInline);
  }

  emitCacheAccessFunction(IGM, accessor, cacheVariable, cacheStrategy,
                          [&](IRGenFunction &IGF, Explosion &params) {
    auto request = DynamicMetadataRequest(params.claimNext());
    return generator(IGF, request, cacheVariable);
  });

  return accessor;
}

/// Emit a standard accessor function to the given non-dependent type.
llvm::Function *
irgen::createDirectTypeMetadataAccessFunction(IRGenModule &IGM, CanType type,
                                              bool allowExistingDefinition) {
  return createTypeMetadataAccessFunction(IGM, type, CacheStrategy::Lazy,
                                          [&](IRGenFunction &IGF,
                                              DynamicMetadataRequest request,
                                              llvm::Constant *cacheVariable) {
    // We should not be called with ForDefinition for nominal types
    // that require in-place initialization.
    return emitDirectTypeMetadataAccessFunctionBody(IGF, request, type);
  }, allowExistingDefinition);
}

/// Get or create an accessor function to the given generic type.
llvm::Function *
irgen::getGenericTypeMetadataAccessFunction(IRGenModule &IGM,
                                            NominalTypeDecl *nominal,
                                            ForDefinition_t shouldDefine) {
  assert(nominal->isGenericContext());
  assert(!isTypeErasedGenericClass(nominal));

  GenericArguments genericArgs;
  genericArgs.collectTypes(IGM, nominal);

  llvm::Function *accessor =
    IGM.getAddrOfGenericTypeMetadataAccessFunction(
        nominal, genericArgs.Types, shouldDefine);

  // If we're not supposed to define the accessor, or if we already
  // have defined it, just return the pointer.
  if (!shouldDefine || !accessor->empty())
    return accessor;

  if (IGM.getOptions().optimizeForSize())
    accessor->addFnAttr(llvm::Attribute::NoInline);

  bool isReadNone =
      (genericArgs.Types.size() <= NumDirectGenericTypeMetadataAccessFunctionArgs);

  emitCacheAccessFunction(IGM, accessor, /*cache*/nullptr, CacheStrategy::None,
                          [&](IRGenFunction &IGF, Explosion &params) {
                            return emitGenericTypeMetadataAccessFunction(
                                    IGF, params, nominal, genericArgs);
                          },
                          isReadNone);

  return accessor;
}

/// Emit a call to the type metadata accessor for the given function.
static MetadataResponse
emitCallToTypeMetadataAccessFunction(IRGenFunction &IGF, CanType type,
                                     DynamicMetadataRequest request) {
  // If we already cached the metadata, use it.
  if (auto local = IGF.tryGetLocalTypeMetadata(type, request))
    return local;

  llvm::Constant *accessor =
    getOrCreateTypeMetadataAccessFunction(IGF.IGM, type);
  llvm::CallInst *call = IGF.Builder.CreateCall(accessor, { request.get(IGF) });
  call->setCallingConv(IGF.IGM.SwiftCC);
  call->setDoesNotAccessMemory();
  call->setDoesNotThrow();

  auto response = MetadataResponse::handle(IGF, request, call);
  
  // Save the metadata for future lookups.
  IGF.setScopedLocalTypeMetadata(type, response);
  
  return response;
}

llvm::Value *IRGenFunction::emitAbstractTypeMetadataRef(CanType type) {
  return emitTypeMetadataRef(type, MetadataState::Abstract).getMetadata();
}

/// Produce the type metadata pointer for the given type.
llvm::Value *IRGenFunction::emitTypeMetadataRef(CanType type) {
  return emitTypeMetadataRef(type, MetadataState::Complete).getMetadata();
}

/// Produce the type metadata pointer for the given type.
MetadataResponse
IRGenFunction::emitTypeMetadataRef(CanType type,
                                   DynamicMetadataRequest request) {
  type = getRuntimeReifiedType(IGM, type);

  if (type->hasArchetype() ||
      isTypeMetadataAccessTrivial(IGM, type)) {
    // FIXME: propagate metadata request!
    return emitDirectTypeMetadataRef(*this, type, request);
  }

  return emitCallToTypeMetadataAccessFunction(*this, type, request);
}

/// Return the address of a function that will return type metadata 
/// for the given non-dependent type.
llvm::Function *irgen::getOrCreateTypeMetadataAccessFunction(IRGenModule &IGM,
                                                             CanType type) {
  type = getRuntimeReifiedType(IGM, type);

  assert(!type->hasArchetype() &&
         "cannot create global function to return dependent type metadata");

  switch (getTypeMetadataAccessStrategy(type)) {
  case MetadataAccessStrategy::ForeignAccessor:
  case MetadataAccessStrategy::PublicUniqueAccessor:
  case MetadataAccessStrategy::HiddenUniqueAccessor:
  case MetadataAccessStrategy::PrivateAccessor:
    return getOtherwiseDefinedTypeMetadataAccessFunction(IGM, type);
  case MetadataAccessStrategy::NonUniqueAccessor:
    return createDirectTypeMetadataAccessFunction(IGM, type,
                                                  /*allow existing*/true);
  }
  llvm_unreachable("bad type metadata access strategy");
}

namespace {
  /// A visitor class for emitting a reference to type metatype for a
  /// SILType, i.e. a lowered representation type.  In general, the type
  /// metadata produced here might not correspond to the formal type that
  /// would belong to the unlowered type.  For correctness, it is important
  /// not to cache the result as if it were the metadata for a formal type
  /// unless the type actually cannot possibly be a formal type, e.g. because
  /// it is one of the special lowered type kinds like SILFunctionType.
  ///
  /// NOTE: If you modify the special cases in this, you should update
  /// isTypeMetadataForLayoutAccessible in SIL.cpp.
  class EmitTypeMetadataRefForLayout
    : public CanTypeVisitor<EmitTypeMetadataRefForLayout, llvm::Value *,
                            DynamicMetadataRequest> {
  private:
    IRGenFunction &IGF;
  public:
    EmitTypeMetadataRefForLayout(IRGenFunction &IGF) : IGF(IGF) {}

    llvm::Value *emitDirectMetadataRef(CanType type,
                                       DynamicMetadataRequest request) {
      return IGF.IGM.getAddrOfTypeMetadata(type);
    }

    /// For most types, we can just emit the usual metadata.
    llvm::Value *visitType(CanType t, DynamicMetadataRequest request) {
      return IGF.emitTypeMetadataRef(t, request).getMetadata();
    }

    llvm::Value *visitBoundGenericEnumType(CanBoundGenericEnumType type,
                                           DynamicMetadataRequest request) {
      // Optionals have a lowered payload type, so we recurse here.
      if (auto objectTy = type.getOptionalObjectType()) {
        if (auto metadata = tryGetLocal(type, request))
          return metadata;

        auto payloadMetadata = visit(objectTy, request);
        llvm::Value *args[] = { payloadMetadata };
        llvm::Type *types[] = { IGF.IGM.TypeMetadataPtrTy };

        // Call the generic metadata accessor function.
        llvm::Function *accessor =
            IGF.IGM.getAddrOfGenericTypeMetadataAccessFunction(
                type->getDecl(), types, NotForDefinition);

        auto response =
          IGF.emitGenericTypeMetadataAccessFunctionCall(accessor, args,
                                                        request);

        return setLocal(type, response);
      }

      // Otherwise, generic arguments are not lowered.
      return visitType(type, request);
    }

    llvm::Value *visitTupleType(CanTupleType type,
                                DynamicMetadataRequest request) {
      if (auto metadata = tryGetLocal(type, request))
        return metadata;

      auto response = emitTupleTypeMetadataRef(IGF, type, request,
                                               /*labels*/ false,
          [&](CanType eltType, DynamicMetadataRequest eltRequest) {
        // This use of 'forComplete' is technically questionable, but in
        // this class we're always producing responses we can ignore, so
        // it's okay.
        return MetadataResponse::forComplete(visit(eltType, eltRequest));
      });

      return setLocal(type, response);
    }

    llvm::Value *visitAnyFunctionType(CanAnyFunctionType type,
                                      DynamicMetadataRequest request) {
      llvm_unreachable("not a SIL type");
    }
      
    llvm::Value *visitSILFunctionType(CanSILFunctionType type,
                                      DynamicMetadataRequest request) {
      // All function types have the same layout regardless of arguments or
      // abstraction level. Use the metadata for () -> () for thick functions,
      // or Builtin.UnknownObject for block functions.
      auto &C = type->getASTContext();
      switch (type->getRepresentation()) {
      case SILFunctionType::Representation::Thin:
      case SILFunctionType::Representation::Method:
      case SILFunctionType::Representation::WitnessMethod:
      case SILFunctionType::Representation::ObjCMethod:
      case SILFunctionType::Representation::CFunctionPointer:
      case SILFunctionType::Representation::Closure:
        // A thin function looks like a plain pointer.
        // FIXME: Except for extra inhabitants?
        return emitDirectMetadataRef(C.TheRawPointerType, request);
      case SILFunctionType::Representation::Thick:
        // All function types look like () -> ().
        // FIXME: It'd be nice not to have to call through the runtime here.
        return IGF.emitTypeMetadataRef(
                 CanFunctionType::get({}, C.TheEmptyTupleType),
                                       request).getMetadata();
      case SILFunctionType::Representation::Block:
        // All block types look like Builtin.UnknownObject.
        return emitDirectMetadataRef(C.TheUnknownObjectType, request);
      }

      llvm_unreachable("Not a valid SILFunctionType.");
    }

    llvm::Value *visitAnyMetatypeType(CanAnyMetatypeType type,
                                      DynamicMetadataRequest request) {
      
      assert(type->hasRepresentation()
             && "not a lowered metatype");

      switch (type->getRepresentation()) {
      case MetatypeRepresentation::Thin:
        // Thin metatypes are empty, so they look like the empty tuple type.
        return emitEmptyTupleTypeMetadataRef(IGF.IGM);

      case MetatypeRepresentation::Thick:
      case MetatypeRepresentation::ObjC:
        // Thick and ObjC metatypes look like pointers with extra inhabitants.
        // Get the metatype metadata from the runtime.
        // FIXME: It'd be nice not to need a runtime call here; we should just
        // have a standard aligned-pointer type metadata.
        return IGF.emitTypeMetadataRef(type);
      }

      llvm_unreachable("Not a valid MetatypeRepresentation.");
    }

    /// Try to find the metatype in local data.
    llvm::Value *tryGetLocal(CanType type, DynamicMetadataRequest request) {
      auto response = IGF.tryGetLocalTypeMetadataForLayout(
                                          SILType::getPrimitiveObjectType(type),
                                          request);
      assert(request.canResponseStatusBeIgnored());
      return (response ? response.getMetadata() : nullptr);
    }

    /// Set the metatype in local data.
    llvm::Value *setLocal(CanType type, MetadataResponse response) {
      IGF.setScopedLocalTypeMetadataForLayout(
                                      SILType::getPrimitiveObjectType(type),
                                      response);
      return response.getMetadata();
    }

  };
} // end anonymous namespace

llvm::Value *IRGenFunction::emitTypeMetadataRefForLayout(SILType type) {
  return emitTypeMetadataRefForLayout(type, MetadataState::Complete);
}

llvm::Value *
IRGenFunction::emitTypeMetadataRefForLayout(SILType type,
                                            DynamicMetadataRequest request) {
  assert(request.canResponseStatusBeIgnored());
  return EmitTypeMetadataRefForLayout(*this).visit(type.getASTType(),
                                                   request);
}

namespace {

  /// A visitor class for emitting a reference to a type layout struct.
  /// There are a few ways we can emit it:
  ///
  /// - If the type is fixed-layout and we have visibility of its value
  ///   witness table (or one close enough), we can project the layout struct
  ///   from it.
  /// - If the type is fixed layout, we can emit our own copy of the layout
  ///   struct.
  /// - If the type is dynamic-layout, we have to instantiate its metadata
  ///   and project out its metadata. (FIXME: This leads to deadlocks in
  ///   recursive cases, though we can avoid many deadlocks because most
  ///   valid recursive types bottom out in fixed-sized types like classes
  ///   or pointers.)
  class EmitTypeLayoutRef
    : public CanTypeVisitor<EmitTypeLayoutRef, llvm::Value *,
                            DynamicMetadataRequest> {
  private:
    IRGenFunction &IGF;
  public:
    EmitTypeLayoutRef(IRGenFunction &IGF) : IGF(IGF) {}

    llvm::Value *emitFromValueWitnessTablePointer(llvm::Value *vwtable) {
      llvm::Value *indexConstant = llvm::ConstantInt::get(IGF.IGM.Int32Ty,
                               (unsigned)ValueWitness::First_TypeLayoutWitness);
      return IGF.Builder.CreateInBoundsGEP(IGF.IGM.Int8PtrTy, vwtable,
                                           indexConstant);
    }

    /// Emit the type layout by projecting it from a value witness table to
    /// which we have linkage.
    llvm::Value *emitFromValueWitnessTable(CanType t) {
      auto *vwtable = IGF.IGM.getAddrOfValueWitnessTable(t);
      return emitFromValueWitnessTablePointer(vwtable);
    }

    /// Emit the type layout by projecting it from dynamic type metadata.
    llvm::Value *emitFromTypeMetadata(CanType t,
                                      DynamicMetadataRequest request) {
      auto *vwtable =
        IGF.emitValueWitnessTableRef(IGF.IGM.getLoweredType(t), request);
      return emitFromValueWitnessTablePointer(vwtable);
    }

    /// Given that the type is fixed-layout, emit the type layout by
    /// emitting a global layout for it.
    llvm::Value *emitFromFixedLayout(CanType t) {
      auto layout = tryEmitFromFixedLayout(t);
      assert(layout && "type must be fixed-size to call emitFromFixedLayout");
      return layout;
    }

    /// If the type is fixed-layout, emit the type layout by
    /// emitting a global layout for it.
    llvm::Value *tryEmitFromFixedLayout(CanType t) {
      auto &ti = IGF.getTypeInfo(SILType::getPrimitiveObjectType(t));
      if (auto fixedTI = dyn_cast<FixedTypeInfo>(&ti))
        return IGF.IGM.emitFixedTypeLayout(t, *fixedTI);
      return nullptr;
    }

    bool hasVisibleValueWitnessTable(CanType t) const {
      // Some builtin and structural types have value witnesses exported from
      // the runtime.
      auto &C = IGF.IGM.Context;
      if (t == C.TheEmptyTupleType
          || t == C.TheNativeObjectType
          || t == C.TheUnknownObjectType
          || t == C.TheBridgeObjectType
          || t == C.TheRawPointerType)
        return true;
      if (auto intTy = dyn_cast<BuiltinIntegerType>(t)) {
        auto width = intTy->getWidth();
        if (width.isPointerWidth())
          return true;
        if (width.isFixedWidth()) {
          switch (width.getFixedWidth()) {
          case 8:
          case 16:
          case 32:
          case 64:
          case 128:
          case 256:
            return true;
          default:
            return false;
          }
        }
        return false;
      }

      // TODO: If a nominal type is in the same source file as we're currently
      // emitting, we would be able to see its value witness table.
      return false;
    }

    /// Fallback default implementation.
    llvm::Value *visitType(CanType t, DynamicMetadataRequest request) {
      auto silTy = IGF.IGM.getLoweredType(t);
      auto &ti = IGF.getTypeInfo(silTy);

      // If the type is in the same source file, or has a common value
      // witness table exported from the runtime, we can project from the
      // value witness table instead of emitting a new record.
      if (hasVisibleValueWitnessTable(t))
        return emitFromValueWitnessTable(t);

      // If the type is a singleton aggregate, the field's layout is equivalent
      // to the aggregate's.
      if (SILType singletonFieldTy = getSingletonAggregateFieldType(IGF.IGM,
                                             silTy, ResilienceExpansion::Maximal))
        return visit(singletonFieldTy.getASTType(), request);

      // If the type is fixed-layout, emit a copy of its layout.
      if (auto fixed = dyn_cast<FixedTypeInfo>(&ti))
        return IGF.IGM.emitFixedTypeLayout(t, *fixed);

      return emitFromTypeMetadata(t, request);
    }
      
    llvm::Value *visitAnyFunctionType(CanAnyFunctionType type,
                                      DynamicMetadataRequest request) {
      llvm_unreachable("not a SIL type");
    }
      
    llvm::Value *visitSILFunctionType(CanSILFunctionType type,
                                      DynamicMetadataRequest request) {
      // All function types have the same layout regardless of arguments or
      // abstraction level. Use the value witness table for
      // @convention(blah) () -> () from the runtime.
      auto &C = type->getASTContext();
      switch (type->getRepresentation()) {
      case SILFunctionType::Representation::Thin:
      case SILFunctionType::Representation::Method:
      case SILFunctionType::Representation::WitnessMethod:
      case SILFunctionType::Representation::ObjCMethod:
      case SILFunctionType::Representation::CFunctionPointer:
      case SILFunctionType::Representation::Closure:
        // A thin function looks like a plain pointer.
        // FIXME: Except for extra inhabitants?
        return emitFromValueWitnessTable(C.TheRawPointerType);
      case SILFunctionType::Representation::Thick:
        // All function types look like () -> ().
        return emitFromValueWitnessTable(
                 CanFunctionType::get({}, C.TheEmptyTupleType));
      case SILFunctionType::Representation::Block:
        // All block types look like Builtin.UnknownObject.
        return emitFromValueWitnessTable(C.TheUnknownObjectType);
      }

      llvm_unreachable("Not a valid SILFunctionType.");
    }

    llvm::Value *visitAnyMetatypeType(CanAnyMetatypeType type,
                                      DynamicMetadataRequest request) {
      
      assert(type->hasRepresentation()
             && "not a lowered metatype");

      switch (type->getRepresentation()) {
      case MetatypeRepresentation::Thin: {
        // Thin metatypes are empty, so they look like the empty tuple type.
        return emitFromValueWitnessTable(IGF.IGM.Context.TheEmptyTupleType);
      }
      case MetatypeRepresentation::Thick:
        if (isa<ExistentialMetatypeType>(type)) {
          return emitFromFixedLayout(type);
        }
        // Otherwise, this is a metatype that looks like a pointer.
        LLVM_FALLTHROUGH;
      case MetatypeRepresentation::ObjC:
        // Thick metatypes look like pointers with spare bits.
        return emitFromValueWitnessTable(
                     CanMetatypeType::get(IGF.IGM.Context.TheNativeObjectType));
      }

      llvm_unreachable("Not a valid MetatypeRepresentation.");
    }

    llvm::Value *visitAnyClassType(ClassDecl *classDecl,
                                   DynamicMetadataRequest request) {
      // All class types have the same layout.
      auto type = classDecl->getDeclaredType()->getCanonicalType();
      switch (type->getReferenceCounting()) {
      case ReferenceCounting::Native:
        return emitFromValueWitnessTable(IGF.IGM.Context.TheNativeObjectType);

      case ReferenceCounting::ObjC:
      case ReferenceCounting::Block:
      case ReferenceCounting::Unknown:
        return emitFromValueWitnessTable(IGF.IGM.Context.TheUnknownObjectType);

      case ReferenceCounting::Bridge:
      case ReferenceCounting::Error:
        llvm_unreachable("classes shouldn't have this kind of refcounting");
      }

      llvm_unreachable("Not a valid ReferenceCounting.");
    }

    llvm::Value *visitClassType(CanClassType type,
                                DynamicMetadataRequest request) {
      return visitAnyClassType(type->getClassOrBoundGenericClass(), request);
    }

    llvm::Value *visitBoundGenericClassType(CanBoundGenericClassType type,
                                            DynamicMetadataRequest request) {
      return visitAnyClassType(type->getClassOrBoundGenericClass(), request);
    }

    llvm::Value *visitTupleType(CanTupleType type,
                                DynamicMetadataRequest request) {
      // Single-element tuples have exactly the same layout as their elements.
      if (type->getNumElements() == 1) {
        return visit(type.getElementType(0), request);
      }

      // If the type is fixed-layout, use a global layout.
      if (auto layout = tryEmitFromFixedLayout(type))
        return layout;

      // TODO: check for cached VWT / metadata for the type.

      // Use swift_getTupleTypeLayout to compute a layout.

      // Create a buffer to hold the result.  We don't have any reasonable
      // way to scope the lifetime of this.
      auto resultPtr = IGF.createAlloca(IGF.IGM.FullTypeLayoutTy,
                                        IGF.IGM.getPointerAlignment())
                          .getAddress();

      switch (type->getNumElements()) {
      case 0:
      case 1:
        llvm_unreachable("filtered out above");

      case 2: {
        auto elt0 = visit(type.getElementType(0), request);
        auto elt1 = visit(type.getElementType(1), request);

        // Ignore the offset.
        auto call = IGF.Builder.CreateCall(IGF.IGM.getGetTupleLayout2Fn(),
                                           {resultPtr, elt0, elt1});
        call->setDoesNotThrow();

        break;
      }

      case 3: {
        auto elt0 = visit(type.getElementType(0), request);
        auto elt1 = visit(type.getElementType(1), request);
        auto elt2 = visit(type.getElementType(2), request);

        // Ignore the offsets.
        auto call = IGF.Builder.CreateCall(IGF.IGM.getGetTupleLayout3Fn(),
                                           {resultPtr, elt0, elt1, elt2});
        call->setDoesNotThrow();

        break;
      }

      default: {
        // Allocate a temporary array for the element layouts.
        auto eltLayoutsArraySize =
          IGF.IGM.getPointerSize() * type->getNumElements();
        auto eltLayoutsArray =
          IGF.createAlloca(IGF.IGM.Int8PtrPtrTy,
                           IGF.IGM.getSize(Size(type->getNumElements())),
                           IGF.IGM.getPointerAlignment());
        IGF.Builder.CreateLifetimeStart(eltLayoutsArray, eltLayoutsArraySize);

        // Emit layouts for all the elements and store them into the array.
        for (auto i : indices(type.getElementTypes())) {
          auto eltLayout = visit(type.getElementType(i), request);
          auto eltLayoutSlot =
            i == 0 ? eltLayoutsArray
                   : IGF.Builder.CreateConstArrayGEP(eltLayoutsArray, i,
                                                     IGF.IGM.getPointerSize());
          IGF.Builder.CreateStore(eltLayout, eltLayoutSlot);
        }

        // Ignore the offsets.
        auto offsetsPtr =
          llvm::ConstantPointerNull::get(IGF.IGM.Int32Ty->getPointerTo());

        // Flags.
        auto flags = TupleTypeFlags().withNumElements(type->getNumElements());
        auto flagsValue = IGF.IGM.getSize(Size(flags.getIntValue()));

        // Compute the layout.
        auto call = IGF.Builder.CreateCall(IGF.IGM.getGetTupleLayoutFn(),
                                           {resultPtr, offsetsPtr, flagsValue,
                                            eltLayoutsArray.getAddress()});
        call->setDoesNotThrow();

        // We're done with the buffer.
        IGF.Builder.CreateLifetimeEnd(eltLayoutsArray, eltLayoutsArraySize);

        break;
      }
      }

      // Cast resultPtr to i8**, our general currency type for type layouts.
      resultPtr = IGF.Builder.CreateBitCast(resultPtr, IGF.IGM.Int8PtrPtrTy);
      return resultPtr;
    }
  };

} // end anonymous namespace

llvm::Value *irgen::emitTypeLayoutRef(IRGenFunction &IGF, SILType type,
                                      MetadataDependencyCollector *collector) {
  auto request =
    DynamicMetadataRequest::getNonBlocking(MetadataState::LayoutComplete,
                                           collector);
  assert(request.canResponseStatusBeIgnored());
  return EmitTypeLayoutRef(IGF).visit(type.getASTType(), request);
}

/// Given a class metatype, produce the necessary heap metadata
/// reference.  This is generally the metatype pointer, but may
/// instead be a reference type.
llvm::Value *irgen::emitClassHeapMetadataRefForMetatype(IRGenFunction &IGF,
                                                        llvm::Value *metatype,
                                                        CanType type) {
  // If the type is known to have Swift metadata, this is trivial.
  if (hasKnownSwiftMetadata(IGF.IGM, type))
    return metatype;

  // Otherwise, we may have to unwrap an ObjC class wrapper.
  assert(IGF.IGM.Context.LangOpts.EnableObjCInterop);
  metatype = IGF.Builder.CreateBitCast(metatype, IGF.IGM.TypeMetadataPtrTy);
  
  // Fetch the metadata for that class.
  auto call = IGF.Builder.CreateCall(IGF.IGM.getGetObjCClassFromMetadataFn(),
                                     metatype);
  call->setDoesNotThrow();
  call->setDoesNotAccessMemory();
  return call;
}

/// Produce the heap metadata pointer for the given class type.  For
/// Swift-defined types, this is equivalent to the metatype for the
/// class, but for Objective-C-defined types, this is the class
/// object.
llvm::Value *irgen::emitClassHeapMetadataRef(IRGenFunction &IGF, CanType type,
                                             MetadataValueType desiredType,
                                             DynamicMetadataRequest request,
                                             bool allowUninitialized) {
  assert(request.canResponseStatusBeIgnored() &&
         "emitClassHeapMetadataRef only supports satisfied requests");
  assert(type->mayHaveSuperclass());

  // Archetypes may or may not be ObjC classes and need unwrapping to get at
  // the class object.
  if (auto archetype = dyn_cast<ArchetypeType>(type)) {
    // Look up the Swift metadata from context.
    auto archetypeMeta = IGF.emitTypeMetadataRef(type, request).getMetadata();
    // Get the class pointer.
    auto classPtr = emitClassHeapMetadataRefForMetatype(IGF, archetypeMeta,
                                                        archetype);
    if (desiredType == MetadataValueType::ObjCClass)
      classPtr = IGF.Builder.CreateBitCast(classPtr, IGF.IGM.ObjCClassPtrTy);
    return classPtr;
  }
  
  if (ClassDecl *theClass = type->getClassOrBoundGenericClass()) {
    if (!hasKnownSwiftMetadata(IGF.IGM, theClass)) {
      llvm::Value *result =
        emitObjCHeapMetadataRef(IGF, theClass, allowUninitialized);
      if (desiredType == MetadataValueType::TypeMetadata)
        result = IGF.Builder.CreateBitCast(result, IGF.IGM.TypeMetadataPtrTy);
      return result;
    }
  }

  llvm::Value *result = IGF.emitTypeMetadataRef(type, request).getMetadata();
  if (desiredType == MetadataValueType::ObjCClass)
    result = IGF.Builder.CreateBitCast(result, IGF.IGM.ObjCClassPtrTy);
  return result;
}

/// Emit a metatype value for a known type.
void irgen::emitMetatypeRef(IRGenFunction &IGF, CanMetatypeType type,
                            Explosion &explosion) {
  switch (type->getRepresentation()) {
  case MetatypeRepresentation::Thin:
    // Thin types have a trivial representation.
    break;

  case MetatypeRepresentation::Thick:
    explosion.add(IGF.emitTypeMetadataRef(type.getInstanceType()));
    break;

  case MetatypeRepresentation::ObjC:
    explosion.add(emitClassHeapMetadataRef(IGF, type.getInstanceType(),
                                           MetadataValueType::ObjCClass,
                                           MetadataState::Complete));
    break;
  }
}

static bool canCheckStateWithBranch(DynamicMetadataRequest request,
                                    MetadataResponse response) {
  assert(request.getDependencyCollector() == nullptr ||
         (request.isStatic() && request.getStaticRequest().isNonBlocking()));

  return (response.hasDynamicState() &&
          request.getDependencyCollector() != nullptr);
}

MetadataResponse
irgen::emitCheckTypeMetadataState(IRGenFunction &IGF,
                                  DynamicMetadataRequest request,
                                  MetadataResponse response) {
  // Note that the structure of this function is mirrored in
  // getCheckTypeMetadataStateCost.

  // If the request is already satisfied by the response, we don't need
  // to check anything.
  if (request.isSatisfiedBy(response))
    return response;

  auto metadata = response.getMetadata();

  // Try to check the already-fetched dynamic state against the required state.
  if (canCheckStateWithBranch(request, response)) {
    auto dynamicState = response.getDynamicState();
    request.getDependencyCollector()
          ->checkDependency(IGF, request, metadata, dynamicState);

    return MetadataResponse(metadata, dynamicState,
                            request.getStaticRequest().getState());
  }

  // Otherwise, we have to ask the runtime.
  return emitGetTypeMetadataDynamicState(IGF, request, metadata);
}

OperationCost
irgen::getCheckTypeMetadataStateCost(DynamicMetadataRequest request,
                                     MetadataResponse response) {
  if (request.isSatisfiedBy(response))
    return OperationCost::Free;

  if (canCheckStateWithBranch(request, response))
    return OperationCost::Arithmetic;

  return OperationCost::Call;
}

/// Call swift_checkMetadataState.
MetadataResponse
irgen::emitGetTypeMetadataDynamicState(IRGenFunction &IGF,
                                       DynamicMetadataRequest request,
                                       llvm::Value *metadata) {
  auto call = IGF.Builder.CreateCall(IGF.IGM.getCheckMetadataStateFn(),
                                     { request.get(IGF), metadata });
  call->setCallingConv(IGF.IGM.SwiftCC);

  return MetadataResponse::handle(IGF, request, call);
}
