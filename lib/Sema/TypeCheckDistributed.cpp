//===--- TypeCheckDistributed.cpp - Distributed ---------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2021 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// This file implements type checking support for Swift's concurrency model.
//
//===----------------------------------------------------------------------===//
#include "TypeCheckConcurrency.h"
#include "TypeCheckDistributed.h"
#include "TypeChecker.h"
#include "TypeCheckType.h"
#include "swift/Strings.h"
#include "swift/AST/ASTWalker.h"
#include "swift/AST/Initializer.h"
#include "swift/AST/ParameterList.h"
#include "swift/AST/ProtocolConformance.h"
#include "swift/AST/DistributedDecl.h"
#include "swift/AST/NameLookupRequests.h"
#include "swift/AST/TypeCheckRequests.h"
#include "swift/AST/TypeVisitor.h"
#include "swift/AST/ExistentialLayout.h"

using namespace swift;

// ==== ------------------------------------------------------------------------

bool swift::ensureDistributedModuleLoaded(Decl *decl) {
  auto &C = decl->getASTContext();
  auto moduleAvailable = evaluateOrDefault(
      C.evaluator, DistributedModuleIsAvailableRequest{decl}, false);
  return moduleAvailable;
}

bool
DistributedModuleIsAvailableRequest::evaluate(Evaluator &evaluator,
                                              Decl *decl) const {
  auto &C = decl->getASTContext();

  if (C.getLoadedModule(C.Id_Distributed))
    return true;

  // seems we're missing the _Distributed module, ask to import it explicitly
  decl->diagnose(diag::distributed_actor_needs_explicit_distributed_import);
  return false;
}

/******************************************************************************/
/************ LOCATING AD-HOC PROTOCOL REQUIREMENT IMPLS **********************/
/******************************************************************************/

static AbstractFunctionDecl *findDistributedAdHocRequirement(
    NominalTypeDecl *decl, Identifier identifier,
    std::function<bool(AbstractFunctionDecl *)> matchFn) {
  auto &C = decl->getASTContext();

  // It would be nice to check if this is a DistributedActorSystem
  // "conforming" type, but we can't do this as we invoke this function WHILE
  // deciding if the type conforms or not;

  // Not via `ensureDistributedModuleLoaded` to avoid generating a warning,
  // we won't be emitting the offending decl after all.
  if (!C.getLoadedModule(C.Id_Distributed)) {
    return nullptr;
  }

  for (auto value : decl->lookupDirect(identifier)) {
    auto func = dyn_cast<AbstractFunctionDecl>(value);
    if (func && matchFn(func))
      return func;
  }

  return nullptr;
}

AbstractFunctionDecl *
GetDistributedActorSystemRemoteCallFunctionRequest::evaluate(
    Evaluator &evaluator, NominalTypeDecl *decl, bool isVoidReturn) const {
  auto &C = decl->getASTContext();
  auto callId = isVoidReturn ? C.Id_remoteCallVoid : C.Id_remoteCall;

  return findDistributedAdHocRequirement(
      decl, callId, [isVoidReturn](AbstractFunctionDecl *func) {
        return func->isDistributedActorSystemRemoteCall(isVoidReturn);
      });
}

AbstractFunctionDecl *
GetDistributedTargetInvocationEncoderRecordArgumentFunctionRequest::evaluate(
    Evaluator &evaluator, NominalTypeDecl *decl) const {
  auto &C = decl->getASTContext();

  return findDistributedAdHocRequirement(
      decl, C.Id_recordArgument, [](AbstractFunctionDecl *func) {
        return func->isDistributedTargetInvocationEncoderRecordArgument();
      });
}

AbstractFunctionDecl *
GetDistributedTargetInvocationEncoderRecordReturnTypeFunctionRequest::evaluate(
    Evaluator &evaluator, NominalTypeDecl *decl) const {
  auto &C = decl->getASTContext();

  return findDistributedAdHocRequirement(
      decl, C.Id_recordReturnType, [](AbstractFunctionDecl *func) {
        return func->isDistributedTargetInvocationEncoderRecordReturnType();
      });
}

