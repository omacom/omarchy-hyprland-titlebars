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
}

namespace {
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

  std::string readSetting(const fs::path& file, const std::string& key) {
    std::ifstream stream(file);
    std::string   line;
    while (std::getline(stream, line)) {
      if (!line.starts_with(key))
        continue;
      auto value = line.substr(key.size());
      const auto start = value.find_first_not_of(" \t=");
      if (start == std::string::npos)
        continue;
      value = value.substr(start);
      while (!value.empty() && (value.back() == ' ' || value.back() == '\t' || value.back() == '"'))
        value.pop_back();
      if (!value.empty() && value.front() == '"')
        value.erase(0, 1);
      return value;
    }
    return "";
  }

  std::vector<fs::path> iconBaseDirs() {
    std::vector<fs::path> dirs;
    const char*           dataHome = std::getenv("XDG_DATA_HOME");
    const char*           home     = std::getenv("HOME");
    if (dataHome && *dataHome)
      dirs.emplace_back(fs::path(dataHome) / "icons");
    else if (home && *home)
      dirs.emplace_back(fs::path(home) / ".local/share/icons");
    if (home && *home)
      dirs.emplace_back(fs::path(home) / ".icons");

    for (const auto& applications : applicationDirs())
      if (applications.parent_path() != dirs.front().parent_path())
        dirs.emplace_back(applications.parent_path() / "icons");

    return dirs;
  }

  // The icon theme GTK apps use, then what it inherits, then hicolor, which
  // every theme falls back to and where apps install their own icons.
  std::vector<std::string> iconThemes(const std::vector<fs::path>& bases) {
    std::vector<std::string> themes;

    const char* configHome = std::getenv("XDG_CONFIG_HOME");
    const char* home       = std::getenv("HOME");
    fs::path    config     = configHome && *configHome ? fs::path(configHome) : fs::path(home ? home : "") / ".config";
    for (const auto* file : {"gtk-4.0/settings.ini", "gtk-3.0/settings.ini"}) {
      const auto theme = readSetting(config / file, "gtk-icon-theme-name");
      if (!theme.empty()) {
        themes.push_back(theme);
        break;
      }
    }

    for (size_t i = 0; i < themes.size() && i < 4; ++i) {
      for (const auto& base : bases) {
        const auto inherits = readSetting(base / themes[i] / "index.theme", "Inherits");
        if (inherits.empty())
          continue;
        size_t start = 0;
        while (start <= inherits.size()) {
          size_t end = inherits.find(',', start);
          if (end == std::string::npos)
            end = inherits.size();
          const auto parent = inherits.substr(start, end - start);
          if (!parent.empty() && std::ranges::find(themes, parent) == themes.end())
            themes.push_back(parent);
          start = end + 1;
        }
        break;
      }
    }

    for (const auto* fallback : {"hicolor", "Adwaita"})
      if (std::ranges::find(themes, fallback) == themes.end())
        themes.emplace_back(fallback);

    return themes;
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
    auto        consider = [&](const fs::path& dir, int rank) {
      if (rank <= bestRank)
        return;
      for (const auto* ext : {".svg", ".png"}) {
        const auto candidate = dir / (icon + ext);
        if (fs::is_regular_file(candidate, ec)) {
          best     = candidate.string();
          bestRank = rank;
          return;
        }
      }
    };

    for (const auto& first : fs::directory_iterator(theme, ec)) {
      if (!first.is_directory(ec))
        continue;
      const auto name = first.path().filename().string();
      // <size>/apps/<icon> (hicolor, Adwaita, Yaru) and apps/<size>/<icon> (Papirus).
      if (name == "apps") {
        for (const auto& second : fs::directory_iterator(first.path(), ec))
          if (second.is_directory(ec))
            consider(second.path(), sizeRank(second.path().filename().string()));
      } else
        consider(first.path() / "apps", sizeRank(name));
    }

    return best;
  }

  std::string iconPath(const std::string& icon) {
    if (icon.empty())
      return "";

    if (const auto cached = g_iconPaths.find(icon); cached != g_iconPaths.end())
      return cached->second;

    if (icon.starts_with('/')) {
      std::error_code ec;
      return g_iconPaths[icon] = fs::is_regular_file(icon, ec) ? icon : "";
    }

    std::string found;
    const auto  bases = iconBaseDirs();
    for (const auto& theme : iconThemes(bases)) {
      for (const auto& base : bases) {
        found = findInTheme(base / theme, icon);
        if (!found.empty())
          break;
      }
      if (!found.empty())
        break;
    }
    if (found.empty()) {
      std::error_code ec;
      for (const auto* ext : {".svg", ".png"}) {
        const auto candidate = fs::path("/usr/share/pixmaps") / (icon + ext);
        if (fs::is_regular_file(candidate, ec)) {
          found = candidate.string();
          break;
        }
      }
    }

    return g_iconPaths[icon] = found;
  }
}

std::string omarchyAppName(const std::string& windowClass) {
  if (windowClass.empty())
    return "";
  return resolve(windowClass).name;
}

std::string omarchyAppIcon(const std::string& windowClass) {
  if (windowClass.empty())
    return "";
  // Without a desktop entry the class itself is the conventional icon name.
  const auto& entry = resolve(windowClass);
  auto        path  = iconPath(entry.icon.empty() ? windowClass : entry.icon);
  if (path.empty() && entry.icon.empty())
    path = iconPath(lower(windowClass));
  return path;
}
