#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <locale>
#include <ostream>
#include <string_view>
#include <vector>

namespace gow3::renderer {
// CP-owned samples; no clock, SDK, renderer, lock or GPU calls. The owner must
// stop every producer before Finish. No background writer or destructor I/O.
class NativeFrameJournal {
public:
    static constexpr size_t kCapacity=65536;
    enum Report : uint8_t {IntervalSummary=1,FrameDetails=2};
    struct Pressure {uint64_t drains=0,gpu_drains=0;double milliseconds=0;};
    struct Scene {
        bool available=false;uint32_t frame=0;
        uint64_t draws=0;double completion_ms=0;
        uint64_t cpu_uploads=0,cpu_payload_bytes=0,gpu_overlay_bytes=0;
        uint64_t overlay_copy_regions=0,overlay_readback_bytes=0,gpu_alias_loads=0;
        bool queue_available=false;
        uint64_t queue_submissions=0,queue_waits=0,queue_polls=0,queue_poll_retirements=0;
    };
    struct Row {
        uint64_t ordinal=0;uint32_t api_frame=0;int64_t steady_ns=0;
        bool interval_valid=false;double interval_ms=0;
        uint64_t operations=0;std::array<uint64_t,7> operation_counts{};
        uint64_t batch_completion_count=0;double batch_completion_ms=0;
        Pressure runtime_pressure,sdk_pressure;
        uint8_t reports=0,previous_reports=0;
        Scene scene;
        double query_wait_ms=0,direct_latency_us=0;uint64_t query_waits=0;
    };
    struct End {
        bool stop_requested=false,runtime_failed=false;
        uint64_t completed_frames=0;uint32_t last_completed_api_frame=UINT32_MAX;
    };
    enum class Result {Disabled,Written,AlreadyFinished,InitializationFailed,OpenFailed,WriteFailed};
    NativeFrameJournal()=default;
    NativeFrameJournal(const NativeFrameJournal&)=delete;
    NativeFrameJournal& operator=(const NativeFrameJournal&)=delete;
    // Reserve once at startup. Failure disables this diagnostic, never rendering.
    void Start(std::string_view path) noexcept {
        if(started_)return;
        started_=true;requested_=!path.empty();
        if(!requested_)return;
        try {path_=std::filesystem::path(path);rows_.reserve(kCapacity);}
        catch(...) {initialization_failed_=true;}
    }
    bool Requested() const noexcept {return requested_;}
    bool Active() const noexcept {return requested_&&!initialization_failed_&&!recording_failed_&&!finished_;}
    bool InitializationFailed() const noexcept {return initialization_failed_;}
    bool RecordingFailed() const noexcept {return recording_failed_;}
    uint64_t Observed() const noexcept {return observed_;}
    uint64_t Dropped() const noexcept {return dropped_;}
    const std::vector<Row>& Rows() const noexcept {return rows_;}
    void Record(Row row) noexcept {
        if(!Active())return;
        ++observed_;last_retained_=false;
        row.previous_reports=last_reports_;row.reports=0;last_reports_=0;
        const auto pressure_valid=[](const Pressure& p) {
            return p.gpu_drains<=p.drains&&std::isfinite(p.milliseconds)&&p.milliseconds>=0;
        };
        // Unavailable is explicit, never a stale frame or a measured zero.
        if(!row.scene.available)row.scene={};
        else if(!row.scene.queue_available) {
            row.scene.queue_submissions=row.scene.queue_waits=0;
            row.scene.queue_polls=row.scene.queue_poll_retirements=0;
        }
        if((row.scene.available&&(row.scene.frame!=row.api_frame||
             !std::isfinite(row.scene.completion_ms)||row.scene.completion_ms<0))||
           !std::isfinite(row.interval_ms)||row.interval_ms<0||
           !std::isfinite(row.batch_completion_ms)||row.batch_completion_ms<0||
           !pressure_valid(row.runtime_pressure)||!pressure_valid(row.sdk_pressure)) {
            recording_failed_=true;++dropped_;return;
        }
        if(rows_.size()==kCapacity){++dropped_;return;}
        try {rows_.push_back(row);last_retained_=true;}
        catch(...) {recording_failed_=true;++dropped_;}
    }
    // Called only from the existing report decision/branch, after Record. Full
    // buffers must not accidentally mark the final retained row for later frames.
    void MarkReport(Report report) noexcept {
        if(!Active()||!observed_)return;
        last_reports_|=uint8_t(report);
        if(last_retained_)rows_.back().reports=last_reports_;
    }
    bool Serialize(std::ostream& out,const End& end) const noexcept {
        try {
            out.imbue(std::locale::classic());out<<std::setprecision(std::numeric_limits<double>::max_digits10);
            out<<"# native-frame-journal-v2\n"
                "# reports,interval_summary=1,frame_details=2\n"
                "# scopes,operations_and_batch=per_api_frame,pressure=lifetime,interval=native_completion_callback_not_display\n"
                "# scene_scope,scene=completed_api_frame,queue=lifetime,completion=legacy_fence_wait_and_queue_entry_retirement,overlaps_runtime_completion,excludes_scene_callback_standalone_transfers_host_presentation\n"
                "ordinal,api_frame,steady_ns,interval_valid,interval_ms,operations,draw,resolve,clear,cpu_write,query,present,other,batch_completion_count,batch_completion_ms,runtime_pressure_drains,runtime_pressure_gpu_drains,runtime_pressure_ms,sdk_pressure_drains,sdk_pressure_gpu_drains,sdk_pressure_ms,reports,previous_reports,scene_available,scene_frame,scene_draws,scene_completion_ms,cpu_uploads,cpu_payload_bytes,gpu_overlay_bytes,overlay_copy_regions,overlay_readback_bytes,gpu_alias_loads,queue_available,queue_submissions,queue_waits,queue_polls,queue_poll_retirements,query_wait_ms,query_waits,direct_latency_us\n";
            for(const auto& r:rows_) {
                out<<r.ordinal<<','<<r.api_frame<<','<<r.steady_ns<<','<<int(r.interval_valid)<<','<<r.interval_ms<<','<<r.operations;
                for(const auto count:r.operation_counts)out<<','<<count;
                out<<','<<r.batch_completion_count<<','<<r.batch_completion_ms
                   <<','<<r.runtime_pressure.drains<<','<<r.runtime_pressure.gpu_drains<<','<<r.runtime_pressure.milliseconds
                   <<','<<r.sdk_pressure.drains<<','<<r.sdk_pressure.gpu_drains<<','<<r.sdk_pressure.milliseconds
                   <<','<<unsigned(r.reports)<<','<<unsigned(r.previous_reports)
                   <<','<<int(r.scene.available)<<','<<r.scene.frame<<','<<r.scene.draws<<','<<r.scene.completion_ms
                   <<','<<r.scene.cpu_uploads<<','<<r.scene.cpu_payload_bytes<<','<<r.scene.gpu_overlay_bytes
                   <<','<<r.scene.overlay_copy_regions<<','<<r.scene.overlay_readback_bytes<<','<<r.scene.gpu_alias_loads
                   <<','<<int(r.scene.queue_available)<<','<<r.scene.queue_submissions<<','<<r.scene.queue_waits
                   <<','<<r.scene.queue_polls<<','<<r.scene.queue_poll_retirements
                   <<','<<r.query_wait_ms<<','<<r.query_waits<<','<<r.direct_latency_us<<'\n';
            }
            out<<"# end,stored="<<rows_.size()<<",observed="<<observed_<<",dropped="<<dropped_
               <<",capacity="<<kCapacity<<",stop_requested="<<int(end.stop_requested)
               <<",runtime_failed="<<int(end.runtime_failed)<<",recording_failed="<<int(recording_failed_)
               <<",completed_frames="<<end.completed_frames<<",last_completed_api_frame="<<end.last_completed_api_frame<<'\n';
            return bool(out);
        }catch(...) {return false;}
    }
    Result Finish(const End& end) noexcept {
        if(finished_)return Result::AlreadyFinished;
        finished_=true;
        if(!requested_)return Result::Disabled;
        if(initialization_failed_)return Result::InitializationFailed;
        try {
            // C++23 exclusive creation. Never truncate an existing path, follow
            // a destination symlink, create parent directories or retry elsewhere.
            std::ofstream out(path_,std::ios::out|std::ios::binary|std::ios::noreplace);
            if(!out)return Result::OpenFailed;
            const bool written=Serialize(out,end);out.close();
            return written&&out?Result::Written:Result::WriteFailed;
        }catch(...) {return Result::WriteFailed;}
    }
    static const char* Name(Result result) noexcept {
        switch(result) {
        case Result::Disabled:return "disabled";
        case Result::Written:return "written";
        case Result::AlreadyFinished:return "already_finished";
        case Result::InitializationFailed:return "initialization_failed";
        case Result::OpenFailed:return "open_failed";
        case Result::WriteFailed:return "write_failed";
        }
        return "unknown";
    }
private:
    std::filesystem::path path_;
    std::vector<Row> rows_;
    uint64_t observed_=0,dropped_=0;
    uint8_t last_reports_=0;
    bool started_=false,requested_=false,initialization_failed_=false,
         recording_failed_=false,finished_=false,last_retained_=false;
};
} // namespace gow3::renderer
