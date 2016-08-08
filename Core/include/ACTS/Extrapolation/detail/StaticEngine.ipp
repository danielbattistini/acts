// This file is part of the ACTS project.
//
// Copyright (C) 2016 ACTS project team
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

///////////////////////////////////////////////////////////////////
// StaticEngine.ipp, ACTS project
///////////////////////////////////////////////////////////////////

#include "ACTS/Detector/TrackingGeometry.hpp"
#include "ACTS/Detector/TrackingVolume.hpp"
#include "ACTS/Extrapolation/IMaterialEffectsEngine.hpp"
#include "ACTS/Extrapolation/INavigationEngine.hpp"
#include "ACTS/Extrapolation/IPropagationEngine.hpp"
#include "ACTS/Layers/Layer.hpp"
#include "ACTS/Surfaces/Surface.hpp"
#include <iomanip>
#include <iostream>

template <class T>
Acts::ExtrapolationCode
Acts::StaticEngine::extrapolateT(Acts::ExtrapolationCell<T>& eCell,
                                 const Acts::Surface*        sf,
                                 Acts::PropDirection         pDir,
                                 const Acts::BoundaryCheck&  bcheck) const
{
  Acts::ExtrapolationCode eCode = Acts::ExtrapolationCode::InProgress;
  // ---- [0] check the direct propagation exit
  //
  //  obviously need a surface to exercise the fallback & need to be configured
  //  to do so
  if (sf
      && eCell.checkConfigurationMode(Acts::ExtrapolationMode::Destination)) {
    EX_MSG_DEBUG(
        ++eCell.navigationStep,
        "extrapolate",
        "",
        "direct extapolation in volume : " << eCell.leadVolume->volumeName());
    // propagate to the surface, possible return codes are : SuccessPathLimit,
    // SucessDestination, FailureDestination
    eCode
        = m_cfg.propagationEngine->propagate(eCell,
                                                *sf,
                                                pDir,
                                                ExtrapolationMode::Destination,
                                                bcheck,
                                                eCell.destinationCurvilinear);
    // eCode can be directly returned
    return eCode;
  }
  EX_MSG_DEBUG(++eCell.navigationStep,
               "extrapolate",
               "",
               "extrapolation in static environment in volume : "
                   << eCell.leadVolume->volumeName());
  // evoke or finish the navigation initialization, possible return codes are:
  // - InProgress        : everything is fine, extrapolation in static volume is
  // in progress
  // - FailureNavigation : navigation setup could not be resolved, but reovery
  // was not configured
  // - Recovered         : navigation setup could not be resolved, recovery by
  // fallback to directly kicked in (and worked)
  eCode = initNavigationT<T>(eCell, sf, pDir, bcheck);
  CHECK_ECODE_CONTINUE(eCell, eCode);
  // ----- [1] handle the ( leadLayer == endLayer )case :
  //
  // - this case does not need a layer to layer loop
  if (sf && eCell.leadLayer == eCell.endLayer && eCell.initialVolume()) {
    // screen output for startLayer == endLayer
    EX_MSG_VERBOSE(eCell.navigationStep,
                   "layer",
                   eCell.leadLayer->geoID().value(),
                   "start and destination layer are identical -> jumping to "
                   "final propagation.");
    // set the leadLayerSurface to the parameters surface
    eCell.leadLayerSurface = &(eCell.leadParameters->associatedSurface());
    // resolve the layer, it is the final extrapolation
    // - InProgress           : layer resolving went without problem
    // - SuccessPathLimit     : path limit reached & configured to stop (rather
    // unlikely within a layer)
    // - SuccessMaterialLimit : material limit reached & configured to stop
    // there
    // if the lead layer is also the startLayer found by initialization -> no
    // material handling
    eCode = resolveLayerT<T>(
        eCell,
        sf,
        pDir,
        bcheck,
        eCell.leadLayer->hasSubStructure(eCell.checkConfigurationMode(
            Acts::ExtrapolationMode::CollectSensitive)),
        (eCell.leadLayer == eCell.startLayer
         && eCell.leadVolume == eCell.startVolume),
        true);
    // Success triggers a return
    CHECK_ECODE_SUCCESS_NODEST(eCell, eCode);
    // extrapolation to destination was not successful
    // - handle the return as configured (e.g. fallback)
    return handleReturnT<T>(eCode, eCell, sf, pDir, bcheck);
  }
  // ----- [2] now do the layer-to-layer loop
  //
  // the volume returns the layers ordered by distance :
  // - give potential start and end layer (latter only for the final volume)
  // - start and end layer will be part of the loop
  // - surface on approach is not yet resolved
  const Acts::Layer* fLayer
      = eCell.finalVolumeReached() ? eCell.endLayer : nullptr;
  auto layerIntersections = eCell.leadVolume->layerCandidatesOrdered(
                                                                     eCell.leadLayer, fLayer, *eCell.leadParameters, pDir, true, eCell.checkConfigurationMode(Acts::ExtrapolationMode::CollectMaterial),eCell.checkConfigurationMode(Acts::ExtrapolationMode::CollectSensitive));

  EX_MSG_VERBOSE(eCell.navigationStep,
                 "layer",
                 "loop",
                 "found " << layerIntersections.size()
                          << " layers for the layer-to-layer loop.");
    
  // layer-to-layer loop starts here
  for (auto& layerCandidate : layerIntersections) {
    // assign the leadLayer
    eCell.leadLayer = layerCandidate.object;
    // screen output for layer-to-layer loop
    EX_MSG_VERBOSE(
        eCell.navigationStep,
        "layer",
        "loop",
        "processing layer with index : " << eCell.leadLayer->geoID().value());
    // resolve the approach surface situation
    // -  it is the approaching surface for all layers but the very first one
    // (where it's the parameter surface)
    eCell.leadLayerSurface = (eCell.leadLayer == eCell.startLayer)
        ? &(eCell.leadParameters->associatedSurface())
        : (eCell.leadLayer->surfaceOnApproach(eCell.leadParameters->position(),
                                              eCell.leadParameters->momentum(),
                                              pDir,
                                              true,
                                              true))
              .object;
    // handle the layer, possible returns are :
    // - InProgress               : fine, whatever happened on the lead layer,
    // may also be missed
    // - SuccessWithPathLimit     : propagation towards layer exceeded path
    // limit
    // - SuccessWithMaterialLimit : material interaction killed track
    // - FailureDestination       : destination was not hit appropriately
    eCode = handleLayerT<T>(eCell, sf, pDir, bcheck);
    EX_MSG_VERBOSE(
        eCell.navigationStep,
        "layer",
        layerCandidate.object->geoID().value(),
        "handleLayerT returned extrapolation code : " << eCode.toString());
    // Possibilities are:
    // - SuccessX  -> return (via handleReturnT)
    // - FailureX  -> return (via handleReturnT that might evoke a fallback)
    // - InProgess -> continue layer-to-layer loop
    if (!eCode.inProgress())
      return handleReturnT<T>(eCode, eCell, sf, pDir, bcheck);
  }
  // the layer 2 layer loop is done, the lead parameters are at the last valid
  // option
  // ----- [3] now resolve the boundary situation, call includes information
  // wheather one is alreay at a boundary
  //
  // the navigaiton engine ca trigger different return codes
  // - InProgress                   : fine, boundary surface has been found
  // - SuccessWithPathLimit         : propagation towards boundary surface
  // exceeded path limit
  // - FailureLoop/Navigation       : problem in boundary resolving
  eCode = m_cfg.navigationEngine->resolveBoundary(eCell, pDir);
  // SuccessX and FailureX trigger a return
  CHECK_ECODE_SUCCESS_NODEST(eCell, eCode);
  // handle the return of the boudnary resolving
  return handleReturnT<T>(eCode, eCell, sf, pDir, bcheck);
}

