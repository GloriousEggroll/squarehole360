#include "renderer/native/frame_journal.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
using gow3::renderer::NativeFrameJournal;
using J=NativeFrameJournal;
namespace {
size_t checks=0;
void Check(bool value,const char* text) {++checks;if(!value)throw std::runtime_error(text);}
std::string Read(const std::filesystem::path& p) {std::ifstream f(p,std::ios::binary);return {std::istreambuf_iterator<char>(f),{}};}
J::Row Sample(uint64_t n) {
    J::Row r;r.ordinal=n;r.api_frame=uint32_t(2351+n);r.steady_ns=1000000000+int64_t(n)*14000000;
    r.interval_valid=n!=1;r.interval_ms=n==1?0:14;r.operations=28;r.operation_counts={1,2,3,4,5,6,7};
    r.batch_completion_count=2;r.batch_completion_ms=3.25;r.runtime_pressure={4,3,8.5};r.sdk_pressure={1,1,2.125};
    r.scene={true,r.api_frame,1,7.75,3,65536,128,2,256,11,true,100+n,90+n,80+n,7+n};return r;
}
struct FailingBuffer final:std::streambuf {
    std::streamsize xsputn(const char*,std::streamsize) override {return 0;}
    int_type overflow(int_type) override {return traits_type::eof();}
};
void Disabled() {
    J j;Check(!j.Active()&&!j.Requested()&&j.Rows().capacity()==0,"default disabled allocates no row buffer");
    j.Start("");j.Record(Sample(1));j.MarkReport(J::FrameDetails);
    Check(j.Rows().empty()&&!j.Observed(),"disabled record/mark are inert");
    Check(j.Finish({})==J::Result::Disabled,"disabled finish");
    Check(j.Finish({})==J::Result::AlreadyFinished,"finish is one-shot");
}
void CopiesMarkersAndSerialization(const std::filesystem::path& root) {
    const auto file=root/"samples.csv";J j;j.Start(file.string());Check(j.Active(),"enabled at startup");
    Check(!std::filesystem::exists(file),"no output before cold export");
    auto row=Sample(1);j.Record(row);row.operation_counts.fill(999);
    j.MarkReport(J::IntervalSummary);j.MarkReport(J::FrameDetails);j.Record(Sample(2));
    j.MarkReport(J::FrameDetails);j.Record(Sample(3));
    Check(j.Rows()[0].operation_counts[0]==1,"row owns values after caller mutation");
    Check(j.Rows()[0].reports==3&&j.Rows()[1].reports==2&&j.Rows()[2].reports==0,"exact two report branches");
    Check(j.Rows()[0].previous_reports==0&&j.Rows()[1].previous_reports==3&&j.Rows()[2].previous_reports==2,"previous flags follow record order");
    const J::End end{true,false,3,2354};std::ostringstream out;Check(j.Serialize(out,end),"serialize complete rows");
    const std::string first="1,2352,1014000000,0,0,28,1,2,3,4,5,6,7,2,3.25,4,3,8.5,1,1,2.125,3,0,1,2352,1,7.75,3,65536,128,2,256,11,1,101,91,81,8\n";
    Check(out.str().starts_with("# native-frame-journal-v2\n"),"v2 schema identified");
    Check(out.str().find("scene_available,scene_frame,scene_draws,scene_completion_ms,cpu_uploads,cpu_payload_bytes,gpu_overlay_bytes,overlay_copy_regions,overlay_readback_bytes,gpu_alias_loads,queue_available,queue_submissions,queue_waits,queue_polls,queue_poll_retirements\n")!=std::string::npos,"exact appended snapshot schema");
    Check(out.str().find("overlaps_runtime_completion,excludes_scene_callback_standalone_transfers_host_presentation")!=std::string::npos,"non-additive scope serialized");
    Check(out.str().find(first)!=std::string::npos,"first-row unavailable interval and exact count/ms fields");
    Check(out.str().find("2,2353,1028000000,1,14,")!=std::string::npos,"second actual interval");
    Check(out.str().ends_with("# end,stored=3,observed=3,dropped=0,capacity=65536,stop_requested=1,runtime_failed=0,recording_failed=0,completed_frames=3,last_completed_api_frame=2354\n"),"completion footer exact");
    Check(j.Finish(end)==J::Result::Written&&Read(file)==out.str(),"exclusive file equals standalone serializer");
    j.Record(Sample(4));j.MarkReport(J::FrameDetails);Check(j.Observed()==3&&!j.Active(),"closed journal cannot mutate");
    Check(j.Finish(end)==J::Result::AlreadyFinished&&Read(file)==out.str(),"repeat close cannot rewrite");
    J occupied;occupied.Start(file.string());occupied.Record(Sample(1));
    Check(occupied.Finish({})==J::Result::OpenFailed&&Read(file)==out.str(),"existing output is never truncated");
    std::error_code ec;std::filesystem::create_symlink(file,root/"link.csv",ec);
    if(!ec) {J linked;linked.Start((root/"link.csv").string());Check(linked.Finish({})==J::Result::OpenFailed&&Read(file)==out.str(),"exclusive create rejects destination symlink");}
    J no_parent;no_parent.Start((root/"missing/sub/file.csv").string());no_parent.Record(Sample(1));
    Check(no_parent.Finish({})==J::Result::OpenFailed&&!std::filesystem::exists(root/"missing"),"missing directory is a diagnostic error, no directory creation");
    J directory;directory.Start(root.string());Check(directory.Finish({})==J::Result::OpenFailed,"directory target refused");
}
void Availability(const std::filesystem::path& root) {
    J j;j.Start((root/"availability.csv").string());
    auto absent=Sample(1);absent.scene.available=false;
    absent.scene.completion_ms=std::numeric_limits<double>::quiet_NaN();j.Record(absent);
    Check(j.Active()&&j.Rows().size()==1,"unavailable snapshot preserves journal row");
    const auto& normalized=j.Rows()[0].scene;
    Check(!normalized.available&&!normalized.frame&&!normalized.draws&&normalized.completion_ms==0&&
        !normalized.cpu_uploads&&!normalized.cpu_payload_bytes&&!normalized.gpu_overlay_bytes&&
        !normalized.overlay_copy_regions&&!normalized.overlay_readback_bytes&&!normalized.gpu_alias_loads&&
        !normalized.queue_available&&!normalized.queue_submissions&&!normalized.queue_waits&&
        !normalized.queue_polls&&!normalized.queue_poll_retirements,"unavailable snapshot has no stale measurements");
    auto no_queue=Sample(2);no_queue.scene.queue_available=false;j.Record(no_queue);
    const auto& scene_only=j.Rows()[1].scene;
    Check(scene_only.available&&scene_only.draws==1&&scene_only.completion_ms==7.75&&
        !scene_only.queue_available&&!scene_only.queue_submissions&&!scene_only.queue_waits&&
        !scene_only.queue_polls&&!scene_only.queue_poll_retirements,"available frame survives unavailable queue counters");
    for(uint32_t frame:{0u,UINT32_MAX,7u}) {
        auto r=Sample(j.Observed()+1);r.api_frame=frame;r.scene.frame=frame;j.Record(r);
        Check(j.Active()&&j.Rows().back().scene.frame==frame,"frame identity supports zero/wrap/skips without ordinal assumptions");
    }
    std::ostringstream out;Check(j.Serialize(out,{true,false,5,7}),"mixed availability serialization");
    Check(out.str().find(",2.125,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0\n")!=std::string::npos,
        "unavailable zeros carry explicit flag in CSV");
    Check(out.str().find(",1,2353,1,7.75,3,65536,128,2,256,11,0,0,0,0,0\n")!=std::string::npos,
        "queue unavailable serialized separately from measured frame");
    for(int mode=0;mode<3;++mode) {
        J bad;bad.Start((root/("bad-scene-"+std::to_string(mode)+".csv")).string());auto row=Sample(1);
        if(mode==0)++row.scene.frame;
        if(mode==1)row.scene.completion_ms=-1;
        if(mode==2)row.scene.completion_ms=std::numeric_limits<double>::infinity();
        bad.Record(row);
        Check(bad.RecordingFailed()&&bad.Rows().empty()&&bad.Dropped()==1,"mismatched/nonfinite snapshot is explicit diagnostic failure");
        std::ostringstream failed;Check(bad.Serialize(failed,{false,false,1,row.api_frame})&&
            failed.str().find("recording_failed=1")!=std::string::npos,"snapshot failure cannot look complete");
    }
}
void Bounds(const std::filesystem::path& root) {
    J j;j.Start((root/"bounded.csv").string());j.Record(Sample(1));const auto* data=j.Rows().data();
    for(uint64_t i=2;i<=J::kCapacity;++i)j.Record(Sample(i));
    j.MarkReport(J::FrameDetails);const auto last=j.Rows().back();
    for(uint64_t i=1;i<=3;++i) {j.Record(Sample(J::kCapacity+i));j.MarkReport(J::IntervalSummary);}
    Check(j.Rows().data()==data&&j.Rows().size()==J::kCapacity,"no buffer growth at capacity");
    Check(j.Observed()==J::kCapacity+3&&j.Dropped()==3,"overflow counted, never overwrites prefix");
    Check(j.Rows().front().ordinal==1&&j.Rows().back().ordinal==J::kCapacity,"retained exact ordered prefix");
    Check(j.Rows().back().reports==last.reports,"dropped frame report never mutates retained tail");
    Check(j.Finish({true,false,J::kCapacity+3,9})==J::Result::Written,"bounded truncated export writes honest footer");
    const auto bytes=Read(root/"bounded.csv");
    Check(bytes.find("# end,stored=65536,observed=65539,dropped=3,capacity=65536,")!=std::string::npos,"truncation serialized explicitly");
}
void Errors(const std::filesystem::path& root) {
    for(int mode=0;mode<5;++mode) {
        J j;j.Start((root/("invalid-"+std::to_string(mode)+".csv")).string());auto r=Sample(1);
        if(mode==0)r.interval_ms=std::numeric_limits<double>::quiet_NaN();
        if(mode==1)r.batch_completion_ms=-1;
        if(mode==2)r.runtime_pressure.milliseconds=std::numeric_limits<double>::infinity();
        if(mode==3)r.sdk_pressure.gpu_drains=r.sdk_pressure.drains+1;
        if(mode==4)r.sdk_pressure.milliseconds=-1;
        j.Record(r);Check(j.RecordingFailed()&&!j.Active()&&j.Rows().empty()&&j.Dropped()==1,"bad samples disable diagnostic only");
        Check(j.Finish({true,true,1,2352})==J::Result::Written,"failed diagnostic exports explicit partial status");
        Check(Read(root/("invalid-"+std::to_string(mode)+".csv")).find("runtime_failed=1,recording_failed=1,")!=std::string::npos,"failure cannot look complete");
    }
    J j;j.Start((root/"stream.csv").string());auto r=Sample(1);r.operation_counts[0]=(uint64_t(1)<<63)+7;j.Record(r);
    std::ostringstream good;Check(j.Serialize(good,{}),"large counters serialize");
    Check(good.str().find("9223372036854775815")!=std::string::npos,"uint64 counts never round through double");
    FailingBuffer buffer;std::ostream bad(&buffer);bad.exceptions(std::ios::badbit|std::ios::failbit);
    Check(!j.Serialize(bad,{}),"stream exception caught as diagnostic error");
    Check(j.Active()&&j.Observed()==1,"serializer does not alter captured owner state");
    const auto unclosed=root/"not-finalized.csv";{J local;local.Start(unclosed.string());local.Record(Sample(1));}
    Check(!std::filesystem::exists(unclosed),"helper destructor alone cannot publish a false orderly completion");
}
}
int main(int argc,char** argv) {
    try {
        Check(argc==2,"pass a fresh report-local output directory");const std::filesystem::path root=argv[1];
        Check(std::filesystem::create_directory(root),"test output must not exist");
        Disabled();CopiesMarkersAndSerialization(root);Availability(root);Bounds(root);Errors(root);
        std::cout<<"PASS "<<checks<<" frame journal checks; no SDK/GPU\n";return 0;
    }catch(const std::exception& error){std::cerr<<"FAIL "<<error.what()<<'\n';return 1;}
}
