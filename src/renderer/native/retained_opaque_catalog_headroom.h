#pragma once
#include "retained_opaque_catalog.h"
#include <algorithm>
#include <cstdint>
namespace gow3::renderer {
// Startup admissions reserve only their actual metadata/byte occupancy at
// later soft trim. No resource is pinned; original hard pressure remains.
class NativeRetainedOpaqueCatalogHeadroom {
public:
    struct Targets {uint64_t modules,bytes,pipelines;};
    bool CanAdmitModule(uint64_t bytes)const noexcept {
        return bytes&&modules_<kRetainedOpaqueCatalogMaxPrograms&&bytes<=kRetainedOpaqueCatalogMaxModuleBytes-bytes_;
    }
    bool AdmitModule(uint64_t bytes)noexcept {
        if(!CanAdmitModule(bytes))return false;++modules_;bytes_+=bytes;return true;
    }
    bool CanAdmitPipeline()const noexcept {return pipelines_<kRetainedOpaqueCatalogMaxPipelines;}
    bool AdmitPipeline()noexcept {if(!CanAdmitPipeline())return false;++pipelines_;return true;}
    Targets SoftTargets(bool enabled,uint64_t modules,uint64_t bytes,uint64_t pipelines)const noexcept {
        const auto base=[](uint64_t v){return v/4*3+(v%4*3)/4;};
        Targets t{base(modules),base(bytes),base(pipelines)};
        if(enabled) {
            t.modules+=std::min(modules_,modules-t.modules);t.bytes+=std::min(bytes_,bytes-t.bytes);
            t.pipelines+=std::min(pipelines_,pipelines-t.pipelines);
        }
        return t;
    }
    uint64_t modules()const noexcept{return modules_;}
    uint64_t bytes()const noexcept{return bytes_;}
    uint64_t pipelines()const noexcept{return pipelines_;}
private:
    uint64_t modules_=0,bytes_=0,pipelines_=0;
};
}
