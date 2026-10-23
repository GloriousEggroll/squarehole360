#pragma once
#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <vector>
#include <optional>
#include "page_change_tracker.h"

namespace gow3::renderer {
// Exact last-writer versions, independent of byte ownership. A uniform block
// stores one value until a partial overwrite needs its individual pages. Max
// queries can then consume complete blocks without visiting every page.
// Assignment (not max-assignment) also preserves serial wrap and older values.
// Used only on the command processor thread; no GPU lifetime is represented.
class NativePageVersionTable {
public:
    static constexpr uint32_t kBlockPages=64;
    struct Statistics {
        uint64_t writes=0,reads=0,logical_written_pages=0,logical_read_pages=0;
        uint64_t uniform_writes=0,summary_reads=0,materialized_pages=0,
                 written_pages=0,scanned_pages=0;
    };
    void Reset(uint32_t pages,bool blocks,bool track_changes=false) {
        std::vector<uint64_t> values(pages);
        std::vector<Block> summaries(blocks?(pages+kBlockPages-1)/kBlockPages:0);
        std::optional<NativePageChangeTracker<>> changes;
        if(track_changes)changes.emplace(pages);
        values_=std::move(values);blocks_=std::move(summaries);changes_=std::move(changes);enabled_=blocks;stats_={};
        ++mutations_;
    }
    bool tracks_changes()const noexcept{return changes_.has_value();}
    uint64_t revision()const noexcept{return changes_?changes_->revision():0;}
    bool UnchangedSince(uint64_t revision,std::span<const NativePageChangeTracker<>::Range> ranges)const {
        return changes_&&changes_->UnchangedSince(revision,ranges);
    }
    uint32_t size()const noexcept{return uint32_t(values_.size());}
    // Changes on every Reset/Assign; equal counts mean every Max is unchanged.
    uint64_t mutations()const noexcept{return mutations_;}
    bool enabled()const noexcept{return enabled_;}
    const Statistics& statistics()const noexcept{return stats_;}
    void Assign(uint32_t first,uint32_t end,uint64_t version) {
        Check(first,end);
        ++mutations_;
        if(changes_)changes_->Write(first,end);
        if(!enabled_) {std::fill(values_.begin()+first,values_.begin()+end,version);return;}
        ++stats_.writes;stats_.logical_written_pages+=end-first;
        while(first<end) {
            const auto index=first/kBlockPages,start=index*kBlockPages;
            const auto limit=std::min(start+kBlockPages,size()),last=std::min(end,limit);
            auto& block=blocks_[index];
            if(first==start&&last==limit) {
                block.maximum=version;block.uniform=true;++stats_.uniform_writes;
            } else if(!block.uniform||block.maximum!=version) {
                if(block.uniform) {
                    std::fill(values_.begin()+start,values_.begin()+limit,block.maximum);
                    stats_.materialized_pages+=limit-start;block.uniform=false;
                }
                std::fill(values_.begin()+first,values_.begin()+last,version);
                stats_.written_pages+=last-first;
                if(version>=block.maximum)block.maximum=version;
                else block.maximum=*std::max_element(values_.begin()+start,values_.begin()+limit);
            }
            first=last;
        }
    }
    uint64_t Max(uint32_t first,uint32_t end)const {
        Check(first,end);
        uint64_t result=0;
        if(!enabled_) {
            for(auto i=first;i<end;++i)result=std::max(result,values_[i]);return result;
        }
        ++stats_.reads;stats_.logical_read_pages+=end-first;
        while(first<end) {
            const auto index=first/kBlockPages,start=index*kBlockPages;
            const auto limit=std::min(start+kBlockPages,size()),last=std::min(end,limit);
            const auto& block=blocks_[index];
            if(block.uniform||(first==start&&last==limit)) {
                result=std::max(result,block.maximum);++stats_.summary_reads;
            } else {
                for(auto i=first;i<last;++i)result=std::max(result,values_[i]);
                stats_.scanned_pages+=last-first;
            }
            first=last;
        }
        return result;
    }
private:
    void Check(uint32_t first,uint32_t end)const {
        if(first>end||end>size())throw std::out_of_range("Invalid native page version range");
    }
    struct Block {uint64_t maximum=0;bool uniform=true;};
    std::vector<uint64_t> values_;
    std::vector<Block> blocks_;
    std::optional<NativePageChangeTracker<>> changes_;
    bool enabled_=false;
    uint64_t mutations_=0;
    mutable Statistics stats_;
};
} // namespace gow3::renderer
