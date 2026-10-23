#include "renderer/native/fsi_immutable_state_hoist.h"
#include "renderer/native/fsi_exact_stores.h"
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
using namespace gow3::renderer;
namespace {
std::string Read(const std::filesystem::path& path) {std::ifstream file(path,std::ios::binary);if(!file)throw std::runtime_error("Cannot read owned hoist source");return {std::istreambuf_iterator<char>(file),{}};}
void Write(const std::filesystem::path& path,const std::string& text) {std::ofstream file(path,std::ios::binary|std::ios::trunc);file.write(text.data(),text.size());if(!file)throw std::runtime_error("Cannot write hoist paired source");}
}
int main(int argc,char** argv)try {
    if(argc!=5&&argc!=6)throw std::runtime_error("Usage: source-test canonical-helper fixture baseline.hlsl candidate.hlsl [incoming.hlsl]");
    size_t checks=0;auto check=[&](bool v){++checks;if(!v)throw std::runtime_error("Immutable FSI hoist closed source mismatch");};
    for(const auto mode:{NativeFsiHoistMode::Disabled,NativeFsiHoistMode::ImmutableState,NativeFsiHoistMode::ImmutableStateAndIncomingDepth})check(NativeFsiHoistModeKnown(mode));
    check(!NativeFsiHoistModeKnown(NativeFsiHoistMode(3)));
    check(!TryNativeFsiHoistSource("",NativeFsiHoistMode::Disabled)&&!RestoreNativeFsiHoistSource("",NativeFsiHoistMode::Disabled));
    // Lower two bits exhaust the enable truth table; all higher bits are
    // irrelevant to the exact original depth_enabled||stencil_enabled guard.
    for(uint32_t higher:{0u,4u,0x700764u,0xfffffffcu})for(uint32_t low=0;low<4;++low) {
        const uint32_t control=higher|low;
        check(((control&3u)!=0u)==(((control&2u)!=0u)||((control&1u)!=0u)));
    }
    const auto canonical=Read(argv[1]);const auto candidate=TryNativeFsiImmutableStateHoistSource(canonical);check(bool(candidate));
    auto restored=RestoreNativeFsiImmutableStateHoistSource(*candidate);check(restored&&*restored==canonical);
    check(!TryNativeFsiImmutableStateHoistSource("")&&!TryNativeFsiImmutableStateHoistSource(canonical+canonical)&&!TryNativeFsiImmutableStateHoistSource(*candidate));
    for(const auto block:{kNativeFsiImmutableStateHoistOriginal,kNativeFsiImmutableStateHoistReplacement}) {
        auto text=block==kNativeFsiImmutableStateHoistOriginal?canonical:*candidate;const auto at=text.find(block);check(at!=std::string::npos);
        text[at+3]='X';check(block==kNativeFsiImmutableStateHoistOriginal?!TryNativeFsiImmutableStateHoistSource(text):!RestoreNativeFsiImmutableStateHoistSource(text));
    }
    // Composition is restoration evidence only; first live proposal is an
    // independent canonical+hoist provider with the slower broad option OFF.
    const auto broad=TryNativeFsiExactStoresSource(canonical);check(bool(broad));
    const auto composed=TryNativeFsiImmutableStateHoistSource(*broad);check(bool(composed));
    const auto unhoisted=RestoreNativeFsiImmutableStateHoistSource(*composed);check(unhoisted&&*unhoisted==*broad);
    restored=RestoreNativeFsiExactStoresSource(*unhoisted);check(restored&&*restored==canonical);
    const auto incoming=TryNativeFsiStateAndIncomingDepthHoistSource(canonical);check(bool(incoming));
    restored=RestoreNativeFsiStateAndIncomingDepthHoistSource(*incoming);check(restored&&*restored==canonical);
    check(TryNativeFsiHoistSource(canonical,NativeFsiHoistMode::ImmutableState)==candidate);
    check(TryNativeFsiHoistSource(canonical,NativeFsiHoistMode::ImmutableStateAndIncomingDepth)==incoming);
    check(RestoreNativeFsiHoistSource(*candidate,NativeFsiHoistMode::ImmutableState)==std::optional<std::string>(canonical));
    check(RestoreNativeFsiHoistSource(*incoming,NativeFsiHoistMode::ImmutableStateAndIncomingDepth)==std::optional<std::string>(canonical));
    check(!RestoreNativeFsiHoistSource(*candidate,NativeFsiHoistMode::ImmutableStateAndIncomingDepth));
    check(!RestoreNativeFsiHoistSource(*incoming,NativeFsiHoistMode::ImmutableState));
    check(!TryNativeFsiStateAndIncomingDepthHoistSource(*incoming)&&!TryNativeFsiIncomingDepthHoistSource(canonical));
    check(!TryNativeFsiIncomingDepthHoistSource(*candidate+"\nnativeDepthStencil(extra,depth,true);"));
    check(!RestoreNativeFsiIncomingDepthHoistSource(*incoming+"\nnativeDepthStencil(extra,incoming_depth,true);"));
    for(const auto block:{kNativeFsiIncomingDepthSignatureOriginal,kNativeFsiIncomingDepthCodeOriginal,kNativeFsiImmutableStateHoistReplacement}) {
        auto text=*candidate;const auto at=text.find(block);check(at!=std::string::npos);text[at+3]='X';check(!TryNativeFsiIncomingDepthHoistSource(text));
    }
    for(const auto block:{kNativeFsiIncomingDepthSignatureReplacement,kNativeFsiIncomingDepthCodeReplacement,kNativeFsiStateAndIncomingDepthHoistReplacement}) {
        auto text=*incoming;const auto at=text.find(block);check(at!=std::string::npos);text[at+3]='X';check(!RestoreNativeFsiIncomingDepthHoistSource(text));
    }
    const auto fixture=Read(argv[2]);constexpr std::string_view marker="// GOW3_INSERT_FSI_HELPER";const auto at=fixture.find(marker);
    check(at!=std::string::npos&&fixture.find(marker,at+1)==std::string::npos);
    auto baseline=fixture,modified=fixture;baseline.replace(at,marker.size(),canonical);modified.replace(at,marker.size(),*candidate);
    Write(argv[3],baseline);Write(argv[4],modified);
    if(argc==6){auto quantized=fixture;quantized.replace(at,marker.size(),*incoming);Write(argv[5],quantized);}
    std::cout<<"PASS: "<<checks<<" exact unconditional immutable state read reorder/source restoration checks; GPU and fault/control owner proof still required\n";
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
