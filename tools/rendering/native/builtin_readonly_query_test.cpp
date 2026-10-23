#include "renderer/native/builtin_readonly_query.h"
#include "renderer/native/content_hash.h"
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
using namespace gow3::renderer;
namespace {
unsigned checks=0;
void Check(bool ok,const char* message) {++checks;if(!ok)throw std::runtime_error(message);}
std::vector<uint32_t> Read(const char* path) {
    std::ifstream file(path,std::ios::binary|std::ios::ate);
    Check(file&&file.tellg()>=20&&file.tellg()%4==0,"Bad input module");
    std::vector<uint32_t> v(size_t(file.tellg())/4);file.seekg(0);
    Check(bool(file.read(reinterpret_cast<char*>(v.data()),v.size()*4)),"Truncated input");return v;
}
}
int main(int argc,char** argv)try {
    if(argc!=4)throw std::runtime_error("Usage: builtin-readonly-query-test builtin.spv pinned-vs.spv output.spv");
    auto original=Read(argv[1]),vs=Read(argv[2]);
    auto output=TryBuiltinReadonlyQuery(original);Check(bool(output),"Pinned original refused");
    Check(std::equal(original.begin(),original.begin()+5,output->begin()),"SPIR-V header changed");
    size_t next=5,removed=0,stores=0,discards=0;
    std::map<uint32_t,uint32_t> pointer_storage,values;
    for(size_t at=5;at<original.size();) {
        const auto count=original[at]>>16,op=original[at]&65535;const auto* a=original.data()+at+1;
        if(op==32)pointer_storage[a[0]]=a[1];
        if((op==59||op==65)&&pointer_storage.contains(a[0]))values[a[1]]=pointer_storage.at(a[0]);
        const bool removed_store=op==62&&values.at(a[0])==12;
        const bool extension=op==10&&std::memcmp(a,"SPV_EXT_fragment_shader_interlock",33)==0;
        const bool erase=removed_store||extension||(op==17&&a[0]==5378)||
            (op==16&&a[1]==5366)||op==225||op==5364||op==5365;
        if(erase)++removed;
        else {
            Check(next+count<=output->size()&&std::equal(original.begin()+at,original.begin()+at+count,output->begin()+next),
                "Transform changed a retained instruction/operand");next+=count;
        }
        if(removed_store)++stores;
        if(op==252)++discards;
        auto mutation=original;mutation[at]^=1;
        Check(!TryBuiltinReadonlyQuery(mutation),"Malformed/unsupported opcode accepted");
        at+=count;
    }
    Check(next==output->size()&&removed==8&&stores==1&&discards==1,"Unexpected removal set or late-discard structure");
    Check(!TryBuiltinReadonlyQuery(*output),"Already transformed shader accepted");
    Check(!TryBuiltinReadonlyQuery({}),"Empty module accepted");
    for(uint32_t op:{62,99,227,259,318,319,5614,5615,6035,65534}) {
        auto v=original;v.push_back((1u<<16)|op);
        Check(!TryBuiltinReadonlyQuery(v),"Unknown write family accepted");
    }
    const auto vb=std::span(reinterpret_cast<const uint8_t*>(vs.data()),vs.size()*4);
    Check(AuditedReadonlyQueryVertex(kReadonlyQueryVertexSource,vb),"Audited pinned VS refused");
    Check(!AuditedReadonlyQueryVertex(std::string(64,'0'),vb),"Wrong source identity accepted");
    std::map<uint32_t,uint32_t> vs_pointer_storage,vs_values;
    unsigned output_stores=0,function_stores=0;
    for(size_t at=5;at<vs.size();) {
        const auto count=vs[at]>>16,op=vs[at]&65535;const auto* a=vs.data()+at+1;
        Check(count&&count<=vs.size()-at,"Pinned VS malformed instruction");
        if(op==32)vs_pointer_storage[a[0]]=a[1];
        if((op==59||op==65)&&vs_pointer_storage.contains(a[0]))vs_values[a[1]]=vs_pointer_storage.at(a[0]);
        if(op==62) {
            Check(vs_values.contains(a[0]),"Pinned VS store has unknown pointer provenance");
            const auto storage=vs_values.at(a[0]);
            Check(storage==3||storage==7,"Pinned VS has external storage write");
            output_stores+=storage==3;function_stores+=storage==7;
        }
        Check(op!=63&&op!=64&&op!=99&&!(op>=228&&op<=242)&&op!=259&&op!=274,
            "Pinned VS has external write/copy/atomic family");
        at+=count;
    }
    Check(output_stores>0&&function_stores>0,"Actual VS output/function-store proof is vacuous");
    vs.back()^=1;
    Check(!AuditedReadonlyQueryVertex(kReadonlyQueryVertexSource,vb),"Modified vertex module accepted");
    NativeFsiConstants r;r.pitch=640;r.depth_base=1440;r.depth_format=1;r.depth_control=0x700762;
    const auto allow=[&](const NativeFsiConstants& s,uint32_t w=640,uint32_t h=360,uint32_t mask=0,
                         bool pixel=false,bool query=true,bool vertex=true) {
        return BuiltinReadonlyQueryAllowed(s,w,h,mask,pixel,query,vertex);
    };
    Check(allow(r),"Pinned dynamic state refused");
    Check(!BuiltinReadonlyQueryAllowed(r,640,360,0,false,true,true,1u<<16)&&
          !BuiltinReadonlyQueryAllowed(r,640,360,0,false,true,true,2u<<16)&&
          !BuiltinReadonlyQueryAllowed(r,640,360,0,false,true,true,3u<<16),"Guest multisampling accepted");
    for(unsigned bit=0;bit<32;++bit) {auto s=r;s.depth_control^=1u<<bit;Check(!allow(s),"Changed control accepted");}
    for(unsigned bit=0;bit<32;++bit)Check(!allow(r,640,360,1u<<bit),"Raw color mask accepted");
    for(unsigned slot=0;slot<4;++slot)for(unsigned bit=0;bit<4;++bit) {
        auto s=r;s.color[slot].mask=1u<<bit;Check(!allow(s),"Effective color writer accepted");
    }
    Check(!allow(r,640,360,0,true)&&!allow(r,640,360,0,false,false)&&!allow(r,640,360,0,false,true,false),"Missing draw proof accepted");
    for(uint32_t v:{0u,2u,UINT32_MAX}) {auto s=r;s.depth_format=v;Check(!allow(s),"Unknown depth format accepted");}
    for(uint32_t v:{0u,8193u,UINT32_MAX})Check(!allow(r,v,360)&&!allow(r,640,v),"Invalid extent accepted");
    for(uint32_t v:{0u,559u,16384u,UINT32_MAX}) {auto s=r;s.pitch=v;Check(!allow(s),"Noninjective pitch accepted");}
    for(uint32_t v:{2048u,UINT32_MAX}) {auto s=r;s.depth_base=v;Check(!allow(s),"Invalid depth base accepted");}
    auto wrapped=r;wrapped.depth_base=2047;Check(allow(wrapped),"Injective wrapped base refused");
    auto wide=r;wide.pitch=8192;Check(!allow(wide,8192,8192),"Wrapped overlapping extent accepted");
    std::ofstream file(argv[3],std::ios::binary);Check(bool(file.write(reinterpret_cast<const char*>(output->data()),output->size()*4)),"Output failed");
    std::cout<<"PASS: "<<checks<<" exact transform, identity and dynamic-gate checks; "
        <<original.size()<<" -> "<<output->size()<<" words\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