AbstractFunctionDecl *
GetDistributedTargetInvocationEncoderRecordErrorTypeFunctionRequest::evaluate(
    Evaluator &evaluator, NominalTypeDecl *decl) const {
  auto &C = decl->getASTContext();
  return findDistributedAdHocRequirement(
      decl, C.Id_recordErrorType, [](AbstractFunctionDecl *func) {
        return func->isDistributedTargetInvocationEncoderRecordErrorType();
      });
}

// ==== ------------------------------------------------------------------------

/// Add Fix-It text for the given protocol type to inherit DistributedActor.
void swift::diagnoseDistributedFunctionInNonDistributedActorProtocol(
    const ProtocolDecl *proto, InFlightDiagnostic &diag) {
  if (proto->getInherited().empty()) {
    SourceLoc fixItLoc = proto->getBraces().Start;
    diag.fixItInsert(fixItLoc, ": DistributedActor");
  } else {
    // Similar to how Sendable FitIts do this, we insert at the end of
    // the inherited types.
    ASTContext &ctx = proto->getASTContext();
    SourceLoc fixItLoc = proto->getInherited().back().getSourceRange().End;
    fixItLoc = Lexer::getLocForEndOfToken(ctx.SourceMgr, fixItLoc);
    diag.fixItInsert(fixItLoc, ", DistributedActor");
  }
}


/// Add Fix-It text for the given nominal type to adopt Codable.
///
/// Useful when 'Codable' is the 'SerializationRequirement' and a non-Codable
/// function parameter or return value type is detected.
void swift::addCodableFixIt(
    const NominalTypeDecl *nominal, InFlightDiagnostic &diag) {
  if (nominal->getInherited().empty()) {
    SourceLoc fixItLoc = nominal->getBraces().Start;
    diag.fixItInsert(fixItLoc, ": Codable");
  } else {
    ASTContext &ctx = nominal->getASTContext();
    SourceLoc fixItLoc = nominal->getInherited().back().getSourceRange().End;
    fixItLoc = Lexer::getLocForEndOfToken(ctx.SourceMgr, fixItLoc);
    diag.fixItInsert(fixItLoc, ", Codable");
  }
}

// ==== ------------------------------------------------------------------------

bool IsDistributedActorRequest::evaluate(
    Evaluator &evaluator, NominalTypeDecl *nominal) const {
  // Protocols are actors if they inherit from `DistributedActor`.
  if (auto protocol = dyn_cast<ProtocolDecl>(nominal)) {
    auto &ctx = protocol->getASTContext();
    auto *distributedActorProtocol = ctx.getDistributedActorDecl();
    return (protocol == distributedActorProtocol ||
            protocol->inheritsFrom(distributedActorProtocol));
  }

  // Class declarations are 'distributed actors' if they are declared with
  // 'distributed actor'
  auto classDecl = dyn_cast<ClassDecl>(nominal);
  if(!classDecl)
    return false;

  return classDecl->isExplicitDistributedActor();
}

// ==== ------------------------------------------------------------------------

static bool checkAdHocRequirementAccessControl(
    NominalTypeDecl *decl,
    ProtocolDecl *proto,
    AbstractFunctionDecl *func) {
  if (!func)
    return false;

  // === check access control
  // if type is public, the remoteCall must be too
  if (decl->getEffectiveAccess() >= AccessLevel::Public &&
      func->getEffectiveAccess() != AccessLevel::Public) {
    func->diagnose(diag::witness_not_accessible_type,
                   diag::RequirementKind::Func,
                   func->getName(),
                   /*isSetter=*/false,
                   /*requiredAccess=*/AccessLevel::Public,
                   AccessLevel::Public,
                   proto->getName());
        return true;
  }

  return false;
}

