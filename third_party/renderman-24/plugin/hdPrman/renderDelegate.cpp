//
// Copyright 2019 Pixar
//
// Licensed under the Apache License, Version 2.0 (the "Apache License")
// with the following modification; you may not use this file except in
// compliance with the Apache License and the following modification to it:
// Section 6. Trademarks. is deleted and replaced with:
//
// 6. Trademarks. This License does not grant permission to use the trade
//    names, trademarks, service marks, or product names of the Licensor
//    and its affiliates, except as required to comply with Section 4(c) of
//    the License and to reproduce the content of the NOTICE file.
//
// You may obtain a copy of the Apache License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the Apache License with the above modification is
// distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied. See the Apache License for the specific
// language governing permissions and limitations under the Apache License.
//
#include "hdPrman/renderDelegate.h"
#include "hdPrman/basisCurves.h"
#include "hdPrman/camera.h"
#include "hdPrman/renderParam.h"
#include "hdPrman/renderBuffer.h"
#include "hdPrman/coordSys.h"
#include "hdPrman/instancer.h"
#include "hdPrman/renderParam.h"
#include "hdPrman/renderPass.h"
#include "hdPrman/light.h"
#include "hdPrman/lightFilter.h"
#include "hdPrman/material.h"
#include "hdPrman/mesh.h"
#include "hdPrman/paramsSetter.h"
#include "hdPrman/points.h"
#include "hdPrman/resourceRegistry.h"
#include "hdPrman/volume.h"

#include "pxr/imaging/hd/bprim.h"
#include "pxr/imaging/hd/camera.h"
#include "pxr/imaging/hd/extComputation.h"
#include "pxr/imaging/hd/rprim.h"
#include "pxr/imaging/hd/sprim.h"
#include "pxr/imaging/hd/tokens.h"

#include "pxr/base/tf/envSetting.h"
#include "pxr/base/tf/getenv.h"

PXR_NAMESPACE_OPEN_SCOPE

extern TfEnvSetting<bool> HD_PRMAN_ENABLE_QUICKINTEGRATE;

TF_DEFINE_PRIVATE_TOKENS(
    _tokens,
    (openvdbAsset)
    (field3dAsset)
    ((mtlxRenderContext, "mtlx"))
    (prmanParams) /* XXX currently duplicated whereever used as to not yet */
                 /* establish a formal convention */
);

TF_DEFINE_PUBLIC_TOKENS(HdPrmanRenderSettingsTokens,
    HDPRMAN_RENDER_SETTINGS_TOKENS);

TF_DEFINE_PUBLIC_TOKENS(HdPrmanExperimentalRenderSpecTokens,
    HDPRMAN_EXPERIMENTAL_RENDER_SPEC_TOKENS);

TF_DEFINE_PUBLIC_TOKENS(HdPrmanIntegratorTokens,
    HDPRMAN_INTEGRATOR_TOKENS);

const TfTokenVector HdPrmanRenderDelegate::SUPPORTED_RPRIM_TYPES =
{
    HdPrimTypeTokens->mesh,
    HdPrimTypeTokens->basisCurves,
    HdPrimTypeTokens->points,
    HdPrimTypeTokens->volume,
};

const TfTokenVector HdPrmanRenderDelegate::SUPPORTED_SPRIM_TYPES =
{
    HdPrimTypeTokens->camera,
    HdPrimTypeTokens->material,
    HdPrimTypeTokens->distantLight,
    HdPrimTypeTokens->domeLight,
    HdPrimTypeTokens->light,
    HdPrimTypeTokens->lightFilter,
    HdPrimTypeTokens->rectLight,
    HdPrimTypeTokens->diskLight,
    HdPrimTypeTokens->cylinderLight,
    HdPrimTypeTokens->sphereLight,
    HdPrimTypeTokens->pluginLight,
    HdPrimTypeTokens->extComputation,
    HdPrimTypeTokens->coordSys,
    _tokens->prmanParams,
};

const TfTokenVector HdPrmanRenderDelegate::SUPPORTED_BPRIM_TYPES =
{
    HdPrimTypeTokens->renderBuffer,
    _tokens->openvdbAsset,
    _tokens->field3dAsset,
};

static
std::string
_ToLower(const std::string &s)
{
    std::string result = s;
    for(auto &c : result) {
        c = tolower(c);
    }
    return result;
}

HdPrmanRenderDelegate::HdPrmanRenderDelegate(
    HdRenderSettingsMap const& settingsMap)
  : HdRenderDelegate(settingsMap)
  , _renderParam(
      std::make_unique<HdPrman_RenderParam>(
          _ToLower(
              GetRenderSetting<std::string>(
                  HdPrmanRenderSettingsTokens->rileyVariant,
                  TfGetenv("RILEY_VARIANT")))))
{
    _Initialize();
}

