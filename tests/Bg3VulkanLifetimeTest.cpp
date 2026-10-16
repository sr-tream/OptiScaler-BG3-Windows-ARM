#include <inputs/Bg3VulkanLifetime.h>
#include <cassert>
#include <cstdio>
#include <functional>

static std::unordered_map<VkFence,VkResult> fences;
static std::vector<VkFence> submissions;
static unsigned created=0,destroyed=0;
static VkResult createResult=VK_SUCCESS,submitResult=VK_SUCCESS;
static VKAPI_ATTR VkResult VKAPI_CALL Create(VkDevice,const VkFenceCreateInfo* info,const VkAllocationCallbacks*,VkFence* fence)
{
    assert(info->flags==0);
    if (createResult!=VK_SUCCESS) return createResult;
    *fence=(VkFence)(uintptr_t)(++created);fences[*fence]=VK_NOT_READY;return VK_SUCCESS;
}
static VKAPI_ATTR VkResult VKAPI_CALL Status(VkDevice,VkFence fence) {return fences.at(fence);}
static VKAPI_ATTR VkResult VKAPI_CALL Reset(VkDevice,uint32_t count,const VkFence* list)
{
    assert(count==1);assert(fences.at(*list)==VK_SUCCESS);fences[*list]=VK_NOT_READY;return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL Destroy(VkDevice,VkFence fence,const VkAllocationCallbacks*)
{assert(fences.erase(fence)==1);++destroyed;}
static VKAPI_ATTR VkResult VKAPI_CALL Submit(VkQueue,uint32_t count,const VkSubmitInfo* info,VkFence fence)
{
    assert(count==1 && info->commandBufferCount==0 && info->waitSemaphoreCount==0 && info->signalSemaphoreCount==0);
    submissions.push_back(fence);return submitResult;
}
struct Owner
{
    std::function<void()> release;
    ~Owner() {if (release) release();}
};
int main()
{
    auto device=(VkDevice)0x100,otherDevice=(VkDevice)0x200;
    auto cmd=(VkCommandBuffer)0x300,otherCmd=(VkCommandBuffer)0x400;
    auto queue=(VkQueue)0x500;
    Bg3VulkanLifetime lifetime({Create,Status,Reset,Destroy});

    unsigned released=0;
    auto context=std::make_shared<Owner>();
    context->release=[&]{++released;lifetime.ResetRecording(otherCmd);};
    std::weak_ptr<Owner> weak=context;
    lifetime.Record(device,cmd,context);
    lifetime.Record(device,cmd,context); // Deduplicated even if init and evaluation both record it.
    assert(lifetime.Submitted(queue,{cmd},Submit)==VK_SUCCESS);
    auto first=submissions.back();
    assert(lifetime.Submitted(queue,{cmd},Submit)==VK_SUCCESS); // Resubmitted command buffer.
    auto second=submissions.back();
    context.reset();lifetime.ResetRecording(cmd);lifetime.Poll();
    assert(!weak.expired() && released==0);
    fences[first]=VK_SUCCESS;lifetime.Poll();
    assert(!weak.expired());
    fences[second]=VK_SUCCESS;lifetime.Poll();
    assert(weak.expired() && released==1); // Destructor reenters the registry, outside its lock.

    auto secondaryContext=std::make_shared<Owner>();std::weak_ptr<Owner> secondaryWeak=secondaryContext;
    lifetime.Record(device,otherCmd,secondaryContext);secondaryContext.reset();
    lifetime.ExecuteCommands(cmd,1,&otherCmd);
    assert(lifetime.Submitted(queue,{cmd},Submit)==VK_SUCCESS);
    auto secondaryFence=submissions.back();
    lifetime.ResetRecording(otherCmd);lifetime.ResetRecording(cmd);lifetime.Poll();
    assert(!secondaryWeak.expired());
    fences[secondaryFence]=VK_SUCCESS;lifetime.Poll();assert(secondaryWeak.expired());

    auto next=std::make_shared<Owner>();std::weak_ptr<Owner> nextWeak=next;
    lifetime.Record(device,cmd,next);next.reset();
    assert(lifetime.Submitted(queue,{cmd},Submit)==VK_SUCCESS);
    assert(created==2); // Uses a recycled, reset fence.
    auto recycled=submissions.back();lifetime.ResetRecording(cmd);
    fences[recycled]=VK_SUCCESS;lifetime.Poll();assert(nextWeak.expired());

    auto cancelled=std::make_shared<Owner>();std::weak_ptr<Owner> cancelledWeak=cancelled;
    lifetime.Record(device,cmd,cancelled);cancelled.reset();lifetime.ResetRecording(cmd);
    assert(cancelledWeak.expired()); // Discarded before submission.

    auto failed=std::make_shared<Owner>();std::weak_ptr<Owner> failedWeak=failed;
    lifetime.Record(device,cmd,failed);failed.reset();submitResult=VK_ERROR_DEVICE_LOST;
    assert(lifetime.Submitted(queue,{cmd},Submit)==VK_ERROR_DEVICE_LOST);
    lifetime.ResetRecording(cmd);lifetime.Poll();assert(!failedWeak.expired());
    submitResult=VK_SUCCESS;

    auto noFence=std::make_shared<Owner>();std::weak_ptr<Owner> noFenceWeak=noFence;
    lifetime.Record(otherDevice,otherCmd,noFence);noFence.reset();createResult=VK_ERROR_OUT_OF_DEVICE_MEMORY;
    assert(lifetime.Submitted(queue,{otherCmd},Submit)==VK_ERROR_OUT_OF_DEVICE_MEMORY);
    lifetime.ResetRecording(otherCmd);lifetime.Poll();assert(!noFenceWeak.expired());
    createResult=VK_SUCCESS;
    lifetime.DestroyDevice(device);assert(failedWeak.expired() && !noFenceWeak.expired());
    lifetime.DestroyDevice(otherDevice);assert(noFenceWeak.expired());
    assert(fences.empty() && destroyed==created);
    puts("PASS: context lifetime across queued/resubmitted/discarded commands; nonblocking retirement, fence reuse, failed markers, per-device teardown.");
}
