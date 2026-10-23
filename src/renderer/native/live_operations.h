#pragma once
#include "live_draw_json.h"
#include <variant>
#include <cstddef>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

namespace gow3::renderer {
// A published operation is immutable. Packet/query prefixes share its owner
// with the completed frame, without cloning its nested JSON objects or retaining
// unrelated frame operations. Each list has its own pointer array, so appending
// on the producer cannot invalidate a consumer's iterators or references.
class LiveOperation {
public:
    explicit LiveOperation(nlohmann::json value):value_(std::move(value)){}
    explicit LiveOperation(TypedLiveDraw value):value_(std::make_unique<const TypedLiveDraw>(std::move(value))){}
    explicit LiveOperation(TypedLiveQuery value):value_(std::make_unique<const TypedLiveQuery>(std::move(value))){}
    const TypedLiveDraw* Draw()const noexcept {
        const auto* owner=std::get_if<std::unique_ptr<const TypedLiveDraw>>(&value_);
        return owner?owner->get():nullptr;
    }
    // Typed query_begin/query_end from the live planner. Offline/test JSON
    // queries remain Control() envelopes; use the Query* accessors for both.
    const TypedLiveQuery* Query()const noexcept {
        const auto* owner=std::get_if<std::unique_ptr<const TypedLiveQuery>>(&value_);
        return owner?owner->get():nullptr;
    }
    // JSON control envelopes only. Throws std::bad_variant_access for typed ops.
    const nlohmann::json& Control()const{return std::get<nlohmann::json>(value_);}
    std::string_view Kind()const {
        if(Draw())return "draw";
        if(const auto* query=Query())return query->begin?"query_begin":"query_end";
        return Control().at("type").get_ref<const std::string&>();
    }
    uint64_t Event()const{if(Draw())return Draw()->event;if(Query())return Query()->event;return Control().at("event").get<uint64_t>();}
    uint32_t Frame()const{if(Draw())return Draw()->frame;if(Query())return Query()->frame;return Control().at("frame").get<uint32_t>();}
    uint32_t QueryHandle()const{return Query()?Query()->handle:Control().at("query_handle").get<uint32_t>();}
    uint32_t QueryReportAddress()const{return Query()?Query()->report_address:Control().at("report_address").get<uint32_t>();}
    uint64_t QueryGeneration()const{return Query()?Query()->generation:Control().at("query_generation").get<uint64_t>();}
    nlohmann::json ToJson()const {
        if(Draw())return gow3::renderer::ToJson(*Draw());
        if(Query())return gow3::renderer::ToJson(*Query());
        return Control();
    }
private:
    // Keep query/control envelopes compact instead of reserving the much
    // larger draw packet inline in every control operation.
    std::variant<nlohmann::json,std::unique_ptr<const TypedLiveDraw>,std::unique_ptr<const TypedLiveQuery>> value_;
};

class LiveOperations {
    using Storage=std::vector<std::shared_ptr<const LiveOperation>>;
public:
    class const_iterator {
    public:
        using iterator_category=std::forward_iterator_tag;
        using value_type=LiveOperation;
        using difference_type=std::ptrdiff_t;
        using pointer=const value_type*;
        using reference=const value_type&;
        const_iterator()=default;
        explicit const_iterator(Storage::const_iterator value):value_(value){}
        reference operator*() const {return **value_;}
        pointer operator->() const {return value_->get();}
        const_iterator& operator++(){++value_;return *this;}
        const_iterator operator++(int){auto previous=*this;++*this;return previous;}
        bool operator==(const const_iterator&) const=default;
    private:
        Storage::const_iterator value_;
    };
    void push_back(nlohmann::json operation) {
        values_.push_back(std::make_shared<const LiveOperation>(std::move(operation)));
    }
    void push_back(TypedLiveDraw operation) {
        values_.push_back(std::make_shared<const LiveOperation>(std::move(operation)));
    }
    void push_back(TypedLiveQuery operation) {
        values_.push_back(std::make_shared<const LiveOperation>(std::move(operation)));
    }
    size_t size() const {return values_.size();}
    bool empty() const {return values_.empty();}
    const LiveOperation& at(size_t index) const {return *values_.at(index);}
    const LiveOperation& operator[](size_t index) const {return *values_[index];}
    const LiveOperation& back() const {return *values_.back();}
    const_iterator begin() const {return const_iterator(values_.begin());}
    const_iterator end() const {return const_iterator(values_.end());}
    LiveOperations Slice(size_t first,size_t end) const {
        if(first>end||end>size())throw std::out_of_range("Invalid live operation range");
        LiveOperations result;
        result.values_.assign(values_.begin()+first,values_.begin()+end);
        return result;
    }
    // Materialize only for failure diagnostics/offline comparison. Rendering
    // consumes const operations directly and never needs this deep copy.
    nlohmann::json ToJson() const {
        auto result=nlohmann::json::array();
        result.get_ref<nlohmann::json::array_t&>().reserve(size());
        for(const auto& operation:*this)result.push_back(operation.ToJson());
        return result;
    }
private:
    Storage values_;
};
} // namespace gow3::renderer
