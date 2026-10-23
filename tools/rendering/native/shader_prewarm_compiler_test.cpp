#include "renderer/native/shader_compiler.h"
#include "renderer/native/shader_prewarm_queue.h"
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>

using namespace gow3::renderer;
void Require(bool value,const char* message) {if(!value)throw std::runtime_error(message);}
auto Read(const std::filesystem::path& path) {
  std::ifstream file(path,std::ios::binary|std::ios::ate);
  Require(bool(file)&&file.tellg()>=0&&file.tellg()<=256*1024,"Invalid shader source");
  auto data=std::make_shared<std::vector<uint8_t>>(size_t(file.tellg()));file.seekg(0);
  Require(bool(file.read(reinterpret_cast<char*>(data->data()),data->size())),"Cannot read shader source");
  return std::shared_ptr<const std::vector<uint8_t>>(std::move(data));
}
int main(int argc,char** argv) {
  try {
    Require(argc==2,"Usage: shader-prewarm-compiler-test source/manifest.json");
    const std::filesystem::path manifest_path=argv[1];std::ifstream file(manifest_path);
    const auto manifest=nlohmann::json::parse(file);
    std::shared_ptr<const std::vector<uint8_t>> vertex,pixel;
    for(const auto& shader:manifest.at("shaders")) {
      const bool is_pixel=shader.at("stage")=="pixel";
      auto& destination=is_pixel?pixel:vertex;
      if(!destination)destination=Read(manifest_path.parent_path()/shader.at("path").get<std::string>());
      if(vertex&&pixel)break;
    }
    Require(vertex&&pixel,"Manifest requires both shader stages");
    NativeShaderCompiler serial,parallel;
    const auto expected_vs=serial.Get(*vertex);
    const auto expected_ps=serial.Get(*pixel);
    const auto expected_fsi=serial.Get(*pixel,NativeShaderMode::FragmentInterlock);
    NativeShaderPrewarmQueue queue([&](const auto& job){parallel.Get(*job.source,job.fragment_interlock?NativeShaderMode::FragmentInterlock:NativeShaderMode::Raster);},2);
    queue.TryEnqueue({expected_vs->source_sha256+".raster",vertex,false});
    queue.TryEnqueue({expected_ps->source_sha256+".raster",pixel,false});
    queue.TryEnqueue({expected_fsi->source_sha256+".fsi",pixel,true});
    // Concurrent actual draw Get is allowed before the worker starts its job.
    const auto concurrent_vs=parallel.Get(*vertex);
    queue.Drain();
    Require(concurrent_vs==parallel.Get(*vertex),"Actual Get lost shared-future identity");
    Require(concurrent_vs->spirv==expected_vs->spirv,"Prewarmed vertex output changed");
    Require(parallel.Get(*pixel)->spirv==expected_ps->spirv,"Prewarmed raster pixel output changed");
    Require(parallel.Get(*pixel,NativeShaderMode::FragmentInterlock)->spirv==expected_fsi->spirv,"Prewarmed FSI output changed");
    auto malformed=std::make_shared<const std::vector<uint8_t>>(35,0);
    queue.TryEnqueue({"malformed.raster",malformed,false});queue.Drain();
    Require(queue.statistics().failures==1,"Speculative malformed input was not isolated");
    bool rejected=false;try{parallel.Get(*malformed);}catch(const std::exception&){rejected=true;}
    Require(rejected,"Actual Get no longer validates malformed input");
    queue.StopAndJoin();const auto stats=queue.statistics();
    Require(stats.completed==3&&stats.failures==1&&!stats.outstanding&&!stats.retained_bytes,"Compiler queue did not retire every source");
    std::cout<<"Exact vertex/raster/FSI parity, concurrent real Get, malformed validation and clean shutdown passed\n";
  }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
