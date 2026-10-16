#pragma once
#include <cmath>
#ifdef TEST_STATIC_FSR
#include <unordered_map>
inline std::unordered_map<VkImage,VkImageView> nativeTestViews;
#endif

// Execute two FSR frames using BG3's actual native Vulkan formats and both masks.
static int TestNativeFrame(VkDevice device, VkPhysicalDevice physical, uint32_t family,
                           ffxContext* context, PfnFfxDispatch dispatch)
{
    struct Owned
    {
        VkDevice device;
        std::vector<VkImage> images;
        std::vector<VkDeviceMemory> memories;
        std::vector<VkImageView> views;
        VkBuffer readback=VK_NULL_HANDLE;
        VkCommandPool pool=VK_NULL_HANDLE;
        VkFence fence=VK_NULL_HANDLE;
        ~Owned()
        {
            vkDeviceWaitIdle(device);
            for (auto view:views) vkDestroyImageView(device,view,nullptr);
#ifdef TEST_STATIC_FSR
            nativeTestViews.clear();
#endif
            if (fence) vkDestroyFence(device,fence,nullptr);
            if (pool) vkDestroyCommandPool(device,pool,nullptr);
            if (readback) vkDestroyBuffer(device,readback,nullptr);
            for (auto image:images) vkDestroyImage(device,image,nullptr);
            for (auto memory:memories) vkFreeMemory(device,memory,nullptr);
        }
    } owned {device};
    VkPhysicalDeviceMemoryProperties memoryProperties;
    vkGetPhysicalDeviceMemoryProperties(physical,&memoryProperties);
    auto memoryType=[&](uint32_t bits,VkMemoryPropertyFlags required) {
        for (uint32_t i=0;i<memoryProperties.memoryTypeCount;++i)
            if ((bits&(1u<<i)) && (memoryProperties.memoryTypes[i].propertyFlags&required)==required) return i;
        return UINT32_MAX;
    };
    const VkFormat formats[]={VK_FORMAT_B10G11R11_UFLOAT_PACK32,VK_FORMAT_R16G16_SFLOAT,
        VK_FORMAT_D24_UNORM_S8_UINT,VK_FORMAT_R8_UNORM,VK_FORMAT_R8_UNORM,
        VK_FORMAT_R32G32_SFLOAT,VK_FORMAT_B10G11R11_UFLOAT_PACK32};
    FfxApiResource resources[7] {};
    for (unsigned i=0;i<7;++i)
    {
        VkImageCreateInfo info {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        info.imageType=VK_IMAGE_TYPE_2D; info.format=formats[i];
        info.extent={i==5?1u:i==6?128u:64u,i==5?1u:i==6?128u:64u,1};
        info.mipLevels=1; info.arrayLayers=1; info.samples=VK_SAMPLE_COUNT_1_BIT;
        info.tiling=VK_IMAGE_TILING_OPTIMAL;
        info.usage=i==6 ? VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT :
            VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if (i==2) info.usage|=VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        VkImage image=VK_NULL_HANDLE;
        if (vkCreateImage(device,&info,nullptr,&image)!=VK_SUCCESS) return 30+i;
        owned.images.push_back(image);
        VkMemoryRequirements requirements;
        vkGetImageMemoryRequirements(device,image,&requirements);
        VkMemoryAllocateInfo allocation {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize=requirements.size; allocation.memoryTypeIndex=memoryType(requirements.memoryTypeBits,0);
        if (allocation.memoryTypeIndex==UINT32_MAX) return 40+i;
        VkDeviceMemory memory=VK_NULL_HANDLE;
        if (vkAllocateMemory(device,&allocation,nullptr,&memory)!=VK_SUCCESS) return 50+i;
        owned.memories.push_back(memory);
        if (vkBindImageMemory(device,image,memory,0)!=VK_SUCCESS) return 60+i;
#ifdef TEST_STATIC_FSR
        VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        viewInfo.image=image;viewInfo.viewType=VK_IMAGE_VIEW_TYPE_2D;viewInfo.format=formats[i];
        viewInfo.subresourceRange={VkImageAspectFlags(i==2?VK_IMAGE_ASPECT_DEPTH_BIT:VK_IMAGE_ASPECT_COLOR_BIT),0,1,0,1};
        VkImageView view=VK_NULL_HANDLE;
        if (vkCreateImageView(device,&viewInfo,nullptr,&view)!=VK_SUCCESS) return 90+i;
        owned.views.push_back(view);nativeTestViews[image]=view;
#endif
        FfxApiResourceDescription description {};
        description.type=FFX_API_RESOURCE_TYPE_TEXTURE2D; description.width=info.extent.width;
        description.height=info.extent.height; description.depth=1; description.mipCount=1;
        const uint32_t ffxFormats[]={FFX_API_SURFACE_FORMAT_R11G11B10_FLOAT,FFX_API_SURFACE_FORMAT_R16G16_FLOAT,
            FFX_API_SURFACE_FORMAT_R32_UINT,FFX_API_SURFACE_FORMAT_R8_UNORM,FFX_API_SURFACE_FORMAT_R8_UNORM,
            FFX_API_SURFACE_FORMAT_R32G32_FLOAT,FFX_API_SURFACE_FORMAT_R11G11B10_FLOAT};
        description.format=ffxFormats[i];
        description.usage=i==6?FFX_API_RESOURCE_USAGE_UAV:FFX_API_RESOURCE_USAGE_READ_ONLY;
        if (i==2) description.usage|=FFX_API_RESOURCE_USAGE_DEPTHTARGET;
        resources[i]=ffxApiGetResourceVK(image,description,i==6?FFX_API_RESOURCE_STATE_UNORDERED_ACCESS:FFX_API_RESOURCE_STATE_COMPUTE_READ);
    }
    VkBufferCreateInfo bufferInfo {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bufferInfo.size=128*128*4; bufferInfo.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (vkCreateBuffer(device,&bufferInfo,nullptr,&owned.readback)!=VK_SUCCESS) return 70;
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(device,owned.readback,&requirements);
    VkMemoryAllocateInfo allocation {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize=requirements.size;
    allocation.memoryTypeIndex=memoryType(requirements.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (allocation.memoryTypeIndex==UINT32_MAX) return 71;
    VkDeviceMemory readbackMemory=VK_NULL_HANDLE;
    if (vkAllocateMemory(device,&allocation,nullptr,&readbackMemory)!=VK_SUCCESS) return 72;
    owned.memories.push_back(readbackMemory);
    if (vkBindBufferMemory(device,owned.readback,readbackMemory,0)!=VK_SUCCESS) return 73;
    VkCommandPoolCreateInfo poolInfo {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolInfo.queueFamilyIndex=family;
    if (vkCreateCommandPool(device,&poolInfo,nullptr,&owned.pool)!=VK_SUCCESS) return 74;
    VkCommandBufferAllocateInfo cmdInfo {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cmdInfo.commandPool=owned.pool; cmdInfo.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY; cmdInfo.commandBufferCount=1;
    VkCommandBuffer cmd=VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(device,&cmdInfo,&cmd)!=VK_SUCCESS) return 75;
    VkFenceCreateInfo fenceInfo {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (vkCreateFence(device,&fenceInfo,nullptr,&owned.fence)!=VK_SUCCESS) return 76;
    VkQueue queue=VK_NULL_HANDLE;
    vkGetDeviceQueue(device,family,0,&queue);
    auto barrier=[&](unsigned i,VkImageLayout oldLayout,VkImageLayout newLayout,VkAccessFlags src,VkAccessFlags dst) {
        VkImageMemoryBarrier b {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.oldLayout=oldLayout; b.newLayout=newLayout; b.srcAccessMask=src; b.dstAccessMask=dst;
        b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        b.image=owned.images[i]; b.subresourceRange={VkImageAspectFlags(i==2?VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT:VK_IMAGE_ASPECT_COLOR_BIT),0,1,0,1};
        vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,nullptr,0,nullptr,1,&b);
    };
    const unsigned testFrames=2;
    for (unsigned frame=0;frame<testFrames;++frame)
    {
        if (frame && vkResetCommandPool(device,owned.pool,0)!=VK_SUCCESS) return 77;
        VkCommandBufferBeginInfo begin {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        if (vkBeginCommandBuffer(cmd,&begin)!=VK_SUCCESS) return 78;
        if (!frame)
        {
            for (unsigned i=0;i<7;++i)
            {
                barrier(i,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_GENERAL,0,i==6?VK_ACCESS_SHADER_WRITE_BIT:VK_ACCESS_TRANSFER_WRITE_BIT);
                if (i==6) continue;
                if (i==2)
                {
                    VkClearDepthStencilValue clear {0.5f,0};
                    VkImageSubresourceRange range {VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT,0,1,0,1};
                    vkCmdClearDepthStencilImage(cmd,owned.images[i],VK_IMAGE_LAYOUT_GENERAL,&clear,1,&range);
                }
                else
                {
                    VkClearColorValue clear {};
                    if (i==0) {clear.float32[0]=0.25f;clear.float32[1]=0.5f;clear.float32[2]=0.75f;}
                    if (i==5) clear.float32[0]=1.0f;
                    VkImageSubresourceRange range {VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
                    vkCmdClearColorImage(cmd,owned.images[i],VK_IMAGE_LAYOUT_GENERAL,&clear,1,&range);
                }
                barrier(i,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT);
            }
        }
        ffxDispatchDescUpscale params {};
        params.header.type=FFX_API_DISPATCH_DESC_TYPE_UPSCALE; params.commandList=cmd;
        params.color=resources[0]; params.motionVectors=resources[1]; params.depth=resources[2];
        params.reactive=resources[3]; params.transparencyAndComposition=resources[4];
        params.exposure=resources[5]; params.output=resources[6];
        params.renderSize={64,64}; params.upscaleSize={128,128}; params.preExposure=1;
        params.motionVectorScale={-1,-1}; params.jitterOffset={frame?0.1875f:-0.0625f,frame?-0.129629f:-0.462963f};
        params.frameTimeDelta=25; params.cameraNear=40000; params.cameraFar=0.1f;
        params.cameraFovAngleVertical=0.837832f; params.viewSpaceToMetersFactor=1; params.reset=!frame;
        Bg3VulkanCommands::Tape recorded;
        ffxReturnCode_t dispatched;
        {Bg3VulkanCommands::Capture capture(cmd,recorded);dispatched=dispatch(context,&params.header);}
        if (!recorded.valid || recorded.calls.empty()) {printf("SDK transaction invalid/empty\n");return 83;}
        printf("SDK GPU transaction: %zu commands\n",recorded.calls.size());
        if (dispatched==FFX_API_RETURN_OK) recorded.Replay();
        printf("Native-format FSR frame %u dispatch: %u\n",frame,dispatched); fflush(stdout);
        if (dispatched!=FFX_API_RETURN_OK) return 79;
        barrier(6,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT);
        VkBufferImageCopy copy {}; copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; copy.imageExtent={128,128,1};
        vkCmdCopyImageToBuffer(cmd,owned.images[6],VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,owned.readback,1,&copy);
        barrier(6,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_GENERAL,VK_ACCESS_TRANSFER_READ_BIT,VK_ACCESS_SHADER_WRITE_BIT);
        if (vkEndCommandBuffer(cmd)!=VK_SUCCESS) return 80;
        VkSubmitInfo submit {VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.commandBufferCount=1; submit.pCommandBuffers=&cmd;
        if (vkResetFences(device,1,&owned.fence)!=VK_SUCCESS || vkQueueSubmit(queue,1,&submit,owned.fence)!=VK_SUCCESS) return 81;
        if (vkWaitForFences(device,1,&owned.fence,VK_TRUE,5000000000ULL)!=VK_SUCCESS) return 82;
    }
    void* data=nullptr;
    if (vkMapMemory(device,readbackMemory,0,VK_WHOLE_SIZE,0,&data)!=VK_SUCCESS) return 83;
    uint32_t pixel=static_cast<const uint32_t*>(data)[64*128+64];
    vkUnmapMemory(device,readbackMemory);
    auto unpack=[](uint32_t bits,unsigned mantissa) {
        unsigned exponent=(bits>>mantissa)&31u, fraction=bits&((1u<<mantissa)-1);
        return exponent?std::ldexp(1.0f+float(fraction)/(1u<<mantissa),int(exponent)-15):std::ldexp(float(fraction),1-15-int(mantissa));
    };
    float r=unpack(pixel&2047u,6),g=unpack((pixel>>11)&2047u,6),b=unpack(pixel>>22,5);
    printf("Native-format FSR GPU readback: %.4f %.4f %.4f (expected 0.25 0.5 0.75)\n",r,g,b);
    return std::abs(r-0.25f)<0.03f && std::abs(g-0.5f)<0.03f && std::abs(b-0.75f)<0.03f ? 0:84;
}