template <class T>
Acts::ExtrapolationCode
Acts::StaticEngine::initNavigationT(Acts::ExtrapolationCell<T>& eCell,
                                    const Acts::Surface*        sf,
                                    Acts::PropDirection         pDir,
                                    const Acts::BoundaryCheck&  bcheck) const
{
  // initialize the Navigation stream
  // ----------------------------------------------------------------------------------------
  //
  // this is the global initialization, it only associated direct objects
  // detailed navigation search needs to be done by the sub engines (since they
  // know best)
  EX_MSG_DEBUG(++eCell.navigationStep,
               "navigation",
               "",
               "complete for static environment.");
  // [A] the initial volume
  if (eCell.startVolume == eCell.leadVolume && eCell.startLayer) {
    // - found the initial start layer through association
    EX_MSG_VERBOSE(eCell.navigationStep,
                   "navigation",
                   "",
                   "this is the initial volume, everything set up already.");
    // assigning it to the leadLayer
    eCell.leadLayer = eCell.startLayer;
    // return progress
    return Acts::ExtrapolationCode::InProgress;
  }
  // [B] any volume if we don't have a leadLayer
  if (!eCell.leadLayer) {
    // - finding it through global search, never a boundary layer ... convention
    // says that you update by exit
    eCell.leadLayer
        = eCell.leadVolume->associatedLayer(eCell.leadParameters->position());
    EX_MSG_VERBOSE(eCell.navigationStep,
                   "navigation",
                   "",
                   "no start layer found yet, looking for it ..."
                       << OH_CHECKFOUND(eCell.leadLayer));
  }
  // [C] the final volume - everything's fine
  if (eCell.leadVolume == eCell.endVolume && sf) {
    if (eCell.endLayer) {
      // the end layer had been found already by association
      EX_MSG_VERBOSE(eCell.navigationStep,
                     "navigation",
                     "",
                     "this is the final volume, everything set up already.");
      return Acts::ExtrapolationCode::InProgress;
    } else {
      // make a straight line intersection
      Acts::Intersection sfI
          = sf->intersectionEstimate(eCell.leadParameters->position(),
                                     pDir * eCell.leadParameters->momentum(),
                                     true);
      // use this to find endVolume and endLayer
      eCell.endLayer = eCell.leadVolume->associatedLayer(sfI.position);
      // if you have a surface you need to require an end layer for the
      // validation, otherwise you need to do a fallbac
      return eCell.endLayer
          ? Acts::ExtrapolationCode::InProgress
          : handleReturnT<T>(Acts::ExtrapolationCode::FailureNavigation,
                             eCell,
                             sf,
                             pDir,
                             bcheck);
    }
  }
  // return that you're in progress
  return Acts::ExtrapolationCode::InProgress;
}

