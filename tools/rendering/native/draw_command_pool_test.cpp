#include "renderer/native/draw_command_pool.h"
#include <array>
#include <iostream>
#include <type_traits>
using namespace gow3::renderer;
namespace {
void Require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
template<class T>T Handle(uintptr_t value) {
    if constexpr(std::is_pointer_v<T>)return reinterpret_cast<T>(value);
    else return static_cast<T>(value);
}
const VkDevice device=Handle<VkDevice>(1);
const VkCommandPool pool=Handle<VkCommandPool>(2);
const VkCommandBuffer commands=Handle<VkCommandBuffer>(3);
std::array<char,2> calls{};uint32_t count=0;
VkResult reset_result=VK_SUCCESS,begin_result=VK_SUCCESS;
VKAPI_ATTR VkResult VKAPI_CALL Reset(VkDevice d,VkCommandPool p,VkCommandPoolResetFlags flags) {
    Require(d==device&&p==pool&&flags==0,"Pool reset changed device/pool or freed driver allocations");
    Require(count<calls.size(),"Extra driver operation");calls[count++]='R';return reset_result;
}
VKAPI_ATTR VkResult VKAPI_CALL Begin(VkCommandBuffer c,const VkCommandBufferBeginInfo* info) {
    Require(c==commands&&info&&info->sType==VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO&&
        !info->pNext&&!info->pInheritanceInfo&&info->flags==VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        "Draw begin changed existing one-time primary buffer flags");
    Require(count<calls.size(),"Extra driver operation");calls[count++]='B';return begin_result;
}
template<class F>void Reject(F&& f) {
    bool rejected=false;try{f();}catch(const std::exception&){rejected=true;}
    Require(rejected,"Failed or malformed draw pool begin was accepted");
}
}
int main()try {
    NativeDrawRecordingStatistics stats;
    for(unsigned n=0;n<1000;++n) {
        count=0;BeginRetiredNativeDrawPool(device,pool,commands,Reset,Begin,&stats);
        Require(count==2&&calls==std::array<char,2>{'R','B'},"Pool reset/begin order changed");
    }
    Require(stats.pool_resets==1000&&stats.pool_reset_ms==0,"Disabled timing read a clock or lost resets");
    count=0;reset_result=VK_ERROR_DEVICE_LOST;
    Reject([&]{BeginRetiredNativeDrawPool(device,pool,commands,Reset,Begin,&stats);});
    Require(count==1&&stats.pool_resets==1000,"Failed reset proceeded to begin or committed success");
    count=0;reset_result=VK_SUCCESS;begin_result=VK_ERROR_OUT_OF_HOST_MEMORY;
    Reject([&]{BeginRetiredNativeDrawPool(device,pool,commands,Reset,Begin,&stats);});
    Require(count==2&&stats.pool_resets==1001,"Begin failure lost the completed reset");
    count=0;begin_result=VK_SUCCESS;
    Reject([&]{BeginRetiredNativeDrawPool({},pool,commands,Reset,Begin);});
    Reject([&]{BeginRetiredNativeDrawPool(device,{},commands,Reset,Begin);});
    Reject([&]{BeginRetiredNativeDrawPool(device,pool,{},Reset,Begin);});
    Reject([&]{BeginRetiredNativeDrawPool(device,pool,commands,nullptr,Begin);});
    Reject([&]{BeginRetiredNativeDrawPool(device,pool,commands,Reset,nullptr);});
    Require(count==0,"Invalid handles reached driver functions");
    BeginRetiredNativeDrawPool(device,pool,commands,Reset,Begin,&stats,true);
    Require(count==2&&stats.pool_resets==1002&&stats.pool_reset_ms>=0,"Optional reset timing broke operation order");
    std::cout<<"PASS: 1000 private-pool reset/begin sequences, exact flags, failure short-circuit, "
        "null handle/function rejection and optional timing; real retirement proof is in Scene GPU fixture\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
