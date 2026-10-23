#include "stencil_material_cutoff.h"
#include "content_hash.h"
#include <array>
#include <cstring>
namespace gow3::renderer {
namespace {
bool Once(std::string_view s,std::string_view needle) {
    const auto at=s.find(needle);return at!=s.npos&&s.find(needle,at+needle.size())==s.npos;
}
std::string Entry(NativeStencilMaterialMode mode) {
    std::string result(kStencilMaterialEntry);
    result+="\nbool gow3StencilMaterialSurvives = gow3StencilMaterialPrefix(uint2(iPos.xy));\n";
    if(mode==NativeStencilMaterialMode::Cutoff||mode==NativeStencilMaterialMode::Uninstrumented)result+="[branch] if(!gow3StencilMaterialSurvives) discard;\n";
    return result;
}
bool Mode(NativeStencilMaterialMode m) {return m==NativeStencilMaterialMode::Observe||m==NativeStencilMaterialMode::Cutoff||m==NativeStencilMaterialMode::Uninstrumented;}
std::string_view Helper(NativeStencilMaterialMode mode) {
    return mode==NativeStencilMaterialMode::Uninstrumented?kStencilMaterialUninstrumentedHelper:kStencilMaterialHelper;
}
bool Shape(std::span<const uint32_t> w,bool derived) {
    if(w.size()<5||w.size()>65536||w[0]!=0x07230203||w[1]!=0x00010500||!w[3]||w[3]>65536||w[4])return false;
    uint32_t entry=0,functions=0,dx=0,dy=0,samples=0,barriers=0,begins=0,ends=0,atomics=0;
    bool in_function=false,past_entry=false,interlock=false,fragment=false;
    for(size_t i=5;i<w.size();) {
        const uint32_t n=w[i]>>16,op=w[i]&65535;if(!n||n>w.size()-i)return false;
        if(op==15){if(n<4||w[i+1]!=4||entry)return false;entry=w[i+2];}
        if(op==16){if(n!=3||w[i+1]!=entry||(w[i+2]!=7&&w[i+2]!=5366))return false;fragment|=w[i+2]==7;interlock|=w[i+2]==5366;}
        if(op==54){if(n!=5||w[i+2]!=entry||++functions!=1)return false;in_function=true;}
        if(op==56)in_function=false;
        // No user functions, implicit derivatives, group or derivative extension
        // instructions. Exact canonical source and baseline binary are also pinned.
        if(op==57||op==63||op==64||op==99||op==87||op==89||op==91||op==93||op==105||
           (op>=207&&op<=212)||op==215||
           (op>=259&&op<=271)||(op>=333&&op<=366)||op==5381||op==5382)return false;
        if(op==213||op==214){if(!in_function||past_entry||n!=4)return false;op==213?++dx:++dy;}
        if(in_function&&(op==249||op==250||op==251||op==252||op==88||(op>=227&&op<=242)))past_entry=true;
        if(op==88)++samples;
        if(op==225)++barriers;
        if(op==5364)++begins;
        if(op==5365)++ends;
        if(op>=227&&op<=242){if(!derived||(op!=234&&op!=241))return false;++atomics;}
        i+=n;
    }
    return entry&&fragment&&interlock&&functions==1&&dx==1&&dy==1&&samples==10&&
        barriers==2&&begins==1&&ends>=2&&(derived?atomics==4:atomics==0);
}
}
bool NativeStencilMaterialCanonicalCertified(std::span<const uint32_t> words,bool vertex) {
    const size_t bytes=vertex?8496:102692;
    if(words.size_bytes()!=bytes)return false;
    const auto hash=ContentHash({reinterpret_cast<const uint8_t*>(words.data()),words.size_bytes()});
    return hash==(vertex?kStencilMaterialVertexSpirv:kStencilMaterialPixelSpirv)&&
        (vertex||Shape(words,false));
}
namespace {
std::string ProgramAuthentication(const NativeStencilMaterialProgram& p) {
    const auto bytes=std::span(reinterpret_cast<const uint8_t*>(p.spirv.data()),p.spirv.size()*sizeof(uint32_t));
    const auto input=p.source+"\n"+p.canonical_version+"\n"+p.derived_version+"\n"+
        std::to_string(uint32_t(p.mode))+"/"+std::to_string(p.specialization_mask)+"/"+ContentHash(bytes);
    return ContentHash({reinterpret_cast<const uint8_t*>(input.data()),input.size()});
}
}
void NativeStencilMaterialProgram::Authenticate(){authentication_=ProgramAuthentication(*this);}
bool NativeStencilMaterialProgram::Authenticated() const {
    return source.size()==64&&canonical_version.size()==64&&derived_version.size()==64&&
        !authentication_.empty()&&spirv.size()<=65536&&authentication_==ProgramAuthentication(*this);
}
bool NativeStencilMaterialDerivedCertified(std::span<const uint32_t> words) {return Shape(words,true);}
bool NativeStencilMaterialDerivedCertified(std::span<const uint32_t> words,NativeStencilMaterialMode mode) {
    if(!Mode(mode))return false;
    if(mode!=NativeStencilMaterialMode::Uninstrumented)return Shape(words,true);
    // Exact canonical descriptor ABI: mode3 introduces no binding1, atomics,
    // counter buffer, readback resource or helper-related feature requirement.
    if(!Shape(words,false)||NativeStencilMaterialCanonicalCertified(words,false))return false;
    bool conditional=false,early_kill=false;
    for(size_t i=5;i<words.size();i+=words[i]>>16) {
        const auto op=words[i]&65535;
        if(op==88||op==5364)break;
        if(op==250)conditional=true;
        if(op==252)early_kill=conditional;
    }
    if(!early_kill)return false;
    std::vector<uint32_t> sets(words[3],UINT32_MAX),bindings(words[3],UINT32_MAX);
    for(size_t i=5;i<words.size();i+=words[i]>>16) {
        const auto count=words[i]>>16,op=words[i]&65535;
        if(op==71&&count>=3&&(words[i+2]==33||words[i+2]==34)) {
            if(count!=4||words[i+1]>=words[3])return false;
            (words[i+2]==33?bindings:sets)[words[i+1]]=words[i+3];
        }
    }
    unsigned edram=0;
    for(size_t i=0;i<sets.size();++i)if(sets[i]==4) {
        if(bindings[i]!=0)return false;
        ++edram;
    }
    return edram==1;
}
std::optional<std::string> TryNativeStencilMaterialSource(std::string_view source,NativeStencilMaterialMode mode) {
    constexpr std::string_view insertion="bool nativeDepthStencil(uint2 p,float depth,bool front_face) {";
    if(!Mode(mode)||!Once(source,kStencilMaterialEntry)||!Once(source,kStencilMaterialStore)||!Once(source,insertion)||
       source.find("gow3StencilMaterial")!=source.npos)return {};
    std::string r(source);
    r.replace(r.find(kStencilMaterialStore),kStencilMaterialStore.size(),kStencilMaterialStoreRemoved);
    r.insert(r.find(insertion),Helper(mode));
    r.replace(r.find(kStencilMaterialEntry),kStencilMaterialEntry.size(),Entry(mode));return r;
}
std::optional<std::string> RestoreNativeStencilMaterialSource(std::string_view source,NativeStencilMaterialMode mode) {
    if(!Mode(mode)||!Once(source,Helper(mode))||!Once(source,Entry(mode))||!Once(source,kStencilMaterialStoreRemoved))return {};
    std::string r(source);r.replace(r.find(Entry(mode)),Entry(mode).size(),kStencilMaterialEntry);
    r.erase(r.find(Helper(mode)),Helper(mode).size());
    r.replace(r.find(kStencilMaterialStoreRemoved),kStencilMaterialStoreRemoved.size(),kStencilMaterialStore);
    if(r.find("gow3StencilMaterial")!=r.npos)return {};return r;
}
} // namespace gow3::renderer
