// Keyboard prompts for the HUD's action icons.
//
// The HUD draws its button prompts (pick up, mantle, revive, chainsaw duel and
// so on) as tiles of two icon atlases through UCanvas::DrawTile. While the
// keyboard and mouse is in use, each controller-button glyph is covered by a
// key cap naming the bound key, drawn with the canvas's own font. Arrows and
// the "press repeatedly" dots around a glyph stay as they are.
#include "gameplay_hooks.h"

#include <rex/input/mnk/mnk_input_driver.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <initializer_list>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

REX_EXTERN(__imp__sub_82736CE0);
REX_EXTERN(__imp__sub_822FC8D0);
REX_EXTERN(__imp__sub_8292D488);
REX_EXTERN(__imp__sub_8292D5C0);
REX_EXTERN(__imp__sub_82942C38);
REX_EXTERN(__imp__sub_82941BB0);
REX_EXTERN(__imp__sub_82B6B5A8);
REX_EXTERN(sub_82B6A348);
REX_EXTERN(sub_822B4FF8);
REX_EXTERN(sub_82737010);

namespace {
bool prompts_enabled = false;

uint32_t Load32(const uint8_t* base, uint32_t address) {
  const auto* p = base + address;
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
void Store32(uint8_t* base, uint32_t address, uint32_t value) {
  for (int i = 0; i < 4; ++i) base[address + i] = uint8_t(value >> (24 - 8 * i));
}
float LoadFloat(const uint8_t* base, uint32_t address) {
  const uint32_t bits = Load32(base, address);
  float value;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}
void StoreFloat(uint8_t* base, uint32_t address, float value) {
  uint32_t bits;
  std::memcpy(&bits, &value, sizeof(bits));
  Store32(base, address, bits);
}

// The engine's name table (FNameEntry pointers); an entry holds its index
// shifted left by one, the low bit set for UTF-16 text, then the text at +8.
constexpr uint32_t kNames = 0x8343D148;
// UObject::Name (FName index) and the UCanvas fields used here.
constexpr uint32_t kObjectName = 44;
constexpr uint32_t kCanvasFont = 0x3C, kCanvasOrgX = 0x40, kCanvasOrgY = 0x44;
constexpr uint32_t kCanvasCurX = 0x50, kCanvasCurY = 0x54, kCanvasDrawColor = 0x60;

bool NameIs(const uint8_t* base, uint32_t object, std::string_view name) {
  const uint32_t names = Load32(base, kNames);
  const uint32_t index = Load32(base, object + kObjectName);
  if (!names || index >= 0x100000) return false;
  const uint32_t entry = Load32(base, names + index * 4);
  if (!entry) return false;
  const bool wide = Load32(base, entry) & 1;
  uint32_t address = entry + 8;
  for (const char c : name) {
    if (wide ? base[address] != 0 || base[address + 1] != uint8_t(c) : base[address] != uint8_t(c))
      return false;
    address += wide ? 2 : 1;
  }
  return wide ? !base[address] && !base[address + 1] : !base[address];
}

enum class Atlas : uint8_t { kNone, kActionIcons, kActionIcons2, kWeaponIcons, kWeaponIcons3 };

// A controller-button glyph in an atlas: its center and height in atlas
// texels and the binding it stands for ("move" for the left stick). A tile
// shows the glyph when the glyph lies inside it.
struct Glyph {
  Atlas atlas;
  float x, y, size;
  const char* bind;
};
constexpr Glyph kGlyphs[] = {
    {Atlas::kActionIcons, 191.5f, 348.5f, 24, "keybind_y"},
    {Atlas::kActionIcons, 229, 343, 24, "keybind_a"},
    {Atlas::kActionIcons, 265, 344, 24, "keybind_a"},
    {Atlas::kActionIcons, 313, 329, 24, "keybind_b"},
    {Atlas::kActionIcons, 344.5f, 328.5f, 24, "keybind_x"},
    {Atlas::kActionIcons, 390.5f, 329.5f, 24, "keybind_x"},
    {Atlas::kActionIcons, 405.5f, 434.5f, 24, "keybind_y"},
    {Atlas::kActionIcons, 442, 469, 30, "keybind_rstick_press"},
    {Atlas::kActionIcons, 478, 469, 30, "keybind_rstick_press"},
    {Atlas::kActionIcons2, 112, 15, 24, "keybind_b"},
    {Atlas::kActionIcons2, 111.5f, 46, 24, "keybind_b"},
    {Atlas::kActionIcons2, 18.5f, 221.5f, 24, "keybind_y"},
    {Atlas::kActionIcons2, 58.5f, 221.5f, 24, "keybind_y"},
    {Atlas::kActionIcons2, 17, 147, 30, "keybind_left_trigger"},
    {Atlas::kActionIcons2, 65, 147, 26, "keybind_right_trigger"},
    {Atlas::kActionIcons2, 17, 185, 26, "keybind_left_trigger"},
    {Atlas::kActionIcons2, 65, 185, 30, "keybind_right_trigger"},
    {Atlas::kWeaponIcons, 181, 370.5f, 27, "move"},
    {Atlas::kWeaponIcons, 241, 307, 30, "keybind_lstick_press"},
    {Atlas::kWeaponIcons, 275.5f, 306.5f, 30, "keybind_lstick_press"},
    {Atlas::kWeaponIcons, 77.5f, 394, 30, "keybind_left_trigger"},
    {Atlas::kWeaponIcons, 115, 394, 30, "keybind_right_trigger"},
    {Atlas::kWeaponIcons3, 305.5f, 254, 22, "keybind_right_shoulder"},
};

std::mutex mutex;
std::unordered_map<uint32_t, Atlas> atlases;                     // texture -> atlas
std::map<std::pair<uint32_t, std::string>, std::pair<int, int>> text_sizes;  // (font, text)
// Offset of UCanvas::DefaultTexture (the white square), found on first use.
int default_texture_offset = 0;

Atlas AtlasOf(const uint8_t* base, uint32_t texture) {
  std::lock_guard lock(mutex);
  if (const auto it = atlases.find(texture); it != atlases.end()) return it->second;
  Atlas atlas = Atlas::kNone;
  if (NameIs(base, texture, "WarfareHUD_ActionIcons")) atlas = Atlas::kActionIcons;
  else if (NameIs(base, texture, "HUD_ActionIcons2")) atlas = Atlas::kActionIcons2;
  else if (NameIs(base, texture, "Gears_WeaponIcons")) atlas = Atlas::kWeaponIcons;
  else if (NameIs(base, texture, "Gears3_WeaponIcons")) atlas = Atlas::kWeaponIcons3;
  if (atlases.size() > 4096) atlases.clear();
  atlases.emplace(texture, atlas);
  return atlas;
}

uint32_t DefaultTexture(const uint8_t* base, uint32_t canvas) {
  std::lock_guard lock(mutex);
  if (default_texture_offset == 0) {
    default_texture_offset = -1;
    for (int offset = 0x64; offset < 0x200; offset += 4) {
      const uint32_t object = Load32(base, canvas + offset);
      if (object >= 0x40000000 && object < 0xA0000000 && NameIs(base, object, "WhiteSquareTexture")) {
        default_texture_offset = offset;
        break;
      }
    }
    REXLOG_INFO("Gears keyboard prompts: canvas default texture at {:#x}", default_texture_offset);
  }
  return default_texture_offset > 0 ? Load32(base, canvas + default_texture_offset) : 0;
}

// Calls into the game use a frame below the caller's stack pointer; the
// frame also holds the text and colors they read.
struct GuestFrame {
  static constexpr uint32_t kSize = 1024;
  explicit GuestFrame(PPCContext& ctx) : sp((ctx.r1.u32 - kSize) & ~15u) {}
  uint32_t sp;
  uint32_t text() const { return sp + 512; }
  uint32_t sizes() const { return sp + 768; }
  uint32_t render_info() const { return sp + 784; }
  uint32_t color() const { return sp + 848; }
};

// UCanvas::WrappedPrint(Draw, XL, YL, Font, ScaleX, ScaleY, Text, RenderInfo)
// at the canvas position, in its draw color.
std::pair<int, int> Print(PPCContext& ctx, uint8_t* base, const GuestFrame& frame, uint32_t canvas,
                          uint32_t font, bool draw, float scale) {
  std::memset(base + frame.render_info(), 0, 64);
  Store32(base, frame.sizes(), 0);
  Store32(base, frame.sizes() + 4, 0);
  Store32(base, frame.sp + 84, frame.render_info());
  ctx.r1.u64 = frame.sp;
  ctx.r3.u64 = canvas;
  ctx.r4.u64 = draw;
  ctx.r5.u64 = frame.sizes();
  ctx.r6.u64 = frame.sizes() + 4;
  ctx.r7.u64 = font;
  ctx.f1.f64 = scale;
  ctx.f2.f64 = scale;
  ctx.r10.u64 = frame.text();
  sub_82737010(ctx, base);
  return {int(Load32(base, frame.sizes())), int(Load32(base, frame.sizes() + 4))};
}

// UCanvas::DrawTile with a solid color, translucent blending.
void FillRect(PPCContext& ctx, uint8_t* base, const GuestFrame& frame, uint32_t canvas, uint32_t texture,
              double z, float x, float y, float w, float h, const float (&rgba)[4]) {
  for (int i = 0; i < 4; ++i) StoreFloat(base, frame.color() + 4 * i, rgba[i]);
  Store32(base, frame.sp + 108, frame.color());
  Store32(base, frame.sp + 116, 2);
  ctx.r1.u64 = frame.sp;
  ctx.r3.u64 = canvas;
  ctx.r4.u64 = texture;
  ctx.f1.f64 = x;
  ctx.f2.f64 = y;
  ctx.f3.f64 = z;
  ctx.f4.f64 = w;
  ctx.f5.f64 = h;
  ctx.f6.f64 = 0;
  ctx.f7.f64 = 0;
  ctx.f8.f64 = 1;
  ctx.f9.f64 = 1;
  __imp__sub_82736CE0(ctx, base);
}

std::string JoinLabels(std::initializer_list<const char*> binds) {
  std::string text;
  for (const char* bind : binds) {
    const std::string label = rex::input::mnk::BoundKeyLabel(bind);
    if (label.empty()) continue;
    if (!text.empty()) text += '/';
    text += label;
  }
  return text;
}

std::string MovementKeys() {
  std::string keys =
      JoinLabels({"keybind_lstick_up", "keybind_lstick_left", "keybind_lstick_down", "keybind_lstick_right"});
  if (keys.size() == 7) std::erase(keys, '/');  // W/A/S/D reads as WASD
  return keys;
}

std::string PromptKeys(std::string_view bind) {
  return bind == "move" ? MovementKeys() : rex::input::mnk::BoundKeyLabel(bind);
}

// A key cap centered on a glyph, cap_h high, with the label in the canvas font.
void DrawKeyCap(PPCContext& ctx, uint8_t* base, uint32_t canvas, uint32_t font, uint32_t white, double z,
                float center_x, float center_y, float cap_h, float alpha, const std::string& label) {
  const PPCContext saved = ctx;
  const GuestFrame frame(ctx);
  uint32_t address = frame.text();
  for (const char c : label.substr(0, 60)) {
    base[address++] = 0;
    base[address++] = uint8_t(c);
  }
  base[address++] = 0;
  base[address] = 0;

  std::pair<int, int> size;
  {
    std::lock_guard lock(mutex);
    if (const auto it = text_sizes.find({font, label}); it != text_sizes.end()) size = it->second;
  }
  if (!size.first) {
    size = Print(ctx, base, frame, canvas, font, false, 1.0f);
    ctx = saved;
    if (size.first <= 0 || size.second <= 0) return;
    std::lock_guard lock(mutex);
    if (text_sizes.size() > 256) text_sizes.clear();
    text_sizes[{font, label}] = size;
  }

  // Wide enough for the text.
  const float text_scale = cap_h * 0.62f / float(size.second);
  const float text_w = float(size.first) * text_scale;
  const float cap_w = std::max(cap_h, text_w + cap_h * 0.45f);
  const float left = center_x - cap_w / 2, top = center_y - cap_h / 2;
  const float border = std::max(1.0f, cap_h * 0.07f);
  FillRect(ctx, base, frame, canvas, white, z, left, top, cap_w, cap_h, {0.85f, 0.85f, 0.85f, alpha});
  FillRect(ctx, base, frame, canvas, white, z, left + border, top + border, cap_w - 2 * border,
           cap_h - 2 * border, {0.0f, 0.0f, 0.0f, alpha * 0.85f});

  const float org_x = LoadFloat(base, canvas + kCanvasOrgX), org_y = LoadFloat(base, canvas + kCanvasOrgY);
  const float cur_x = LoadFloat(base, canvas + kCanvasCurX), cur_y = LoadFloat(base, canvas + kCanvasCurY);
  const uint32_t draw_color = Load32(base, canvas + kCanvasDrawColor);
  StoreFloat(base, canvas + kCanvasCurX, center_x - text_w / 2 - org_x);
  StoreFloat(base, canvas + kCanvasCurY, center_y - float(size.second) * text_scale / 2 - org_y);
  Store32(base, canvas + kCanvasDrawColor, uint32_t(std::clamp(alpha, 0.0f, 1.0f) * 255.0f) << 24 | 0xFFFFFF);
  Print(ctx, base, frame, canvas, font, true, text_scale);
  StoreFloat(base, canvas + kCanvasCurX, cur_x);
  StoreFloat(base, canvas + kCanvasCurY, cur_y);
  Store32(base, canvas + kCanvasDrawColor, draw_color);
  ctx = saved;
}

// Draws the original tile, then a key cap over each glyph in it. Returns false
// when the tile shows no known glyph with a bound key.
bool DrawKeyPrompt(PPCContext& ctx, uint8_t* base) {
  const uint32_t canvas = ctx.r3.u32, texture = ctx.r4.u32;
  const Atlas atlas = AtlasOf(base, texture);
  if (atlas == Atlas::kNone) return false;
  const float u = float(ctx.f6.f64), v = float(ctx.f7.f64), ul = float(ctx.f8.f64), vl = float(ctx.f9.f64);
  if (ul <= 0 || vl <= 0) return false;
  std::pair<const Glyph*, std::string> caps[4];
  int count = 0;
  for (const auto& g : kGlyphs) {
    const float margin = g.size * 0.4f;
    if (g.atlas != atlas || g.x - margin < u || g.x + margin > u + ul || g.y - margin < v ||
        g.y + margin > v + vl || count == 4)
      continue;
    if (std::string label = PromptKeys(g.bind); !label.empty()) caps[count++] = {&g, std::move(label)};
  }
  const uint32_t font = count ? Load32(base, canvas + kCanvasFont) : 0;
  const uint32_t white = font ? DefaultTexture(base, canvas) : 0;
  if (!white) return false;

  const PPCContext saved = ctx;
  const uint32_t tile_color = Load32(base, ctx.r1.u32 + 108);
  const float alpha = tile_color ? LoadFloat(base, tile_color + 12) : 1.0f;
  const double z = ctx.f3.f64;
  const float x = float(ctx.f1.f64), y = float(ctx.f2.f64);
  const float scale_x = float(ctx.f4.f64) / ul, scale_y = float(ctx.f5.f64) / vl;

  // The tile itself, for the arrows and dots around the glyph.
  __imp__sub_82736CE0(ctx, base);
  ctx = saved;
  for (int i = 0; i < count; ++i) {
    const Glyph& g = *caps[i].first;
    DrawKeyCap(ctx, base, canvas, font, white, z, x + (g.x - u) * scale_x, y + (g.y - v) * scale_y,
               g.size * scale_y, alpha, caps[i].second);
  }
  ctx = saved;
  return true;
}

// Menus and message boxes show controller buttons through localized
// GearGameUI.ControllerFont strings, glyphs of the controller font.
bool Utf16Equals(const uint8_t* base, uint32_t address, std::string_view text, bool ignore_case = false) {
  if (!address) return false;
  for (const char c : text) {
    const char g = char(base[address + 1]);
    if (base[address] || (ignore_case ? std::tolower(g) != std::tolower(c) : g != c)) return false;
    address += 2;
  }
  return !base[address] && !base[address + 1];
}

// The keys for a ControllerFont entry; empty for entries that are not buttons
// (player numbers, invites) or have nothing bound.
std::string ControllerFontKeys(const uint8_t* base, uint32_t key) {
  static const std::pair<const char*, const char*> kButtons[] = {
      {"Controller_A", "keybind_a"},
      {"Controller_B", "keybind_b"},
      {"Controller_X", "keybind_x"},
      {"Controller_Y", "keybind_y"},
      {"Controller_LeftStickDown", "keybind_lstick_down"},
      {"Controller_LeftStickClick", "keybind_lstick_press"},
      {"Controller_RightStickClick", "keybind_rstick_press"},
      {"Controller_LeftTrigger", "keybind_left_trigger"},
      {"Controller_RightTrigger", "keybind_right_trigger"},
      {"Controller_LeftBumper", "keybind_left_shoulder"},
      {"Controller_RightBumper", "keybind_right_shoulder"},
      {"Controller_DPadUp", "keybind_dpad_up"},
      {"Controller_DPadDown", "keybind_dpad_down"},
      {"Controller_DPadLeft", "keybind_dpad_left"},
      {"Controller_DPadRight", "keybind_dpad_right"},
      {"Controller_ArrowUp", "keybind_dpad_up"},
      {"Controller_ArrowDown", "keybind_dpad_down"},
      {"Controller_ArrowLeft", "keybind_dpad_left"},
      {"Controller_ArrowRight", "keybind_dpad_right"},
      {"Controller_Start", "keybind_start"},
      {"Controller_Back", "keybind_back"},
  };
  for (const auto& [name, bind] : kButtons) {
    if (Utf16Equals(base, key, name, true)) return rex::input::mnk::BoundKeyLabel(bind);
  }
  if (Utf16Equals(base, key, "Controller_LeftStick", true)) return MovementKeys();
  if (Utf16Equals(base, key, "Controller_RightStick", true)) return "MOUSE";
  if (Utf16Equals(base, key, "Controller_DPadUpDown", true) || Utf16Equals(base, key, "Controller_UpDown", true))
    return JoinLabels({"keybind_dpad_up", "keybind_dpad_down"});
  if (Utf16Equals(base, key, "Controller_DPadLeftRight", true))
    return JoinLabels({"keybind_dpad_left", "keybind_dpad_right"});
  if (Utf16Equals(base, key, "Controller_DPad", true) || Utf16Equals(base, key, "Controller_UpDownLeftRight", true))
    return JoinLabels({"keybind_dpad_up", "keybind_dpad_down", "keybind_dpad_left", "keybind_dpad_right"});
  return {};
}
}  // namespace

void Gow3ConfigureInputPrompts(const uint8_t* guest_base) {
  // The native table names UCanvas::execDrawTile and execDrawText; DrawTile
  // calls 0x82736CE0 and DrawText 0x82737010 (WrappedPrint).
  prompts_enabled = Load32(guest_base, 0x83396358) == 0x8213ED64 &&
                    Load32(guest_base, 0x8339635C) == 0x82737240 &&
                    Load32(guest_base, 0x83396310) == 0x8213EE50 &&
                    Load32(guest_base, 0x83396314) == 0x82737C80 &&
                    Load32(guest_base, 0x82736CE0) == 0x7D8802A6 &&
                    Load32(guest_base, 0x82737010) == 0x7D8802A6 &&
                    Load32(guest_base, 0x822FC8D0) == 0x7D8802A6 &&
                    Load32(guest_base, 0x8292D488) == 0x7D8802A6 &&
                    Load32(guest_base, 0x8292D5C0) == 0x7D8802A6 &&
                    Load32(guest_base, 0x82942C38) == 0x7D8802A6 &&
                    Load32(guest_base, 0x82941BB0) == 0x7D8802A6 &&
                    Load32(guest_base, 0x82B6B5A8) == 0x7D8802A6 &&
                    Utf16Equals(guest_base, 0x8223EA2C, "ControllerFont") &&
                    Utf16Equals(guest_base, 0x8223EA14, "GearGameUI");
  if (!prompts_enabled) REXLOG_WARN("Gears keyboard prompts disabled: executable does not match");
}

// UCanvas::DrawTile(Texture, X, Y, Z, XL, YL, U, V, UL, VL, Color, Blend).
REX_EXTERN(sub_82736CE0) {
  if (prompts_enabled && ctx.r4.u32 && rex::input::mnk::ShowKeyboardPrompts() && DrawKeyPrompt(ctx, base))
    return;
  __imp__sub_82736CE0(ctx, base);
}

// Localize(Result, Section, Key, Package, LangExt, bOptional) returns Result.
// Localized text is resolved when a menu opens, so menus follow the device
// used last before they opened.
namespace {
// Both forms of each ControllerFont entry seen so far: the controller glyph
// markup and the key text, keyed by the markup that names the entry. Scripts
// sometimes build text from these values themselves.
struct FontValue {
  std::u16string markup, glyph, keys;
};
std::mutex font_mutex;
std::vector<FontValue> font_values;

std::u16string Widen(std::string_view text) { return std::u16string(text.begin(), text.end()); }

void StoreText(uint8_t* base, uint32_t address, std::u16string_view text) {
  for (const char16_t c : text) {
    base[address++] = uint8_t(c >> 8);
    base[address++] = uint8_t(c);
  }
  base[address++] = 0;
  base[address] = 0;
}
}  // namespace

REX_EXTERN(sub_822FC8D0) {
  const uint32_t section = ctx.r4.u32, key = ctx.r5.u32, package = ctx.r6.u32;
  __imp__sub_822FC8D0(ctx, base);
  if (!prompts_enabled || !Utf16Equals(base, section, "ControllerFont") || !Utf16Equals(base, package, "GearGameUI"))
    return;
  const std::string keys = ControllerFontKeys(base, key);
  if (keys.empty()) return;
  const uint32_t result = ctx.r3.u32;
  const uint32_t data = Load32(base, result);
  if (!data) return;
  const std::u16string text = Widen("<Color:R=1.0,G=1.0,B=1.0>[" + keys + "]</Color>");
  {
    std::u16string markup = u"<Strings:GearGameUI.ControllerFont.";
    for (uint32_t a = key; (base[a] || base[a + 1]) && markup.size() < 120; a += 2)
      markup += char16_t(base[a] << 8 | base[a + 1]);
    markup += u'>';
    std::u16string glyph;
    for (uint32_t a = data; (base[a] || base[a + 1]) && glyph.size() < 200; a += 2)
      glyph += char16_t(base[a] << 8 | base[a + 1]);
    std::lock_guard lock(font_mutex);
    const auto it = std::find_if(font_values.begin(), font_values.end(),
                                 [&](const FontValue& v) { return v.markup == markup; });
    if (it != font_values.end()) {
      it->keys = text;
      if (glyph != text) it->glyph = std::move(glyph);
    } else if (font_values.size() < 64 && glyph != text) {
      font_values.push_back({std::move(markup), std::move(glyph), text});
    }
  }
  // The original glyph markup is longer, so the result is rewritten in place.
  if (!rex::input::mnk::ShowKeyboardPrompts() || Load32(base, result + 8) < text.size() + 1) return;
  StoreText(base, data, text);
  Store32(base, result + 4, uint32_t(text.size() + 1));
}

// Menus set Scaleform text through UGFxObject::SetString(Member, Text,
// Context) and SetText(Text, Context), which resolve markup such as
// GearGameUI.ControllerFont strings first; scripts also resolve text with
// TranslateString and pass the result to either. Text that names controller
// buttons is remembered with its source markup and set again when the player
// switches between the controller and the keyboard and mouse, so open menus
// follow the device too.
namespace {
constexpr uint32_t kSetString = 0x8292D488, kSetText = 0x8292D5C0;

struct MenuText {
  uint32_t setter;
  uint32_t object;
  std::u16string member, text;
  uint32_t context;
  uint32_t identity[4];  // vtable, outer, name and class: the object is still the same
};
// A recent translation of text naming controller buttons.
struct Translation {
  std::u16string result, source;
  uint32_t context;
};
// A call into an ActionScript function of a UGFxObject (ActionScriptVoid and
// similar) whose string arguments include translated button text. Arguments
// are Scaleform values: 16 bytes, the type at +4 and the value at +8.
struct MenuCall {
  uint32_t object;
  uint32_t identity[4];
  std::string method;
  std::vector<std::array<uint8_t, 16>> args;
  std::vector<std::u16string> strings;  // per argument: source markup or text, for strings
  std::vector<bool> translated;
  std::vector<uint32_t> contexts;
};
constexpr uint32_t kValueString = 4, kValueStringW = 5;

std::mutex menu_mutex;
std::vector<MenuText> menu_texts;
std::vector<MenuCall> menu_calls;
thread_local uint32_t invoking_object = 0;
std::vector<Translation> translations;
bool menu_prompts_keyboard = false;

std::u16string LoadFString(const uint8_t* base, uint32_t fstring) {
  std::u16string text;
  const uint32_t data = Load32(base, fstring);
  const uint32_t count = Load32(base, fstring + 4);
  for (uint32_t i = 0; data && i + 1 < count && i < 2048; ++i)
    text += char16_t(base[data + 2 * i] << 8 | base[data + 2 * i + 1]);
  return text;
}

// Writes an FString and its text at address; returns the next free address.
uint32_t StoreFString(uint8_t* base, uint32_t address, std::u16string_view text) {
  const uint32_t data = address + 16;
  for (size_t i = 0; i < text.size(); ++i) {
    base[data + 2 * i] = uint8_t(text[i] >> 8);
    base[data + 2 * i + 1] = uint8_t(text[i]);
  }
  base[data + 2 * text.size()] = 0;
  base[data + 2 * text.size() + 1] = 0;
  Store32(base, address, data);
  Store32(base, address + 4, uint32_t(text.size() + 1));
  Store32(base, address + 8, uint32_t(text.size() + 1));
  return (data + 2 * uint32_t(text.size()) + 2 + 15) & ~15u;
}

void ObjectIdentity(const uint8_t* base, uint32_t object, uint32_t (&identity)[4]) {
  identity[0] = Load32(base, object);
  identity[1] = Load32(base, object + 40);
  identity[2] = Load32(base, object + 44);
  identity[3] = Load32(base, object + 52);
}

bool NamesControllerButtons(std::u16string_view text) {
  return text.find(u"ControllerFont") != std::u16string_view::npos;
}

// Replaces either form of each known ControllerFont value with its markup.
bool RestoreFontMarkup(std::u16string& text) {
  bool replaced = false;
  std::lock_guard lock(font_mutex);
  for (const auto& v : font_values) {
    for (const std::u16string* form : {&v.glyph, &v.keys}) {
      if (form->empty()) continue;
      for (size_t at = text.find(*form); at != std::u16string::npos; at = text.find(*form, at + v.markup.size())) {
        text.replace(at, form->size(), v.markup);
        replaced = true;
      }
    }
  }
  return replaced;
}

// The current device's form of each ControllerFont markup in text.
std::u16string ResolveFontMarkup(std::u16string text, bool keyboard) {
  std::lock_guard lock(font_mutex);
  for (const auto& v : font_values) {
    const std::u16string& form = keyboard ? v.keys : v.glyph;
    for (size_t at = text.find(v.markup); at != std::u16string::npos; at = text.find(v.markup, at + form.size()))
      text.replace(at, v.markup.size(), form);
  }
  return text;
}

// Remembers (or forgets) the text a setter is about to resolve and set.
void NoteMenuText(const uint8_t* base, uint32_t setter, uint32_t object, std::u16string member,
                  std::u16string text, uint32_t context) {
  std::lock_guard lock(menu_mutex);
  std::erase_if(menu_texts, [&](const MenuText& t) {
    return t.setter == setter && t.object == object && t.member == member;
  });
  if (!NamesControllerButtons(text) && !RestoreFontMarkup(text)) {
    // Already resolved by the script: find the markup it came from.
    const auto it = std::find_if(translations.rbegin(), translations.rend(),
                                 [&](const Translation& t) { return t.result == text; });
    if (it == translations.rend()) return;
    text = it->source;
    context = it->context;
  }
  if (text.size() >= 1000 || member.size() >= 200) return;
  if (menu_texts.size() >= 256) menu_texts.erase(menu_texts.begin());
  MenuText entry{setter, object, std::move(member), std::move(text), context, {}};
  ObjectIdentity(base, object, entry.identity);
  menu_texts.push_back(std::move(entry));
}
}  // namespace

// TranslateString(Context, Text, Result); Result holds the text at +8.
REX_EXTERN(sub_82942C38) {
  const uint32_t context = ctx.r3.u32, source = ctx.r4.u32, result = ctx.r5.u32;
  std::u16string text = prompts_enabled ? LoadFString(base, source) : std::u16string();
  __imp__sub_82942C38(ctx, base);
  if (!NamesControllerButtons(text) || text.size() >= 1000) return;
  Translation translation{LoadFString(base, result + 8), std::move(text), context};
  std::lock_guard lock(menu_mutex);
  if (translations.size() >= 64) translations.erase(translations.begin());
  translations.push_back(std::move(translation));
}

REX_EXTERN(sub_8292D488) {
  if (prompts_enabled)
    NoteMenuText(base, kSetString, ctx.r3.u32, LoadFString(base, ctx.r4.u32), LoadFString(base, ctx.r5.u32),
                 ctx.r6.u32);
  __imp__sub_8292D488(ctx, base);
}

REX_EXTERN(sub_8292D5C0) {
  if (prompts_enabled) NoteMenuText(base, kSetText, ctx.r3.u32, {}, LoadFString(base, ctx.r4.u32), ctx.r5.u32);
  __imp__sub_8292D5C0(ctx, base);
}

// UGFxObject's ActionScript natives call this with the object's value.
REX_EXTERN(sub_82941BB0) {
  const uint32_t previous = invoking_object;
  invoking_object = ctx.r3.u32 - 0x3C;
  __imp__sub_82941BB0(ctx, base);
  invoking_object = previous;
}

// Scaleform Invoke(Interface, Object, Result, Method, Args, ArgCount, IsDisplayObject).
REX_EXTERN(sub_82B6B5A8) {
  const uint32_t object = invoking_object;
  if (prompts_enabled && object && ctx.r6.u32 && ctx.r8.u32 <= 16) {
    MenuCall call{object, {}, {}, {}, {}, {}, {}};
    bool plain = true, buttons = false;
    for (uint32_t a = ctx.r6.u32; base[a] && call.method.size() < 128; ++a) call.method += char(base[a]);
    std::lock_guard lock(menu_mutex);
    for (uint32_t i = 0; i < ctx.r8.u32; ++i) {
      const uint32_t value = ctx.r7.u32 + 16 * i;
      std::array<uint8_t, 16> raw;
      std::memcpy(raw.data(), base + value, 16);
      const uint32_t type = Load32(base, value + 4);
      std::u16string text;
      bool translated = false;
      uint32_t context = 0;
      if (type == kValueStringW) {
        for (uint32_t a = Load32(base, value + 8); a && (base[a] || base[a + 1]) && text.size() < 1000; a += 2)
          text += char16_t(base[a] << 8 | base[a + 1]);
        const auto it = std::find_if(translations.rbegin(), translations.rend(),
                                     [&](const Translation& t) { return t.result == text; });
        if (it != translations.rend()) {
          text = it->source;
          context = it->context;
          translated = buttons = true;
        } else if (RestoreFontMarkup(text)) {
          buttons = true;
        }
      } else if (type > kValueStringW) {
        plain = false;  // objects hold references that a later call cannot reuse
      }
      if (type == kValueString) plain = false;
      call.args.push_back(raw);
      call.strings.push_back(std::move(text));
      call.translated.push_back(translated);
      call.contexts.push_back(context);
    }
    // Calls with the same method and non-text arguments replace each other.
    const auto same = [&](const MenuCall& c) {
      if (c.object != object || c.method != call.method || c.args.size() != call.args.size()) return false;
      for (size_t i = 0; i < c.args.size(); ++i) {
        if (Load32(c.args[i].data(), 4) == kValueStringW) continue;
        if (c.args[i] != call.args[i]) return false;
      }
      return true;
    };
    std::erase_if(menu_calls, same);
    if (buttons && plain) {
      ObjectIdentity(base, object, call.identity);
      if (menu_calls.size() >= 128) menu_calls.erase(menu_calls.begin());
      menu_calls.push_back(std::move(call));
    }
  }
  __imp__sub_82B6B5A8(ctx, base);
}

void Gow3RefreshInputPrompts(PPCContext& ctx, uint8_t* base) {
  if (!prompts_enabled) return;
  const bool keyboard = rex::input::mnk::ShowKeyboardPrompts();
  std::vector<MenuText> texts;
  std::vector<MenuCall> calls;
  {
    std::lock_guard lock(menu_mutex);
    if (keyboard == menu_prompts_keyboard) return;
    menu_prompts_keyboard = keyboard;
    const auto gone = [&](const auto& t) {
      uint32_t identity[4];
      ObjectIdentity(base, t.object, identity);
      return !std::equal(std::begin(identity), std::end(identity), std::begin(t.identity));
    };
    std::erase_if(menu_texts, gone);
    std::erase_if(menu_calls, gone);
    texts = menu_texts;
    calls = menu_calls;
  }
  const PPCContext saved = ctx;
  for (const auto& t : texts) {
    const uint32_t sp = (saved.r1.u32 - 8192) & ~15u;
    const uint32_t member = sp + 1024;
    const uint32_t text = StoreFString(base, member, t.member);
    StoreFString(base, text, t.text);
    ctx.r1.u64 = sp;
    ctx.r3.u64 = t.object;
    if (t.setter == kSetString) {
      ctx.r4.u64 = member;
      ctx.r5.u64 = text;
      ctx.r6.u64 = t.context;
      __imp__sub_8292D488(ctx, base);
    } else {
      ctx.r4.u64 = text;
      ctx.r5.u64 = t.context;
      __imp__sub_8292D5C0(ctx, base);
    }
  }
  for (const auto& c : calls) {
    const uint32_t sp = (saved.r1.u32 - 16384) & ~15u;
    const uint32_t result = sp + 1024, args = result + 32, name = args + 16 * 16;
    uint32_t free_space = (name + uint32_t(c.method.size()) + 16) & ~15u;
    std::memcpy(base + name, c.method.c_str(), c.method.size() + 1);
    for (size_t i = 0; i < c.args.size(); ++i) {
      std::memcpy(base + args + 16 * i, c.args[i].data(), 16);
      if (Load32(c.args[i].data(), 4) != kValueStringW) continue;
      std::u16string text = c.strings[i];
      if (!c.translated[i]) {
        text = ResolveFontMarkup(std::move(text), keyboard);
      } else {
        // TranslateString(Context, Text, Result) again, for the current device.
        const uint32_t source = free_space, translation = source + 2048 + 2 * uint32_t(text.size());
        StoreFString(base, source, text);
        std::memset(base + translation, 0, 32);
        ctx.r1.u64 = sp;
        ctx.r3.u64 = c.contexts[i];
        ctx.r4.u64 = source;
        ctx.r5.u64 = translation;
        __imp__sub_82942C38(ctx, base);
        text = LoadFString(base, translation + 8);
        if (const uint32_t data = Load32(base, translation + 8)) {
          ctx.r1.u64 = sp;
          ctx.r3.u64 = data;
          sub_822B4FF8(ctx, base);
        }
      }
      const uint32_t data = free_space;
      for (size_t k = 0; k < text.size(); ++k) {
        base[data + 2 * k] = uint8_t(text[k] >> 8);
        base[data + 2 * k + 1] = uint8_t(text[k]);
      }
      base[data + 2 * text.size()] = 0;
      base[data + 2 * text.size() + 1] = 0;
      Store32(base, args + 16 * uint32_t(i) + 8, data);
      free_space = (data + 2 * uint32_t(text.size()) + 2 + 15) & ~15u;
    }
    std::memset(base + result, 0, 16);
    ctx.r1.u64 = sp;
    ctx.r3.u64 = Load32(base, c.object + 0x3C);
    ctx.r4.u64 = Load32(base, c.object + 0x44);
    ctx.r5.u64 = result;
    ctx.r6.u64 = name;
    ctx.r7.u64 = args;
    ctx.r8.u64 = c.args.size();
    ctx.r9.u64 = (Load32(base, c.object + 0x40) & 0x8F) == 8;
    if (!ctx.r3.u32 || !ctx.r4.u32) continue;
    __imp__sub_82B6B5A8(ctx, base);
    if (Load32(base, result + 4) >> 6 & 1) {  // release a managed result
      ctx.r1.u64 = sp;
      ctx.r3.u64 = Load32(base, result);
      ctx.r4.u64 = result;
      ctx.r5.u64 = Load32(base, result + 8);
      sub_82B6A348(ctx, base);
    }
  }
  ctx = saved;
}
