// Same live output detection used by the renderer, with an independent Vulkan
// format check for the settings app. Creates no window and launches no game.
#define VK_USE_PLATFORM_WAYLAND_KHR
#include <vulkan/vulkan.h>
#include <wayland-client.h>
#include <rex/ui/hdr_display.h>
#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <vector>

static bool VulkanWayland(bool require_hdr) {
  wl_display* display = wl_display_connect(nullptr);
  if (!display) return false;
  auto* registry = wl_display_get_registry(display);
  wl_compositor* compositor = nullptr;
  static const wl_registry_listener listener = {
    [](void* p, wl_registry* r, uint32_t name, const char* type, uint32_t version) {
      if (!std::strcmp(type, "wl_compositor")) *static_cast<wl_compositor**>(p) =
          static_cast<wl_compositor*>(wl_registry_bind(r, name, &wl_compositor_interface, std::min(version, 4u)));
    }, [](void*, wl_registry*, uint32_t) {}};
  wl_registry_add_listener(registry, &listener, &compositor);
  bool ok = rex::ui::WaylandRoundtrip(display, nullptr) && compositor;
  auto* surface = ok ? wl_compositor_create_surface(compositor) : nullptr;
  VkInstance instance = VK_NULL_HANDLE;
  VkSurfaceKHR vk_surface = VK_NULL_HANDLE;
  const char* extensions[] = {"VK_KHR_surface", "VK_KHR_wayland_surface", "VK_EXT_swapchain_colorspace"};
  VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  ci.enabledExtensionCount = require_hdr ? 3 : 2;
  ci.ppEnabledExtensionNames = extensions;
  bool hdr = false;
  if (surface && vkCreateInstance(&ci, nullptr, &instance) == VK_SUCCESS) {
    VkWaylandSurfaceCreateInfoKHR sci{VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR};
    sci.display = display;
    sci.surface = surface;
    if (vkCreateWaylandSurfaceKHR(instance, &sci, nullptr, &vk_surface) == VK_SUCCESS) {
      uint32_t count = 0;
      vkEnumeratePhysicalDevices(instance, &count, nullptr);
      std::vector<VkPhysicalDevice> devices(count);
      vkEnumeratePhysicalDevices(instance, &count, devices.data());
      for (auto gpu : devices) {
        uint32_t queue_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(gpu, &queue_count, nullptr);
        std::vector<VkQueueFamilyProperties> queues(queue_count);
        vkGetPhysicalDeviceQueueFamilyProperties(gpu, &queue_count, queues.data());
        bool can_present = false;
        for (uint32_t q = 0; q < queue_count; ++q) {
          VkBool32 supported = false;
          if (vkGetPhysicalDeviceSurfaceSupportKHR(gpu, q, vk_surface, &supported) == VK_SUCCESS &&
              supported && (queues[q].queueFlags & VK_QUEUE_GRAPHICS_BIT)) can_present = true;
        }
        if (!can_present) continue;
        uint32_t n = 0;
        if (vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, vk_surface, &n, nullptr) != VK_SUCCESS) continue;
        std::vector<VkSurfaceFormatKHR> formats(n);
        if (vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, vk_surface, &n, formats.data()) != VK_SUCCESS) continue;
        if (!require_hdr && n) hdr = true;
        for (const auto& f : formats) {
          hdr |= f.colorSpace == VK_COLOR_SPACE_HDR10_ST2084_EXT &&
              (f.format == VK_FORMAT_A2B10G10R10_UNORM_PACK32 || f.format == VK_FORMAT_A2R10G10B10_UNORM_PACK32);
        }
      }
    }
    if (vk_surface) vkDestroySurfaceKHR(instance, vk_surface, nullptr);
    vkDestroyInstance(instance, nullptr);
  }
  if (surface) wl_surface_destroy(surface);
  if (compositor) wl_compositor_destroy(compositor);
  wl_registry_destroy(registry);
  wl_display_disconnect(display);
  return hdr;
}
static std::string JsonString(const std::string& text) {
  std::ostringstream out;
  out << '"';
  for (unsigned char c : text) {
    if (c == '"' || c == '\\') out << '\\' << c;
    else if (c < 32) out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << unsigned(c);
    else out << c;
  }
  out << '"';
  return out.str();
}
int main(int argc, char** argv) {
  if (argc == 2 && !std::strcmp(argv[1], "--wayland-supported")) return VulkanWayland(false) ? 0 : 1;
  auto displays = rex::ui::ProbeHdrDisplays();
  bool hdr_display = std::any_of(displays.begin(), displays.end(), [](const auto& d) { return d.hdr; });
  const char* backend = std::getenv("GDK_BACKEND");
  const bool forced_x11 = backend && std::string(backend) == "x11";
  bool vulkan_hdr = !forced_x11 && hdr_display && VulkanWayland(true);
  std::cout << "{\"available\":" << (vulkan_hdr ? "true" : "false") << ",\"displays\":[";
  for (size_t i = 0; i < displays.size(); ++i) {
    const auto& d = displays[i];
    if (i) std::cout << ',';
    std::cout << "{\"name\":" << JsonString(d.name) << ",\"description\":" << JsonString(d.description)
              << ",\"hdr\":" << (d.hdr && vulkan_hdr ? "true" : "false") << ",\"peak_nits\":" << d.peak_nits << '}';
  }
  std::cout << "],\"reason\":" << JsonString(vulkan_hdr ? "HDR display and Vulkan HDR10 available" :
      forced_x11 ? "Xwayland is forced; HDR is unavailable" :
      hdr_display ? "Vulkan HDR10 output unavailable" : "No active HDR output detected") << "}\n";
}
