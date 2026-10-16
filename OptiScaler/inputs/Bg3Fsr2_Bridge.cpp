#include "pch.h"
#include "Bg3Fsr2_Bridge.h"
#include "Bg3VulkanLifetime.h"
#include "Bg3Fsr2_Vk.h"
#include <NVNGX_Parameter.h>
#include <upscalers/FeatureProvider_Vk.h>
#include <hooks/Vulkan_Hooks.h>
#include <nvsdk_ngx_vk.h>
#include <inputs/FG/Upscaler_Inputs_Vk.h>
#include <bit>

namespace Bg3Fsr2Bridge
{
namespace
{
struct Key
{
    VkDevice device;
    uint32_t width,height,displayWidth,displayHeight;
    Upscaler backend;
    int ffxIndex;
    uint64_t epoch;
    bool operator==(const Key&) const = default;
};
struct Context
{
    Key key;
    std::unique_ptr<NVNGX_Parameters> parameters;
    std::unique_ptr<IFeature_Vk> feature;
    bool resetPending=true;
    std::chrono::steady_clock::time_point lastFrame {};
};
std::mutex selectionMutex,dispatchMutex;
std::shared_ptr<Context> current;
uint64_t epoch=0;
std::optional<Key> retryKey;
std::chrono::steady_clock::time_point retryAt {};
std::atomic<bool> suspended=false,resetRequested=false;
std::atomic<unsigned> rejectedFrames=0;
Bg3VulkanLifetime lifetime({vkCreateFence,vkGetFenceStatus,vkResetFences,vkDestroyFence});

struct Resources
{
    NVSDK_NGX_Resource_VK color,motion,depth,reactive,composition,exposure,output;
};
struct ClearResourcePointers
{
    NVNGX_Parameters& parameters;
    ~ClearResourcePointers()
    {
        for (const char* key:{NVSDK_NGX_Parameter_Color,NVSDK_NGX_Parameter_MotionVectors,NVSDK_NGX_Parameter_Depth,
            NVSDK_NGX_Parameter_Output,NVSDK_NGX_Parameter_ExposureTexture,"FSR.reactive","FSR.transparencyAndComposition",
            NVSDK_NGX_Parameter_DLSS_Input_Bias_Current_Color_Mask}) parameters.Set(key,(void*)nullptr);
    }
};
float Float(const Frame& f,size_t index) {return std::bit_cast<float>(f.constants[index]);}
NVSDK_NGX_Resource_VK Resource(const Image& image,bool output=false)
{
    NVSDK_NGX_Resource_VK result {};
    result.Type=NVSDK_NGX_RESOURCE_VK_TYPE_VK_IMAGEVIEW;
    result.Resource.ImageViewInfo.Image=image.image; result.Resource.ImageViewInfo.ImageView=image.view;
    result.Resource.ImageViewInfo.SubresourceRange=image.range; result.Resource.ImageViewInfo.Format=image.format;
    result.Resource.ImageViewInfo.Width=image.width; result.Resource.ImageViewInfo.Height=image.height;
    result.ReadWrite=output;
    return result;
}
bool Valid(const Frame& f)
{
    const auto& c=f.constants;
    if (f.stages!=0x7f || !f.instance || !f.physicalDevice || !f.commandBuffer || !f.output.device ||
        !c[4] || !c[5] || c[4]>c[8] || c[5]>c[9] || c[8]>16384 || c[9]>16384 ||
        c[0] || c[1] || c[38] || c[39]) return false;
    for (const Image* image:{&f.color,&f.motion,&f.depth,&f.reactive,&f.composition,&f.exposure,&f.output})
        if (!image->image || !image->view || image->device!=f.output.device || image->range.baseMipLevel ||
            image->range.levelCount!=1 || image->range.baseArrayLayer || image->range.layerCount!=1) return false;
    for (const Image* image:{&f.color,&f.motion,&f.depth,&f.reactive,&f.composition})
        if (image->width!=c[4] || image->height!=c[5] || image->layout!=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) return false;
    if (f.color.format!=VK_FORMAT_B10G11R11_UFLOAT_PACK32 || f.motion.format!=VK_FORMAT_R16G16_SFLOAT ||
        f.depth.format!=VK_FORMAT_D24_UNORM_S8_UINT || f.reactive.format!=VK_FORMAT_R8_UNORM ||
        f.composition.format!=VK_FORMAT_R8_UNORM || f.exposure.format!=VK_FORMAT_R32G32_SFLOAT ||
        f.exposure.width!=1 || f.exposure.height!=1 || f.exposure.layout!=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL ||
        f.output.format!=VK_FORMAT_B10G11R11_UFLOAT_PACK32 || f.output.width!=c[8] || f.output.height!=c[9] ||
        f.output.layout!=VK_IMAGE_LAYOUT_GENERAL) return false;
    for (size_t i:{16,17,18,19,20,21,22,23,28,32}) if (!std::isfinite(Float(f,i))) return false;
    return Float(f,16)<0 && Float(f,17)>0 && Float(f,19)>0 && Float(f,28)>0 && Float(f,32)>0 && Float(f,32)<=1;
}
void Reset()
{
    if (current && State::Instance().currentFeature==current->feature.get()) State::Instance().currentFeature=nullptr;
    current.reset();
}
void Parameters(NVNGX_Parameters& p,const Frame& f,Resources& r,bool reset)
{
    const auto& c=f.constants;
    const float ratio=float(c[8])/c[4];
    const auto quality=ratio>2.0f?NVSDK_NGX_PerfQuality_Value_UltraPerformance:ratio>1.7f?NVSDK_NGX_PerfQuality_Value_MaxPerf:
        ratio>1.5f?NVSDK_NGX_PerfQuality_Value_Balanced:ratio>1.3f?NVSDK_NGX_PerfQuality_Value_MaxQuality:
        ratio>1.0f?NVSDK_NGX_PerfQuality_Value_UltraQuality:NVSDK_NGX_PerfQuality_Value_DLAA;
    p.Set(NVSDK_NGX_Parameter_Width,c[4]);p.Set(NVSDK_NGX_Parameter_Height,c[5]);
    p.Set(NVSDK_NGX_Parameter_OutWidth,c[8]);p.Set(NVSDK_NGX_Parameter_OutHeight,c[9]);
    p.Set(NVSDK_NGX_Parameter_PerfQualityValue,(int)quality);
    p.Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags,(unsigned)(NVSDK_NGX_DLSS_Feature_Flags_IsHDR |
        NVSDK_NGX_DLSS_Feature_Flags_DepthInverted | NVSDK_NGX_DLSS_Feature_Flags_MVLowRes | NVSDK_NGX_DLSS_Feature_Flags_DoSharpening));
    p.Set(NVSDK_NGX_Parameter_Color,&r.color);p.Set(NVSDK_NGX_Parameter_MotionVectors,&r.motion);
    p.Set(NVSDK_NGX_Parameter_Depth,&r.depth);p.Set(NVSDK_NGX_Parameter_Output,&r.output);
    p.Set(NVSDK_NGX_Parameter_ExposureTexture,&r.exposure);p.Set("FSR.reactive",&r.reactive);
    p.Set("FSR.transparencyAndComposition",&r.composition);p.Set(NVSDK_NGX_Parameter_DLSS_Input_Bias_Current_Color_Mask,&r.reactive);
    p.Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width,c[4]);p.Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height,c[5]);
    p.Set(NVSDK_NGX_Parameter_Jitter_Offset_X,Float(f,20));p.Set(NVSDK_NGX_Parameter_Jitter_Offset_Y,Float(f,21));
    p.Set(NVSDK_NGX_Parameter_MV_Scale_X,Float(f,22)*c[4]);p.Set(NVSDK_NGX_Parameter_MV_Scale_Y,Float(f,23)*c[5]);
    p.Set(NVSDK_NGX_Parameter_DLSS_Pre_Exposure,Float(f,28));p.Set(NVSDK_NGX_Parameter_FrameTimeDeltaInMsec,Float(f,32)*1000);
    p.Set(NVSDK_NGX_Parameter_Reset,(unsigned)reset);p.Set(NVSDK_NGX_Parameter_Sharpness,f.sharpness);
    p.Set("FSR.cameraNear",-Float(f,17)/Float(f,16));p.Set("FSR.cameraFar",Float(f,17)/(1-Float(f,16)));
    p.Set("FSR.cameraFovAngleVertical",2*std::atan(Float(f,19)));p.Set("FSR.frameTimeDelta",Float(f,32)*1000);
    p.Set("FSR.viewSpaceToMetersFactor",Float(f,34));p.Set("FSR.upscaleSize.width",c[8]);p.Set("FSR.upscaleSize.height",c[9]);
}
}

