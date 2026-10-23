#include "renderer/native/fsi_state_recipe_cache.h"
#include "renderer/native/content_hash.h"
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <thread>
using namespace gow3::renderer;
using namespace plume;
namespace {
void Require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
template<class F>void Reject(F&& action,const char* message){bool refused=false;try{action();}catch(const std::exception&){refused=true;}Require(refused,message);}
// A deadlocked shutdown must fail the fixture, not retain its CI worker forever.
struct Deadline {
    std::mutex mutex;std::condition_variable changed;bool finished=false;
    std::thread worker{[this]{std::unique_lock lock(mutex);if(!changed.wait_for(lock,std::chrono::seconds(5),[&]{return finished;}))std::_Exit(2);}};
    ~Deadline(){{std::lock_guard lock(mutex);finished=true;}changed.notify_one();worker.join();}
};
}
int main()try {
    NativeFsiConstants constants;constants.depth_control=0xf0700762u;constants.depth_format=1;constants.blend_enabled=15;
    for(size_t i=0;i<4;++i){constants.color[i].format=3;constants.color[i].mask=15;constants.color[i].blend=0xfedcba98u+uint32_t(i);}
    const auto original=NativeFsiStateSpecialization::From(constants);
    Require(original.words[0]==0xf0700762u&&original.words[1]==1&&original.words[2]==15,"Final packed values were canonicalized");
    std::map<std::optional<NativeFsiStateSpecialization>,unsigned> keys;keys.emplace(std::nullopt,0);keys.emplace(original,1);
    for(size_t word=0;word<15;++word){auto changed=original;changed.words[word]^=0x80000000u;Require(keys.emplace(changed,unsigned(word+2)).second,"Different exact tuple word collided");}
    Require(keys.size()==17,"Generic and specialized tuple identities collided");
    // Addresses, pitch, bias, reference/mask words and float blend constants stay dynamic.
    constants.pitch=16384;constants.depth_base=2047;constants.stencil_front=0xffffffffu;constants.stencil_back=0xabcdef01u;
    constants.front_slope_bias=-0.0f;constants.front_constant_bias=1.25f;constants.back_slope_bias=-0.75f;constants.back_constant_bias=100;
    constants.blend_constant={-0.0f,1,0.5f,-100};for(auto& color:constants.color)color.base=12345;
    Require(NativeFsiStateSpecialization::From(constants)==original,"Dynamic address/bias/reference/blend values changed the PSO state");
    const std::array<uint32_t,6> raster{uint32_t(RenderCullMode::BACK),uint32_t(RenderFrontFace::CLOCKWISE),1,
        uint32_t(RenderPrimitiveTopology::TRIANGLE_LIST),0x702,0};
    const auto check=[&](int32_t color) {
        NativeFsiPipelineDescriptor descriptor(raster,color,-1,nullptr,nullptr,nullptr,&original);
        Require(descriptor.descriptor.specConstants==descriptor.fsi_specialization->values.data()&&descriptor.descriptor.specConstantsCount==17,
            "Specialized descriptor does not retain its owned constants");
        const auto* spec=descriptor.descriptor.specConstants;
        Require(spec[0].index==0&&spec[0].value==raster[4]&&spec[1].index==32&&spec[1].value==1,"Alpha/enable specialization ABI changed");
        for(size_t i=0;i<15;++i)Require(spec[i+2].index==i+33&&spec[i+2].value==original.words[i],"Wrong exact specialization ID/value");
        Require(descriptor.descriptor.renderTargetCount==(color==-1?0u:1u),"Host attachment policy was inferred from guest packed color format");
    };
    check(-1);check(int32_t(RenderFormat::R8G8B8A8_UNORM));
    NativeFsiPipelineDescriptor generic(raster,int32_t(RenderFormat::R8G8B8A8_UNORM),-1,nullptr,nullptr,nullptr);
    Require(!generic.fsi_specialization&&generic.descriptor.specConstantsCount==1&&generic.descriptor.specConstants==&generic.specialization,
        "Default generic descriptor changed specialization ABI");
    auto vs=std::make_shared<const std::vector<uint8_t>>(std::vector<uint8_t>{1,2,3,4});
    auto ps=std::make_shared<const std::vector<uint8_t>>(std::vector<uint8_t>{5,6,7,8});
    NativeFsiStateRecipe recipe{{ContentHash(*vs),ContentHash(*ps),raster,int32_t(RenderFormat::R8G8B8A8_UNORM),-1},original};
    Require(ValidFsiStateRecipe(recipe),"Valid arbitrary high depth/blend bits refused");
    const auto encoded=FsiStateRecipeJson(recipe);Require(ParseFsiStateRecipe(encoded)==recipe,"Exact tuple JSON did not round trip");
    auto second=recipe;second.state.words[0]^=1;
    const std::string version(64,'a');NativeFsiStateSidecar sidecar{version,{{recipe,7},{second,91}}};
    std::array<ObservedNativePipeline,1> authenticated{{{recipe.generic,vs,ps}}};
    const auto ranked=NativeFsiStateSidecar::Parse(sidecar.Json().dump(),version).Authenticated(authenticated);
    Require(ranked.size()==2&&ranked[0].recipe==second&&ranked[0].observed_count==91,"Hot exact tuples were not ranked first");
    auto absent=authenticated;absent[0].recipe.vertex=std::string(64,'b');Require(sidecar.Authenticated(absent).empty(),"Unreferenced generic source identity authorized a variant");
    for(const auto bad:nlohmann::json{-1,4294967296ull,1.5})Reject([&]{auto value=encoded;value["fsi_state"][0]=bad;ParseFsiStateRecipe(value);},"Non-uint32 state value accepted");
    Reject([&]{auto value=encoded;value["fsi_state"].erase(0);ParseFsiStateRecipe(value);},"Incomplete tuple accepted");
    Reject([&]{auto value=encoded;value["fsi_state"][4]=16;ParseFsiStateRecipe(value);},"Unknown color mask accepted");
    Reject([&]{auto duplicate=sidecar;duplicate.records.push_back(duplicate.records.front());NativeFsiStateSidecar::Parse(duplicate.Json().dump(),version);},"Duplicate exact tuple accepted");
    Reject([&]{NativeFsiStateSidecar::Parse(sidecar.Json().dump(),std::string(64,'b'));},"Wrong derived compiler namespace accepted");
    Reject([&]{NativeFsiStateSidecar::Parse(std::string(NativeFsiStateSidecar::kMaxBytes+1,' '),version);},"Sidecar byte limit ignored");
    Deadline deadline;
    const auto directory=std::filesystem::temp_directory_path()/("gow3-fsi-state-fixture-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    struct Cleanup{std::filesystem::path path;~Cleanup(){std::error_code error;std::filesystem::remove_all(path,error);}}cleanup{directory};
    const auto bind=[&](NativeFsiStateRecipeCache& cache,const NativeFsiStateRecipe& item) {
        for(;;){if(auto* counter=cache.Bind(item))return counter;std::this_thread::yield();}
    };
    {
        NativeFsiStateRecipeCache cache(directory,version);auto* counter=bind(cache,recipe);
        counter->fetch_add(5);Require(bind(cache,recipe)==counter,"Deduplicated tuple counter address changed");
        // Capacity exhaustion cannot revoke stable resident counter pointers.
        for(size_t i=1;i<NativeFsiStateSidecar::kMaxRecipes;++i){auto item=recipe;item.state.words[0]=uint32_t(i);bind(cache,item);}
        Require(cache.statistics().resident==NativeFsiStateSidecar::kMaxRecipes,"Fixture did not fill bounded sidecar");
        auto overflow=recipe;overflow.state.words[0]=0xfedcba98u;Require(!cache.Bind(overflow),"Sidecar record limit ignored");
        Require(bind(cache,recipe)==counter,"Full sidecar revoked its resident tuple counter");
        cache.Drain();cache.StopAccepting();cache.Drain();
        Require(!cache.Bind(recipe),"Stopped observer accepted new bindings");
        cache.Drain(); // Original deadlock: joinable does not mean worker is live.
        counter->fetch_add(1); // Existing pointers remain valid until owner destruction.
    }
    {
        NativeFsiStateRecipeCache cache(directory,version);
        const auto loaded=cache.Load(authenticated);Require(!loaded.empty()&&loaded[0].observed_count==5,"Committed exact counts were not restored");
        cache.StopAccepting();std::this_thread::sleep_for(std::chrono::milliseconds(50));cache.Drain();cache.Drain();
    }
    {
        NativeFsiStateRecipeCache owner(directory,version),reader(directory,version); // Busy writer lease is an optional persistence miss.
        reader.StopAccepting();reader.Drain();reader.Drain();owner.StopAccepting();owner.Drain();
    }
    {
        const auto failed=directory/"writer-failure";std::filesystem::create_directories(failed/"states.json");
        NativeFsiStateRecipeCache cache(failed,version);bind(cache,recipe)->fetch_add(1);cache.Drain();
        Require(cache.statistics().failures>0,"Writer error fixture never exercised failed atomic publication");
        cache.StopAccepting();cache.Drain();cache.Drain();
    }
    std::cout<<"Exact15-word state, owned17 constants, authenticated bounded ranking, and shutdown re-entry fixtures passed\n";
    return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
