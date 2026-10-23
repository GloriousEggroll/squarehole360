#pragma once
#include "live_draw.h"
#include <nlohmann/json.hpp>

namespace gow3::renderer {
nlohmann::json ToJson(const TypedSurface& surface);
nlohmann::json ToJson(const TypedTextureIdentity& identity);
nlohmann::json ToJson(const TypedLiveDraw& draw);
nlohmann::json ToJson(const TypedLiveQuery& query);
} // namespace gow3::renderer
