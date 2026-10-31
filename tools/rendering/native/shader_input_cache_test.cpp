#include "renderer/native/shader_input_cache.h"
#include "renderer/native/fast_shader_fingerprint.h"
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <string>

using namespace gow3::renderer;
namespace {
using Blocks=std::vector<GraphicsBoundaryBlock>;
// Run the existing independent legacy oracle unchanged in either real hash
// mode, including source mutation, eviction, malformed inputs and capture data.
struct TestCache:BasicShaderInputCache<RuntimeShaderInputFingerprint> {
    using Base=BasicShaderInputCache<RuntimeShaderInputFingerprint>;
    explicit TestCache(size_t entries=Base::kMaxEntries,size_t bytes=Base::kMaxRetainedBytes):Base(entries,bytes) {
        ConfigureFingerprint({std::getenv("GOW3_TEST_FAST_FINGERPRINTS")!=nullptr});
    }
};
void Require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
void Word(std::vector<uint8_t>& bytes,size_t offset,uint32_t value) {
    for(unsigned i=0;i<4;++i)bytes.at(offset+i)=uint8_t(value>>(24-i*8));
}
Blocks Shader(bool pixel,unsigned slot) {
    const unsigned kind=pixel?4:2;
    Blocks blocks{{kind,0x1000,std::vector<uint8_t>(pixel?68:72)},
        {kind+1,0x2000,std::vector<uint8_t>(24)}};
    auto& header=blocks[0].bytes;auto& code=blocks[1].bytes;
    Word(header,0,pixel?0x102a1100:0x102a1141);Word(header,4,header.size());Word(header,8,code.size());
    Word(header,24,36);Word(header,40,code.size());
    const uint64_t execute=(2ull<<44)|1|(1<<12)|(1<<16);
    Word(code,0,uint32_t(execute));Word(code,4,uint32_t(execute>>32));
    const unsigned base=slot==96?31:slot/3,component=slot==96?3:slot%3;
    Word(code,12,pixel?1|(slot<<20):(base<<20)|(component<<25));
    return blocks;
}
Blocks Draw(unsigned vertex=95,unsigned texture=31) {
    auto blocks=Shader(false,vertex);auto pixel=Shader(true,texture);
    blocks.insert(blocks.end(),pixel.begin(),pixel.end());return blocks;
}
template<class Cache>void Compare(Cache& cache,const Blocks& blocks) {
    // Leave the legacy concatenation/parser/set-union implementation unchanged
    // so both optional-result semantics and all selected slots are independent.
    const auto expected=SnapshotShaderInputs(blocks);const auto actual=cache.Get(blocks);
    Require(bool(expected)==bool(actual),"Cached shader selection changed optional/failure behavior");
    if(!expected)return;
    std::set<unsigned> vertices,textures;
    actual->ForEachVertex([&](unsigned slot){vertices.insert(slot);});
    actual->ForEachTexture([&](unsigned slot){textures.insert(slot);});
    Require(vertices==expected->first&&textures==expected->second,"Cached shader requirements differ from legacy sets");
    for(unsigned slot=0;slot<130;++slot) {
        Require(actual->HasVertex(slot)==expected->first.contains(slot),"Vertex capture membership differs from legacy");
        Require(actual->HasTexture(slot)==expected->second.contains(slot),"Texture capture membership differs from legacy");
    }
}
struct ConstantFingerprint {size_t operator()(const ShaderByteSegments&) const {return 0;}};
void Synthetic() {
    BasicShaderInputCache<ConstantFingerprint> collisions;
    for(unsigned vertex=0;vertex<=96;++vertex) {
        const auto blocks=Draw(vertex,vertex%32);Compare(collisions,blocks);Compare(collisions,blocks);
        Require(collisions.size()<=256&&collisions.retained_bytes()<=8u*1024*1024,"Collision cache exceeded fixed bounds");
    }
    Require(collisions.statistics().hits>0,"Collision fixture never used a cached decode");
    TestCache cache;
    auto blocks=Draw();Compare(cache,blocks);const auto decoded=cache.statistics().decodes;
    for(unsigned i=0;i<50;++i)Compare(cache,blocks);
    Require(cache.statistics().decodes==decoded&&cache.statistics().hits==100,"Identical bytes were reparsed or uncached");
    blocks[0].bytes[16]^=0x40;Compare(cache,blocks);
    Require(cache.statistics().decodes==decoded+1,"Changed header byte trusted previous decode");
    Word(blocks[1].bytes,12,30u<<20);Compare(cache,blocks);
    Require(cache.statistics().decodes==decoded+2,"Changed code byte trusted previous decode");
    blocks=Draw();Compare(cache,blocks);
    Require(cache.statistics().decodes==decoded+2,"Returning to prior exact bytes lost immutable cache entry");

    // Source vectors and the whole event disappear. Only owned immutable bytes
    // may back the later hit; returned masks stay valid after cache retirement.
    cache.Clear();std::optional<ShaderRequirementMasks> retained;
    {auto ephemeral=Draw(96,17);retained=cache.Get(ephemeral);}
    std::vector<std::vector<uint8_t>> overwritten(32,std::vector<uint8_t>(256,0xdd));
    Compare(cache,Draw(96,17));Require(cache.statistics().decodes==2,"Cache retained borrowed event bytes");
    cache.Clear();Require(cache.size()==0&&cache.retained_bytes()==0&&cache.statistics().decodes==0,
        "Bridge lifecycle clear retained bytes or counters");
    Require(retained&&retained->HasVertex(96)&&retained->HasTexture(17),"Cache clear invalidated returned masks");

    // A shader the analysis rejects (here an unlinked vertex shader, like the
    // game's memcpy kernel) is analyzed once; identical later draws get the
    // same full-capture answer without re-running and re-throwing.
    TestCache failures;auto unlinked_kernel=Draw();Word(unlinked_kernel[0].bytes,0,0x102a1101);
    for(unsigned i=0;i<20;++i)Compare(failures,unlinked_kernel);
    Require(failures.statistics().decodes==1&&failures.statistics().unsupported==1&&
            failures.statistics().unsupported_hits==19,"Unsupported shader was analyzed again");
    Compare(failures,Draw());
    Require(failures.statistics().decodes==3&&failures.statistics().unsupported==1,
            "A linked shader reused an unsupported entry");

    TestCache entries(3,1024);
    for(unsigned slot=0;slot<8;++slot) {
        Compare(entries,Shader(false,slot));
        Require(entries.size()<=3&&entries.retained_bytes()<=1024,"Entry pressure escaped bound");
    }
    Require(entries.statistics().evictions==6,"Entry pressure did not retire bounded immutable snapshots");
    TestCache bytes(256,150);
    Compare(bytes,Draw());Compare(bytes,Draw(94,30));
    Require(bytes.size()==1&&bytes.retained_bytes()==92&&bytes.statistics().evictions==3,
        "Byte pressure did not retire snapshots before admitting a new shader");
    TestCache oversized(256,40),disabled(0,1024);
    Compare(oversized,Draw());Compare(disabled,Draw());
    Require(!oversized.size()&&!oversized.retained_bytes()&&oversized.statistics().uncached==2&&
        !disabled.size()&&disabled.statistics().uncached==2,"Oversized/disabled cache changed valid uncached selection");

    std::vector<Blocks> edge_cases;
    edge_cases.push_back({});edge_cases.push_back(Shader(true,3));
    auto missing_code=Shader(false,2);missing_code.pop_back();edge_cases.push_back(missing_code);
    auto depth=Shader(false,4);edge_cases.push_back(depth);
    auto empty_pixel=Draw();empty_pixel[2].bytes.clear();edge_cases.push_back(empty_pixel);
    auto missing_pixel_code=Draw();missing_pixel_code.pop_back();edge_cases.push_back(missing_pixel_code);
    auto empty_code=Shader(false,5);empty_code[1].bytes.clear();Word(empty_code[0].bytes,8,0);Word(empty_code[0].bytes,40,0);
    edge_cases.push_back(empty_code); // Legacy accepts an empty valid program.
    auto truncated=Draw();truncated[0].bytes.resize(8);edge_cases.push_back(truncated);
    auto invalid_exec=Draw();Word(invalid_exec[1].bytes,0,(1<<12)|(1<<16));edge_cases.push_back(invalid_exec);
    auto invalid_pixel=Draw();Word(invalid_pixel[2].bytes,40,25);edge_cases.push_back(invalid_pixel);
    auto unlinked=Draw();Word(unlinked[0].bytes,0,0x102a1101);edge_cases.push_back(unlinked);
    auto duplicate=Draw();auto replacement=Shader(false,96);
    duplicate.insert(duplicate.end(),replacement.begin(),replacement.end());edge_cases.push_back(duplicate);
    // Existing decoder intentionally reads block.bytes, not another namespace.
    auto shared_only=Draw();shared_only[0].shared_bytes=std::make_shared<const std::vector<uint8_t>>(shared_only[0].bytes);
    shared_only[0].bytes.clear();edge_cases.push_back(shared_only);
    for(const auto& edge:edge_cases){Compare(cache,edge);Compare(cache,edge);}
    TestCache reconfigured;Compare(reconfigured,Draw(96,17));
    const auto held_masks=reconfigured.Get(Draw(96,17));
    reconfigured.ConfigureFingerprint({true});
    Require(!reconfigured.size()&&!reconfigured.statistics().hits,"Hash reconfiguration retained old buckets");
    Compare(reconfigured,Draw(96,17));
    reconfigured.ConfigureFingerprint({false});Compare(reconfigured,Draw(96,17));
    Require(held_masks&&held_masks->HasVertex(96)&&held_masks->HasTexture(17),"Hash reconfiguration invalidated retained masks");
    bool rejected=false;
    try {TestCache invalid(257,1024);}catch(const std::invalid_argument&){rejected=true;}
    Require(rejected,"Cache permitted an unbounded entry setting");
    std::cout<<"PASS: legacy selection oracle, all 97 vertex/32 texture slots, exact unchanged/changed bytes, "
        "forced collisions, source destruction, bounds, lifecycle, optional/malformed shaders and duplicate blocks\n";
}
uint32_t LE(std::span<const uint8_t> bytes,size_t offset) {
    if(offset>bytes.size()||bytes.size()-offset<4)throw std::runtime_error("Truncated capture word");
    return uint32_t(bytes[offset])|(uint32_t(bytes[offset+1])<<8)|(uint32_t(bytes[offset+2])<<16)|(uint32_t(bytes[offset+3])<<24);
}
void Capture(const char* path) {
    std::ifstream file(path,std::ios::binary|std::ios::ate);const auto size=file.tellg();
    Require(bool(file)&&size>=16&&uint64_t(size)<=256u*1024*1024,"Invalid shader input capture");
    std::vector<uint8_t> bytes(static_cast<size_t>(size));file.seekg(0);file.read(reinterpret_cast<char*>(bytes.data()),size);
    Require(bool(file)&&LE(bytes,8)==6,"Expected complete version-6 capture");
    std::map<uint32_t,std::pair<size_t,size_t>> references;
    TestCache cache;size_t cursor=16,events=0;
    while(cursor<bytes.size()) {
        const auto length=LE(bytes,cursor),function=LE(bytes,cursor+4);
        if(!function)break;
        Require(length>=60&&cursor+length<=bytes.size(),"Invalid captured event extent");
        Blocks blocks;size_t block_cursor=cursor+60;
        for(unsigned i=0;i<LE(bytes,cursor+56);++i) {
            const auto kind=LE(bytes,block_cursor),address=LE(bytes,block_cursor+4),extent=LE(bytes,block_cursor+8);
            block_cursor+=12;size_t first=block_cursor,count=extent;
            if(extent&0x80000000u) {const auto previous=references.at(extent&0x7fffffffu);first=previous.first;count=previous.second;}
            else {if(extent)references.emplace(uint32_t(block_cursor),std::pair{block_cursor,size_t(extent)});block_cursor+=extent;}
            Require(first<=bytes.size()&&count<=bytes.size()-first,"Captured shader reference exceeds input");
            if(kind>=2&&kind<=5)blocks.push_back({kind,address,{bytes.begin()+first,bytes.begin()+first+count}});
        }
        Require(block_cursor==cursor+length,"Captured blocks disagree with event extent");
        Compare(cache,blocks);Require(cache.size()<=256&&cache.retained_bytes()<=8u*1024*1024,"Capture cache exceeded bounds");
        ++events;cursor+=length;
    }
    const auto& stats=cache.statistics();
    std::cout<<"Capture PASS: "<<events<<" complete legacy comparisons; cache hits="<<stats.hits
        <<", decodes="<<stats.decodes<<", evictions="<<stats.evictions<<", entries="<<cache.size()
        <<", retained bytes="<<cache.retained_bytes()<<'\n';
}
}
int main(int argc,char** argv) try {
    Synthetic();if(argc==2)Capture(argv[1]);else Require(argc==1,"usage: shader-input-cache-test [capture.g3d]");
    return 0;
} catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
