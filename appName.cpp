#include "appName.hpp"

#include <algorithm>
#include <chrono>
#include <climits>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace fs = std::filesystem;

namespace {
  struct SEntry {
    std::string id, name, wmClass, icon;
  };

  std::vector<SEntry>                          g_entries;
  std::unordered_map<std::string, SEntry>      g_resolved;
  std::unordered_map<std::string, SAppIcon>    g_icons;
  std::vector<fs::file_time_type>              g_stamps;
  std::chrono::steady_clock::time_point        g_lastCheck;
  bool                                         g_loaded = false;

  std::vector<fs::path> applicationDirs() {
    std::vector<fs::path> dirs;

    const char* dataHome = std::getenv("XDG_DATA_HOME");
    const char* home     = std::getenv("HOME");
    if (dataHome && *dataHome)
      dirs.emplace_back(fs::path(dataHome) / "applications");
    else if (home && *home)
      dirs.emplace_back(fs::path(home) / ".local/share/applications");

    const char*       dataDirs = std::getenv("XDG_DATA_DIRS");
    const std::string list     = dataDirs && *dataDirs ? dataDirs : "/usr/local/share:/usr/share";
    size_t            start    = 0;
    while (start <= list.size()) {
      size_t end = list.find(':', start);
      if (end == std::string::npos)
        end = list.size();
      if (end > start)
        dirs.emplace_back(fs::path(list.substr(start, end - start)) / "applications");
      start = end + 1;
    }

    return dirs;
  }

  // Directory times move when an entry is added, removed or renamed, which is how
  // an installed web app reaches this cache without a Hyprland reload.
  std::vector<fs::file_time_type> stamps(const std::vector<fs::path>& dirs) {
    std::vector<fs::file_time_type> result;
    for (const auto& dir : dirs) {
      std::error_code ec;
      const auto      time = fs::last_write_time(dir, ec);
      result.push_back(ec ? fs::file_time_type::min() : time);
    }
    return result;
  }

  SEntry readEntry(const fs::path& path) {
    SEntry        entry{.id = path.stem().string()};
    std::ifstream file(path);
    std::string   line;
    bool          inEntry = false;

    while (std::getline(file, line)) {
      if (line.starts_with('[')) {
        inEntry = line == "[Desktop Entry]";
        continue;
      }
      if (!inEntry)
        continue;
      if (line.starts_with("Name="))
        entry.name = line.substr(5);
      else if (line.starts_with("StartupWMClass="))
        entry.wmClass = line.substr(15);
      else if (line.starts_with("Icon="))
        entry.icon = line.substr(5);
    }

    return entry;
  }

  void load(const std::vector<fs::path>& dirs) {
    g_entries.clear();
    g_resolved.clear();
    g_icons.clear();

    // Earlier directories win, so a user's own entry overrides the system one.
    std::unordered_set<std::string> seen;
    for (const auto& dir : dirs) {
      std::error_code ec;
      for (const auto& file : fs::directory_iterator(dir, ec)) {
        if (file.path().extension() != ".desktop" || !seen.insert(file.path().filename().string()).second)
          continue;
        auto entry = readEntry(file.path());
        if (!entry.name.empty())
          g_entries.push_back(std::move(entry));
      }
    }
  }

  std::string lower(std::string text) {
    std::ranges::transform(text, text.begin(), [](unsigned char c) { return std::tolower(c); });
    return text;
  }
}

namespace {
  // When the file was last written, following a link to it. Omarchy's icon
  // sets link each icon into hicolor once and redraw the file behind the link
  // when the theme changes, so the path stays put while this moves.
  long long writeStamp(const std::string& path) {
    std::error_code ec;
    const auto      time = fs::last_write_time(path, ec);
    return ec ? 0 : static_cast<long long>(time.time_since_epoch().count());
  }

  // Rescans the desktop entries when their directories change, and restamps the
  // icons found so far, at most every couple of seconds. Icons that were not
  // found are looked up again, so artwork installed after a window opened
  // still reaches its titlebar.
  void refresh() {
    const auto now = std::chrono::steady_clock::now();
    if (g_loaded && now - g_lastCheck <= std::chrono::seconds(2))
      return;

    g_lastCheck      = now;
    const auto dirs  = applicationDirs();
    auto       times = stamps(dirs);
    if (!g_loaded || times != g_stamps) {
      load(dirs);
      g_stamps = std::move(times);
      g_loaded = true;
    }

    std::erase_if(g_icons, [](const auto& icon) { return icon.second.path.empty(); });
    for (auto& [_, icon] : g_icons)
      icon.stamp = writeStamp(icon.path);
  }