template <class T>
Acts::ExtrapolationCode
Acts::StaticEngine::handleLayerT(Acts::ExtrapolationCell<T>& eCell,
                                 const Acts::Surface*        sf,
                                 Acts::PropDirection         pDir,
                                 const Acts::BoundaryCheck&  bcheck) const
{
  Acts::ExtrapolationCode eCode = Acts::ExtrapolationCode::InProgress;

  /// prepare the layer output number
  auto layerValue = eCell.leadLayer->geoID().value(GeometryID::layer_mask,
                                                   GeometryID::layer_shift);
  // screen output
  EX_MSG_DEBUG(++eCell.navigationStep,
               "layer",
               layerValue,
               "in volume " << eCell.leadVolume->volumeName());
  // layer has sub structure - this can be (and the layer will tell you):
  //      - sensitive surface which should be tried to hit
  //      - material sub structure to be resolved (independent of sensitive
  //      surface)
  bool hasSubStructure = eCell.leadLayer->hasSubStructure(
      eCell.checkConfigurationMode(Acts::ExtrapolationMode::CollectSensitive));
  // [A] layer is a pure navigation layer and has no sub structure -> skip it,
  // but only if it is not the final layer
  if (eCell.leadLayer->layerType() == navigation
      || (!hasSubStructure && eCell.leadLayer != eCell.endLayer)) {
    EX_MSG_VERBOSE(eCell.navigationStep,
                   "layer",
                   layerValue,
                   "layer is a navigation layer -> skipping it ...");
    return Acts::ExtrapolationCode::InProgress;
  }
  // [B] layer resolving is necessary -> resolve it
  // - (a) layer has sub structure - this can be (and the layer will tell you):
  //      - sensitive surface which should be tried to hit
  //      - material sub structure to be resolved (independent of sensitive
  //      surface)
  // - (b) layer is start layer (can not be if there was a volume switch)
  bool isStartLayer = eCell.initialVolume()
      && eCell.leadLayer->onLayer(*eCell.leadParameters);
  // - (c) layer is destination layer
  //      - final propagation to the layer and update if necessary
  bool isDestinationLayer = (sf && eCell.leadLayer == eCell.endLayer);
  //  sub structure, start and destination need resolving of the layer setp
  if (hasSubStructure || isStartLayer || isDestinationLayer) {
    // screen output for sub strucutred layer
    EX_MSG_VERBOSE(eCell.navigationStep,
                   "layer",
                   layerValue,
                   "has sub structure, is start layer, or destination layer -> "
                   "resolving it ...");
    // resolve the layer, it handles all possible return types and gives them
    // directly to extrapolateT<T>
    // - InProgress           : layer resolving went without problem
    // - SuccessPathLimit     : path limit reached & configured to stop
    // - SuccessMaterialLimit : material limit reached & configured to stop
    // - SuccessDestination   : destination reached & everything is fine
    return resolveLayerT<T>(eCell,
                            sf,
                            pDir,
                            bcheck,
                            hasSubStructure,
                            isStartLayer,
                            isDestinationLayer);
  }
  // [C] layer is a material layer without sub structure but material
  // -> pass through no resolving ob sub structure to be done,
  // an intermediate layer to be crossed
  EX_MSG_VERBOSE(
      eCell.navigationStep,
      "layer",
      layerValue,
      "intermediate layer without sub structure ->  passing through ...");
  //    propagate to it, possible return codes ( with the default of
  //    finalPropagation = false):
  //    - SuccessPathLimit       : propagation to layer exceeded path limit
  //    - InProgress             : layer was hit successfuly,
  //                               try to handle the material and sub structure
  //    - Recovered              : layer was not hit, skip for layer-to-layer
  eCode = m_cfg.propagationEngine->propagate(eCell,
                                             *eCell.leadLayerSurface,
                                             pDir,
                                             ExtrapolationMode::CollectPassive,
                                             true,
                                             eCell.navigationCurvilinear);
  CHECK_ECODE_SUCCESS_NODEST(eCell, eCode);
  // check if the layer was actually hit
  if (eCode.inProgress()) {
    // successful layer hit
    EX_MSG_VERBOSE(eCell.navigationStep,
                   "layer",
                   layerValue,
                   "has been succesful hit, handling material update.");
    // layer has no sub-structure : it is an intermediate layer that just needs
    // pass-throgh
    // return possbilities:
    // - InProgress            : material update performed
    // - SuccessMaterialLimit  : material limit reached & configured to stop
    eCode = m_cfg.materialEffectsEngine->handleMaterial(
        eCell, pDir, Acts::fullUpdate);
    CHECK_ECODE_CONTINUE(eCell, eCode);
    // return the progress eCode back to the extrapolateT
    return eCode;
  }
  // hit or not hit : it's always in progress since we are in the layer to layer
  // loop
  return Acts::ExtrapolationCode::InProgress;
}

