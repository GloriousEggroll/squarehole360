#pragma once

#include <nlohmann/json.hpp>
#include <picosha2.h>
#include <array>
#include <cstdio>
#include <filesystem>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

namespace gow3::renderer {

// CPU-only diagnostics. These bytes have already been fetched for decoding;
// probing never initiates a GPU readback or changes their ownership.
class NativeTextureProbe {
public:
    static constexpr uint64_t kMaxBytes=256ull*1024*1024;
    static constexpr size_t kMaxRecords=128,kMaxHashes=32;

    static std::set<std::string,std::less<>> Parse(
        const std::filesystem::path& directory,std::string_view hashes) {
        if(directory.empty()!=hashes.empty())
            throw std::invalid_argument("Texture probe requires both directory and payload hashes");
        std::set<std::string,std::less<>> result;
        if(hashes.empty())return result;
        size_t fields=0;
        for(size_t begin=0;;) {
            const auto end=hashes.find(',',begin);
            const auto hash=hashes.substr(begin,end==std::string_view::npos?end:end-begin);
            if(++fields>kMaxHashes||hash.size()!=64||
                hash.find_first_not_of("0123456789abcdef")!=std::string_view::npos)
                throw std::invalid_argument("Texture probe requires at most 32 lowercase SHA-256 payload hashes");
            result.emplace(hash);
            if(end==std::string_view::npos)break;
            begin=end+1;
        }
        return result;
    }

    NativeTextureProbe(std::filesystem::path directory,std::string_view hashes)
        :directory_(std::move(directory)),hashes_(Parse(directory_,hashes)) {
        if(hashes_.empty())return;
        try {
            if(!directory_.parent_path().empty())
                std::filesystem::create_directories(directory_.parent_path());
            if(!std::filesystem::create_directory(directory_))
                throw std::runtime_error("Texture probe directory must be new");
            enabled_=true;
        } catch(const std::exception& error) {Disable(error.what());}
    }

    bool Selected(std::string_view base_payload) const {
        return enabled_&&hashes_.contains(base_payload);
    }

    void Record(const std::array<uint32_t,6>& fetch,uint64_t version,
                uint64_t frame,uint64_t event,unsigned slot,
                std::string_view base_payload,std::string_view mip_payload,
                uint64_t overlaid_bytes,std::span<const uint8_t> base,
                std::span<const uint8_t> mips) noexcept {
        if(!Selected(base_payload))return;
        try {
            using Json=nlohmann::json;
            if(base.size()>kMaxBytes-bytes_||mips.size()>kMaxBytes-bytes_-base.size())
                throw std::runtime_error("Texture probe byte limit reached");
            const auto base_hash=picosha2::hash256_hex_string(base.begin(),base.end());
            const auto mip_hash=picosha2::hash256_hex_string(mips.begin(),mips.end());
            const auto identity=Json::array({fetch,base_hash,mip_hash}).dump();
            if(identities_.contains(identity))return;
            if(records_>=kMaxRecords)throw std::runtime_error("Texture probe record limit reached");
            const auto name="texture-"+std::to_string(records_);
            Json metadata={{"version",1},{"provenance","native_cpu_texture_after_gpu_overlay_before_decode"},
                {"fetch",fetch},{"memory_version",version},{"frame",frame},{"event",event},{"slot",slot},
                {"source_base_payload",base_payload.empty()?Json(nullptr):Json(base_payload)},
                {"source_mip_payload",mip_payload.empty()?Json(nullptr):Json(mip_payload)},
                {"overlaid_bytes",overlaid_bytes},
                {"base",{{"file",name+"-base.bin"},{"bytes",base.size()},{"sha256",base_hash}}},
                {"mips",{{"file",name+"-mips.bin"},{"bytes",mips.size()},{"sha256",mip_hash}}}};
            const auto json=metadata.dump(2)+"\n";
            if(base.size()>kMaxBytes-bytes_||mips.size()>kMaxBytes-bytes_-base.size()||
                json.size()>kMaxBytes-bytes_-base.size()-mips.size())
                throw std::runtime_error("Texture probe byte limit reached");
            WriteExclusive(directory_/(name+"-base.bin"),base);
            WriteExclusive(directory_/(name+"-mips.bin"),mips);
            // Metadata is the commit marker: consumers ignore incomplete files.
            WriteExclusive(directory_/(name+".json"),
                {reinterpret_cast<const uint8_t*>(json.data()),json.size()});
            bytes_+=base.size()+mips.size()+json.size();
            identities_.insert(identity);++records_;
        } catch(const std::exception& error) {Disable(error.what());}
        catch(...) {Disable("Unknown texture probe failure");}
    }

    nlohmann::json Status() const {
        return {{"directory",directory_.string()},{"enabled",enabled_},
            {"records",records_},{"bytes",bytes_},{"error",error_},
            {"max_records",kMaxRecords},{"max_bytes",kMaxBytes}};
    }

private:
    static void WriteExclusive(const std::filesystem::path& path,std::span<const uint8_t> bytes) {
#ifdef _WIN32
        FILE* file=_wfopen(path.c_str(),L"wbx");
#else
        FILE* file=std::fopen(path.c_str(),"wbx");
#endif
        if(!file)throw std::runtime_error("Cannot exclusively create texture probe file");
        const bool written=bytes.empty()||std::fwrite(bytes.data(),1,bytes.size(),file)==bytes.size();
        const bool closed=std::fclose(file)==0;
        if(!written||!closed)throw std::runtime_error("Cannot finish texture probe file");
    }
    void Disable(const char* reason) noexcept {
        enabled_=false;
        try {error_=reason;} catch(...) {}
        std::fprintf(stderr,"Native texture probe disabled: %s\n",reason);
    }
    std::filesystem::path directory_;
    std::set<std::string,std::less<>> hashes_,identities_;
    bool enabled_=false;
    uint64_t bytes_=0;
    size_t records_=0;
    std::string error_;
};
}
