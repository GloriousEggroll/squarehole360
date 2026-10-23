#include "retained_opaque_state_fixture.h"
#include <iostream>
using namespace gow3::renderer;
namespace {
uint64_t checks=0;
void Check(bool condition,const char* message) {++checks;if(!condition)throw std::runtime_error(message);}
void Set(std::vector<uint8_t>& bytes,size_t at,uint32_t value) {
    for(unsigned i=0;i<4;++i)bytes.at(at+i)=uint8_t(value>>(24-8*i));
}
LiveOperations Input(bool lazy=false) {
    LiveOperations result;uint32_t index=0;
    result.push_back(nlohmann::json{{"type","query_begin"},{"event",0},{"frame",9}});
    for(const auto& pair:kRetainedOpaqueCatalogPairs) {
        std::vector<uint8_t> bytes(LazyDeviceState::kBytes);
        for(size_t i=0;i<bytes.size();++i)bytes[i]=uint8_t(i*37+index);
        Set(bytes,0x2918,std::bit_cast<uint32_t>(-1.f));
        Set(bytes,0x291c,std::bit_cast<uint32_t>(1.f));
        Set(bytes,0x28dc,15);Set(bytes,0x2934,0x700766);Set(bytes,0x2938,0x10001);
        Set(bytes,0x2948,0);for(size_t at=0x2a50;at<0x2a60;at+=4)Set(bytes,at,0);
        auto owner=std::make_shared<const std::vector<uint8_t>>(bytes);
        TypedLiveDraw draw;draw.event=++index;draw.frame=9;draw.args={7,4,0,3,0,0,0,0};
        if(lazy)draw.lazy_device_state=std::make_shared<const LazyDeviceState>(owner);
        else draw.device_state={CanonicalDigest(ContentHash(bytes)),owner};
        draw.vertex_shader=CanonicalResourceRef{CanonicalDigest(pair.vertex),owner};
        draw.pixel_shader=CanonicalResourceRef{CanonicalDigest(pair.pixel),owner};
        TypedGeometry geometry;geometry.kind=14;geometry.payload={CanonicalDigest(ContentHash(bytes)),owner};
        draw.geometry.push_back(geometry);TypedTexture texture;texture.slot=3;
        texture.base_payload=CanonicalResourceRef{CanonicalDigest(ContentHash(bytes)),owner};draw.textures.push_back(texture);
        result.push_back(std::move(draw));
    }
    result.push_back(nlohmann::json{{"type","query_end"},{"event",4},{"frame",9}});return result;
}
std::vector<const LiveOperation*> Pointers(const LiveOperations& operations) {
    std::vector<const LiveOperation*> result;for(const auto& operation:operations)result.push_back(&operation);return result;
}
template<class Fn>void Refuses(Fn&& run) {
    bool refused=false;try{run();}catch(const std::invalid_argument&){refused=true;}Check(refused,"Malformed state fixture was accepted");
}
}
int main()try {
    for(bool lazy:{false,true})for(std::string_view mode:{"reverse-viewport","masked-color","depth-readonly","blend-enabled","nonzero-bias"}) {
        auto original=Input(lazy);const auto pointers=Pointers(original);
        std::vector<std::vector<uint8_t>> before;
        for(size_t i=1;i<=3;++i) {
            const auto& draw=*pointers[i]->Draw();before.push_back(*(draw.lazy_device_state?draw.lazy_device_state->Owner():draw.device_state.owner));
        }
        auto fixture=validation::MakeRetainedOpaqueStateFixture(pointers,mode);
        Check(fixture.owners.size()==3&&fixture.operations.size()==pointers.size(),"Altered operation count/owner extent");
        Check(fixture.operations.front()==pointers.front()&&fixture.operations.back()==pointers.back(),"Control envelope changed");
        Check(fixture.metadata.at("modified_draws")==3&&fixture.metadata.at("live_provenance_unmodified")==false,"Synthetic provenance missing");
        Check(fixture.expects_state_refusal,"Wrong negative state gate");
        for(size_t i=1;i<=3;++i) {
            const auto& old=*pointers[i]->Draw();const auto& changed=*fixture.operations[i]->Draw();
            Check(old.event==changed.event&&old.frame==changed.frame&&old.function==changed.function&&old.args==changed.args,"Draw envelope changed");
            Check(!changed.lazy_device_state&&changed.device_state.owner&&changed.device_state.owner!=old.device_state.owner,"Changed state lacks its own owner");
            Check(ContentHash(*changed.device_state.owner)==changed.device_state.hash.View(),"Changed state identity mismatches bytes");
            Check(changed.vertex_shader->owner==old.vertex_shader->owner&&changed.vertex_shader->hash==old.vertex_shader->hash&&
                  changed.pixel_shader->owner==old.pixel_shader->owner&&changed.pixel_shader->hash==old.pixel_shader->hash,"Shader owner/identity changed");
            Check(changed.geometry[0].payload.owner==old.geometry[0].payload.owner&&
                  changed.textures[0].base_payload->owner==old.textures[0].base_payload->owner,"Material resource owner changed");
            const auto& original_owner=old.lazy_device_state?old.lazy_device_state->Owner():old.device_state.owner;
            Check(*original_owner==before[i-1],"Original immutable owner mutated");
            auto restored=*changed.device_state.owner;
            for(const auto& edit:fixture.metadata.at("records")[i-1].at("edits"))
                Set(restored,edit.at("offset").get<size_t>(),edit.at("old_word").get<uint32_t>());
            Check(restored==before[i-1],"Unrelated state byte changed");
        }
        if(mode=="reverse-viewport") {
            Check(fixture.metadata.at("viewport_mutation_direction")=="original1->0 to unproved0->1","Viewport direction mislabeled");
            for(size_t i=1;i<=3;++i) {
                const auto& bytes=*fixture.operations[i]->Draw()->device_state.owner;
                const auto word=[&](size_t at){return uint32_t(bytes[at])<<24|uint32_t(bytes[at+1])<<16|uint32_t(bytes[at+2])<<8|bytes[at+3];};
                Check(word(0x2918)==std::bit_cast<uint32_t>(1.f)&&word(0x291c)==0,"Refusal mutation is not the exact0->1 range");
            }
            Refuses([&]{validation::MakeRetainedOpaqueStateFixture(fixture.operations,mode);});
        }
        // Returned owners independently retain the altered states after every
        // temporary producer/first fixture copy has disappeared.
        const auto retained=fixture.operations[1]->Draw()->device_state.owner;fixture={};
        Check(retained&&retained->size()==LazyDeviceState::kBytes,"Altered state owner died with pointer vector");
    }
    auto original=Input();const auto pointers=Pointers(original);
    LiveOperations switched;switched.push_back(nlohmann::json{{"type","query_begin"},{"event",0},{"frame",9}});
    for(size_t i=1;i<=3;++i)switched.push_back(*pointers[i]->Draw());
    for(size_t i=4;i<=6;++i){auto copy=*pointers[1]->Draw();copy.event=i;switched.push_back(std::move(copy));}
    switched.push_back(nlohmann::json{{"type","query_end"},{"event",7},{"frame",9}});
    const auto switch_input=Pointers(switched);
    auto triplet=validation::MakeRetainedOpaqueStateFixture(switch_input,"source-switch");
    Check(triplet.owners.size()==3&&triplet.operations.size()==switch_input.size(),"Source-switch triplet bounds");
    Check(triplet.metadata.at("requires_positive_retained_source_switches")==true&&
        triplet.metadata.at("records")[0].at("event")==4,"Source-switch fixture lacks explicit gate/location");
    for(size_t i=0;i<3;++i) {
        const auto& moved=*triplet.operations[4+i]->Draw();const auto& donor=*switch_input[1+i]->Draw();
        Check(moved.event==4+i&&moved.frame==donor.frame,"Source-switch envelope order changed");
        Check(moved.vertex_shader->hash==donor.vertex_shader->hash&&moved.pixel_shader->hash==donor.pixel_shader->hash&&
            moved.device_state.owner==donor.device_state.owner&&moved.geometry[0].payload.owner==donor.geometry[0].payload.owner&&
            moved.textures[0].base_payload->owner==donor.textures[0].base_payload->owner,"Donor material/geometry/constants owner mixed");
    }
    Refuses([&]{validation::MakeRetainedOpaqueStateFixture(pointers,"source-switch");});
    Refuses([&]{validation::MakeRetainedOpaqueStateFixture(pointers,"unknown");});
    Refuses([&]{validation::MakeRetainedOpaqueStateFixture({},"reverse-viewport");});
    auto missing=pointers;missing.erase(missing.begin()+2);
    Refuses([&]{validation::MakeRetainedOpaqueStateFixture(missing,"masked-color");});
    auto bad=std::vector<uint8_t>(*pointers[1]->Draw()->device_state.owner);Set(bad,0x2918,0x7fc00000);
    auto changed=*pointers[1]->Draw();changed.device_state={CanonicalDigest(ContentHash(bad)),std::make_shared<const std::vector<uint8_t>>(bad)};
    LiveOperation bad_viewport(std::move(changed));auto invalid=pointers;invalid[1]=&bad_viewport;
    Refuses([&]{validation::MakeRetainedOpaqueStateFixture(invalid,"reverse-viewport");});
    changed=*pointers[1]->Draw();changed.device_state.owner=std::make_shared<const std::vector<uint8_t>>(35,0);
    LiveOperation short_state(std::move(changed));invalid=pointers;invalid[1]=&short_state;
    Refuses([&]{validation::MakeRetainedOpaqueStateFixture(invalid,"masked-color");});
    invalid=pointers;invalid[1]=nullptr;
    Refuses([&]{validation::MakeRetainedOpaqueStateFixture(invalid,"masked-color");});
    std::cout<<"PASS retained catalogue state fixture: "<<checks<<" checks; exact unrelated bytes and owners, all3 pairs/eager+lazy, exact1->0-to0->1 refusal seed and malformed refusal; GPU proof pending\n";
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