template <class T>
Acts::ExtrapolationCode
Acts::StaticEngine::resolveLayerT(ExtrapolationCell<T>& eCell,
                                  const Surface*        sf,
                                  PropDirection         pDir,
                                  const BoundaryCheck&  bcheck,
                                  bool                  hasSubStructure,
                                  bool                  isStartLayer,
                                  bool isDestinationLayer) const
{
  ExtrapolationCode eCode = ExtrapolationCode::InProgress;
  /// prepare the layer output number
  auto layerValue = eCell.leadLayer->geoID().value(GeometryID::layer_mask,
                                                   GeometryID::layer_shift);
  // screen output
  EX_MSG_DEBUG(
      ++eCell.navigationStep,
      "layer",
      layerValue,
      "resolve it with" << (hasSubStructure ? " " : "out ") << "sub structure"
                        << (isDestinationLayer
                                ? " -> destination layer."
                                : (isStartLayer ? " -> start layer." : "")));

  // cache the leadLayer - this is needed for the layer-to-layer loop not to be
  // broken
  const Layer* initialLayer = eCell.leadLayer;
  // ----- [0] the start situation on the layer needs to be resolved:
  // - either for sensitive parameters
  // - or for material substructure
  // [A] the layer is not the start layer and not the destination layer
  // - the surfaceOnApproach() call should have sorted out that this is actually
  // an approaching representation
  // - the destination layer is excluded from the propagation because it can
  // lead to punch-through to the other side of layers
  if (!isStartLayer && !isDestinationLayer) {
    EX_MSG_VERBOSE(eCell.navigationStep,
                   "layer",
                   layerValue,
                   "not the start layer (with sub structue), propagate to it.");
    // propagate to the representing surface of this layer
    // - InProgress       : propagation to approaching surface worked - check
    // material update
    // - SuccessPathLimit : propagation to approaching surface reached the path
    // limit
    // - Recovered        : layer was not hit, so can be ignored in the layer to
    // layer loop
    eCode = m_cfg.propagationEngine->propagate(
        eCell,
        *eCell.leadLayerSurface,
        pDir,
        ExtrapolationMode::CollectPassive,
        true,
        eCell.sensitiveCurvilinear);
    CHECK_ECODE_SUCCESS_NODEST(eCell, eCode);
    // the extrapolation to the initial layer did not succeed - skip this layer
    // in the layer-to-layer loop
    if (eCode == ExtrapolationCode::Recovered) {
      EX_MSG_VERBOSE(eCell.navigationStep,
                     "layer",
                     layerValue,
                     "has not been hit, skipping it.");
      return Acts::ExtrapolationCode::InProgress;
    }
    EX_MSG_VERBOSE(
        eCell.navigationStep, "layer", layerValue, "successfuly hit.");
    // the correct material layer needs to be assigned - in case of the approach
    // surface not being hit, his can be the layer surface
    if (eCell.leadLayerSurface->associatedMaterial()) {
      // set the material surface for the update
      eCell.materialSurface = eCell.leadLayerSurface;
      // now handle the material (full update when passing approach surface),
      // return codes are:
      // - SuccessMaterialLimit : material limit reached, return back
      // - InProgress           : material update done or not (depending on the
      // material description)
      eCode = m_cfg.materialEffectsEngine->handleMaterial(
          eCell, pDir, Acts::fullUpdate);
      CHECK_ECODE_CONTINUE(eCell, eCode);
    }
  } else if (isStartLayer) {
    // [B] the layer is the start layer
    //  - let's check if a post update on the start surface has to be done
    EX_MSG_VERBOSE(
        eCell.navigationStep,
        "layer",
        layerValue,
        "start layer (with sub structure), no propagation to be done.");
    // the start surface could have a material layer attached
    if (eCell.leadParameters->associatedSurface().associatedMaterial()) {
      // set the material surface
      eCell.materialSurface = &(eCell.leadParameters->associatedSurface());
      // now handle the material (post update on start layer), return codes are:
      // - SuccessMaterialLimit : material limit reached, return back
      // - InProgress           : material update done or not (depending on the
      // material description)
      eCode = m_cfg.materialEffectsEngine->handleMaterial(
          eCell, pDir, Acts::postUpdate);
      CHECK_ECODE_CONTINUE(eCell, eCode);
      // let's reset the lead layer
      eCell.leadLayer = initialLayer;
    }
  }
  // ----- [1] the sub structure of the layer needs to be resolved:
  // resolve the substructure
  std::vector<Acts::SurfaceIntersection> cSurfaces;
  // this will give you the compatible surfaces of the layer : provided start
  // and destination surface are excluded
  // - surfaces without material are only provided if they are active and
  // CollectSensitive is configured
  // - surfaces with material are provided in order to make the necessary
  // material update
  bool orderedSurfaces = eCell.leadLayer->compatibleSurfaces(
      cSurfaces,
      *eCell.leadParameters,
      pDir,
      bcheck,
      eCell.checkConfigurationMode(ExtrapolationMode::CollectSensitive),
      eCell.checkConfigurationMode(ExtrapolationMode::CollectPassive),
      eCell.searchMode,
      (isStartLayer ? &(eCell.leadParameters->associatedSurface())
                    : eCell.leadLayerSurface),
      (isDestinationLayer ? sf : 0));
  // how many test surfaces do we have
  size_t ncSurfaces = cSurfaces.size();
  // some screen output for the sub structure
  EX_MSG_VERBOSE(eCell.navigationStep,
                 "layer",
                 layerValue,
                 "found " << ncSurfaces << " sub structure surfaces to test.");
  // check if you have to do something
  if (ncSurfaces) {
    // if the search mode returns unordered surfaces :
    // - it needs to cache the initial lead parameters
    // - ATTENTION this invalidates the correct material estimate
    const T* dbgLeadParameters = eCell.leadParameters;
    // now loop over the surfaces:
    // the surfaces will be sorted
    for (auto& csf : cSurfaces) {
      // the surface to try
      EX_MSG_VERBOSE(eCell.navigationStep,
                     "surface",
                     layerValue,
                     "trying candidate surfaces with straight line path length "
                         << csf.intersection.pathLength);
      // record the parameters as sensitive or passive depending on the surface
      Acts::ExtrapolationMode::eMode emode = csf.object->associatedDetectorElement()
          ? Acts::ExtrapolationMode::CollectSensitive
          : Acts::ExtrapolationMode::CollectPassive;
      // propagate to the compatible surface, return types are
      // - InProgress       : propagation to compatible surface worked
      // - Recovered        : propagation to compatible surface did not work,
      // leadParameters stay the same
      // - SuccessPathLimit : propagation to compatible surface reached the path
      // limit
      eCode = m_cfg.propagationEngine->propagate(eCell,
                                                 *(csf.object),
                                                 pDir,
                                                 emode,
                                                 bcheck,
                                                 eCell.sensitiveCurvilinear);
      CHECK_ECODE_SUCCESS_NODEST(eCell, eCode);
      // check if the propagation was successful
      if (eCode.inProgress()) {
        EX_MSG_VERBOSE(eCell.navigationStep,
                       "layer",
                       layerValue,
                       "successfully hit sub structure surface.");
        // check if the surface holds material and hence needs to be processed
        if (csf.object->associatedMaterial()) {
          // screen output
          EX_MSG_VERBOSE(eCell.navigationStep,
                         "surface",
                         csf.object->geoID().value(),
                         "applying material effects.");
          // set the material surface
          eCell.materialSurface = csf.object;
          // now handle the material, return codes are:
          // - SuccessMaterialLimit : material limit reached,return back
          // - InProgress           : material update done or not (depending on
          // the material description)
          eCode = m_cfg.materialEffectsEngine->handleMaterial(
              eCell, pDir, Acts::fullUpdate);
          CHECK_ECODE_CONTINUE(eCell, eCode);
        }
        // if the search mode returns unordered surfaces :
        // - ATTENTION this invalidates the correct material estimate
        if (!orderedSurfaces)
          eCell.leadParameters = dbgLeadParameters;
      }

    }  // loop over test surfaces done
  }    // there are compatible surfaces

  // ----- [3] the destination situation on the layer needs to be resolved:
  // the layer is a destination layer
  // - the final propagation call is indepenent of whether sub structure was
  // resolved or not
  // - the eCell.leadParameters are at the last possible parameters
  if (sf && isDestinationLayer) {
    // [B] the layer is start and destination layer but has no sub-structure
    // -> propagation to destination surface
    //  (a) the starting layer is the same layer :
    // - neither preUpdate nore postUpdate to be done, this is old-style
    // within-layer extrapolation
    // - material will be taken into account either when the layer was reached
    // from another layer
    //   or when the layer is left to another destination
    //  (b) the starting layer is not the same layer :
    // - apply the preUpdate on the parameters whein they reached the surface
    // Possible return types:
    // - SuccessDestination  : great, desintation surface hit - but post-update
    // needs to be done
    // - SuccessPathLimit    : pathlimit was reached on the way to the
    // destination surface
    eCode
        = m_cfg.propagationEngine->propagate(eCell,
                                                *sf,
                                                pDir,
                                                ExtrapolationMode::Destination,
                                                false,
                                                eCell.destinationCurvilinear);
    // check for success return path limit
    CHECK_ECODE_SUCCESS_NODEST(eCell, eCode);
    EX_MSG_VERBOSE(eCell.navigationStep,
                   "layer",
                   layerValue,
                   "attempt to hit destination surface resulted in "
                       << eCode.toString());
    // check for a potential preUpdate
    // - in case teh destination surface has material and the surface was hit
    if (sf->associatedMaterial() && eCode.isSuccess()) {
      // set the material surface for the update
      eCell.materialSurface = sf;
      // finally do the material update, but even as this is the final call
      //  - still check for SuccessMaterialLimit
      m_cfg.materialEffectsEngine->handleMaterial(
          eCell, pDir, Acts::preUpdate);
      // check if success was triggered through path limit reached on the way to
      // the layer
      CHECK_ECODE_SUCCESS(eCell, eCode);
    }
    // return what you have handleLayerT or extrapolateT will resolve that
    return eCode;
  }
  // reset the lead layer to ensure the layer-to-layer loop
  eCell.leadLayer = initialLayer;
  // return the code:
  // - if it came until here, return InProgress to not break the layer-to-layer
  // loop
  return Acts::ExtrapolationCode::InProgress;
}

