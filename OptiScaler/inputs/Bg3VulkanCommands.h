#pragma once
#include <vulkan/vulkan.h>
#include <functional>
#include <string>
#include <vector>
#include <cstring>
#include <utility>

// A recording transaction, not a secondary command buffer: committing it does
// not invalidate the game's graphics state. Failed SDK recordings are discarded.
namespace Bg3VulkanCommands
{
enum class Kind { Binding, Dispatch, Barrier, Query, Transfer, Unsupported };
struct Call { Kind kind; std::function<void()> run; };
struct Tape
{
    std::vector<Call> calls;
    bool valid=true;
    Tape() { calls.reserve(64); }
    void Add(Kind kind,std::function<void()> run)
    {
        if (kind==Kind::Unsupported || calls.size()>=512) valid=false;
        calls.push_back({kind,std::move(run)});
    }
    void Replay() const { for (const auto& call:calls) call.run(); }
    void ReplaySynchronization() const
    {
        for (const auto& call:calls)
            if (call.kind==Kind::Barrier || call.kind==Kind::Query) call.run();
    }
};
inline thread_local Tape* sdkTape=nullptr;
inline thread_local VkCommandBuffer sdkCommand=VK_NULL_HANDLE;
inline bool (*nativeSink)(VkCommandBuffer,Kind,std::function<void()>)=nullptr;
inline bool (*nativeActive)(VkCommandBuffer)=nullptr;
inline bool Recording(VkCommandBuffer cmd)
{return (sdkTape && cmd==sdkCommand) || (nativeActive && nativeActive(cmd));}
struct Capture
{
    Capture(VkCommandBuffer cmd,Tape& tape) { sdkCommand=cmd;sdkTape=&tape; }
    ~Capture() { sdkTape=nullptr;sdkCommand=VK_NULL_HANDLE; }
};
template<class F> inline bool Record(VkCommandBuffer cmd,Kind kind,F&& call)
{
    if (sdkTape && cmd==sdkCommand) {sdkTape->Add(kind,std::forward<F>(call));return true;}
    return nativeActive && nativeActive(cmd) && nativeSink && nativeSink(cmd,kind,std::forward<F>(call));
}
template<class T> std::vector<T> Copy(uint32_t count,const T* data)
{return count?std::vector<T>(data,data+count):std::vector<T>{};}
template<class T> bool NoExtensions(const std::vector<T>& values)
{for (const auto& value:values) if (value.pNext) return false;return true;}

#define BG3_CAPTURE_FUNCTIONS(X) \
 X(CmdBindPipeline) X(CmdBindDescriptorSets) X(CmdPushConstants) X(CmdDispatch) X(CmdDispatchIndirect) X(CmdDispatchBase) \
 X(CmdPipelineBarrier) X(CmdPipelineBarrier2) X(CmdWriteTimestamp) X(CmdWriteTimestamp2) X(CmdResetQueryPool) \
 X(CmdCopyBuffer) X(CmdCopyBufferToImage) X(CmdCopyImageToBuffer) X(CmdCopyImage) X(CmdFillBuffer) X(CmdClearColorImage) \
 X(CmdBeginDebugUtilsLabelEXT) X(CmdEndDebugUtilsLabelEXT) X(CmdSetViewport) X(CmdSetScissor) \
 X(CmdBeginRenderPass) X(CmdEndRenderPass) X(CmdDraw)
#define BG3_DECLARE_CAPTURE(name) inline PFN_vk##name original##name=nullptr;
BG3_CAPTURE_FUNCTIONS(BG3_DECLARE_CAPTURE)
#undef BG3_DECLARE_CAPTURE

inline bool BindPipeline(VkCommandBuffer cmd,PFN_vkCmdBindPipeline fn,VkPipelineBindPoint point,VkPipeline pipeline)
{return Record(cmd,point==VK_PIPELINE_BIND_POINT_COMPUTE?Kind::Binding:Kind::Unsupported,[=]{fn(cmd,point,pipeline);});}
inline bool BindDescriptors(VkCommandBuffer cmd,PFN_vkCmdBindDescriptorSets fn,VkPipelineBindPoint point,VkPipelineLayout layout,uint32_t first,uint32_t count,const VkDescriptorSet* sets,uint32_t offsets,const uint32_t* dynamic)
{
    if (!Recording(cmd)) return false;
    auto s=Copy(count,sets);auto d=Copy(offsets,dynamic);
    return Record(cmd,point==VK_PIPELINE_BIND_POINT_COMPUTE?Kind::Binding:Kind::Unsupported,
        [=]{fn(cmd,point,layout,first,(uint32_t)s.size(),s.data(),(uint32_t)d.size(),d.data());});
}
inline bool Push(VkCommandBuffer cmd,PFN_vkCmdPushConstants fn,VkPipelineLayout layout,VkShaderStageFlags stages,uint32_t offset,uint32_t size,const void* data)
{
    if (!Recording(cmd)) return false;
    auto bytes=Copy(size,(const uint8_t*)data);
    return Record(cmd,Kind::Binding,[=]{fn(cmd,layout,stages,offset,(uint32_t)bytes.size(),bytes.data());});
}
inline VKAPI_ATTR void VKAPI_CALL CmdBindPipeline(VkCommandBuffer c,VkPipelineBindPoint p,VkPipeline v)
{if (!BindPipeline(c,originalCmdBindPipeline,p,v)) originalCmdBindPipeline(c,p,v);}
inline VKAPI_ATTR void VKAPI_CALL CmdBindDescriptorSets(VkCommandBuffer c,VkPipelineBindPoint p,VkPipelineLayout l,uint32_t f,uint32_t n,const VkDescriptorSet* s,uint32_t d,const uint32_t* o)
{if (!BindDescriptors(c,originalCmdBindDescriptorSets,p,l,f,n,s,d,o)) originalCmdBindDescriptorSets(c,p,l,f,n,s,d,o);}
inline VKAPI_ATTR void VKAPI_CALL CmdPushConstants(VkCommandBuffer c,VkPipelineLayout l,VkShaderStageFlags s,uint32_t o,uint32_t n,const void* v)
{if (!Push(c,originalCmdPushConstants,l,s,o,n,v)) originalCmdPushConstants(c,l,s,o,n,v);}
inline VKAPI_ATTR void VKAPI_CALL CmdDispatch(VkCommandBuffer c,uint32_t x,uint32_t y,uint32_t z)
{if (!Record(c,Kind::Dispatch,[=]{originalCmdDispatch(c,x,y,z);})) originalCmdDispatch(c,x,y,z);}
inline VKAPI_ATTR void VKAPI_CALL CmdDispatchIndirect(VkCommandBuffer c,VkBuffer b,VkDeviceSize o)
{if (!Record(c,Kind::Dispatch,[=]{originalCmdDispatchIndirect(c,b,o);})) originalCmdDispatchIndirect(c,b,o);}
inline VKAPI_ATTR void VKAPI_CALL CmdDispatchBase(VkCommandBuffer c,uint32_t bx,uint32_t by,uint32_t bz,uint32_t x,uint32_t y,uint32_t z)
{if (!Record(c,Kind::Dispatch,[=]{originalCmdDispatchBase(c,bx,by,bz,x,y,z);})) originalCmdDispatchBase(c,bx,by,bz,x,y,z);}
inline VKAPI_ATTR void VKAPI_CALL CmdPipelineBarrier(VkCommandBuffer c,VkPipelineStageFlags src,VkPipelineStageFlags dst,VkDependencyFlags flags,uint32_t mn,const VkMemoryBarrier* m,uint32_t bn,const VkBufferMemoryBarrier* b,uint32_t in,const VkImageMemoryBarrier* i)
{
    if (!Recording(c)) {originalCmdPipelineBarrier(c,src,dst,flags,mn,m,bn,b,in,i);return;}
    auto ms=Copy(mn,m);auto bs=Copy(bn,b);auto is=Copy(in,i);
    const auto kind=NoExtensions(ms)&&NoExtensions(bs)&&NoExtensions(is)?Kind::Barrier:Kind::Unsupported;
    if (!Record(c,kind,[=]{originalCmdPipelineBarrier(c,src,dst,flags,(uint32_t)ms.size(),ms.data(),(uint32_t)bs.size(),bs.data(),(uint32_t)is.size(),is.data());}))
        originalCmdPipelineBarrier(c,src,dst,flags,mn,m,bn,b,in,i);
}
inline VKAPI_ATTR void VKAPI_CALL CmdPipelineBarrier2(VkCommandBuffer c,const VkDependencyInfo* info)
{
    if (!Recording(c)) {originalCmdPipelineBarrier2(c,info);return;}
    auto ms=Copy(info->memoryBarrierCount,info->pMemoryBarriers);auto bs=Copy(info->bufferMemoryBarrierCount,info->pBufferMemoryBarriers);auto is=Copy(info->imageMemoryBarrierCount,info->pImageMemoryBarriers);
    VkDependencyInfo owned=*info;owned.pNext=nullptr;
    const auto kind=!info->pNext&&NoExtensions(ms)&&NoExtensions(bs)&&NoExtensions(is)?Kind::Barrier:Kind::Unsupported;
    if (!Record(c,kind,[=]() mutable {owned.pMemoryBarriers=ms.data();owned.pBufferMemoryBarriers=bs.data();owned.pImageMemoryBarriers=is.data();originalCmdPipelineBarrier2(c,&owned);})) originalCmdPipelineBarrier2(c,info);
}
inline VKAPI_ATTR void VKAPI_CALL CmdWriteTimestamp(VkCommandBuffer c,VkPipelineStageFlagBits s,VkQueryPool p,uint32_t q)
{if (!Record(c,Kind::Query,[=]{originalCmdWriteTimestamp(c,s,p,q);})) originalCmdWriteTimestamp(c,s,p,q);}
inline VKAPI_ATTR void VKAPI_CALL CmdWriteTimestamp2(VkCommandBuffer c,VkPipelineStageFlags2 s,VkQueryPool p,uint32_t q)
{if (!Record(c,Kind::Query,[=]{originalCmdWriteTimestamp2(c,s,p,q);})) originalCmdWriteTimestamp2(c,s,p,q);}
inline VKAPI_ATTR void VKAPI_CALL CmdResetQueryPool(VkCommandBuffer c,VkQueryPool p,uint32_t first,uint32_t n)
{if (!Record(c,Kind::Query,[=]{originalCmdResetQueryPool(c,p,first,n);})) originalCmdResetQueryPool(c,p,first,n);}
inline VKAPI_ATTR void VKAPI_CALL CmdCopyBuffer(VkCommandBuffer c,VkBuffer src,VkBuffer dst,uint32_t n,const VkBufferCopy* regions)
{if (!Recording(c)) {originalCmdCopyBuffer(c,src,dst,n,regions);return;}auto r=Copy(n,regions);if (!Record(c,Kind::Transfer,[=]{originalCmdCopyBuffer(c,src,dst,(uint32_t)r.size(),r.data());})) originalCmdCopyBuffer(c,src,dst,n,regions);}
inline VKAPI_ATTR void VKAPI_CALL CmdCopyBufferToImage(VkCommandBuffer c,VkBuffer src,VkImage dst,VkImageLayout l,uint32_t n,const VkBufferImageCopy* regions)
{if (!Recording(c)) {originalCmdCopyBufferToImage(c,src,dst,l,n,regions);return;}auto r=Copy(n,regions);if (!Record(c,Kind::Transfer,[=]{originalCmdCopyBufferToImage(c,src,dst,l,(uint32_t)r.size(),r.data());})) originalCmdCopyBufferToImage(c,src,dst,l,n,regions);}
inline VKAPI_ATTR void VKAPI_CALL CmdCopyImageToBuffer(VkCommandBuffer c,VkImage src,VkImageLayout l,VkBuffer dst,uint32_t n,const VkBufferImageCopy* regions)
{if (!Recording(c)) {originalCmdCopyImageToBuffer(c,src,l,dst,n,regions);return;}auto r=Copy(n,regions);if (!Record(c,Kind::Transfer,[=]{originalCmdCopyImageToBuffer(c,src,l,dst,(uint32_t)r.size(),r.data());})) originalCmdCopyImageToBuffer(c,src,l,dst,n,regions);}
inline VKAPI_ATTR void VKAPI_CALL CmdCopyImage(VkCommandBuffer c,VkImage src,VkImageLayout sl,VkImage dst,VkImageLayout dl,uint32_t n,const VkImageCopy* regions)
{if (!Recording(c)) {originalCmdCopyImage(c,src,sl,dst,dl,n,regions);return;}auto r=Copy(n,regions);if (!Record(c,Kind::Transfer,[=]{originalCmdCopyImage(c,src,sl,dst,dl,(uint32_t)r.size(),r.data());})) originalCmdCopyImage(c,src,sl,dst,dl,n,regions);}
inline VKAPI_ATTR void VKAPI_CALL CmdFillBuffer(VkCommandBuffer c,VkBuffer b,VkDeviceSize o,VkDeviceSize n,uint32_t value)
{if (!Record(c,Kind::Transfer,[=]{originalCmdFillBuffer(c,b,o,n,value);})) originalCmdFillBuffer(c,b,o,n,value);}
inline VKAPI_ATTR void VKAPI_CALL CmdClearColorImage(VkCommandBuffer c,VkImage i,VkImageLayout l,const VkClearColorValue* value,uint32_t n,const VkImageSubresourceRange* ranges)
{if (!Recording(c)) {originalCmdClearColorImage(c,i,l,value,n,ranges);return;}auto v=*value;auto r=Copy(n,ranges);if (!Record(c,Kind::Transfer,[=]{originalCmdClearColorImage(c,i,l,&v,(uint32_t)r.size(),r.data());})) originalCmdClearColorImage(c,i,l,value,n,ranges);}
inline VKAPI_ATTR void VKAPI_CALL CmdBeginDebugUtilsLabelEXT(VkCommandBuffer c,const VkDebugUtilsLabelEXT* label)
{
    std::string name=label->pLabelName?label->pLabelName:"";auto l=*label;l.pNext=nullptr;
    if (!Record(c,Kind::Query,[=]() mutable {l.pLabelName=name.c_str();originalCmdBeginDebugUtilsLabelEXT(c,&l);})) originalCmdBeginDebugUtilsLabelEXT(c,label);
}
inline VKAPI_ATTR void VKAPI_CALL CmdEndDebugUtilsLabelEXT(VkCommandBuffer c)
{if (!Record(c,Kind::Query,[=]{originalCmdEndDebugUtilsLabelEXT(c);})) originalCmdEndDebugUtilsLabelEXT(c);}
inline VKAPI_ATTR void VKAPI_CALL CmdSetViewport(VkCommandBuffer c,uint32_t first,uint32_t n,const VkViewport* values)
{if (!Recording(c)) {originalCmdSetViewport(c,first,n,values);return;}auto v=Copy(n,values);if (!Record(c,Kind::Unsupported,[=]{originalCmdSetViewport(c,first,(uint32_t)v.size(),v.data());})) originalCmdSetViewport(c,first,n,values);}
inline VKAPI_ATTR void VKAPI_CALL CmdSetScissor(VkCommandBuffer c,uint32_t first,uint32_t n,const VkRect2D* values)
{if (!Recording(c)) {originalCmdSetScissor(c,first,n,values);return;}auto v=Copy(n,values);if (!Record(c,Kind::Unsupported,[=]{originalCmdSetScissor(c,first,(uint32_t)v.size(),v.data());})) originalCmdSetScissor(c,first,n,values);}
inline VKAPI_ATTR void VKAPI_CALL CmdBeginRenderPass(VkCommandBuffer c,const VkRenderPassBeginInfo* info,VkSubpassContents contents)
{if (!Record(c,Kind::Unsupported,[=]{originalCmdBeginRenderPass(c,info,contents);})) originalCmdBeginRenderPass(c,info,contents);}
inline VKAPI_ATTR void VKAPI_CALL CmdEndRenderPass(VkCommandBuffer c)
{if (!Record(c,Kind::Unsupported,[=]{originalCmdEndRenderPass(c);})) originalCmdEndRenderPass(c);}
inline VKAPI_ATTR void VKAPI_CALL CmdDraw(VkCommandBuffer c,uint32_t vertices,uint32_t instances,uint32_t firstVertex,uint32_t firstInstance)
{if (!Record(c,Kind::Unsupported,[=]{originalCmdDraw(c,vertices,instances,firstVertex,firstInstance);})) originalCmdDraw(c,vertices,instances,firstVertex,firstInstance);}

inline PFN_vkVoidFunction Resolve(const char* name,PFN_vkVoidFunction original)
{
    if (!original) return nullptr;
    const char* alias=name;
    if (!strcmp(name,"vkCmdPipelineBarrier2KHR")) alias="vkCmdPipelineBarrier2";
    if (!strcmp(name,"vkCmdWriteTimestamp2KHR")) alias="vkCmdWriteTimestamp2";
    if (!strcmp(name,"vkCmdDispatchBaseKHR")) alias="vkCmdDispatchBase";
#define BG3_RESOLVE_CAPTURE(function) if (!strcmp(alias,"vk" #function)) {if (!original##function) original##function=(PFN_vk##function)original;return (PFN_vkVoidFunction)function;}
    BG3_CAPTURE_FUNCTIONS(BG3_RESOLVE_CAPTURE)
#undef BG3_RESOLVE_CAPTURE
    return nullptr;
}
}
