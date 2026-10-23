#pragma once
#include "renderer/native/content_hash.h"
#include "renderer/native/live_operations.h"
#include "renderer/native/retained_opaque_catalog.h"
#include <algorithm>
#include <array>
#include <bit>
#include <memory>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>
namespace gow3::renderer::validation {
// Diagnostic-only altered input, never a producer or production draw path.
// The originals, geometry, textures and shaders remain immutable. The returned
// owners must outlive every pointer in operations and the complete Scene run.
struct RetainedOpaqueStateFixture {
    std::vector<std::shared_ptr<const LiveOperation>> owners;
    std::vector<const LiveOperation*> operations;
    nlohmann::json metadata;
    bool expects_state_refusal=false;
};
inline RetainedOpaqueStateFixture MakeRetainedOpaqueStateFixture(
    std::span<const LiveOperation* const> original,std::string_view mode) {
    if(mode=="source-switch") {
        if(original.size()>65536)throw std::invalid_argument("Retained source-switch operation bound");
        std::array<const TypedLiveDraw*,kRetainedOpaqueCatalogMaxPrograms> donors{};
        uint32_t donor_frame=UINT32_MAX;size_t donor_end=0,anchor=original.size();
        for(size_t i=0;i<original.size();++i) {
            if(!original[i])throw std::invalid_argument("Null source-switch operation");
            const auto* draw=original[i]->Draw();
            if(!draw||!draw->vertex_shader||!draw->pixel_shader)continue;
            const auto* pair=RetainedOpaqueCatalogPair(draw->vertex_shader->hash.View(),draw->pixel_shader->hash.View());
            if(!pair)continue;
            if(donor_frame!=draw->frame){donors={};donor_frame=draw->frame;}
            const auto index=size_t(pair-kRetainedOpaqueCatalogPairs.data());
            if(!donors[index]){donors[index]=draw;donor_end=i;}
            if(!std::all_of(donors.begin(),donors.end(),[](auto* item){return item!=nullptr;}))continue;
            for(size_t j=std::max(i,donor_end+1);j+3<=original.size();++j) {
                bool eligible=true;
                for(size_t k=0;k<3;++k) {
                    if(!original[j+k])throw std::invalid_argument("Null source-switch anchor");
                    const auto* member=original[j+k]->Draw();
                    eligible=eligible&&member&&member->frame==donor_frame&&member->vertex_shader&&member->pixel_shader&&
                        RetainedOpaqueCatalogPair(member->vertex_shader->hash.View(),member->pixel_shader->hash.View());
                }
                if(eligible) {
                    const auto* first=original[j]->Draw();
                    for(size_t k=1;k<3;++k) {
                        const auto* next=original[j+k]->Draw();
                        eligible=eligible&&first->vertex_shader->hash==next->vertex_shader->hash&&
                            first->pixel_shader->hash==next->pixel_shader->hash;
                    }
                }
                if(eligible){anchor=j;break;}
            }
            if(anchor!=original.size())break;
        }
        if(anchor==original.size())throw std::invalid_argument("No late contiguous triplet after all actual source donors");
        RetainedOpaqueStateFixture result;result.operations.reserve(original.size());auto records=nlohmann::json::array();
        for(size_t i=0;i<original.size();++i) {
            if(i<anchor||i>=anchor+3){result.operations.push_back(original[i]);continue;}
            const auto* replaced=original[i]->Draw();const auto* donor=donors[i-anchor];
            TypedLiveDraw clone=*donor;clone.event=replaced->event;clone.frame=replaced->frame;
            auto published=std::make_shared<const LiveOperation>(std::move(clone));
            result.operations.push_back(published.get());result.owners.push_back(std::move(published));
            records.push_back({{"event",replaced->event},{"frame",replaced->frame},{"donor_event",donor->event},
                {"donor_frame",donor->frame},{"vertex",donor->vertex_shader->hash.View()},{"pixel",donor->pixel_shader->hash.View()},
                {"source_swap",true},{"donor_state_sha256",donor->lazy_device_state?donor->lazy_device_state->Digest():donor->device_state.hash.View()}});
        }
        result.metadata={{"mode",mode},{"synthetic_state_fixture",true},{"live_provenance_unmodified",false},
            {"original_operations",original.size()},{"modified_draws",3},{"records",std::move(records)},
            {"expects_state_refusal",false},{"requires_positive_retained_source_switches",true},
            {"scope","A late three-draw segment is replaced by complete original captured donor draws from each authenticated source pair. Each donor keeps its own shader, device state, geometry and texture owners; only event/frame envelopes are relocated. All donors have already appeared in this same frame. OFF/ON must receive this identical controlled sequence. Real GPU source-switch continuity, queried counts and full memory/image parity remain mandatory."}};
        return result;
    }
    const bool reverse=mode=="reverse-viewport",mask=mode=="masked-color",
        depth=mode=="depth-readonly",bias=mode=="nonzero-bias",blend=mode=="blend-enabled";
    if(!reverse&&!mask&&!depth&&!bias&&!blend)throw std::invalid_argument("Unknown retained state fixture");
    if(original.size()>65536)throw std::invalid_argument("Retained state fixture operation bound");
    RetainedOpaqueStateFixture result;result.expects_state_refusal=true;
    result.operations.reserve(original.size());result.owners.reserve(original.size());
    std::array<uint64_t,kRetainedOpaqueCatalogMaxPrograms> modified{};
    auto records=nlohmann::json::array();
    const auto word=[](std::span<const uint8_t> bytes,size_t at) {
        if(at>bytes.size()||bytes.size()-at<4)throw std::invalid_argument("State fixture word extent");
        return uint32_t(bytes[at])<<24|uint32_t(bytes[at+1])<<16|uint32_t(bytes[at+2])<<8|bytes[at+3];
    };
    const auto set=[](std::vector<uint8_t>& bytes,size_t at,uint32_t value) {
        for(unsigned i=0;i<4;++i)bytes.at(at+i)=uint8_t(value>>(24-8*i));
    };
    for(const auto* operation:original) {
        if(!operation)throw std::invalid_argument("Null retained state fixture operation");
        const auto* draw=operation->Draw();const NativeRetainedOpaqueCatalogPair* pair=nullptr;
        if(draw&&draw->vertex_shader&&draw->pixel_shader)
            pair=RetainedOpaqueCatalogPair(draw->vertex_shader->hash.View(),draw->pixel_shader->hash.View());
        if(!pair){result.operations.push_back(operation);continue;}
        const auto& owner=draw->lazy_device_state?draw->lazy_device_state->Owner():draw->device_state.owner;
        if(!owner||owner->size()!=LazyDeviceState::kBytes)
            throw std::invalid_argument("State fixture requires exact immutable full device state");
        auto bytes=std::vector<uint8_t>(*owner);auto edits=nlohmann::json::array();
        const auto change=[&](size_t at,uint32_t value) {
            edits.push_back({{"offset",at},{"old_word",word(bytes,at)},{"new_word",value}});set(bytes,at,value);
        };
        if(reverse) {
            const auto zs=std::bit_cast<float>(word(bytes,0x2918)),zo=std::bit_cast<float>(word(bytes,0x291c));
            // Historical mode name is retained for CLI compatibility. The
            // authenticated material capture starts1->0; this mutation makes
            // the unproved0->1 range, which must refuse the hardware writer.
            if(!(zs==-1.f&&zo==1.f))
                throw std::invalid_argument("Viewport refusal fixture requires the exact original1->0 range");
            change(0x2918,std::bit_cast<uint32_t>(-zs));change(0x291c,std::bit_cast<uint32_t>(1.f-zo));
        } else if(mask)change(0x28dc,(word(bytes,0x28dc)&~15u)|7u);
        else if(depth)change(0x2934,word(bytes,0x2934)&~4u);
        else if(blend)change(0x2938,0x01010101u); // ADD with ONE source/destination, RGBA.
        else {
            change(0x2948,word(bytes,0x2948)|(1u<<11)|(1u<<12));
            change(0x2a50,0);change(0x2a54,std::bit_cast<uint32_t>(.125f));
            change(0x2a58,0);change(0x2a5c,std::bit_cast<uint32_t>(.125f));
        }
        // Prove every unrelated byte is retained before publishing the clone.
        auto restored=bytes;
        for(const auto& edit:edits)set(restored,edit.at("offset").get<size_t>(),edit.at("old_word").get<uint32_t>());
        if(restored!=*owner||bytes==*owner)throw std::logic_error("State fixture changed unrelated bytes or no bytes");
        const auto digest=ContentHash(bytes);TypedLiveDraw clone=*draw;
        clone.lazy_device_state.reset();clone.device_state={CanonicalDigest(digest),
            std::make_shared<const std::vector<uint8_t>>(std::move(bytes))};
        auto published=std::make_shared<const LiveOperation>(std::move(clone));
        result.operations.push_back(published.get());result.owners.push_back(std::move(published));
        ++modified[size_t(pair-kRetainedOpaqueCatalogPairs.data())];
        records.push_back({{"event",draw->event},{"frame",draw->frame},{"vertex",pair->vertex},{"pixel",pair->pixel},
            {"original_state_sha256",ContentHash(*owner)},{"modified_state_sha256",digest},{"edits",std::move(edits)}});
    }
    auto programs=nlohmann::json::array();uint64_t total=0;
    for(size_t i=0;i<modified.size();++i) {
        if(!modified[i])throw std::invalid_argument("State fixture lacks a positive actual source pair");
        total+=modified[i];programs.push_back({{"vertex",kRetainedOpaqueCatalogPairs[i].vertex},
            {"pixel",kRetainedOpaqueCatalogPairs[i].pixel},{"modified_draws",modified[i]}});
    }
    result.metadata={{"mode",mode},{"synthetic_state_fixture",true},{"live_provenance_unmodified",false},
        {"original_operations",original.size()},{"modified_draws",total},{"programs",std::move(programs)},
        {"expects_state_refusal",result.expects_state_refusal},
        {"viewport_mutation_direction",reverse?nlohmann::json("original1->0 to unproved0->1"):nlohmann::json(nullptr)},
        {"records",std::move(records)},
        {"scope","Controlled immutable device-state clones of actual material draws; event/frame/order and all unrelated bytes, shader/geometry/texture owners retained. Both ordinary Native and candidate must receive the identical altered inputs; this is not an authentic unmodified gameplay capture."}};
    return result;
}
}