bool
HdPrmanRenderDelegate::IsInteractive() const
{
    return GetRenderSetting<bool>(
        HdRenderSettingsTokens->enableInteractive, true);
}

void
HdPrmanRenderDelegate::_Initialize()
{
    std::string integrator = HdPrmanIntegratorTokens->PxrPathTracer;
    std::string integratorEnv = TfGetenv("HD_PRMAN_INTEGRATOR");
    if (!integratorEnv.empty()) {
        integrator = integratorEnv;
    }

    // 64 samples is RenderMan default
    int maxSamples = TfGetenvInt("HD_PRMAN_MAX_SAMPLES", 64);

    float pixelVariance = 0.001f;

    // Prepare list of render settings descriptors
    _settingDescriptors.reserve(5);

    _settingDescriptors.push_back({
        std::string("Integrator"),
        HdPrmanRenderSettingsTokens->integratorName,
        VtValue(integrator) 
    });

    if (TfGetEnvSetting(HD_PRMAN_ENABLE_QUICKINTEGRATE)) {
        const std::string interactiveIntegrator = 
            HdPrmanIntegratorTokens->PxrDirectLighting;
        _settingDescriptors.push_back({
            std::string("Interactive Integrator"),
            HdPrmanRenderSettingsTokens->interactiveIntegrator,
            VtValue(interactiveIntegrator)
        });

        // If >0, the time in ms that we'll render quick output before switching
        // to path tracing
        _settingDescriptors.push_back({
            std::string("Interactive Integrator Timeout (ms)"),
            HdPrmanRenderSettingsTokens->interactiveIntegratorTimeout,
            VtValue(200)
        });
    }

    _settingDescriptors.push_back({
        std::string("Max Samples"),
        HdRenderSettingsTokens->convergedSamplesPerPixel,
        VtValue(maxSamples)
    });

    _settingDescriptors.push_back({
        std::string("Variance Threshold"),
        HdRenderSettingsTokens->convergedVariance,
        VtValue(pixelVariance)
    });

    _settingDescriptors.push_back({
        std::string("Riley variant"),
        HdPrmanRenderSettingsTokens->rileyVariant,
        VtValue(TfGetenv("RILEY_VARIANT"))
    });

    _settingDescriptors.push_back({
        std::string("Disable motion blur"),
        HdPrmanRenderSettingsTokens->disableMotionBlur,
        VtValue(false)});

    _PopulateDefaultSettings(_settingDescriptors);

    _renderParam->Begin(this);

    _resourceRegistry = std::make_shared<HdPrman_ResourceRegistry>(
        _renderParam);
}

HdPrmanRenderDelegate::~HdPrmanRenderDelegate()
{
    _renderParam.reset();
}

HdRenderSettingsMap
HdPrmanRenderDelegate::GetRenderSettingsMap() const
{
    return _settingsMap;
}

HdRenderSettingDescriptorList
HdPrmanRenderDelegate::GetRenderSettingDescriptors() const
{
    return _settingDescriptors;
}

HdRenderParam*
HdPrmanRenderDelegate::GetRenderParam() const
{
    return _renderParam.get();
}

void
HdPrmanRenderDelegate::CommitResources(HdChangeTracker *tracker)
{
    // Do nothing
}

TfTokenVector const&
HdPrmanRenderDelegate::GetSupportedRprimTypes() const
{
    return SUPPORTED_RPRIM_TYPES;
}

TfTokenVector const&
HdPrmanRenderDelegate::GetSupportedSprimTypes() const
{
    return SUPPORTED_SPRIM_TYPES;
}

TfTokenVector const&
HdPrmanRenderDelegate::GetSupportedBprimTypes() const
{
    return SUPPORTED_BPRIM_TYPES;
}

HdResourceRegistrySharedPtr
HdPrmanRenderDelegate::GetResourceRegistry() const
{
    return _resourceRegistry;
}

HdRenderPassSharedPtr
HdPrmanRenderDelegate::CreateRenderPass(HdRenderIndex *index,
                                        HdRprimCollection const& collection)
{
    if (!_renderPass) {
        _renderPass = std::make_shared<HdPrman_RenderPass>(
            index, collection, _renderParam);
    }
    return _renderPass;
}

HdInstancer *
HdPrmanRenderDelegate::CreateInstancer(HdSceneDelegate *delegate,
                                       SdfPath const& id)
{
    return new HdPrmanInstancer(delegate, id);
}