bool swift::checkDistributedActorSystemAdHocProtocolRequirements(
    ASTContext &C,
    ProtocolDecl *Proto,
    NormalProtocolConformance *Conformance,
    Type Adoptee,
    bool diagnose) {
  auto decl = Adoptee->getAnyNominal();
  auto anyMissingAdHocRequirements = false;

  // ==== ----------------------------------------------------------------------
  // Check the ad-hoc requirements of 'DistributedActorSystem":
  // - remoteCall
  // - remoteCallVoid
  if (Proto->isSpecificProtocol(KnownProtocolKind::DistributedActorSystem)) {
    auto systemProto = C.getProtocol(KnownProtocolKind::DistributedActorSystem);

    auto remoteCallDecl =
        C.getRemoteCallOnDistributedActorSystem(decl, /*isVoidReturn=*/false);
    if (!remoteCallDecl && diagnose) {
      decl->diagnose(
          diag::distributed_actor_system_conformance_missing_adhoc_requirement,
          decl->getDescriptiveKind(), decl->getName(), C.Id_remoteCall);
      decl->diagnose(
          diag::note_distributed_actor_system_conformance_missing_adhoc_requirement,
          decl->getName(), C.Id_remoteCall,
          "func remoteCall<Act, Err, Res>(\n"
          "    on actor: Act,\n"
          "    target: RemoteCallTarget,\n"
          "    invocation: inout InvocationEncoder,\n"
          "    throwing: Err.Type,\n"
          "    returning: Res.Type\n"
          ") async throws -> Res\n"
          "  where Act: DistributedActor,\n"
          "        Act.ID == ActorID,\n"
          "        Err: Error,\n"
          "        Res: SerializationRequirement\n");
      anyMissingAdHocRequirements = true;
    }
    if (checkAdHocRequirementAccessControl(decl, systemProto, remoteCallDecl))
      anyMissingAdHocRequirements = true;

    auto remoteCallVoidDecl =
        C.getRemoteCallOnDistributedActorSystem(decl, /*isVoidReturn=*/true);
    if (!remoteCallVoidDecl && diagnose) {
      decl->diagnose(
          diag::distributed_actor_system_conformance_missing_adhoc_requirement,
          decl->getDescriptiveKind(), decl->getName(), C.Id_remoteCallVoid);
      decl->diagnose(
          diag::note_distributed_actor_system_conformance_missing_adhoc_requirement,
          decl->getName(), C.Id_remoteCallVoid,
          "func remoteCallVoid<Act, Err>(\n"
          "    on actor: Act,\n"
          "    target: RemoteCallTarget,\n"
          "    invocation: inout InvocationEncoder,\n"
          "    throwing: Err.Type\n"
          ") async throws\n"
          "  where Act: DistributedActor,\n"
          "        Act.ID == ActorID,\n"
          "        Err: Error\n");
      anyMissingAdHocRequirements = true;
    }
    if (checkAdHocRequirementAccessControl(decl, systemProto, remoteCallVoidDecl))
      anyMissingAdHocRequirements = true;

    return anyMissingAdHocRequirements;
  }

  // ==== ----------------------------------------------------------------------
  // Check the ad-hoc requirements of 'DistributedTargetInvocationEncoder'
  // -
  if (Proto->isSpecificProtocol(KnownProtocolKind::DistributedTargetInvocationEncoder)) {
    // - recordArgument
    if (!C.getRecordArgumentOnDistributedInvocationEncoder(decl)) {
      decl->diagnose(
          diag::distributed_actor_system_conformance_missing_adhoc_requirement,
          decl->getDescriptiveKind(), decl->getName(), C.Id_recordArgument);
      decl->diagnose(diag::note_distributed_actor_system_conformance_missing_adhoc_requirement,
                     decl->getName(), C.Id_recordArgument,
                     "mutating func recordArgument<Argument: SerializationRequirement>(_ argument: Argument) throws\n");
      anyMissingAdHocRequirements = true;
    }

    // - recordErrorType
    if (!C.getRecordErrorTypeOnDistributedInvocationEncoder(decl)) {
      decl->diagnose(
          diag::distributed_actor_system_conformance_missing_adhoc_requirement,
          decl->getDescriptiveKind(), decl->getName(), C.Id_recordErrorType);
      decl->diagnose(diag::note_distributed_actor_system_conformance_missing_adhoc_requirement,
                     decl->getName(), C.Id_recordErrorType,
                     "mutating func recordErrorType<Err: Error>(_ errorType: Err.Type) throws\n");
      anyMissingAdHocRequirements = true;
    }

    // - recordReturnType
    if (!C.getRecordReturnTypeOnDistributedInvocationEncoder(decl)) {
      decl->diagnose(
          diag::distributed_actor_system_conformance_missing_adhoc_requirement,
          decl->getDescriptiveKind(), decl->getName(), C.Id_recordReturnType);
      decl->diagnose(diag::note_distributed_actor_system_conformance_missing_adhoc_requirement,
                     decl->getName(), C.Id_recordReturnType,
                     "mutating func recordReturnType<Res: SerializationRequirement>(_ resultType: Res.Type) throws\n");
      anyMissingAdHocRequirements = true;
    }

    if (anyMissingAdHocRequirements)
      return true; // found errors
  }

  // ==== ----------------------------------------------------------------------
  // Check the ad-hoc requirements of 'DistributedTargetInvocationDecoder'
  // - decodeNextArgument
  if (Proto->isSpecificProtocol(KnownProtocolKind::DistributedTargetInvocationDecoder)) {
    // FIXME(distributed): implement finding this the requirements here
  }

  // === -----------------------------------------------------------------------
  // Check the ad-hoc requirements of 'DistributedTargetInvocationResultHandler'
  // - onReturn
  if (Proto->isSpecificProtocol(KnownProtocolKind::DistributedTargetInvocationResultHandler)) {

  }

  return false;
}

