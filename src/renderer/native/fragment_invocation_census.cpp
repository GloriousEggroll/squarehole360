#include "fragment_invocation_census.h"
#include "plume_vulkan.h"
#include <algorithm>

namespace gow3::renderer {
namespace {
using Json=nlohmann::json;
Json KeyJson(const NativeFragmentInvocationCensus::Key& k) {
    return {{"vertex",k.vertex},{"pixel",k.pixel},{"raster",k.raster},
        {"vertex_variant",k.vertex_variant},{"pixel_variant",k.pixel_variant},
        {"depth_control",k.depth_control},{"color_mask",k.color_mask},{"blend_mask",k.blend_mask},
        {"pitch",k.pitch},{"guest_depth_base",k.guest_depth_base},{"guest_depth_format",k.guest_depth_format},
        {"guest_color_bases",k.guest_color_bases},{"guest_color_formats",k.guest_color_formats},
        {"guest_color_masks",k.guest_color_masks},{"guest_blend_controls",k.guest_blend_controls},
        {"width",k.width},{"height",k.height},{"color_format",k.color_format},{"depth_format",k.depth_format},
        {"queried",k.queried},{"hardware_depth",k.hardware_depth},
        {"hardware_depth_write",k.hardware_depth_write},{"hardware_depth_compare",k.hardware_depth_compare}};
}
}
struct NativeFragmentInvocationCensus::State {
    VkDevice device=VK_NULL_HANDLE;VkQueryPool pool=VK_NULL_HANDLE;PFN_vkResetQueryPool reset=nullptr;
    plume::RenderCommandQueue* queue=nullptr;uint32_t capacity=0;
    std::unique_ptr<NativeFragmentInvocationLedger> ledger;
    std::array<VkCommandBuffer,NativeFragmentInvocationLedger::kSlots> recording{};
    std::vector<Key> keys;
    bool statistics_enabled=false,host_reset_enabled=false,disabled=false;
    VkPhysicalDeviceProperties properties{};
    uint64_t failures=0,host_resets=0,query_reads=0,discarded=0,budget_skipped=0;
    const char* status="unavailable";const char* failure=nullptr;
    void Disable(const char* why) noexcept {
        disabled=true;if(failures!=UINT64_MAX)++failures;if(!failure)failure=why;
    }
    ~State() {
        // Scene's existing destructor drains accepted work before destroying
        // this pool. Diagnostic failure never releases rendering ownership.
        if(pool)vkDestroyQueryPool(device,pool,nullptr);
    }
};
NativeFragmentInvocationCensus::NativeFragmentInvocationCensus(plume::RenderDevice& device,
    plume::RenderCommandQueue& queue,uint32_t capacity,bool statistics_enabled):state_(std::make_unique<State>()) {
    auto& s=*state_;s.capacity=capacity;s.statistics_enabled=statistics_enabled;
    if(capacity!=64&&capacity!=128)throw std::invalid_argument("Fragment census capacity must be64 or128");
    auto* vk=dynamic_cast<plume::VulkanDevice*>(&device);
    auto* q=dynamic_cast<plume::VulkanCommandQueue*>(&queue);
    if(!vk||!q||q->device!=vk||!vk->vk) {s.status="unsupported: foreign Vulkan queue/device";return;}
    s.properties=vk->physicalDeviceProperties;
    if(s.properties.deviceType==VK_PHYSICAL_DEVICE_TYPE_CPU) {s.status="unsupported: hardware GPU required for this invocation census";return;}
    if(!statistics_enabled) {s.status="unsupported: pipelineStatisticsQuery not enabled by owned replay device";return;}
    s.host_reset_enabled=vk->hostQueryResetEnabled;
    if(!s.host_reset_enabled) {s.status="unsupported: hostQueryReset not enabled; pass boundaries left unchanged";return;}
    s.reset=reinterpret_cast<PFN_vkResetQueryPool>(vkGetDeviceProcAddr(vk->vk,"vkResetQueryPool"));
    if(!s.reset) {s.status="unsupported: enabled host query reset has no callable entry point";return;}
    s.device=vk->vk;s.queue=&queue;s.keys.reserve(NativeFragmentInvocationLedger::kKeys);
    s.ledger=std::make_unique<NativeFragmentInvocationLedger>(capacity);
    VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    info.queryType=VK_QUERY_TYPE_PIPELINE_STATISTICS;info.queryCount=capacity;
    info.pipelineStatistics=VK_QUERY_PIPELINE_STATISTIC_FRAGMENT_SHADER_INVOCATIONS_BIT;
    if(vkCreateQueryPool(s.device,&info,nullptr,&s.pool)!=VK_SUCCESS) {
        s.ledger.reset();s.status="unsupported: fragment statistics pool allocation failed";return;
    }
    s.status="enabled";
}
NativeFragmentInvocationCensus::~NativeFragmentInvocationCensus()=default;
uint32_t NativeFragmentInvocationCensus::Intern(Key key) noexcept {
    auto& s=*state_;if(!s.pool||s.disabled)return 0;
    try {
        if(key.vertex.size()>64||key.pixel.size()>64)throw std::length_error("Unbounded census source identity");
        const auto i=std::find(s.keys.begin(),s.keys.end(),key);
        if(i!=s.keys.end())return uint32_t(i-s.keys.begin()+1);
        if(s.keys.size()==NativeFragmentInvocationLedger::kKeys)throw std::length_error("Census key budget exhausted");
        s.keys.push_back(std::move(key));return uint32_t(s.keys.size());
    } catch(...) {s.Disable("source/state key invalid or bounded key allocation failed");return 0;}
}
bool NativeFragmentInvocationCensus::Begin(Token token,Tag tag,plume::RenderCommandList& commands) noexcept {
    auto& s=*state_;if(!s.pool||s.disabled)return false;
    try {
        if(!s.ledger->BudgetAvailable()) {s.budget_skipped=NativeFragmentInvocationLedger::Add(s.budget_skipped,1);return false;}
        auto* vk=dynamic_cast<plume::VulkanCommandList*>(&commands);
        if(!vk||vk->queue!=s.queue||!vk->vk)throw std::logic_error("Foreign census command list");
        if(!tag.key||tag.key>s.keys.size())throw std::logic_error("Unknown census key");
        // Plume normally starts this same pass inside drawIndexedInstanced.
        // Begin/end both belong inside it, also when no guest occlusion query
        // has already started the pass. Existing shared passes stay open.
        vk->checkActiveRenderPass();
        const auto resets=NativeFragmentInvocationLedger::Add(s.host_resets,1);
        s.ledger->Begin(token,tag); // Proves this exact physical slot retired.
        // Host reset touches only this separate diagnostic pool. It works while
        // the command list has an open shared pass, with no render-pass break.
        s.reset(s.device,s.pool,token.slot,1);s.host_resets=resets;
        vkCmdBeginQuery(vk->vk,s.pool,token.slot,0);s.recording[token.slot]=vk->vk;
        return true;
    } catch(...) {s.Disable("begin ownership, metadata or counter failure");return false;}
}
void NativeFragmentInvocationCensus::End(Token token) noexcept {
    auto& s=*state_;
    if(token.slot>=s.capacity||!s.recording[token.slot]) {s.Disable("unmatched statistics query end");return;}
    // Validate before touching commands: a stale token must never close a
    // newer owner's query. Still balance correct owners after other failures.
    try {s.ledger->End(token);}catch(...) {s.Disable("end ownership failure");return;}
    vkCmdEndQuery(s.recording[token.slot],s.pool,token.slot);s.recording[token.slot]=VK_NULL_HANDLE;
}
void NativeFragmentInvocationCensus::Accept(Token token) noexcept {
    try {state_->ledger->Accept(token);}catch(...) {state_->Disable("accepted statistics token mismatch");}
}
void NativeFragmentInvocationCensus::Cancel(Token token) noexcept {
    auto& s=*state_;
    if(token.slot<s.capacity&&s.recording[token.slot])End(token);
    try {s.ledger->Cancel(token);}catch(...) {s.Disable("cancelled statistics token mismatch");}
    // A failed preparing draw may leave commands in an unsubmitted buffer.
    // Freeze all query IDs until the Scene's existing final drain/destruction.
    s.Disable("unaccepted draw cancelled; diagnostic pool will not be reused");
}
void NativeFragmentInvocationCensus::RejectMetadata() noexcept {state_->Disable("draw metadata unavailable");}
void NativeFragmentInvocationCensus::Discarded() noexcept {
    try {state_->discarded=NativeFragmentInvocationLedger::Add(state_->discarded,1);}
    catch(...) {state_->Disable("discarded draw counter overflow");}
}
void NativeFragmentInvocationCensus::Retire(std::span<const Token> tokens,uint64_t submission) noexcept {
    auto& s=*state_;if(!s.ledger||!s.pool)return;
    try {
        s.ledger->ValidateRetirement(tokens); // Validate whole real fence first.
        for(auto token:tokens)if(s.ledger->Pending(token)) {
            struct Value {uint64_t count,available;} value{};
            const auto reads=NativeFragmentInvocationLedger::Add(s.query_reads,1);
            // No WAIT/PARTIAL flag and no polling. This draw's existing queue
            // fence has already completed; never read a newer submission.
            const auto result=vkGetQueryPoolResults(s.device,s.pool,token.slot,1,sizeof(value),&value,sizeof(value),
                VK_QUERY_RESULT_64_BIT|VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
            s.query_reads=reads;
            const bool available=result==VK_SUCCESS&&value.available;
            s.ledger->Retire(token,submission,available?std::optional<uint64_t>(value.count):std::nullopt);
            if(!available)s.Disable("statistics result unavailable/error after its real fence");
        }
    } catch(...) {s.Disable("fenced statistics ownership or counter failure");}
}
Json NativeFragmentInvocationCensus::Report() const {
    const auto& s=*state_;
    const NativeFragmentInvocationLedger::Totals empty;
    const auto& t=s.ledger?s.ledger->totals():empty;
    Json out{{"enabled",true},{"supported",bool(s.pool)},{"status",s.disabled?"disabled after diagnostic failure":s.status},
        {"failure",s.failure?Json(s.failure):Json(nullptr)},{"failures",s.failures},
        {"pipeline_statistics_enabled",s.statistics_enabled},{"host_query_reset_enabled",s.host_reset_enabled},
        {"device",{{"name",s.properties.deviceName},{"vendor_id",s.properties.vendorID},{"device_id",s.properties.deviceID},
            {"type",s.properties.deviceType},{"api_version",s.properties.apiVersion},{"driver_version",s.properties.driverVersion}}},
        {"semantics","Actual VK_QUERY_PIPELINE_STATISTIC_FRAGMENT_SHADER_INVOCATIONS_BIT around each original draw. Counts are driver-defined shader invocations, including implementation helper/quad effects; not passing samples, exact early-kill counts, exclusive shader time or frame GPU time."},
        {"capacity",s.capacity},{"maximum_samples",NativeFragmentInvocationLedger::kSamples},
        {"maximum_keys",NativeFragmentInvocationLedger::kKeys},{"host_resets",s.host_resets},{"query_reads",s.query_reads},
        {"discarded_draws",s.discarded},{"budget_skipped",s.budget_skipped},
        {"begun",t.begun},{"accepted",t.accepted},{"completed",t.completed},{"cancelled",t.cancelled},
        {"available",t.available},{"unavailable",t.unavailable},{"pending",t.pending},{"fragment_invocations",t.invocations},
        {"incomplete",!s.pool||s.disabled||s.budget_skipped||t.cancelled||t.pending||t.unavailable||t.accepted!=t.completed},
        {"keys",Json::array()},{"samples",Json::array()},{"aggregates",Json::array()}};
    std::vector<uint64_t> counts(s.keys.size()),available(s.keys.size()),sums(s.keys.size());
    for(uint32_t i=0;i<s.keys.size();++i) {auto key=KeyJson(s.keys[i]);key["key"]=i+1;out["keys"].push_back(std::move(key));}
    if(s.ledger)for(const auto& sample:s.ledger->samples()) {
        const auto& tag=sample.tag;const auto k=tag.key-1;
        // Successful ledger retirement checked the global sum, so these
        // nonnegative subset sums cannot overflow independently.
        ++counts[k];available[k]+=bool(sample.invocations);sums[k]+=sample.invocations.value_or(0);
        out["samples"].push_back({{"event",tag.event},{"frame",tag.frame},{"key",tag.key},{"indices",tag.indices},
            {"slot",sample.token.slot},{"generation",sample.token.generation},{"submission",sample.submission},
            {"early_depth_anchor",tag.early_depth_anchor},{"early_depth_current",tag.early_depth_current},
            {"fragment_invocations",sample.invocations?Json(*sample.invocations):Json(nullptr)}});
    }
    for(uint32_t i=0;i<s.keys.size();++i)out["aggregates"].push_back({{"key",i+1},{"draws",counts[i]},
        {"available",available[i]},{"fragment_invocations",sums[i]}});
    return out;
}
}