void
HdPrmanRenderDelegate::DestroyInstancer(HdInstancer *instancer)
{
    delete instancer;
}

HdRprim *
HdPrmanRenderDelegate::CreateRprim(TfToken const& typeId,
                                    SdfPath const& rprimId)
{
    if (typeId == HdPrimTypeTokens->mesh) {
        return new HdPrman_Mesh(rprimId);
    } else if (typeId == HdPrimTypeTokens->basisCurves) {
        return new HdPrman_BasisCurves(rprimId);
    } else if (typeId == HdPrimTypeTokens->points) {
        return new HdPrman_Points(rprimId);
    } else if (typeId == HdPrimTypeTokens->volume) {
        return new HdPrman_Volume(rprimId);
    } else {
        TF_CODING_ERROR("Unknown Rprim Type %s", typeId.GetText());
    }

    return nullptr;
}

void
HdPrmanRenderDelegate::DestroyRprim(HdRprim *rPrim)
{
    delete rPrim;
}

HdSprim *
HdPrmanRenderDelegate::CreateSprim(TfToken const& typeId,
                                    SdfPath const& sprimId)
{
    HdSprim* sprim = nullptr;
    if (typeId == HdPrimTypeTokens->camera) {
        sprim = new HdPrmanCamera(sprimId);
    } else if (typeId == HdPrimTypeTokens->material) {
        sprim = new HdPrmanMaterial(sprimId);
    } else if (typeId == HdPrimTypeTokens->coordSys) {
        sprim = new HdPrmanCoordSys(sprimId);
    } else if (typeId == HdPrimTypeTokens->lightFilter) {
        sprim = new HdPrmanLightFilter(sprimId, typeId);
    } else if (typeId == HdPrimTypeTokens->light ||
               typeId == HdPrimTypeTokens->distantLight ||
               typeId == HdPrimTypeTokens->domeLight ||
               typeId == HdPrimTypeTokens->rectLight ||
               typeId == HdPrimTypeTokens->diskLight ||
               typeId == HdPrimTypeTokens->cylinderLight ||
               typeId == HdPrimTypeTokens->sphereLight ||
               typeId == HdPrimTypeTokens->pluginLight) {
        sprim = new HdPrmanLight(sprimId, typeId);

        // Disregard fallback prims in count.
        if (sprim->GetId() != SdfPath()) {
            _renderParam->IncreaseSceneLightCount();
        }
    } else if (typeId == HdPrimTypeTokens->extComputation) {
        sprim = new HdExtComputation(sprimId);
    
    } else if (typeId == _tokens->prmanParams) {
        sprim = new HdPrmanParamsSetter(sprimId);
    } else {
        TF_CODING_ERROR("Unknown Sprim Type %s", typeId.GetText());
    }

    return sprim;
}

HdSprim *
HdPrmanRenderDelegate::CreateFallbackSprim(TfToken const& typeId)
{
    // For fallback sprims, create objects with an empty scene path.
    // They'll use default values and won't be updated by a scene delegate.
    if (typeId == HdPrimTypeTokens->camera) {
        return new HdPrmanCamera(SdfPath::EmptyPath());
    } else if (typeId == HdPrimTypeTokens->material) {
        return new HdPrmanMaterial(SdfPath::EmptyPath());
    } else if (typeId == HdPrimTypeTokens->coordSys) {
        return new HdPrmanCoordSys(SdfPath::EmptyPath());
    } else if (typeId == HdPrimTypeTokens->lightFilter) {
        return new HdPrmanLightFilter(SdfPath::EmptyPath(), typeId);
    } else if (typeId == HdPrimTypeTokens->light ||
               typeId == HdPrimTypeTokens->distantLight ||
               typeId == HdPrimTypeTokens->domeLight ||
               typeId == HdPrimTypeTokens->rectLight ||
               typeId == HdPrimTypeTokens->diskLight ||
               typeId == HdPrimTypeTokens->cylinderLight ||
               typeId == HdPrimTypeTokens->sphereLight ||
               typeId == HdPrimTypeTokens->pluginLight) {
        return new HdPrmanLight(SdfPath::EmptyPath(), typeId);
    } else if (typeId == HdPrimTypeTokens->extComputation) {
        return new HdExtComputation(SdfPath::EmptyPath());
    } else if (typeId == _tokens->prmanParams) {
        return new HdPrmanParamsSetter(SdfPath::EmptyPath());
    } else {
        TF_CODING_ERROR("Unknown Sprim Type %s", typeId.GetText());
    }

    return nullptr;
}

