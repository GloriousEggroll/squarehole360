#pragma once
#include <stdexcept>
#include <string_view>

enum class Gow3RendererMode { Native, Xenia };
inline Gow3RendererMode ParseGow3RendererMode(std::string_view value) {
  if (value == "native") return Gow3RendererMode::Native;
  if (value == "xenia" || value == "reference") return Gow3RendererMode::Xenia;
  throw std::invalid_argument("Renderer must be Native or Xenia");
}
constexpr std::string_view Gow3RendererName(Gow3RendererMode mode) {
  return mode == Gow3RendererMode::Native ? "native" : "xenia";
}
