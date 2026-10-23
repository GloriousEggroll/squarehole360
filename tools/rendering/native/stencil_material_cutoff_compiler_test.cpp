#include "renderer/native/stencil_material_cutoff_compiler.h"
#include "renderer/native/content_hash.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <tuple>
#include <sstream>
#include <stdexcept>
using namespace gow3::renderer;
namespace {
uint64_t checks=0;
void Check(bool value,const char* message){++checks;if(!value)throw std::runtime_error(message);}
std::vector<uint8_t> Read(const std::filesystem::path& path){std::ifstream input(path,std::ios::binary|std::ios::ate);if(!input)throw std::runtime_error("Missing input: "+path.string());const auto n=static_cast<std::streamoff>(input.tellg());if(n<36||n>1024*1024)throw std::runtime_error("Input extent");std::vector<uint8_t> bytes(static_cast<size_t>(n));input.seekg(0);if(!input.read(reinterpret_cast<char*>(bytes.data()),n))throw std::runtime_error("Input read");return bytes;}
std::string Hash(std::span<const uint32_t> words){return ContentHash({reinterpret_cast<const uint8_t*>(words.data()),words.size_bytes()});}
void Write(const std::filesystem::path& path,std::span<const uint32_t> words){std::ofstream output(path,std::ios::binary|std::ios::trunc);output.write(reinterpret_cast<const char*>(words.data()),words.size_bytes());if(!output)throw std::runtime_error("Output write");}
void Refusal(NativeShaderCompiler& compiler,std::span<const uint8_t> source,NativeStencilMaterialMode mode){bool refused=false;try{CompileNativeStencilMaterial(compiler,source,mode);}catch(const std::exception&){refused=true;}Check(refused,"Foreign/malformed container or mode was accepted");}
void DerivedRefusals(const std::vector<uint32_t>& original,NativeStencilMaterialMode mode){
 Check(NativeStencilMaterialDerivedCertified(original,mode),"Derived certificate expected");Check(!NativeStencilMaterialCanonicalCertified(original,false),"Derived cannot use canonical identity");
 for(size_t word:{size_t(0),size_t(1),size_t(4)}){auto changed=original;changed[word]^=1;Check(!NativeStencilMaterialDerivedCertified(changed,mode),"Bad SPIRV header refused");}
 auto malformed=original;malformed[5]&=65535;Check(!NativeStencilMaterialDerivedCertified(malformed,mode),"Zero instruction extent refused");malformed=original;malformed.resize(6);Check(!NativeStencilMaterialDerivedCertified(malformed,mode),"Truncated instruction refused");
 for(const auto pair:std::array<std::array<uint32_t,2>,7>{{{213,214},{214,213},{88,87},{234,242},{5364,0},{225,0},{54,57}}}){
  if(pair[0]==234&&mode==NativeStencilMaterialMode::Uninstrumented)continue;
  auto changed=original;bool found=false;
  for(size_t i=5;i<changed.size();i+=changed[i]>>16)if((changed[i]&65535)==pair[0]){changed[i]=(changed[i]&0xffff0000)|pair[1];found=true;break;}
  Check(found,"Expected structural instruction exists");Check(!NativeStencilMaterialDerivedCertified(changed,mode),"Forbidden structural change refused");
 }
 for(uint32_t opcode:{57u,63u,64u,99u,333u,227u,242u}){
  // Each independent forbidden-instruction carrier is inserted after the header;
  // this is a structural refusal test, not a claim of valid alternate SPIR-V.
  auto changed=original;changed.insert(changed.begin()+5,(1u<<16)|opcode);
  Check(!NativeStencilMaterialDerivedCertified(changed,mode),"Calls/group instruction refused");
 }
}
// Independently compare the complete descriptor ABI, rather than relying only
// on the candidate certificate's EDRAM binding check.
std::vector<std::tuple<uint32_t,uint32_t,uint32_t>> Descriptors(const std::vector<uint32_t>& words) {
 struct D {uint32_t set=UINT32_MAX,binding=UINT32_MAX,storage=UINT32_MAX;};
 std::map<uint32_t,D> ids;
 for(size_t i=5;i<words.size();i+=words[i]>>16){const auto n=words[i]>>16,op=words[i]&65535;
  if(op==71&&n==4&&(words[i+2]==33||words[i+2]==34)) {
   auto& d=ids[words[i+1]];(words[i+2]==33?d.binding:d.set)=words[i+3];
  }
  if(op==59&&n>=4)ids[words[i+2]].storage=words[i+3];
 }
 std::vector<std::tuple<uint32_t,uint32_t,uint32_t>> result;
 for(const auto& [id,d]:ids)if(d.set!=UINT32_MAX||d.binding!=UINT32_MAX){
  Check(d.set!=UINT32_MAX&&d.binding!=UINT32_MAX&&d.storage!=UINT32_MAX,"Complete descriptor variable identity");
  result.emplace_back(d.set,d.binding,d.storage);
 }
 std::sort(result.begin(),result.end());return result;
}
void UninstrumentedAbi(const std::vector<uint32_t>& original,const std::vector<uint32_t>& words) {
 Check(Descriptors(original)==Descriptors(words),"Uninstrumented full descriptor ABI unchanged");
 const std::string_view raw(reinterpret_cast<const char*>(words.data()),words.size()*sizeof(uint32_t));
 Check(raw.find("gow3StencilMaterialCounters")==raw.npos,"No diagnostic counter resource name");
 bool set4=false,binding_tested=false;uint32_t edram=UINT32_MAX;unsigned atomics=0;
 for(size_t i=5;i<words.size();i+=words[i]>>16){const auto n=words[i]>>16,op=words[i]&65535;
  atomics+=op>=227&&op<=242;
  if(op==71&&n==4&&words[i+2]==34&&words[i+3]==4){Check(!set4,"One EDRAM descriptor");set4=true;edram=words[i+1];}
 }
 Check(atomics==0&&set4,"Mode3 has zero atomics and retains EDRAM");
 for(size_t i=5;i<words.size();i+=words[i]>>16)if((words[i]&65535)==71&&(words[i]>>16)==4&&words[i+1]==edram&&words[i+2]==33){
  Check(words[i+3]==0,"Only original EDRAM binding0");auto changed=words;changed[i+3]=1;
  Check(!NativeStencilMaterialDerivedCertified(changed,NativeStencilMaterialMode::Uninstrumented),"Counter binding1 ABI refused");binding_tested=true;
 }
 Check(binding_tested,"EDRAM binding mutation exercised");
}
void AuthenticationRefusals(const NativeStencilMaterialProgram& program,const std::vector<uint32_t>& canonical) {
 Check(program.Authenticated(),"Compiler-issued full-byte program authentication");
 auto copy=program;Check(copy.Authenticated(),"Unchanged immutable program copy authenticates");
 for(unsigned field=0;field<8;++field){copy=program;
  switch(field){
   case 0:copy.source[0]^=1;break;
   case 1:copy.canonical_version[0]^=1;break;
   case 2:copy.derived_version[0]^=1;break;
   case 3:copy.specialization_mask^=1;break;
   case 4:copy.mode=NativeStencilMaterialMode::Disabled;break;
   case 5:copy.spirv[copy.spirv.size()/2]^=1;break;
   case 6:copy.spirv=canonical;break;
   case 7:copy.spirv.pop_back();break;
  }
  Check(!copy.Authenticated(),"Mutated program identity/code cannot retain compiler certificate");
 }
 NativeStencilMaterialProgram spoof;spoof.mode=program.mode;spoof.source=program.source;
 spoof.canonical_version=program.canonical_version;spoof.derived_version=program.derived_version;
 spoof.specialization_mask=program.specialization_mask;spoof.spirv=program.spirv;
 Check(!spoof.Authenticated(),"Public field reconstruction lacks private compiler certificate");
}
void JsonProgram(std::ostream& out,const NativeStencilMaterialProgram& program,const char* name){out<<"{\"name\":"<<std::quoted(name)<<",\"mode\":"<<uint32_t(program.mode)<<",\"source_sha256\":"<<std::quoted(program.source)<<",\"canonical_version\":"<<std::quoted(program.canonical_version)<<",\"derived_version\":"<<std::quoted(program.derived_version)<<",\"specialization_mask\":"<<program.specialization_mask<<",\"spirv_bytes\":"<<program.spirv.size()*4<<",\"spirv_sha256\":"<<std::quoted(Hash(program.spirv))<<'}';}
}
int main(int argc,char** argv)try{
 if(argc!=5)throw std::runtime_error("Usage: compiler-test exact-5dcdf6.bin exact-3216ee.bin excluded-fb9aabe.bin fresh-output-directory");
 const auto pixel=Read(argv[1]),vertex=Read(argv[2]),excluded=Read(argv[3]);const auto original_pixel=pixel,original_vertex=vertex,original_excluded=excluded;
 Check(ContentHash(pixel)==kStencilMaterialPixel&&pixel.size()==764,"Actual pinned PS container");Check(ContentHash(vertex)==kStencilMaterialVertex&&vertex.size()==220,"Actual pinned VS container");Check(ContentHash(excluded)=="fb9aabe4c8ab08d39dcdbb8f85f5b156b427df661e9f7e97e5a54a9beb8b60c9","Actual implicit-LOD exclusion container");
 const std::filesystem::path output=argv[4];if(std::filesystem::exists(output))throw std::runtime_error("Output must be a fresh owned directory");std::filesystem::create_directories(output);
 NativeShaderCompiler compiler(output/"canonical-cache");
 Check(NativeShaderCompiler::CacheVersion()==kStencilMaterialCanonicalVersion,"Original compiler version pinned");Check(NativeStencilMaterialCompilerVersion()!=kStencilMaterialCanonicalVersion,"Derived compiler namespace separate");
 const auto vs=compiler.Get(vertex,NativeShaderMode::Raster),ps=compiler.Get(pixel,NativeShaderMode::FragmentInterlock);
 Check(vs&&vs->stage==NativeShaderStage::Vertex&&vs->source_sha256==kStencilMaterialVertex&&NativeStencilMaterialCanonicalCertified(vs->spirv,true),"Actual canonical VS certificate");
 Check(ps&&ps->stage==NativeShaderStage::Pixel&&ps->mode==NativeShaderMode::FragmentInterlock&&ps->source_sha256==kStencilMaterialPixel&&NativeStencilMaterialCanonicalCertified(ps->spirv,false),"Actual canonical PS certificate");
 Check(!NativeStencilMaterialCanonicalCertified(vs->spirv,false)&&!NativeStencilMaterialCanonicalCertified(ps->spirv,true),"Wrong shader stage refused");Check(!NativeStencilMaterialDerivedCertified(ps->spirv),"Canonical PS cannot claim derived structure");
 const auto original_vs=vs->spirv,original_ps=ps->spirv;
 for(const auto* module:{&original_vs,&original_ps})for(size_t index:{size_t(0),size_t(1),module->size()/2,module->size()-1}){auto changed=*module;changed[index]^=1;Check(!NativeStencilMaterialCanonicalCertified(changed,module==&original_vs),"Mutated canonical module refused");}
 Refusal(compiler,{},NativeStencilMaterialMode::Cutoff);Refusal(compiler,vertex,NativeStencilMaterialMode::Cutoff);Refusal(compiler,excluded,NativeStencilMaterialMode::Cutoff);Refusal(compiler,excluded,NativeStencilMaterialMode::Uninstrumented);Refusal(compiler,vertex,NativeStencilMaterialMode::Uninstrumented);Refusal(compiler,pixel,NativeStencilMaterialMode::Disabled);Refusal(compiler,pixel,static_cast<NativeStencilMaterialMode>(99));
 auto malformed=pixel;malformed.pop_back();Refusal(compiler,malformed,NativeStencilMaterialMode::Cutoff);malformed=pixel;malformed.push_back(0);Refusal(compiler,malformed,NativeStencilMaterialMode::Observe);
 for(size_t i=0;i<pixel.size();++i){auto changed=pixel;changed[i]^=1;Refusal(compiler,changed,NativeStencilMaterialMode::Cutoff);Refusal(compiler,changed,NativeStencilMaterialMode::Uninstrumented);}
 const auto observe=CompileNativeStencilMaterial(compiler,pixel,NativeStencilMaterialMode::Observe),cutoff=CompileNativeStencilMaterial(compiler,pixel,NativeStencilMaterialMode::Cutoff),uninstrumented=CompileNativeStencilMaterial(compiler,pixel,NativeStencilMaterialMode::Uninstrumented);
 for(const auto& candidate:{observe,cutoff,uninstrumented}){
  Check(candidate&&candidate->source==ps->source_sha256&&candidate->canonical_version==kStencilMaterialCanonicalVersion&&candidate->derived_version==NativeStencilMaterialCompilerVersion()&&candidate->specialization_mask==ps->specialization_mask,"Derived source/version/specialization ABI");Check(candidate->spirv!=original_ps,"Derived module distinct");DerivedRefusals(candidate->spirv,candidate->mode);AuthenticationRefusals(*candidate,original_ps);
 }
 Check(observe->mode==NativeStencilMaterialMode::Observe&&cutoff->mode==NativeStencilMaterialMode::Cutoff&&observe->spirv!=cutoff->spirv,"Modes produce distinct modules");
 Check(uninstrumented->mode==NativeStencilMaterialMode::Uninstrumented&&uninstrumented->spirv!=observe->spirv&&uninstrumented->spirv!=cutoff->spirv,"Mode3 namespace and module distinct");
 Check(!NativeStencilMaterialDerivedCertified(original_ps,NativeStencilMaterialMode::Uninstrumented),"Canonical bytes under mode3 identity refused");
 Check(!NativeStencilMaterialDerivedCertified(uninstrumented->spirv),"Uninstrumented module cannot claim diagnostic counters");
 Check(!NativeStencilMaterialDerivedCertified(observe->spirv,NativeStencilMaterialMode::Uninstrumented)&&!NativeStencilMaterialDerivedCertified(cutoff->spirv,NativeStencilMaterialMode::Uninstrumented),"Counter-bearing modules cannot claim mode3");
 UninstrumentedAbi(original_ps,uninstrumented->spirv);
 const auto again=CompileNativeStencilMaterial(compiler,pixel,NativeStencilMaterialMode::Cutoff);Check(again->spirv==cutoff->spirv,"Repeated compile deterministic");
 NativeShaderCompiler warm(output/"canonical-cache");const auto warm_vs=warm.Get(vertex,NativeShaderMode::Raster),warm_ps=warm.Get(pixel,NativeShaderMode::FragmentInterlock);Check(warm_vs->disk_cache_hit&&warm_ps->disk_cache_hit&&warm_vs->spirv==original_vs&&warm_ps->spirv==original_ps,"Canonical disk cache untouched");
 const auto warm_cutoff=CompileNativeStencilMaterial(warm,pixel,NativeStencilMaterialMode::Cutoff);Check(warm_cutoff->spirv==cutoff->spirv,"Warm canonical provider keeps derived identity");
 const auto warm_uninstrumented=CompileNativeStencilMaterial(warm,pixel,NativeStencilMaterialMode::Uninstrumented);Check(warm_uninstrumented->spirv==uninstrumented->spirv,"Warm mode3 deterministic with canonical cache unchanged");
 Check(compiler.Get(vertex)->spirv==original_vs&&compiler.Get(pixel,NativeShaderMode::FragmentInterlock)->spirv==original_ps,"Canonical provider output unchanged");Check(pixel==original_pixel&&vertex==original_vertex&&excluded==original_excluded,"Caller byte containers untouched");
 Write(output/(std::string(kStencilMaterialVertex)+".canonical.spv"),original_vs);Write(output/(std::string(kStencilMaterialPixel)+".canonical.fsi.spv"),original_ps);Write(output/(std::string(kStencilMaterialPixel)+".observe.fsi.spv"),observe->spirv);Write(output/(std::string(kStencilMaterialPixel)+".cutoff.fsi.spv"),cutoff->spirv);Write(output/(std::string(kStencilMaterialPixel)+".uninstrumented.fsi.spv"),uninstrumented->spirv);
 std::ofstream manifest(output/"metadata.json");manifest<<"{\"passed\":true,\"checks\":"<<checks<<",\"canonical_vertex_bytes\":"<<original_vs.size()*4<<",\"canonical_vertex_sha256\":"<<std::quoted(Hash(original_vs))<<",\"canonical_vertex_specialization_mask\":"<<vs->specialization_mask<<",\"canonical_pixel_bytes\":"<<original_ps.size()*4<<",\"canonical_pixel_sha256\":"<<std::quoted(Hash(original_ps))<<",\"canonical_pixel_specialization_mask\":"<<ps->specialization_mask<<",\"programs\":[";JsonProgram(manifest,*observe,"observe");manifest<<',';JsonProgram(manifest,*cutoff,"cutoff");manifest<<',';JsonProgram(manifest,*uninstrumented,"uninstrumented");manifest<<"],\"GPU_runs\":0}\n";Check(bool(manifest),"Metadata write");
 std::cout<<"PASS: "<<checks<<" actual stencil-material source/compiler/canonical-cache/refusal/structural checks; output="<<output.string()<<"; physical GPU parity remains separate\n";
}catch(const std::exception& error){std::cerr<<"FAIL after "<<checks<<": "<<error.what()<<'\n';return 1;}