static bool checkDistributedTargetResultType(
    ModuleDecl *module, ValueDecl *valueDecl,
    const llvm::SmallPtrSetImpl<ProtocolDecl *> &serializationRequirements,
    bool diagnose) {
  auto &C = valueDecl->getASTContext();

  Type resultType;
  if (auto func = dyn_cast<FuncDecl>(valueDecl)) {
    resultType = func->mapTypeIntoContext(func->getResultInterfaceType());
  } else if (auto var = dyn_cast<VarDecl>(valueDecl)) {
    resultType = var->getInterfaceType();
  } else {
    llvm_unreachable("Unsupported distributed target");
  }

  if (resultType->isVoid())
    return false;

  // If the serialization requirement is specifically `Codable`
  // we can issue slightly better warnings
  llvm::SmallPtrSet<ProtocolDecl *, 2> codableRequirements = {
      C.getProtocol(swift::KnownProtocolKind::Encodable),
      C.getProtocol(swift::KnownProtocolKind::Decodable),
  };

  auto isCodableRequirement =
      checkDistributedSerializationRequirementIsExactlyCodable(
          C, serializationRequirements);

  for(auto serializationReq : serializationRequirements) {
    auto conformance =
        TypeChecker::conformsToProtocol(resultType, serializationReq, module);
    if (conformance.isInvalid()) {
      if (diagnose) {
        llvm::StringRef conformanceToSuggest = isCodableRequirement ?
            "Codable" : // Codable is a typealias, easier to diagnose like that
            serializationReq->getNameStr();

        auto diag = valueDecl->diagnose(
            diag::distributed_actor_target_result_not_codable,
            resultType,
            valueDecl->getDescriptiveKind(),
            valueDecl->getBaseIdentifier(),
            conformanceToSuggest
        );

        if (isCodableRequirement) {
          if (auto resultNominalType = resultType->getAnyNominal()) {
            addCodableFixIt(resultNominalType, diag);
          }
        }

        return true;
      }
    }
  }

  return false;
}

