#pragma once
#include <vulkan/vulkan.h>
#include <algorithm>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

// Recorded command buffers and private submission fences retain context owners.
// Polling never waits on the GPU or requires the game to reach Present.
class Bg3VulkanLifetime
{
  public:
    struct Functions
    {
        PFN_vkCreateFence create;
        PFN_vkGetFenceStatus status;
        PFN_vkResetFences reset;
        PFN_vkDestroyFence destroy;
    };
  private:
    struct Recording { VkDevice device; std::vector<std::shared_ptr<void>> owners; };
    struct Pending { VkDevice device; VkFence fence; std::vector<std::shared_ptr<void>> owners; };
    Functions functions;
    std::mutex mutex;
    std::unordered_map<VkCommandBuffer,Recording> recordings;
    std::vector<Pending> pending;
    std::unordered_map<VkDevice,std::vector<VkFence>> available;

  public:
    explicit Bg3VulkanLifetime(Functions f) : functions(f) {}
    void Record(VkDevice device,VkCommandBuffer cmd,std::shared_ptr<void> owner)
    {
        std::scoped_lock lock(mutex);
        auto& record=recordings[cmd]; record.device=device;
        if (std::find(record.owners.begin(),record.owners.end(),owner)==record.owners.end())
            record.owners.push_back(std::move(owner));
    }
    void ResetRecording(VkCommandBuffer cmd)
    {
        Recording released {};
        { std::scoped_lock lock(mutex);
          if (auto it=recordings.find(cmd);it!=recordings.end()) {released=std::move(it->second);recordings.erase(it);} }
        // Destructors can call Vulkan. Run them outside the registry lock.
    }
    void ExecuteCommands(VkCommandBuffer primary,uint32_t count,const VkCommandBuffer* secondary)
    {
        std::scoped_lock lock(mutex);
        Recording inherited {};
        for (uint32_t i=0;i<count;++i)
            if (auto record=recordings.find(secondary[i]);record!=recordings.end())
            {
                inherited.device=record->second.device;
                for (const auto& owner:record->second.owners)
                    if (std::find(inherited.owners.begin(),inherited.owners.end(),owner)==inherited.owners.end())
                        inherited.owners.push_back(owner);
            }
        if (inherited.owners.empty()) return;
        auto& record=recordings[primary];record.device=inherited.device;
        for (auto& owner:inherited.owners)
            if (std::find(record.owners.begin(),record.owners.end(),owner)==record.owners.end())
                record.owners.push_back(std::move(owner));
    }
    VkResult Submitted(VkQueue queue,const std::vector<VkCommandBuffer>& commands,PFN_vkQueueSubmit submit)
    {
        Pending ticket {};
        {
            std::scoped_lock lock(mutex);
            for (auto cmd:commands)
                if (auto record=recordings.find(cmd);record!=recordings.end())
                {
                    if (ticket.device && ticket.device!=record->second.device) return VK_ERROR_INITIALIZATION_FAILED;
                    ticket.device=record->second.device;
                    for (const auto& owner:record->second.owners)
                        if (std::find(ticket.owners.begin(),ticket.owners.end(),owner)==ticket.owners.end()) ticket.owners.push_back(owner);
                }
            if (ticket.owners.empty()) return VK_SUCCESS;
            auto& free=available[ticket.device];
            if (!free.empty()) {ticket.fence=free.back();free.pop_back();}
        }
        VkResult result=VK_SUCCESS;
        if (!ticket.fence)
        {
            VkFenceCreateInfo info {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            result=functions.create(ticket.device,&info,nullptr,&ticket.fence);
        }
        if (result==VK_SUCCESS)
        {
            // Executed inside the application's queue-submit call, while its queue synchronization is held.
            VkSubmitInfo marker {VK_STRUCTURE_TYPE_SUBMIT_INFO};
            result=submit(queue,1,&marker,ticket.fence);
        }
        if (result!=VK_SUCCESS)
        {
            if (ticket.fence) functions.destroy(ticket.device,ticket.fence,nullptr);
            ticket.fence=VK_NULL_HANDLE; // Keep resources pinned until device teardown on a marker failure.
        }
        { std::scoped_lock lock(mutex);pending.push_back(std::move(ticket)); }
        return result;
    }
    void Poll()
    {
        std::vector<Pending> released;
        {
            std::scoped_lock lock(mutex);
            for (auto it=pending.begin();it!=pending.end();)
            {
                if (it->fence && functions.status(it->device,it->fence)==VK_SUCCESS)
                {
                    if (functions.reset(it->device,1,&it->fence)==VK_SUCCESS) available[it->device].push_back(it->fence);
                    else functions.destroy(it->device,it->fence,nullptr);
                    released.push_back(std::move(*it)); it=pending.erase(it);
                }
                else ++it;
            }
        }
    }
    void DestroyDevice(VkDevice device)
    {
        std::vector<Recording> releasedRecordings;
        std::vector<Pending> releasedTickets;
        {
            std::scoped_lock lock(mutex);
            for (auto it=recordings.begin();it!=recordings.end();)
                if (it->second.device==device) {releasedRecordings.push_back(std::move(it->second));it=recordings.erase(it);} else ++it;
            for (auto it=pending.begin();it!=pending.end();)
                if (it->device==device)
                {
                    if (it->fence) functions.destroy(device,it->fence,nullptr);
                    releasedTickets.push_back(std::move(*it)); it=pending.erase(it);
                }
                else ++it;
            if (auto free=available.find(device);free!=available.end())
            {for (auto fence:free->second) functions.destroy(device,fence,nullptr);available.erase(free);}
        }
    }
};
