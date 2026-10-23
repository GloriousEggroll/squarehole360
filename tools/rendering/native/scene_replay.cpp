// File-backed diagnostic driver for the native scene executor.
#include "renderer/native/scene_executor.h"
#include <nlohmann/json.hpp>
#include <iostream>
#include <fstream>
#include <picosha2.h>
#include <stdexcept>
int main(int argc,char** argv) try {
    if(argc<4)throw std::runtime_error("Usage: gow3-native-scene-replay <capture directory> <compiled shaders> <output directory> [last event] [--diagnostic-zero-history] [--debug-raw-images] [--native-fsi] [--diagnostic-state=manifest --first-event=N]");
    uint32_t through=UINT32_MAX;
    std::filesystem::path diagnostic_state;
    uint32_t first_event=UINT32_MAX;
    gow3::renderer::SceneExecutorConfig config;
    config.capture=argv[1];config.compiled=argv[2];config.output=argv[3];
    for(int i=4;i<argc;++i) {
        if(std::string_view(argv[i])=="--diagnostic-zero-history")config.diagnostic_zero_history=true;
        else if(std::string_view(argv[i])=="--debug-raw-images")config.debug_raw_images=true;
        else if(std::string_view(argv[i])=="--native-fsi")config.native_fsi=true;
        else if(std::string_view(argv[i])=="--implicit-fsi-barriers")config.implicit_fsi_barriers=true;
        else if(std::string_view(argv[i])=="--builtin-atomic-depth")config.builtin_atomic_depth=true;
        else if(std::string_view(argv[i])=="--reset-draw-command-pool")config.reset_draw_command_pool=true;
        else if(std::string_view(argv[i])=="--recycle-vertex-buffers")config.recycle_vertex_buffers=true;
        else if(std::string_view(argv[i])=="--raw-gpu-float-cpu-textures")config.raw_gpu_float_cpu_textures=true;
        else if(std::string_view(argv[i])=="--forward-cpu-upload-receipts")config.forward_cpu_upload_receipts=true;
        else if(std::string_view(argv[i])=="--share-cpu-texture-sampling-variants")config.share_cpu_texture_sampling_variants=true;
        else if(std::string_view(argv[i])=="--time-draw-recording")config.time_draw_recording=true;
        else if(std::string_view(argv[i])=="--batch-draw-descriptors")config.batch_draw_descriptors=true;
        else if(std::string_view(argv[i])=="--packed-zero-depth-clear")config.packed_zero_depth_clear=true;
        else if(std::string_view(argv[i])=="--skip-full-color-clear-restore")config.skip_full_color_clear_restore=true;
        else if(std::string_view(argv[i])=="--batch-transfers")config.batch_transfers=true;
        else if(std::string_view(argv[i])=="--unified-gpu-queue") {
            config.batch_transfers=true;config.unified_gpu_queue=true;
        }
        else if(std::string_view(argv[i])=="--async-gpu-submissions") {
            config.batch_transfers=true;config.unified_gpu_queue=true;config.async_gpu_submissions=true;
        }
        else if(std::string_view(argv[i])=="--stencil-export-restores")config.restore_depth_stencil_export=true;
        else if(std::string_view(argv[i])=="--gpu-draw-timestamps") {
            config.gpu_draw_timestamps=true;config.unified_gpu_queue=true;config.batch_transfers=true;
        }
        else if(std::string_view(argv[i])=="--gpu-timestamps") {
            config.batch_transfers=true;config.unified_gpu_queue=true;config.gpu_timestamps=true;
        }
        else if(std::string_view(argv[i]).starts_with("--texture-probe-dir="))config.texture_probe_directory=argv[i]+20;
        else if(std::string_view(argv[i]).starts_with("--texture-probe-hashes="))config.texture_probe_hashes=argv[i]+23;
        else if(std::string_view(argv[i]).starts_with("--draw-batch=")) {
            size_t n=0;const auto text=std::string(argv[i]+13);const auto value=std::stoul(text,&n);
            if(n!=text.size()||value<1||value>64)throw std::runtime_error("Invalid draw batch size");
            config.max_draw_batch_size=uint32_t(value);
        }
        else if(std::string_view(argv[i]).starts_with("--probe-pixel=")) {
            const std::string text=argv[i]+14;const auto comma=text.find(',');
            if(comma==std::string::npos)throw std::runtime_error("Pixel probe requires x,y");
            std::array<uint32_t,2> pixel{};
            for(unsigned c=0;c<2;++c) {
                const auto field=c?text.substr(comma+1):text.substr(0,comma);size_t n=0;
                const auto value=std::stoul(field,&n);
                if(n!=field.size()||value>16383)throw std::runtime_error("Invalid diagnostic pixel");
                pixel[c]=uint32_t(value);
            }
            config.debug_pixels.push_back(pixel);
        }
        else if(std::string_view(argv[i]).starts_with("--anisotropy=")) {
            size_t n=0;const auto text=std::string(argv[i]+13);const int value=std::stoi(text,&n);
            if(n!=text.size()||value < -1||value>5)throw std::runtime_error("Invalid anisotropy override");
            config.anisotropy_override=[value]{return value;};
        }
        else if(std::string_view(argv[i]).starts_with("--diagnostic-state="))diagnostic_state=argv[i]+19;
        else if(std::string_view(argv[i]).starts_with("--first-event=")) {
            size_t n=0;const auto text=std::string(argv[i]+14);const auto value=std::stoull(text,&n);
            if(n!=text.size()||value>=UINT32_MAX)throw std::runtime_error("Invalid diagnostic first event");first_event=uint32_t(value);
        }
        else {size_t n=0;const auto value=std::stoull(argv[i],&n);if(argv[i][n]||value>UINT32_MAX)throw std::runtime_error("Invalid last event");through=uint32_t(value);}
    }
    if(diagnostic_state.empty()!=(first_event==UINT32_MAX)||(!diagnostic_state.empty()&&config.diagnostic_zero_history))
        throw std::runtime_error("Diagnostic reference state requires an explicit first event and real GPU history");
    const bool draw_timestamps=config.gpu_draw_timestamps;
    const auto capture=config.capture,output=config.output;
    gow3::renderer::SceneExecutor replay(std::move(config));
    if(diagnostic_state.empty())replay.RunCapture(through);
    else {
        using Json=nlohmann::json;
        std::ifstream stream(diagnostic_state);Json state;stream>>state;
        if(state.at("version")!=1||state.at("provenance")!="diagnostic_replayed_gpu_state")
            throw std::runtime_error("Unverified diagnostic reference state");
        auto load=[&](const Json& entry) {
            const auto hash=entry.at("sha256").get<std::string>();const auto size=entry.at("bytes").get<uint64_t>();
            if(hash.size()!=64||hash.find_first_not_of("0123456789abcdef")!=std::string::npos||size>512u*1024*1024)
                throw std::runtime_error("Invalid diagnostic payload metadata");
            std::ifstream file(diagnostic_state.parent_path()/(hash+".gpu.bin"),std::ios::binary|std::ios::ate);
            if(!file||file.tellg()!=size)throw std::runtime_error("Invalid diagnostic payload extent");
            std::vector<uint8_t> bytes(size);file.seekg(0);file.read(reinterpret_cast<char*>(bytes.data()),bytes.size());
            if(!file||picosha2::hash256_hex_string(bytes)!=hash)throw std::runtime_error("Invalid diagnostic payload hash");
            return bytes;
        };
        for(const auto& entry:state.at("ranges"))replay.InitializeMemory(entry.at("address"),load(entry));
        replay.InitializeEdram(load(state.at("edram")));
        std::ifstream plan_file(capture/"scene.json");Json plan;plan_file>>plan;
        if(plan.at("version")!=3)throw std::runtime_error("Diagnostic continuation requires scene plan3");
        bool found=false;
        for(const auto& operation:plan.at("operations")) {
            const uint32_t event=operation.at("event");
            if(operation.at("frame")!=state.at("frame")||event<first_event||event>through)continue;
            if(!found&&event!=first_event)throw std::runtime_error("Diagnostic first event is absent");
            found=true;
            for(const auto& issue:plan.at("issues"))if(issue.at("event")==event)throw std::runtime_error(issue.at("reason"));
            replay.Execute(operation);
        }
        if(!found)throw std::runtime_error("Diagnostic continuation range is empty");
        replay.FlushPendingDraws();
        auto report=replay.report();report["mode"]="diagnostic continuation with replayed reference inputs";
        report["valid_for_equivalence"]=false;report["diagnostic_first_event"]=first_event;
        report["reference_state_sha256"]=state.at("reference_state_sha256");
        report["source_trace_sha256"]=state.at("source_trace_sha256");
        std::ofstream result(output/"replay.json");result<<report.dump(2)<<'\n';
        if(!result)throw std::runtime_error("Cannot save diagnostic continuation report");
        std::cout<<report.dump(2)<<'\n';return 0;
    }
    if(draw_timestamps) {
        std::ofstream file(output/"gpu-draws.json");file<<replay.GpuDrawTimingReport(true).dump(2)<<'\n';
        if(!file)throw std::runtime_error("Cannot save draw timestamp report");
    }
    std::cout<<replay.report().dump(2)<<'\n';return 0;
} catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