/// Check whether the function is a proper distributed function
///
/// \param diagnose Whether to emit a diagnostic when a problem is encountered.
///
/// \returns \c true if there was a problem with adding the attribute, \c false
/// otherwise.
bool swift::checkDistributedFunction(FuncDecl *func, bool diagnose) {
  assert(func->isDistributed());

  auto &C = func->getASTContext();
  auto declContext = func->getDeclContext();
  auto module = func->getParentModule();

  // === All parameters and the result type must conform
  // SerializationRequirement
  llvm::SmallPtrSet<ProtocolDecl *, 2> serializationRequirements;
  if (auto extension = dyn_cast<ExtensionDecl>(declContext)) {
    serializationRequirements = extractDistributedSerializationRequirements(
        C, extension->getGenericRequirements());
  } else if (auto actor = dyn_cast<ClassDecl>(declContext)) {
    serializationRequirements = getDistributedSerializationRequirementProtocols(
        actor, C.getProtocol(KnownProtocolKind::DistributedActor));
  } // TODO(distributed): need to handle ProtocolDecl too?

  // If the requirement is exactly `Codable` we diagnose it ia bit nicer.
  auto serializationRequirementIsCodable =
      checkDistributedSerializationRequirementIsExactlyCodable(
          C, serializationRequirements);

  // --- Check parameters for 'Codable' conformance
  for (auto param : *func->getParameters()) {
    auto paramTy = func->mapTypeIntoContext(param->getInterfaceType());

    for (auto req : serializationRequirements) {
      if (TypeChecker::conformsToProtocol(paramTy, req, module).isInvalid()) {
        if (diagnose) {
          auto diag = func->diagnose(
              diag::distributed_actor_func_param_not_codable,
              param->getArgumentName().str(), param->getInterfaceType(),
              func->getDescriptiveKind(),
              serializationRequirementIsCodable ? "Codable"
                                                : req->getNameStr());

          if (auto paramNominalTy = paramTy->getAnyNominal()) {
            addCodableFixIt(paramNominalTy, diag);
          } // else, no nominal type to suggest the fixit for, e.g. a closure
        }
        return true;
      }
    }

    if (param->isInOut()) {
      param->diagnose(
          diag::distributed_actor_func_inout,
          param->getName(),
          func->getDescriptiveKind(), func->getName()
      ).fixItRemove(SourceRange(param->getTypeSourceRangeForDiagnostics().Start,
                                param->getTypeSourceRangeForDiagnostics().Start.getAdvancedLoc(1)));
      // FIXME(distributed): the fixIt should be on param->getSpecifierLoc(), but that Loc is invalid for some reason?
      return true;
    }

    if (param->isVariadic()) {
      param->diagnose(
          diag::distributed_actor_func_variadic,
          param->getName(),
          func->getDescriptiveKind(), func->getName()
      );
    }
  }

  // --- Result type must be either void or a codable type
  if (checkDistributedTargetResultType(module, func, serializationRequirements, diagnose)) {
    return true;
  }

  return false;
}

/// Check whether the function is a proper distributed computed property
///
/// \param diagnose Whether to emit a diagnostic when a problem is encountered.
///
/// \returns \c true if there was a problem with adding the attribute, \c false
/// otherwise.
bool swift::checkDistributedActorProperty(VarDecl *var, bool diagnose) {
  auto &C = var->getASTContext();
  auto DC = var->getDeclContext();
  /// === Check if the declaration is a valid combination of attributes
  if (var->isStatic()) {
    var->diagnose(diag::distributed_property_cannot_be_static,
                      var->getName());
    // TODO(distributed): fixit, offer removing the static keyword
    return true;
  }

  // it is not a computed property
  if (var->isLet() || var->hasStorageOrWrapsStorage()) {
    var->diagnose(diag::distributed_property_can_only_be_computed,
                  var->getDescriptiveKind(), var->getName());
    return true;
  }

  // distributed properties cannot have setters
  if (var->getWriteImpl() != swift::WriteImplKind::Immutable) {
    var->diagnose(diag::distributed_property_can_only_be_computed_get_only,
                  var->getName());
    return true;
  }

  // === Check the type of the property
  auto serializationRequirements =
      getDistributedSerializationRequirementProtocols(
          DC->getSelfNominalTypeDecl(),
          C.getProtocol(KnownProtocolKind::DistributedActor));

  auto module = var->getModuleContext();
  if (checkDistributedTargetResultType(module, var, serializationRequirements, diagnose)) {
    return true;
  }

  return false;
}

