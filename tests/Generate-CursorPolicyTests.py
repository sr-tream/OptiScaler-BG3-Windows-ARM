from pathlib import Path
import subprocess
repo=Path(__file__).resolve().parents[1]
root=repo/'ARM64EC'/'CursorPolicyTests'
root.mkdir(parents=True,exist_ok=True)
source=(repo/'OptiScaler/menu/input/input_system_cursor.cpp').read_text()
original=subprocess.check_output(['git','-C',str(repo),'-c','core.excludesFile=','show','8e9e6f99b45801fda82be40ab18e134838aa5f72:OptiScaler/menu/input/input_system_cursor.cpp'],text=True)
def helpers(text):
    return text[text.index('void BeginCursorClipBlockLocked()'):text.index('void StoreDeferredClipCursorLocked')]
header=r'''
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>
#define LOG_INFO(...) ((void)0)
#define LOG_DEBUG(...) ((void)0)
struct State {
 HWND InputHwnd=(HWND)0x123,TargetHwnd=(HWND)0x123;
 RECT SavedClipRect{},DeferredClipRect{};
 bool HasSavedClipRect=false,SavedClipWasActive=false,HasDeferredClipRect=false,DeferredClipIsNull=false;
 bool CursorClipReleasedForMenu=false,CursorClipApplyPending=false,CursorClipApplied=false,CursorClipMessagePosted=false;
 uint64_t CursorClipGeneration=0;
 std::recursive_mutex Mutex;
} _state;
static unsigned violations=0,getCalls=0,clipCalls=0;
static RECT screen{0,0,1920,1080},nativeClip{10,20,100,200},appliedRect{};
static bool appliedNull=false;
static std::function<void()> onGet,onClip;
static std::vector<UINT> messages;
static void CheckUnlocked() {
 bool unlocked=false;
 std::thread other([&]{unlocked=_state.Mutex.try_lock();if(unlocked)_state.Mutex.unlock();});other.join();
 if(!unlocked)++violations;
}
static BOOL WINAPI Get(LPRECT out) {CheckUnlocked();++getCalls;*out=nativeClip;if(onGet)onGet();return TRUE;}
static BOOL WINAPI Clip(const RECT* rect) {CheckUnlocked();++clipCalls;appliedNull=!rect;if(rect)appliedRect=*rect;if(onClip)onClip();return TRUE;}
static BOOL WINAPI Post(HWND,UINT msg,WPARAM,LPARAM) {messages.push_back(msg);return TRUE;}
static auto o_GetClipCursor=Get;
static auto o_ClipCursor=Clip;
static auto o_PostMessageW=Post;
static bool Same(const RECT& a,const RECT& b) {return a.left==b.left && a.top==b.top && a.right==b.right && a.bottom==b.bottom;}
static bool IsVirtualScreenRect(const RECT& rect) {return Same(rect,screen);}
'''
baseline=r'''
int main(){
 {std::unique_lock lock(_state.Mutex);BeginCursorClipBlockLocked();EndCursorClipBlockLocked();}
 if(violations<3)return 1;
 printf("PASS baseline reproduction: %u cursor API calls require another thread while input state is held.\n",violations);
}
'''
test=r'''
static void Pump(){
 unsigned count=0;
 while(!messages.empty()){auto msg=messages.front();messages.erase(messages.begin());assert(ProcessCursorClipMessage(_state.InputHwnd,msg));assert(++count<10);}
}
int main(){
 {std::unique_lock lock(_state.Mutex);BeginCursorClipBlockLocked();}
 assert(getCalls==0 && clipCalls==0);
 ScheduleCursorClipPolicy();assert(getCalls==0 && clipCalls==0 && messages.size()==1);
 Pump();assert(getCalls==1 && clipCalls==1 && appliedNull && _state.HasSavedClipRect);
 {std::unique_lock lock(_state.Mutex);EndCursorClipBlockLocked();}
 ScheduleCursorClipPolicy();Pump();assert(clipCalls==2 && !appliedNull && Same(appliedRect,nativeClip));
 assert(!_state.HasSavedClipRect && !_state.CursorClipApplied);
 // A close before the window processed the open must not alter its existing confinement.
 const auto before=clipCalls;
 {std::unique_lock lock(_state.Mutex);BeginCursorClipBlockLocked();EndCursorClipBlockLocked();}
 ScheduleCursorClipPolicy();Pump();assert(clipCalls==before);
 // Restore the last game request which was deferred while the menu owned the cursor.
 {std::unique_lock lock(_state.Mutex);BeginCursorClipBlockLocked();}
 ScheduleCursorClipPolicy();Pump();
 RECT deferred{30,40,300,400};
 {std::unique_lock lock(_state.Mutex);_state.DeferredClipRect=deferred;_state.HasDeferredClipRect=true;EndCursorClipBlockLocked();}
 ScheduleCursorClipPolicy();Pump();assert(!appliedNull && Same(appliedRect,deferred));
 // A focus transition from a cursor callback supersedes the in-flight policy.
 onClip=[] {onClip=nullptr;std::unique_lock lock(_state.Mutex);EndCursorClipBlockLocked();};
 {std::unique_lock lock(_state.Mutex);BeginCursorClipBlockLocked();}
 ScheduleCursorClipPolicy();Pump();assert(!_state.CursorClipApplied && !_state.HasSavedClipRect && !appliedNull && Same(appliedRect,nativeClip));
 // Capturing the old clip must not commit it to a policy cancelled by the callback.
 onGet=[] {onGet=nullptr;std::unique_lock lock(_state.Mutex);EndCursorClipBlockLocked();};
 const auto prior=clipCalls;
 {std::unique_lock lock(_state.Mutex);BeginCursorClipBlockLocked();}
 ScheduleCursorClipPolicy();Pump();assert(clipCalls==prior && !_state.HasSavedClipRect);
 assert(violations==0);
 puts("PASS: cursor work deferred to window messages; APIs run outside input mutex; saved/deferred confinement, focus roundtrips, cancelled and superseded policies.");
}
'''
(root/'CursorFocusBaseline.cpp').write_text(header+helpers(original)+baseline)
(root/'CursorFocusProbe.cpp').write_text(header+helpers(source)+test)
print('Generated baseline and current cursor-policy probes from product sources')
