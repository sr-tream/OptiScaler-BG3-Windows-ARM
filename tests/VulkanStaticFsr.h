#pragma once
#include <fsr2/ffx_fsr2.h>
#include <fsr2/vk/ffx_fsr2_vk.h>
#include <fsr2_212/ffx_fsr2.h>
#include <fsr2_212/vk/ffx_fsr2_vk.h>

static unsigned staticVersion=0;
static ffxReturnCode_t StaticDispatch(ffxContext* context,const ffxDispatchDescHeader* header)
{
    const auto& p=*(const ffxDispatchDescUpscale*)header;
    const VkFormat formats[]={VK_FORMAT_B10G11R11_UFLOAT_PACK32,VK_FORMAT_R16G16_SFLOAT,VK_FORMAT_D24_UNORM_S8_UINT,
        VK_FORMAT_R8_UNORM,VK_FORMAT_R8_UNORM,VK_FORMAT_R32G32_SFLOAT,VK_FORMAT_B10G11R11_UFLOAT_PACK32};
    const FfxApiResource* inputs[]={&p.color,&p.motionVectors,&p.depth,&p.reactive,&p.transparencyAndComposition,&p.exposure,&p.output};
#define COPY_PARAMS(desc,ResourceType,getResource,list,state,ctx) \
    desc.commandList=list((VkCommandBuffer)p.commandList); \
    ResourceType converted[7]{}; \
    for (unsigned i=0;i<7;++i) {auto image=(VkImage)inputs[i]->resource; \
        converted[i]=getResource(ctx,image,nativeTestViews.at(image),inputs[i]->description.width,inputs[i]->description.height,formats[i],nullptr,i==6?state##UNORDERED_ACCESS:state##COMPUTE_READ);} \
    desc.color=converted[0];desc.motionVectors=converted[1];desc.depth=converted[2];desc.reactive=converted[3]; \
    desc.transparencyAndComposition=converted[4];desc.exposure=converted[5];desc.output=converted[6]; \
    desc.renderSize={p.renderSize.width,p.renderSize.height};desc.jitterOffset={p.jitterOffset.x,p.jitterOffset.y}; \
    desc.motionVectorScale={p.motionVectorScale.x,p.motionVectorScale.y};desc.reset=p.reset;desc.preExposure=p.preExposure; \
    desc.frameTimeDelta=p.frameTimeDelta;desc.cameraNear=p.cameraNear;desc.cameraFar=p.cameraFar; \
    desc.cameraFovAngleVertical=p.cameraFovAngleVertical;
    if (!staticVersion) {
        auto ctx=(FfxFsr2Context*)*context;FfxFsr2DispatchDescription desc{};
        COPY_PARAMS(desc,FfxResource,ffxGetTextureResourceVK,ffxGetCommandListVK,FFX_RESOURCE_STATE_,ctx)
        desc.viewSpaceToMetersFactor=p.viewSpaceToMetersFactor;
        return ffxFsr2ContextDispatch(ctx,&desc)==FFX_OK?FFX_API_RETURN_OK:FFX_API_RETURN_ERROR;
    }
    auto ctx=(Fsr212::FfxFsr2Context*)*context;Fsr212::FfxFsr2DispatchDescription desc{};
    COPY_PARAMS(desc,Fsr212::FfxResource,Fsr212::ffxGetTextureResourceVK212,Fsr212::ffxGetCommandListVK212,Fsr212::FFX_RESOURCE_STATE_,ctx)
    return Fsr212::ffxFsr2ContextDispatch212(ctx,&desc)==Fsr212::FFX_OK?FFX_API_RETURN_OK:FFX_API_RETURN_ERROR;
#undef COPY_PARAMS
}

static int TestStaticFsr(VkDevice device,VkPhysicalDevice physical,uint32_t family)
{
    int result=0;
    for (staticVersion=0;staticVersion<2;++staticVersion) {
        printf("Static FSR %s recorded GPU test\n",staticVersion?"2.1.2":"2.2.1");
        if (!staticVersion) {
            FfxFsr2ContextDescription desc{};desc.maxRenderSize={64,64};desc.displaySize={128,128};
            desc.device=ffxGetDeviceVK(device);desc.flags=FFX_FSR2_ENABLE_HIGH_DYNAMIC_RANGE|FFX_FSR2_ENABLE_DEPTH_INVERTED;
#ifdef TEST_AUTO_EXPOSURE
            desc.flags|=FFX_FSR2_ENABLE_AUTO_EXPOSURE;
#endif
            const auto size=ffxFsr2GetScratchMemorySizeVK(physical);void* scratch=calloc(size,1);
            if (ffxFsr2GetInterfaceVK(&desc.callbacks,scratch,size,physical,RecordingResolver)!=FFX_OK) return 100;
            FfxFsr2Context ctx{};if (ffxFsr2ContextCreate(&ctx,&desc)!=FFX_OK) return 101;
            ffxContext opaque=&ctx;result=TestNativeFrame(device,physical,family,&opaque,StaticDispatch);
            ffxFsr2ContextDestroy(&ctx);free(scratch);
        } else {
            Fsr212::FfxFsr2ContextDescription desc{};desc.maxRenderSize={64,64};desc.displaySize={128,128};
            desc.device=Fsr212::ffxGetDeviceVK212(device);desc.flags=Fsr212::FFX_FSR2_ENABLE_HIGH_DYNAMIC_RANGE|Fsr212::FFX_FSR2_ENABLE_DEPTH_INVERTED;
#ifdef TEST_AUTO_EXPOSURE
            desc.flags|=Fsr212::FFX_FSR2_ENABLE_AUTO_EXPOSURE;
#endif
            const auto size=Fsr212::ffxFsr2GetScratchMemorySizeVK212(physical);void* scratch=calloc(size,1);
            if (Fsr212::ffxFsr2GetInterfaceVK212(&desc.callbacks,scratch,size,physical,RecordingResolver)!=Fsr212::FFX_OK) return 102;
            Fsr212::FfxFsr2Context ctx{};if (Fsr212::ffxFsr2ContextCreate212(&ctx,&desc)!=Fsr212::FFX_OK) return 103;
            ffxContext opaque=&ctx;result=TestNativeFrame(device,physical,family,&opaque,StaticDispatch);
            Fsr212::ffxFsr2ContextDestroy212(&ctx);free(scratch);
        }
        if (result) return result;
    }
    return result;
}