void swift::checkDistributedActorProperties(const ClassDecl *decl) {
  auto &C = decl->getASTContext();

  for (auto member : decl->getMembers()) {
    if (auto prop = dyn_cast<VarDecl>(member)) {
      if (prop->isSynthesized())
        continue;

      auto id = prop->getName();
      if (id == C.Id_actorSystem || id == C.Id_id) {
        prop->diagnose(diag::distributed_actor_user_defined_special_property,
                      id);
      }
    }
  }
}

void swift::checkDistributedActorConstructor(const ClassDecl *decl, ConstructorDecl *ctor) {
  // bail out unless distributed actor, only those have special rules to check here
  if (!decl->isDistributedActor())
    return;

  // Only designated initializers need extra checks
  if (!ctor->isDesignatedInit())
    return;

  // === Designated initializers must accept exactly one actor transport that
  // matches the actor transport type of the actor.
  SmallVector<ParamDecl*, 2> transportParams;
  int transportParamsCount = 0;
  Type actorSystemTy = ctor->mapTypeIntoContext(
      getDistributedActorSystemType(const_cast<ClassDecl *>(decl)));
  for (auto param : *ctor->getParameters()) {
    auto paramTy = ctor->mapTypeIntoContext(param->getInterfaceType());
    if (paramTy->isEqual(actorSystemTy)) {
      transportParamsCount += 1;
      transportParams.push_back(param);
    }
  }

  // missing transport parameter
  if (transportParamsCount == 0) {
    ctor->diagnose(diag::distributed_actor_designated_ctor_missing_transport_param,
                   ctor->getName());
    // TODO(distributed): offer fixit to insert 'system: DistributedActorSystem'
    return;
  }

  // ok! We found exactly one transport parameter
  if (transportParamsCount == 1)
    return;

  // TODO(distributed): rdar://81824959 report the error on the offending (2nd) matching parameter
  //                    Or maybe we can issue a note about the other offending params?
  ctor->diagnose(diag::distributed_actor_designated_ctor_must_have_one_distributedactorsystem_param,
                 ctor->getName(), transportParamsCount);
}

// ==== ------------------------------------------------------------------------

void TypeChecker::checkDistributedActor(ClassDecl *decl) {
  if (!decl)
    return;

  // ==== Ensure the _Distributed module is available,
  // without it there's no reason to check the decl in more detail anyway.
  if (!swift::ensureDistributedModuleLoaded(decl))
    return;

  // ==== Constructors
  // --- Get the default initializer
  // If applicable, this will create the default 'init(transport:)' initializer
  (void)decl->getDefaultInitializer();

  for (auto member : decl->getMembers()) {
    // --- Check all constructors
    if (auto ctor = dyn_cast<ConstructorDecl>(member))
      checkDistributedActorConstructor(decl, ctor);
  }

  // ==== Properties
  checkDistributedActorProperties(decl);
  // --- Synthesize the 'id' property here rather than via derived conformance
  //     because the 'DerivedConformanceDistributedActor' won't trigger for 'id'
  //     because it has a default impl via 'Identifiable' (ObjectIdentifier)
  //     which we do not want.
  (void)decl->getDistributedActorIDProperty();
}

llvm::SmallPtrSet<ProtocolDecl *, 2>
swift::getDistributedSerializationRequirementProtocols(
    NominalTypeDecl *nominal, ProtocolDecl *protocol) {
  if (!protocol)
    return {};

//  auto ty = ctx.getDistributedSerializationRequirementType(nominal);
//  if (ty->hasError())
//    return {};
  auto ty = getDistributedSerializationRequirementType(
      nominal, protocol);
  if (ty->hasError())
    return {};

  auto serialReqType =
      ty->castTo<ExistentialType>()->getConstraintType()->getDesugaredType();

  // TODO(distributed): check what happens with Any
  return flattenDistributedSerializationTypeToRequiredProtocols(serialReqType);
}

