#include "renderer/native/owned_payload.h"
#include <algorithm>
#include <iostream>
#include <unordered_map>

using namespace gow3::renderer;
namespace {
void Require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
bool Equal(std::span<const uint8_t> a,std::span<const uint8_t> b) {
    return a.size()==b.size()&&std::equal(a.begin(),a.end(),b.begin());
}
}
int main() try {
    for(size_t size:{0u,1u,48u,0x2a80u,65537u}) {
        std::vector<uint8_t> expected(size);
        for(size_t i=0;i<size;++i)expected[i]=uint8_t(i*131+(i>>7));
        auto snapshot=std::make_shared<const std::vector<uint8_t>>(expected);
        OwnedPayload borrowed(snapshot);
        Require(Equal(borrowed.bytes(),expected)&&borrowed.bytes().data()==snapshot->data(),
            "Immutable payload was copied or changed");
        auto moved=std::move(borrowed);
        Require(Equal(moved.bytes(),expected)&&moved.bytes().data()==snapshot->data(),
            "Moving the view lost its immutable owner");
        auto writable=std::move(moved).TakeMutable();
        if(size) {
            Require(writable.data()!=snapshot->data(),"Mutable overlay aliases immutable snapshot");
            writable[0]^=0xff;
        }
        Require(Equal(*snapshot,expected),"Mutable overlay changed immutable source bytes");

        auto legacy=expected;const auto* allocation=legacy.data();
        OwnedPayload fallback(std::move(legacy));
        Require(Equal(fallback.bytes(),expected)&&fallback.bytes().data()==allocation,
            "Legacy/file payload bytes were copied during ownership transfer");
        auto moved_fallback=std::move(fallback);
        auto returned=std::move(moved_fallback).TakeMutable();
        Require(returned==expected&&returned.data()==allocation,
            "Legacy/file mutable payload lost its original allocation");
    }
    // Replacing a provider entry models the next immutable capture at the same
    // guest address. An outstanding view must keep its original bytes alive.
    std::unordered_map<std::string,OwnedPayload::Snapshot> provider;
    provider["state"]=std::make_shared<const std::vector<uint8_t>>(std::initializer_list<uint8_t>{1,2,3,4});
    std::weak_ptr<const std::vector<uint8_t>> previous=provider.at("state");
    std::vector<uint8_t> upload;
    {
        OwnedPayload view(provider.at("state"));
        provider["state"]=std::make_shared<const std::vector<uint8_t>>(std::initializer_list<uint8_t>{8,7,6,5});
        Require(!previous.expired()&&Equal(view.bytes(),std::vector<uint8_t>{1,2,3,4}),
            "Provider replacement invalidated an outstanding payload view");
        upload.assign(view.bytes().begin(),view.bytes().end());
        provider.clear();
        Require(Equal(view.bytes(),upload),"Provider/frame destruction invalidated owned CPU view");
    }
    Require(previous.expired()&&upload==std::vector<uint8_t>{1,2,3,4},
        "Completed CPU consumption leaked the snapshot or changed copied upload bytes");
    bool rejected=false;
    try {OwnedPayload invalid(OwnedPayload::Snapshot{});}
    catch(const std::runtime_error&){rejected=true;}
    Require(rejected,"Null immutable snapshot was accepted as empty valid data");
    std::cout<<"PASS: exact borrowed bytes, move-only legacy/file ownership, immutable overlay copies, "
        "provider replacement/destruction, copied upload lifetime, null rejection and empty payloads\n";
    return 0;
} catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