/** handle the failure - as configured */
template <class T>
Acts::ExtrapolationCode
Acts::StaticEngine::handleReturnT(ExtrapolationCode     eCode,
                                  ExtrapolationCell<T>& eCell,
                                  const Surface*        sf,
                                  PropDirection         pDir,
                                  const BoundaryCheck&  bcheck) const
{
  EX_MSG_DEBUG(++eCell.navigationStep,
               "return",
               "",
               "handleReturnT with code " << eCode.toString() << " called.");
  if (eCode.isSuccessOrRecovered() || eCode.inProgress()) {
    EX_MSG_VERBOSE(eCell.navigationStep,
                   "return",
                   "",
                   "leaving static extrapolator successfully with code "
                       << eCode.toString());
    return eCode;
  }
  EX_MSG_VERBOSE(eCell.navigationStep,
                 "return",
                 "",
                 "failure detected as "
                     << eCode.toString()
                     << " - checking fallback configuration.");
  // obviously we need a surface to exercise the fallback
  if (sf && !eCell.checkConfigurationMode(ExtrapolationMode::AvoidFallback)) {
    EX_MSG_VERBOSE(eCell.navigationStep,
                   "return",
                   "",
                   "fallback configured. Trying to "
                   "hit destination surface from last "
                   "valid parameters.");
    // check if you hit the surface, could still be stopped by PathLimit, but
    // would also count as recovered
    eCode
        = m_cfg.propagationEngine->propagate(eCell,
                                                *sf,
                                                pDir,
                                                ExtrapolationMode::Propagation,
                                                bcheck,
                                                eCell.destinationCurvilinear);
  }
  // return the extrapolation code
  return eCode;
}
