#include "appName.hpp"

#include <algorithm>
#include <chrono>
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

  struct SResolved {
    std::string name, icon;
  };

  std::vector<SEntry>                        g_entries;
  std::unordered_map<std::string, SResolved> g_resolved;
  std::unordered_map<std::string, std::string> g_iconPaths;
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
    g_iconPaths.clear();

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

  // Chromium names a web app's window class after the profile that opened it,
  // chrome-<site>-Default or chrome-<site>-Profile_2, while its desktop entry
  // records whichever profile installed it. Compare classes without that suffix.
  std::string withoutProfile(const std::string& windowClass) {
    const auto lowered = lower(windowClass);
    if (!lowered.starts_with("chrome-"))
      return lowered;
    if (lowered.ends_with("-default"))
      return lowered.substr(0, lowered.size() - 8);
    const auto dash = lowered.rfind("-profile_");
    if (dash != std::string::npos && dash + 9 < lowered.size() &&
        std::ranges::all_of(lowered.substr(dash + 9), [](unsigned char c) { return std::isdigit(c); }))
      return lowered.substr(0, dash);
    return lowered;
  }

  std::vector<fs::path> iconBases() {
    std::vector<fs::path> bases;
    const char*           dataHome = std::getenv("XDG_DATA_HOME");
    const char*           home     = std::getenv("HOME");
    if (dataHome && *dataHome)
      bases.emplace_back(fs::path(dataHome) / "icons");
    else if (home && *home)
      bases.emplace_back(fs::path(home) / ".local/share/icons");
    if (home && *home)
      bases.emplace_back(fs::path(home) / ".icons");

    const char*       dataDirs = std::getenv("XDG_DATA_DIRS");
    const std::string list     = dataDirs && *dataDirs ? dataDirs : "/usr/local/share:/usr/share";
    size_t            start    = 0;
    while (start <= list.size()) {
      size_t end = list.find(':', start);
      if (end == std::string::npos)
        end = list.size();
      if (end > start)
        bases.emplace_back(fs::path(list.substr(start, end - start)) / "icons");
      start = end + 1;
    }
    return bases;
  }

  // An entry's Icon is a file path or a name looked up in the hicolor theme,
  // which holds each app's own artwork, and then in /usr/share/pixmaps.
  std::string iconPath(const std::string& icon) {
    if (icon.empty())
      return "";
    if (const auto cached = g_iconPaths.find(icon); cached != g_iconPaths.end())
      return cached->second;

    std::string     found;
    std::error_code ec;
    if (icon.starts_with('/')) {
      if (fs::is_regular_file(icon, ec))
        found = icon;
    } else if (icon.find('/') == std::string::npos) {
      static const char* SIZES[] = {"scalable", "512x512", "256x256", "192x192", "128x128", "96x96", "64x64", "48x48", "32x32"};
      for (const auto& base : iconBases()) {
        for (const auto* size : SIZES) {
          for (const auto* ext : {".svg", ".png"}) {
            const auto path = base / "hicolor" / size / "apps" / (icon + ext);
            if (fs::is_regular_file(path, ec)) {
              found = path.string();
              break;
            }
          }
          if (!found.empty())
            break;
        }
        if (!found.empty())
          break;
      }
      if (found.empty()) {
        for (const auto* ext : {".svg", ".png"}) {
          const auto path = fs::path("/usr/share/pixmaps") / (icon + ext);
          if (fs::is_regular_file(path, ec)) {
            found = path.string();
            break;
          }
        }
      }
    }

    g_iconPaths[icon] = found;
    return found;
  }

  const SResolved& resolve(const std::string& windowClass) {
    static const SResolved EMPTY;
    if (windowClass.empty())
      return EMPTY;

    const auto now = std::chrono::steady_clock::now();
    if (!g_loaded || now - g_lastCheck > std::chrono::seconds(2)) {
      g_lastCheck      = now;
      const auto dirs  = applicationDirs();
      auto       times = stamps(dirs);
      if (!g_loaded || times != g_stamps) {
        load(dirs);
        g_stamps = std::move(times);
        g_loaded = true;
      }
    }

    if (const auto cached = g_resolved.find(windowClass); cached != g_resolved.end())
      return cached->second;

    const SEntry* match = nullptr;
    for (const auto& entry : g_entries) {
      if (entry.wmClass == windowClass) {
        match = &entry;
        break;
      }
    }
    if (!match) {
      const auto wanted = lower(windowClass);
      for (const auto& entry : g_entries) {
        if (lower(entry.id) == wanted) {
          match = &entry;
          break;
        }
      }
    }
    if (!match) {
      const auto wanted = withoutProfile(windowClass);
      if (wanted != lower(windowClass)) {
        for (const auto& entry : g_entries) {
          if (!entry.wmClass.empty() && withoutProfile(entry.wmClass) == wanted) {
            match = &entry;
            break;
          }
        }
      }
    }

    SResolved resolved;
    if (match) {
      resolved.name = match->name;
      resolved.icon = iconPath(match->icon);
    }
    if (resolved.icon.empty())
      resolved.icon = iconPath(windowClass);
    if (resolved.icon.empty())
      resolved.icon = iconPath(lower(windowClass));

    return g_resolved[windowClass] = std::move(resolved);
  }
}

std::string omarchyAppName(const std::string& windowClass) {
  return resolve(windowClass).name;
}

std::string omarchyAppIcon(const std::string& windowClass) {
  return resolve(windowClass).icon;
}