void
HdPrmanRenderDelegate::DestroySprim(HdSprim *sprim)
{
    // Disregard fallback prims in count.
    if (sprim->GetId() != SdfPath()) {
        _renderParam->DecreaseSceneLightCount();
    }
    delete sprim;
}

HdBprim *
HdPrmanRenderDelegate::CreateBprim(
    TfToken const& typeId,
    SdfPath const& bprimId)
{
    if (typeId == _tokens->openvdbAsset ||
        typeId == _tokens->field3dAsset) {
        return new HdPrman_Field(typeId, bprimId);
    } else if (typeId == HdPrimTypeTokens->renderBuffer) {
        return new HdPrmanRenderBuffer(bprimId);
    } else {
        TF_CODING_ERROR("Unknown Bprim Type %s", typeId.GetText());
    }
    return nullptr;
}

HdBprim *
HdPrmanRenderDelegate::CreateFallbackBprim(TfToken const& typeId)
{
    if (typeId == _tokens->openvdbAsset ||
        typeId == _tokens->field3dAsset) {
        return new HdPrman_Field(typeId, SdfPath::EmptyPath());
    } else if (typeId == HdPrimTypeTokens->renderBuffer) {
        return new HdPrmanRenderBuffer(SdfPath::EmptyPath());
    } else {
        TF_CODING_ERROR("Unknown Bprim Type %s", typeId.GetText());
    }
    return nullptr;
}

void
HdPrmanRenderDelegate::DestroyBprim(HdBprim *bPrim)
{
    delete bPrim;
}

HdAovDescriptor
HdPrmanRenderDelegate::GetDefaultAovDescriptor(
    TfToken const& name) const
{
    if (IsInteractive()) {
        if (name == HdAovTokens->color) {
            return HdAovDescriptor(
                HdFormatFloat32Vec4, 
                false,
                VtValue(GfVec4f(0.0f)));
        } else if (name == HdAovTokens->depth) {
            return HdAovDescriptor(HdFormatFloat32, false, VtValue(1.0f));
        } else if (name == HdAovTokens->primId ||
                   name == HdAovTokens->instanceId ||
                   name == HdAovTokens->elementId) {
            return HdAovDescriptor(HdFormatInt32, false, VtValue(-1));
        }
        return HdAovDescriptor(
            HdFormatFloat32Vec3, 
            false,
            VtValue(GfVec3f(0.0f)));
    }
    return HdAovDescriptor();
}

TfToken
HdPrmanRenderDelegate::GetMaterialBindingPurpose() const
{
    return HdTokens->full;
}

#if HD_API_VERSION < 41
TfToken
HdPrmanRenderDelegate::GetMaterialNetworkSelector() const
{
    static const TfToken ri("ri");
    return ri;
}
#else
TfTokenVector
HdPrmanRenderDelegate::GetMaterialRenderContexts() const
{
    static const TfToken ri("ri");
#ifdef PXR_MATERIALX_SUPPORT_ENABLED
    return {ri, _tokens->mtlxRenderContext};
#else
    return {ri};
#endif
}
#endif

TfTokenVector
HdPrmanRenderDelegate::GetShaderSourceTypes() const
{
    return HdPrmanMaterial::GetShaderSourceTypes();
}

void
HdPrmanRenderDelegate::SetRenderSetting(TfToken const &key, 
                                        VtValue const &value)
{
    // update settings version only if a setting actually changed
    auto it = _settingsMap.find(key);
    if (it != _settingsMap.end()) {
        if (value != it->second) {
            _settingsVersion++;
        }
    } else {
        _settingsVersion++;
    }

    _settingsMap[key] = value;

    if (TfDebug::IsEnabled(HD_RENDER_SETTINGS)) {
        std::cout << "Render Setting [" << key << "] = " << value << std::endl;
    }
}

bool
HdPrmanRenderDelegate::IsStopSupported() const
{
    if (IsInteractive()) {
        return true;
    }
    return false;
}

bool
HdPrmanRenderDelegate::IsStopped() const
{
    if (IsInteractive()) {
        return !_renderParam->IsRendering();
    }
    return true;
}

bool
HdPrmanRenderDelegate::Stop(bool blocking)
{
    if (IsInteractive()) {
        _renderParam->StopRender(blocking);
        return !_renderParam->IsRendering();
    }
    return true;
}

bool
HdPrmanRenderDelegate::Restart()
{
    if (IsInteractive()) {
        // Next call into HdPrman_RenderPass::_Execute will do a StartRender
        _renderParam->sceneVersion++;
        return true;
    }
    return false;
}

PXR_NAMESPACE_CLOSE_SCOPE
