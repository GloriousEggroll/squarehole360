#include "pipeline_prewarm.h"
#include "content_hash.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <bit>
#include <chrono>
#include <fstream>
#include <map>
#include <stdexcept>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace gow3::renderer {
namespace {
using Json = nlohmann::json;
constexpr uint32_t kSchema = 1;
// Only the recorder holds this nonblocking lease. Concurrent readers use the
// complete atomic manifest; another game instance may simply miss persistence.
// OS handles release on process exit, so a crash leaves no stale lock token.
class CacheWriterLease {
  public:
    explicit CacheWriterLease(const std::filesystem::path &path) {
#ifdef _WIN32
        handle_ = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle_ == INVALID_HANDLE_VALUE)
            throw std::runtime_error("Observed pipeline writer is unavailable");
#else
        descriptor_ = open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
        if (descriptor_ < 0)
            throw std::runtime_error("Observed pipeline writer is unavailable");
        if (flock(descriptor_, LOCK_EX | LOCK_NB) != 0) {
            close(descriptor_);
            descriptor_ = -1;
            throw std::runtime_error("Observed pipeline writer is busy");
        }
#endif
    }
    ~CacheWriterLease() {
#ifdef _WIN32
        if (handle_ != INVALID_HANDLE_VALUE)
            CloseHandle(handle_);
#else
        if (descriptor_ >= 0)
            close(descriptor_);
#endif
    }

  private:
#ifdef _WIN32
    HANDLE handle_ = INVALID_HANDLE_VALUE;
#else
    int descriptor_ = -1;
#endif
};