  const SEntry& resolve(const std::string& windowClass) {
    refresh();

    if (const auto cached = g_resolved.find(windowClass); cached != g_resolved.end())
      return cached->second;

    const SEntry* found = nullptr;
    for (const auto& entry : g_entries) {
      if (entry.wmClass == windowClass) {
        found = &entry;
        break;
      }
    }
    if (!found) {
      const auto wanted = lower(windowClass);
      for (const auto& entry : g_entries) {
        if (lower(entry.id) == wanted) {
          found = &entry;
          break;
        }
      }
    }

    return g_resolved[windowClass] = found ? *found : SEntry{};
  }

  // hicolor is where apps install their own icons, and where Omarchy links the
  // icons of the icon set in use, ahead of the packaged ones in /usr/share.
  std::vector<fs::path> hicolorDirs() {
    // applicationDirs() lists $XDG_DATA_HOME first, then $XDG_DATA_DIRS; the
    // legacy ~/.icons goes between them, as in the icon theme spec.
    std::vector<fs::path> dirs;
    for (const auto& applications : applicationDirs())
      dirs.emplace_back(applications.parent_path() / "icons/hicolor");
    const char* home = std::getenv("HOME");
    if (home && *home)
      dirs.insert(dirs.begin() + std::min<size_t>(1, dirs.size()), fs::path(home) / ".icons/hicolor");
    return dirs;
  }

  // A size directory reads "48x48", "48x48@2", "48" or "scalable". Scalable
  // wins, then the largest raster, so the icon stays sharp when scaled down.
  int sizeRank(const std::string& name) {
    if (name.starts_with("scalable"))
      return INT_MAX;
    try {
      return std::stoi(name);
    } catch (...) { return 0; }
  }

  std::string findInTheme(const fs::path& theme, const std::string& icon) {
    std::error_code ec;
    if (!fs::is_directory(theme, ec))
      return "";

    std::string best;
    int         bestRank = -1;
    for (const auto& size : fs::directory_iterator(theme, ec)) {
      if (!size.is_directory(ec))
        continue;
      const int rank = sizeRank(size.path().filename().string());
      if (rank <= bestRank)
        continue;
      for (const auto* ext : {".svg", ".png"}) {
        const auto candidate = size.path() / "apps" / (icon + ext);
        if (fs::is_regular_file(candidate, ec)) {
          best     = candidate.string();
          bestRank = rank;
          break;
        }
      }
    }

    return best;
  }

  std::string findIcon(const std::string& icon) {
    if (icon.empty())
      return "";

    std::error_code ec;
    if (icon.starts_with('/'))
      return fs::is_regular_file(icon, ec) ? icon : "";
    // An Icon= is a name, never a relative path.
    if (icon.find('/') != std::string::npos)
      return "";

    for (const auto& theme : hicolorDirs())
      if (auto found = findInTheme(theme, icon); !found.empty())
        return found;

    for (const auto* ext : {".svg", ".png"}) {
      const auto candidate = fs::path("/usr/share/pixmaps") / (icon + ext);
      if (fs::is_regular_file(candidate, ec))
        return candidate.string();
    }

    return "";
  }
}

std::string omarchyAppName(const std::string& windowClass) {
  if (windowClass.empty())
    return "";
  return resolve(windowClass).name;
}

SAppIcon omarchyAppIcon(const std::string& windowClass) {
  if (windowClass.empty())
    return {};

  const auto& entry = resolve(windowClass);
  if (const auto cached = g_icons.find(windowClass); cached != g_icons.end())
    return cached->second;

  // Without a desktop entry the class itself is the conventional icon name.
  auto path = findIcon(entry.icon.empty() ? windowClass : entry.icon);
  if (path.empty() && entry.icon.empty())
    path = findIcon(lower(windowClass));

  return g_icons[windowClass] = SAppIcon{.path = path, .stamp = writeStamp(path)};
}