bool Dispatch(const Frame& f)
{
    lifetime.Poll();
    WindowSuspended(VulkanHooks::GameWindowInactive());
    if (suspended.load()) return false;
    if (!Valid(f))
    {
        if (rejectedFrames.fetch_add(1)<8) LOG_WARN("BG3FSR replacement packet rejected; stages:{:X} render:{}x{} output:{}x{}",f.stages,f.constants[4],f.constants[5],f.constants[8],f.constants[9]);
        resetRequested=true;return false;
    }
    std::scoped_lock dispatchLock(dispatchMutex);
    auto& state=State::Instance();auto& cfg=*Config::Instance();
    const auto now=std::chrono::steady_clock::now();
    std::shared_ptr<Context> context;
    Key key {};
    bool replace=false;
    {
        std::scoped_lock lock(selectionMutex);
        if (state.changeBackend[HandleId])
        {
            if (state.newBackend!=Upscaler::Reset) cfg.VulkanUpscaler=state.newBackend;
            ++epoch;state.changeBackend[HandleId]=false;state.newBackend=Upscaler::Reset;
        }
        context=current;
        auto backend=cfg.VulkanUpscaler.value_or(Upscaler::FFX);
        switch (backend)
        {
        case Upscaler::FSR21: case Upscaler::FSR22: case Upscaler::FFX:
        case Upscaler::XeSS: case Upscaler::DLSS: case Upscaler::DLSSD:
            break;
        default:
            // Interop changes graphics state and introduces cross-API queue waits.
            // This native compute interception only supports Vulkan backends.
            LOG_WARN("BG3FSR backend {} requires unsupported interop; retaining native Vulkan backend",UpscalerDisplayName(backend));
            backend=context?context->key.backend:Upscaler::FFX;
            cfg.VulkanUpscaler=backend;
            break;
        }
        key={f.output.device,f.constants[4],f.constants[5],f.constants[8],f.constants[9],backend,
            backend==Upscaler::FFX || backend==Upscaler::FFX_on12?cfg.FfxUpscalerIndex.value_or_default():-1,epoch};
        replace=!context || !(context->key==key);
        if (replace && retryKey && *retryKey==key && now<retryAt) return false;
    }
    Resources resources {Resource(f.color),Resource(f.motion),Resource(f.depth),Resource(f.reactive),
        Resource(f.composition),Resource(f.exposure),Resource(f.output,true)};
    if (replace)
    {
        auto candidate=std::make_shared<Context>();candidate->key=key;
        candidate->parameters=std::make_unique<NVNGX_Parameters>(API::Vulkan,false);
        ClearResourcePointers clear {*candidate->parameters};
        Parameters(*candidate->parameters,f,resources,true);
        state.autoExposure.reset();
        lifetime.Record(f.output.device,f.commandBuffer,candidate);
        const bool loaded=FeatureProvider_Vk::GetFeature(key.backend,HandleId,candidate->parameters.get(),&candidate->feature);
        const bool ready=loaded && candidate->feature->Init(f.instance,f.physicalDevice,f.output.device,f.commandBuffer,
            vkGetInstanceProcAddr,cfg.OptimizeBg3Fsr2.value_or_default() &&
                (key.backend==Upscaler::FFX || key.backend==Upscaler::FSR21 || key.backend==Upscaler::FSR22)?
                Bg3Fsr2::RecordingDeviceProcAddr:VulkanHooks::GetDeviceProcAddr,candidate->parameters.get());
        if (!ready)
        {
            {std::scoped_lock lock(selectionMutex);retryKey=key;retryAt=now+std::chrono::seconds(1);}
            if (context) cfg.VulkanUpscaler=context->key.backend;
            LOG_WARN("BG3FSR replacement initialization failed for {}; native FSR2 retained; retry allowed",UpscalerDisplayName(key.backend));
            return false;
        }
        candidate->key.backend=cfg.VulkanUpscaler.value_or_default();
        candidate->key.ffxIndex=candidate->key.backend==Upscaler::FFX || candidate->key.backend==Upscaler::FFX_on12?
            cfg.FfxUpscalerIndex.value_or_default():-1;
        {std::scoped_lock lock(selectionMutex);current=candidate;retryKey.reset();state.currentFeature=candidate->feature.get();}
        context=candidate;
        LOG_INFO("BG3FSR replacement context ready; backend:{} version:{}.{}.{} input:{}x{} output:{}x{}",
            UpscalerDisplayName(context->feature->GetUpscalerType()),context->feature->Version().major,context->feature->Version().minor,
            context->feature->Version().patch,key.width,key.height,key.displayWidth,key.displayHeight);
    }
    const bool reset=context->resetPending || resetRequested.exchange(false) || f.constants[15]==0 ||
        (context->lastFrame.time_since_epoch().count() && now-context->lastFrame>std::chrono::milliseconds(500));
    ClearResourcePointers clear {*context->parameters};
    Parameters(*context->parameters,f,resources,reset);
    state.currentFeature=context->feature.get();state.currentInputApiName=ApiUpscalerInput::FSR2X_VK;
    state.currentVkDevice=f.output.device;state.api=API::Vulkan;
    lifetime.Record(f.output.device,f.commandBuffer,context);
    UpscalerInputsVk::UpscaleStart(f.commandBuffer,context->parameters.get(),context->feature.get());
    if (!context->feature->Evaluate(f.commandBuffer,context->parameters.get()))
    {
        std::scoped_lock lock(selectionMutex);
        if (current==context) Reset();
        retryKey=key;retryAt=now+std::chrono::seconds(1);
        LOG_WARN("BG3FSR replacement dispatch failed; native FSR2 retained; context will be retried");
        return false;
    }
    context->resetPending=false;context->lastFrame=now;
    return true;
}
void ResetRecording(VkCommandBuffer cmd) {lifetime.ResetRecording(cmd);}
void ExecuteCommands(VkCommandBuffer primary,uint32_t count,const VkCommandBuffer* secondary)
{lifetime.ExecuteCommands(primary,count,secondary);}
void Submitted(VkQueue queue,const std::vector<VkCommandBuffer>& commands,PFN_vkQueueSubmit submit)
{
    const auto result=lifetime.Submitted(queue,commands,submit);
    if (result!=VK_SUCCESS) LOG_WARN("BG3FSR context retirement marker failed: {}; resources retained",(int)result);
    lifetime.Poll();
}
std::shared_ptr<IFeature_Vk> CurrentFeatureLifetime()
{
    std::scoped_lock lock(selectionMutex);
    if (!current || State::Instance().currentFeature!=current->feature.get()) return {};
    return std::shared_ptr<IFeature_Vk>(current,current->feature.get());
}
void WindowSuspended(bool value)
{
    if (suspended.exchange(value)!=value)
    {resetRequested=true;LOG_INFO("BG3FSR window suspended:{}; temporal reset requested",value);}
    if (value)
    {
        if (auto fg=State::Instance().currentVkFG;fg)
        {
            OwnedTryLockGuard lock(fg->Mutex,587);
            if (lock) {fg->Deactivate();fg->ResetCounters();}
        }
    }
}
void SwapchainChanged() {resetRequested=true;}
void DiscardEvaluation()
{
    std::shared_ptr<Context> released;
    {
        std::scoped_lock lock(selectionMutex);
        if (current && State::Instance().currentFeature==current->feature.get()) State::Instance().currentFeature=nullptr;
        released=std::move(current);retryKey.reset();resetRequested=true;
    }
}
bool UsesAutoExposure()
{
    std::scoped_lock lock(selectionMutex);
    return current && current->feature && current->feature->AutoExposure();
}
void DestroyDevice(VkDevice device)
{
    std::shared_ptr<Context> released;
    {
        std::scoped_lock lock(selectionMutex);
        if (current && current->key.device==device)
        {
            if (State::Instance().currentFeature==current->feature.get()) State::Instance().currentFeature=nullptr;
            released=std::move(current);retryKey.reset();
        }
    }
    // vkDestroyDevice requires the caller to have completed all device work already.
    lifetime.DestroyDevice(device);
}
}