bool Digest(std::string_view s) {
    return s.size() == 64 && std::all_of(s.begin(), s.end(), [](char c) {
               return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
           });
}
// Match only names emitted by WriteAtomic. The exclusive writer lease makes
// these files abandoned recovery work, rather than another writer's live data.
bool OwnedTemporary(std::string_view name) {
    constexpr std::string_view marker = ".tmp-";
    const auto marker_at = name.find(marker);
    if (marker_at == std::string_view::npos)
        return false;
    const auto target = name.substr(0, marker_at);
    if (target != "recipes.json" &&
        !(target.size() == 68 && target.ends_with(".bin") && Digest(target.substr(0, 64))))
        return false;
    const auto suffix = name.substr(marker_at + marker.size());
    const auto separator = suffix.find('-');
    const auto decimal = [](std::string_view value) {
        return !value.empty() &&
               std::all_of(value.begin(), value.end(), [](char c) { return c >= '0' && c <= '9'; });
    };
    return separator != std::string_view::npos && decimal(suffix.substr(0, separator)) &&
           decimal(suffix.substr(separator + 1));
}
bool ValidRecipe(const NativeFsiPipelineRecipe &r) noexcept {
    return r.hardware.empty() ? ValidNativeFsiPipelineRecipe(r) : ValidNativeHardwarePipelineRecipe(r);
}
// Hardware keys end in unused zero words; store them as trimmed hex.
std::string HardwareHex(const std::vector<uint32_t> &words) {
    size_t used = words.size();
    while (used && !words[used - 1])
        --used;
    static constexpr char kHex[] = "0123456789abcdef";
    std::string text;
    text.reserve(used * 8);
    for (size_t i = 0; i < used; ++i)
        for (int shift = 28; shift >= 0; shift -= 4)
            text.push_back(kHex[(words[i] >> shift) & 15]);
    return text;
}
std::vector<uint32_t> HardwareWords(const std::string &text) {
    if (text.size() % 8 || text.size() / 8 > kDrawStateWords)
        throw std::runtime_error("Recipe hardware key extent");
    std::vector<uint32_t> words(kDrawStateWords);
    for (size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        const uint32_t digit = c >= '0' && c <= '9' ? uint32_t(c - '0')
                               : c >= 'a' && c <= 'f' ? uint32_t(c - 'a' + 10)
                                                      : throw std::runtime_error("Recipe hardware key digit");
        words[i / 8] = words[i / 8] << 4 | digit;
    }
    return words;
}
Json ToJson(const NativeFsiPipelineRecipe &r) {
    Json j{{"vertex", r.vertex},
           {"pixel", r.pixel ? Json(*r.pixel) : Json(nullptr)},
           {"raster", r.raster},
           {"color", r.color_format},
           {"depth", r.depth_format}};
    if (!r.hardware.empty())
        j["hardware"] = HardwareHex(r.hardware);
    return j;
}
int64_t Integer(const Json &value, int64_t minimum, int64_t maximum) {
    if (value.is_number_unsigned()) {
        const auto n = value.get<uint64_t>();
        if (n > uint64_t(maximum))
            throw std::runtime_error("Recipe integer overflow");
        return int64_t(n);
    }
    if (!value.is_number_integer())
        throw std::runtime_error("Recipe requires exact integers");
    const auto n = value.get<int64_t>();
    if (n < minimum || n > maximum)
        throw std::runtime_error("Recipe integer out of range");
    return n;
}
NativeFsiPipelineRecipe FromJson(const Json &j) {
    NativeFsiPipelineRecipe r;
    r.vertex = j.at("vertex").get<std::string>();
    if (!j.at("pixel").is_null())
        r.pixel = j.at("pixel").get<std::string>();
    const auto &words = j.at("raster");
    if (!words.is_array() || words.size() != 6)
        throw std::runtime_error("Recipe raster extent");
    for (size_t i = 0; i < 6; ++i)
        r.raster[i] = uint32_t(Integer(words[i], 0, UINT32_MAX));
    r.color_format = int32_t(Integer(j.at("color"), -1, INT32_MAX));
    r.depth_format = int32_t(Integer(j.at("depth"), -1, INT32_MAX));
    if (const auto hardware = j.find("hardware"); hardware != j.end())
        r.hardware = HardwareWords(hardware->get<std::string>());
    if (!ValidRecipe(r))
        throw std::runtime_error("Invalid observed pipeline recipe");
    return r;
}
std::vector<uint8_t> ReadBounded(const std::filesystem::path &path, size_t maximum) {
    if (std::filesystem::is_symlink(path) || !std::filesystem::is_regular_file(path))
        throw std::runtime_error("Invalid observed pipeline cache file");
    const auto size = std::filesystem::file_size(path);
    if (!size || size > maximum)
        throw std::runtime_error("Observed pipeline cache extent exceeds its bound");
    std::vector<uint8_t> bytes(size);
    std::ifstream file(path, std::ios::binary);
    if (!file.read(reinterpret_cast<char *>(bytes.data()), bytes.size()))
        throw std::runtime_error("Truncated observed pipeline cache");
    return bytes;
}
void WriteAtomic(const std::filesystem::path &path, std::span<const uint8_t> bytes) {
    static std::atomic<uint64_t> serial{0};
    const auto suffix =
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
        std::to_string(++serial);
    const auto temporary = path.string() + ".tmp-" + suffix;
    try {
        {
            std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
            if (!file.write(reinterpret_cast<const char *>(bytes.data()), bytes.size()) ||
                !file.flush())
                throw std::runtime_error("Cannot write observed pipeline cache");
        }
        // A concurrent process may win. A missed cache update is harmless;
        // never delete the previous complete file to make replacement succeed.
#ifdef _WIN32
        if (!MoveFileExW(std::filesystem::path(temporary).c_str(), path.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            throw std::runtime_error("Cannot atomically replace observed pipeline cache");
#else
        std::filesystem::rename(temporary, path);
#endif
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw;
    }
}
} // namespace
bool ValidNativeFsiPipelineRecipe(const NativeFsiPipelineRecipe &r) noexcept {
    using namespace plume;
    const auto &w = r.raster;
    const bool alpha = w[4] == 0 || ((w[4] & ~0x700u) == 2 && ((w[4] >> 8) & 7) != 1);
    return r.hardware.empty() && Digest(r.vertex) && (!r.pixel || Digest(*r.pixel)) &&
           w[0] >= uint32_t(RenderCullMode::NONE) && w[0] <= uint32_t(RenderCullMode::BACK) &&
           w[1] >= uint32_t(RenderFrontFace::CLOCKWISE) &&
           w[1] <= uint32_t(RenderFrontFace::COUNTER_CLOCKWISE) && w[2] <= 1 &&
           w[3] == uint32_t(RenderPrimitiveTopology::TRIANGLE_LIST) && alpha && w[5] <= 1 &&
           r.color_format == int32_t(RenderFormat::R8G8B8A8_UNORM) && r.depth_format == -1;
}
NativeHardwareRaster EncodeNativeHardwareRaster(const plume::RenderGraphicsPipelineDesc &p,
                                                uint32_t specialization, bool rectangle) {
    NativeHardwareRaster key{};
    size_t n = 0;
    auto put = [&](auto value) { key.at(n++) = uint32_t(value); };
    put(p.primitiveTopology);put(p.cullMode);put(p.frontFace);put(p.depthClipEnabled);put(p.dynamicDepthBiasEnabled);
    put(p.depthBias);put(std::bit_cast<uint32_t>(p.depthBiasClamp));put(std::bit_cast<uint32_t>(p.slopeScaledDepthBias));
    put(p.depthEnabled);put(p.depthWriteEnabled);put(p.depthFunction);
    put(p.stencilEnabled);put(p.stencilReadMask);put(p.stencilWriteMask);put(p.stencilReference);
    for (const auto *face : {&p.stencilFrontFace, &p.stencilBackFace}) {
        put(face->failOp);put(face->depthFailOp);put(face->passOp);put(face->compareFunction);
    }
    put(p.alphaToCoverageEnabled);put(p.logicOpEnabled);put(p.logicOp);
    for (const auto &blend : p.renderTargetBlend) {
        put(blend.blendEnabled);put(blend.srcBlend);put(blend.dstBlend);put(blend.blendOp);
        put(blend.srcBlendAlpha);put(blend.dstBlendAlpha);put(blend.blendOpAlpha);put(blend.renderTargetWriteMask);
    }
    put(specialization);put(rectangle);
    return key;
}
namespace {
// Inverse of EncodeNativeHardwareRaster. Every enum must be in range and the
// decoded state must encode back to the same words.
bool DecodeHardwareRaster(const NativeHardwareRaster &w, plume::RenderGraphicsPipelineDesc &p,
                          uint32_t &specialization, bool &rectangle) noexcept {
    using namespace plume;
    size_t n = 0;
    bool valid = true;
    auto get = [&](uint32_t maximum) {
        const uint32_t value = w[n++];
        valid = valid && value <= maximum;
        return value;
    };
    auto flag = [&] { return get(1) != 0; };
    const uint32_t any = UINT32_MAX;
    const auto compare = uint32_t(RenderComparisonFunction::ALWAYS), stencil = uint32_t(RenderStencilOp::DECREMENT_AND_WRAP);
    const auto factor = uint32_t(RenderBlend::INV_SRC1_ALPHA), operation = uint32_t(RenderBlendOperation::MAX);
    p.primitiveTopology = RenderPrimitiveTopology(get(uint32_t(RenderPrimitiveTopology::TRIANGLE_FAN)));
    p.cullMode = RenderCullMode(get(uint32_t(RenderCullMode::BACK)));
    p.frontFace = RenderFrontFace(get(uint32_t(RenderFrontFace::COUNTER_CLOCKWISE)));
    p.depthClipEnabled = flag();p.dynamicDepthBiasEnabled = flag();
    p.depthBias = int32_t(get(any));
    p.depthBiasClamp = std::bit_cast<float>(get(any));
    p.slopeScaledDepthBias = std::bit_cast<float>(get(any));
    p.depthEnabled = flag();p.depthWriteEnabled = flag();
    p.depthFunction = RenderComparisonFunction(get(compare));
    p.stencilEnabled = flag();
    p.stencilReadMask = get(any);p.stencilWriteMask = get(any);p.stencilReference = get(any);
    for (auto *face : {&p.stencilFrontFace, &p.stencilBackFace}) {
        face->failOp = RenderStencilOp(get(stencil));
        face->depthFailOp = RenderStencilOp(get(stencil));
        face->passOp = RenderStencilOp(get(stencil));
        face->compareFunction = RenderComparisonFunction(get(compare));
    }
    p.alphaToCoverageEnabled = flag();p.logicOpEnabled = flag();
    p.logicOp = RenderLogicOperation(get(uint32_t(RenderLogicOperation::OR_INVERTED)));
    for (auto &blend : p.renderTargetBlend) {
        blend.blendEnabled = flag();
        blend.srcBlend = RenderBlend(get(factor));blend.dstBlend = RenderBlend(get(factor));
        blend.blendOp = RenderBlendOperation(get(operation));
        blend.srcBlendAlpha = RenderBlend(get(factor));blend.dstBlendAlpha = RenderBlend(get(factor));
        blend.blendOpAlpha = RenderBlendOperation(get(operation));
        blend.renderTargetWriteMask = uint8_t(get(15));
    }
    specialization = get(any);
    rectangle = flag();
    return valid && EncodeNativeHardwareRaster(p, specialization, rectangle) == w;
}
bool ValidFormat(int32_t format) noexcept {
    return format >= -1 && format < int32_t(plume::RenderFormat::MAX) &&
           format != int32_t(plume::RenderFormat::UNKNOWN);
}
} // namespace
bool ValidNativeHardwarePipelineRecipe(const NativeFsiPipelineRecipe &r) noexcept {
    if (r.hardware.size() != kDrawStateWords || r.raster != std::array<uint32_t, 6>{} ||
        !Digest(r.vertex) || (r.pixel && !Digest(*r.pixel)) || !ValidFormat(r.color_format) ||
        !ValidFormat(r.depth_format))
        return false;
    NativeHardwareRaster words;
    std::copy(r.hardware.begin(), r.hardware.end(), words.begin());
    plume::RenderGraphicsPipelineDesc descriptor;
    uint32_t specialization;
    bool rectangle;
    return DecodeHardwareRaster(words, descriptor, specialization, rectangle);
}
NativeHardwarePipelineDescriptor::NativeHardwarePipelineDescriptor(
    const NativeHardwareRaster &raster, int32_t color, int32_t depth,
    const plume::RenderPipelineLayout *layout, const plume::RenderShader *vertex,
    const plume::RenderShader *pixel) {
    using namespace plume;
    uint32_t value = 0;
    bool rectangle = false;
    if (!DecodeHardwareRaster(raster, descriptor, value, rectangle) || !ValidFormat(color) ||
        !ValidFormat(depth))
        throw std::invalid_argument("Invalid hardware pipeline recipe");
    specialization = RenderSpecConstant(0, value);
    auto &p = descriptor;
    p.pipelineLayout = layout;
    p.vertexShader = vertex;
    p.pixelShader = pixel;
    if (rectangle) {
        p.inputSlots = &slot;
        p.inputSlotsCount = 1;
        p.inputElements = &element;
        p.inputElementsCount = 1;
    }
    p.renderTargetCount = color >= 0 ? 1 : 0;
    if (color >= 0)
        p.renderTargetFormat[0] = RenderFormat(color);
    if (depth >= 0)
        p.depthTargetFormat = RenderFormat(depth);
    p.specConstants = &specialization;
    p.specConstantsCount = 1;
}
NativeFsiPipelineDescriptor::NativeFsiPipelineDescriptor(const std::array<uint32_t, 6> &w,
                                                         int32_t color, int32_t depth,
                                                         const plume::RenderPipelineLayout *layout,
                                                         const plume::RenderShader *vertex,
                                                         const plume::RenderShader *pixel,
                                                         const NativeFsiStateSpecialization *state)
    : specialization(0, w[4]) {
    using namespace plume;
    auto &p = descriptor;
    p.pipelineLayout = layout;
    p.vertexShader = vertex;
    p.pixelShader = pixel;
    p.cullMode = RenderCullMode(w[0]);
    p.frontFace = RenderFrontFace(w[1]);
    p.depthClipEnabled = w[2];
    p.primitiveTopology = RenderPrimitiveTopology(w[3]);
    // Canonical disabled state: these fields never participate in the FSI key
    // or framebuffer operations. Keep Draw and startup descriptors identical.
    p.depthEnabled = p.depthWriteEnabled = p.stencilEnabled = p.dynamicDepthBiasEnabled = false;
    for (auto &blend : p.renderTargetBlend) {
        blend = RenderBlendDesc::Copy();
        blend.renderTargetWriteMask = 0;
    }
    if (w[5]) {
        p.inputSlots = &slot;
        p.inputSlotsCount = 1;
        p.inputElements = &element;
        p.inputElementsCount = 1;
    }
    p.renderTargetCount = color >= 0 ? 1 : 0;
    if (color >= 0)
        p.renderTargetFormat[0] = RenderFormat(color);
    if (depth >= 0)
        p.depthTargetFormat = RenderFormat(depth);
    if(state) {
        fsi_specialization.emplace(w[4],*state);
        p.specConstants=fsi_specialization->values.data();p.specConstantsCount=17;
    } else {
        p.specConstants = &specialization;p.specConstantsCount = 1;
    }
}
NativePipelineRecipeCache::NativePipelineRecipeCache(std::filesystem::path directory,
                                                     std::string version)
    : NativePipelineRecipeCache(std::move(directory), std::move(version), Limits{}) {}
NativePipelineRecipeCache::NativePipelineRecipeCache(std::filesystem::path directory,
                                                     std::string version, Limits limits)
    : directory_(std::move(directory)), version_(std::move(version)), limits_(limits) {
    if (directory_.empty() || !Digest(version_) || !limits.recipes ||
        limits.recipes > kMaxRecipes || !limits.sources || limits.sources > kMaxSources ||
        !limits.source_bytes || limits.source_bytes > kMaxSourceBytes || !limits.pending ||
        limits.pending > kMaxPending || !limits.pending_bytes ||
        limits.pending_bytes > kMaxPendingBytes)
        throw std::invalid_argument("Invalid native pipeline cache namespace or bounds");
    worker_ = std::thread([this] { Worker(); });
}
NativePipelineRecipeCache::~NativePipelineRecipeCache() {
    StopAccepting();
    if (worker_.joinable())
        worker_.join();
}
void NativePipelineRecipeCache::StopAccepting() noexcept {
    std::lock_guard lock(mutex_);
    stopping_ = true;
    // Finish only the already-running atomic write. Closing/fallback must not
    // drain a backlog of optional records before releasing the renderer.
    for (const auto &item : pending_) {
        --stats_.pending;
        stats_.pending_bytes -= item.vertex->size() + (item.pixel ? item.pixel->size() : 0);
        ++stats_.dropped;
    }
    pending_.clear();
    changed_.notify_all();
}
void NativePipelineRecipeCache::Drain() {
    std::unique_lock lock(mutex_);
    changed_.wait(lock, [&] { return pending_.empty() && !working_; });
}
NativePipelineRecipeCache::Statistics NativePipelineRecipeCache::statistics() const {
    std::lock_guard lock(mutex_);
    auto result = stats_;
    result.dropped += contended_drops_.load();
    return result;
}
void NativePipelineRecipeCache::Observe(ObservedNativePipeline o) noexcept {
    try {
        if (!ValidRecipe(o.recipe) || !o.vertex || o.vertex->empty() ||
            bool(o.recipe.pixel) != bool(o.pixel) || (o.pixel && o.pixel->empty()))
            return;
        const size_t bytes = o.vertex->size() + (o.pixel ? o.pixel->size() : 0);
        std::unique_lock lock(mutex_, std::try_to_lock);
        if (!lock.owns_lock()) {
            ++contended_drops_;
            return;
        }
        if (stopping_ || observed_.size() >= limits_.recipes || stats_.pending >= limits_.pending ||
            bytes > limits_.pending_bytes || stats_.pending_bytes > limits_.pending_bytes - bytes) {
            ++stats_.dropped;
            return;
        }
        auto [entry, inserted] = observed_.insert(o.recipe);
        if (!inserted) {
            ++stats_.duplicate;
            return;
        }
        try {
            pending_.push_back(std::move(o));
        } catch (...) {
            observed_.erase(entry);
            throw;
        }
        ++stats_.admitted;
        ++stats_.pending;
        stats_.pending_bytes += bytes;
        stats_.peak_pending = std::max(stats_.peak_pending, stats_.pending);
        stats_.peak_pending_bytes = std::max(stats_.peak_pending_bytes, stats_.pending_bytes);
        changed_.notify_one();
    } catch (...) { /* Cache recording must never change the accepted draw outcome. */
    }
}
std::vector<ObservedNativePipeline> NativePipelineRecipeCache::Load() const noexcept {
    try {
        const auto raw = ReadBounded(directory_ / "recipes.json", 16u * 1024 * 1024);
        const auto manifest = Json::parse(raw);
        if (manifest.at("schema") != kSchema || manifest.at("compiler") != version_)
            return {};
        const auto &entries = manifest.at("recipes");
        if (!entries.is_array() || entries.size() > limits_.recipes)
            return {};
        std::map<std::string, PipelineShaderSource> sources;
        size_t total = 0;
        std::vector<ObservedNativePipeline> result;
        result.reserve(entries.size());
        std::set<NativeFsiPipelineRecipe> unique;
        for (const auto &entry : entries)
            try {
                auto recipe = FromJson(entry);
                auto load = [&](const std::string &hash) -> PipelineShaderSource {
                    if (auto found = sources.find(hash); found != sources.end())
                        return found->second;
                    if (sources.size() >= limits_.sources)
                        throw std::runtime_error("Too many observed shader sources");
                    auto bytes =
                        ReadBounded(directory_ / (hash + ".bin"), limits_.source_bytes - total);
                    if (ContentHash(bytes) != hash)
                        throw std::runtime_error("Observed shader source identity mismatch");
                    auto owner = std::make_shared<const std::vector<uint8_t>>(std::move(bytes));
                    total += owner->size();
                    sources.emplace(hash, owner);
                    return owner;
                };
                auto vs = load(recipe.vertex);
                auto ps = recipe.pixel ? load(*recipe.pixel) : nullptr;
                if (unique.insert(recipe).second)
                    result.push_back({std::move(recipe), std::move(vs), std::move(ps)});
            } catch (...) { /* A corrupt recipe is a miss, not a renderer failure. */
            }
        return result;
    } catch (...) {
        return {};
    }
}
void NativePipelineRecipeCache::Worker() noexcept {
    std::vector<NativeFsiPipelineRecipe> stored;
    std::map<std::string, size_t> sources;
    size_t source_bytes = 0;
    bool loaded = false, manifest_dirty = false;
    std::unique_ptr<CacheWriterLease> writer_lease;
    // The manifest is rewritten whole, so a burst of new pipelines writes it
    // once after the last of them rather than once per recipe.
    const auto write_manifest = [&] {
        Json manifest{{"schema", kSchema}, {"compiler", version_}, {"recipes", Json::array()}};
        for (const auto &item : stored)
            manifest["recipes"].push_back(ToJson(item));
        const auto text = manifest.dump();
        WriteAtomic(directory_ / "recipes.json",
                    std::span(reinterpret_cast<const uint8_t *>(text.data()), text.size()));
        manifest_dirty = false;
    };
    for (;;) {
        ObservedNativePipeline job;
        size_t bytes = 0;
        {
            std::unique_lock lock(mutex_);
            changed_.wait(lock, [&] { return stopping_ || !pending_.empty(); });
            if (pending_.empty()) {
                lock.unlock();
                if (manifest_dirty)
                    try {
                        write_manifest();
                    } catch (...) { /* Optional persistence; the next session records again. */
                    }
                return;
            }
            job = std::move(pending_.front());
            pending_.pop_front();
            working_ = true;
            bytes = job.vertex->size() + (job.pixel ? job.pixel->size() : 0);
        }
        bool success = false;
        try {
            if (!writer_lease) {
                std::filesystem::create_directories(directory_);
                writer_lease = std::make_unique<CacheWriterLease>(directory_ / "writer.lock");
            }
            if (!loaded) {
                stored.clear();
                for (auto &item : Load())
                    stored.push_back(std::move(item.recipe));
                sources.clear();
                source_bytes = 0;
                // Count orphan source files too: a failed atomic manifest
                // update must not make later writes exceed the disk budget.
                size_t scanned = 0;
                if (std::filesystem::exists(directory_))
                    for (const auto &file : std::filesystem::directory_iterator(directory_)) {
                        if (++scanned > 4096)
                            throw std::runtime_error(
                                "Observed cache directory exceeds its entry bound");
                        if (OwnedTemporary(file.path().filename().string())) {
                            if (file.is_symlink() || !file.is_regular_file())
                                throw std::runtime_error("Invalid observed cache temporary");
                            std::filesystem::remove(file.path());
                            continue;
                        }
                        const auto name = file.path().stem().string();
                        if (file.path().extension() != ".bin" || !Digest(name))
                            continue;
                        if (file.is_symlink() || !file.is_regular_file())
                            throw std::runtime_error("Invalid observed shader file");
                        const auto size = file.file_size();
                        if (sources.size() >= limits_.sources ||
                            size > limits_.source_bytes - source_bytes)
                            throw std::runtime_error(
                                "Observed shader disk cache exceeds its bound");
                        sources.emplace(name, size);
                        source_bytes += size;
                    }
                loaded = true;
            }
            const auto existing = std::find_if(stored.begin(), stored.end(),
                                               [&](const auto &o) { return o == job.recipe; });
            if (existing != stored.end())
                success = true;
            else {
                if (stored.size() >= limits_.recipes)
                    throw std::runtime_error("Observed recipe cache is full");
                size_t resulting_bytes = source_bytes, count = 0;
                std::map<std::string, PipelineShaderSource> incoming{
                    {job.recipe.vertex, job.vertex}};
                if (job.pixel)
                    incoming.emplace(*job.recipe.pixel, job.pixel);
                for (const auto &[hash, owner] : incoming) {
                    if (ContentHash(*owner) != hash)
                        throw std::runtime_error("Observed source hash mismatch");
                    auto old = sources.find(hash);
                    if (old == sources.end())
                        ++count;
                    else
                        resulting_bytes -= old->second;
                    if (owner->size() > limits_.source_bytes - resulting_bytes)
                        throw std::runtime_error("Observed shader source cache is full");
                    resulting_bytes += owner->size();
                }
                if (sources.size() + count > limits_.sources)
                    throw std::runtime_error("Observed shader source count is full");
                std::filesystem::create_directories(directory_);
                for (const auto &[hash, owner] : incoming) {
                    WriteAtomic(directory_ / (hash + ".bin"), *owner);
                    auto [entry, inserted] = sources.try_emplace(hash, owner->size());
                    if (!inserted) {
                        source_bytes -= entry->second;
                        entry->second = owner->size();
                    }
                    source_bytes += owner->size();
                }
                stored.push_back(job.recipe);
                manifest_dirty = true;
            }
            bool more;
            {
                std::lock_guard lock(mutex_);
                more = !pending_.empty() && !stopping_;
            }
            if (manifest_dirty && !more)
                write_manifest();
            success = true;
        } catch (...) {
            // A rename may have committed before later allocation/bookkeeping
            // failed. Rebuild accounting from disk under the lease before any
            // subsequent admission, including orphan sources and temporaries.
            loaded = false;
        }
        {
            std::lock_guard lock(mutex_);
            if (success)
                ++stats_.written;
            else
                ++stats_.failures;
            --stats_.pending;
            stats_.pending_bytes -= bytes;
            working_ = false;
            job = {};
            changed_.notify_all();
        }
    }
}
} // namespace gow3::renderer
