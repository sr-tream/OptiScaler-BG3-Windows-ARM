#include <inputs/Bg3VulkanCommands.h>
#include <cassert>
#include <cstdio>
using namespace Bg3VulkanCommands;
static std::vector<unsigned> events;
static VkImageLayout observedLayout=VK_IMAGE_LAYOUT_UNDEFINED;
static uint32_t observedOffset=0,observedPush=0;
static VKAPI_ATTR void VKAPI_CALL Barrier(VkCommandBuffer,VkPipelineStageFlags,VkPipelineStageFlags,VkDependencyFlags,uint32_t,const VkMemoryBarrier*,uint32_t,const VkBufferMemoryBarrier*,uint32_t count,const VkImageMemoryBarrier* image)
{assert(count==1);observedLayout=image->newLayout;events.push_back(1);}
static VKAPI_ATTR void VKAPI_CALL Bind(VkCommandBuffer,VkPipelineBindPoint,VkPipelineLayout,uint32_t,uint32_t n,const VkDescriptorSet* sets,uint32_t d,const uint32_t* offsets)
{assert(n==1 && sets[0]==(VkDescriptorSet)0x123 && d==1);observedOffset=offsets[0];events.push_back(2);}
static VKAPI_ATTR void VKAPI_CALL PushValue(VkCommandBuffer,VkPipelineLayout,VkShaderStageFlags,uint32_t,uint32_t n,const void* data)
{assert(n==4);observedPush=*(const uint32_t*)data;events.push_back(3);}
static VKAPI_ATTR void VKAPI_CALL DispatchValue(VkCommandBuffer,uint32_t x,uint32_t,uint32_t)
{events.push_back(10+x);}
static VKAPI_ATTR void VKAPI_CALL Query(VkCommandBuffer,VkQueryPool,uint32_t,uint32_t) {events.push_back(4);}
static VKAPI_ATTR void VKAPI_CALL Viewport(VkCommandBuffer,uint32_t,uint32_t,const VkViewport*) {events.push_back(99);}
int main()
{
    auto cmd=(VkCommandBuffer)0x100;
    originalCmdPipelineBarrier=Barrier;originalCmdBindDescriptorSets=Bind;
    originalCmdPushConstants=PushValue;originalCmdDispatch=DispatchValue;
    originalCmdResetQueryPool=Query;originalCmdSetViewport=Viewport;
    Tape native;
    {
        Capture capture(cmd,native);
        VkImageMemoryBarrier barrier {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};barrier.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        CmdPipelineBarrier(cmd,1,2,0,0,nullptr,0,nullptr,1,&barrier);
        VkDescriptorSet set=(VkDescriptorSet)0x123;uint32_t offset=64,value=77;
        CmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,(VkPipelineLayout)0x200,1,1,&set,1,&offset);
        CmdPushConstants(cmd,(VkPipelineLayout)0x200,VK_SHADER_STAGE_COMPUTE_BIT,0,4,&value);
        for (unsigned pass=1;pass<=4;++pass) CmdDispatch(cmd,pass,1,1);
        CmdResetQueryPool(cmd,(VkQueryPool)0x300,0,2);
        barrier.newLayout=VK_IMAGE_LAYOUT_UNDEFINED;set=VK_NULL_HANDLE;offset=0;value=0;
    }
    assert(events.empty() && native.valid);
    native.Replay();assert((events==std::vector<unsigned>{1,2,3,11,12,13,14,4}));
    assert(observedLayout==VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL && observedOffset==64 && observedPush==77);
    // Successful replacement keeps native transitions/queries but removes all four dispatches and binds.
    events.clear();native.ReplaySynchronization();
    Tape sdk;{Capture capture(cmd,sdk);CmdDispatch(cmd,9,1,1);}
    sdk.Replay();assert((events==std::vector<unsigned>{1,4,19}));
    // A failed SDK recording performs no partial SDK dispatch before restoring the original order.
    events.clear();Tape failed;
    {Capture capture(cmd,failed);CmdDispatch(cmd,9,1,1);VkViewport viewport {};CmdSetViewport(cmd,0,1,&viewport);}
    assert(!failed.valid && events.empty());native.Replay();assert((events==std::vector<unsigned>{1,2,3,11,12,13,14,4}));
    // Plain calls still execute immediately outside a recording scope.
    events.clear();CmdDispatch(cmd,5,1,1);assert((events==std::vector<unsigned>{15}));
    puts("PASS: owned command arguments, four-pass removal with synchronization retained, failed SDK transaction discarded, exact native replay, ordinary forwarding.");
}
