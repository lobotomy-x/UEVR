#pragma once

#include "uevr/API.h"
#include <sdk/FRenderResource.hpp>
#include <sdk/FTexture.hpp>
#include <sdk/UTexture.hpp>
#include <sdk/FRenderTarget.hpp>
#include <sdk/FTextureResource.hpp>
#include <sdk/USceneCaptureComponent2D.hpp>
#include <sdk/FTextureRenderTargetResource.hpp>

#include <utility/PointerHook.hpp>

#include <sdk/StereoStuff.hpp>
#include <sdk/FViewportInfo.hpp>
#include <sdk/threading/ThreadWorker.hpp>
#include <sdk/RHICommandList.hpp>
#include <sdk/UTexture.hpp>
#include <sdk/UObjectReference.hpp>
#include <sdk/AActor.hpp>
#include <sdk/USceneCaptureComponent2D.hpp>
#include <sdk/UTexture.hpp>
namespace uevr {
struct FRHICommandListImmediate;;
struct UCanvas;
struct IStereoLayers;

namespace sdk {
struct FSceneViewStateInterface;
class FViewport;
class FCanvas;
class UGameViewportClient;
class AActor;
class UObject;
class USceneCaptureComponent2D;
class UTexture;
class FSceneViewFamily;
class FSceneView;
}


namespace frhitexture2d {
void* get_native_resource(UEVR_FRHITexture2DHandle handle);

extern UEVR_FRHITexture2DFunctions functions;
}
}