ConstructorDecl*
GetDistributedRemoteCallTargetInitFunctionRequest::evaluate(
    Evaluator &evaluator,
    NominalTypeDecl *nominal) const {
  auto &C = nominal->getASTContext();

  // not via `ensureDistributedModuleLoaded` to avoid generating a warning,
  // we won't be emitting the offending decl after all.
  if (!C.getLoadedModule(C.Id_Distributed))
    return nullptr;

  if (!nominal->getDeclaredInterfaceType()->isEqual(
          C.getRemoteCallTargetType()))
    return nullptr;

  for (auto value : nominal->getMembers()) {
    auto ctor = dyn_cast<ConstructorDecl>(value);
    if (!ctor)
      continue;

    auto params = ctor->getParameters();
    if (params->size() != 1)
      return nullptr;

    if (params->get(0)->getArgumentName() == C.getIdentifier("_mangledName"))
      return ctor;

    return nullptr;
  }

  // TODO(distributed): make a Request for it?
  return nullptr;
}

NominalTypeDecl *
GetDistributedActorInvocationDecoderRequest::evaluate(Evaluator &evaluator,
                                                      NominalTypeDecl *actor) const {
  auto &ctx = actor->getASTContext();
  auto decoderTy =
      ctx.getAssociatedTypeOfDistributedSystemOfActor(actor, ctx.Id_InvocationDecoder);
  return decoderTy->hasError() ? nullptr : decoderTy->getAnyNominal();
}

FuncDecl *
GetDistributedActorArgumentDecodingMethodRequest::evaluate(Evaluator &evaluator,
                                                           NominalTypeDecl *actor) const {
  auto &ctx = actor->getASTContext();

  auto *decoder = ctx.getDistributedActorInvocationDecoder(actor);
  assert(decoder);

  auto decoderTy = decoder->getInterfaceType()->getMetatypeInstanceType();

  auto members = TypeChecker::lookupMember(actor->getDeclContext(), decoderTy,
                                           DeclNameRef(ctx.Id_decodeNextArgument));

  // typealias SerializationRequirement = any ...
  llvm::SmallPtrSet<ProtocolDecl *, 2> serializationReqs =
      getDistributedSerializationRequirementProtocols(
          actor, ctx.getProtocol(KnownProtocolKind::DistributedActor));

  SmallVector<FuncDecl *, 2> candidates;
  // Looking for `decodeNextArgument<Arg: <SerializationReq>>() throws -> Arg`
  for (auto &member : members) {
    auto *FD = dyn_cast<FuncDecl>(member.getValueDecl());
    if (!FD || FD->hasAsync() || !FD->hasThrows())
      continue;

    auto *params = FD->getParameters();
    // No arguemnts.
    if (params->size() != 0)
      continue;

    auto genericParamList = FD->getGenericParams();
    // A single generic parameter.
    if (genericParamList->size() != 1)
      continue;

    auto paramTy = genericParamList->getParams()[0]
                       ->getInterfaceType()
                       ->getMetatypeInstanceType();

    // `decodeNextArgument` should return its generic parameter value
    if (!FD->getResultInterfaceType()->isEqual(paramTy))
      continue;

    // Let's find out how many serialization requirements does this method cover
    // e.g. `Codable` is two requirements - `Encodable` and `Decodable`.
    unsigned numSerializationReqsCovered = llvm::count_if(
        FD->getGenericRequirements(), [&](const Requirement &requirement) {
          if (!(requirement.getFirstType()->isEqual(paramTy) &&
                requirement.getKind() == RequirementKind::Conformance))
            return 0;

          return serializationReqs.count(requirement.getProtocolDecl()) ? 1 : 0;
        });

    // If the current method covers all of the serialization requirements,
    // it's a match. Note that it might also have other requirements, but
    // we let that go as long as there are no two candidates that differ
    // only in generic requirements.
    if (numSerializationReqsCovered == serializationReqs.size())
      candidates.push_back(FD);
  }

  // Type-checker should reject any definition of invocation decoder
  // that doesn't have a correct version of `decodeNextArgument` declared.
  assert(candidates.size() == 1);
  return candidates.front();
}
