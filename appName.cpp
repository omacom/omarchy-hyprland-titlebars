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

  std::vector<SEntry>                          g_entries;
  std::unordered_map<std::string, std::string> g_resolved;
  std::unordered_map<std::string, std::string> g_resolvedIcons;
  std::vector<fs::file_time_type>              g_stamps;
  std::chrono::steady_clock::time_point        g_lastCheck;
  bool                                         g_loaded = false;

  // The XDG data directories, the user's first, each with a subdirectory.
  std::vector<fs::path> dataDirs(const std::string& subdir) {
    std::vector<fs::path> dirs;

    const char* dataHome = std::getenv("XDG_DATA_HOME");
    const char* home     = std::getenv("HOME");
    if (dataHome && *dataHome)
      dirs.emplace_back(fs::path(dataHome) / subdir);
    else if (home && *home)
      dirs.emplace_back(fs::path(home) / ".local/share" / subdir);

    const char*       dataDirs = std::getenv("XDG_DATA_DIRS");
    const std::string list     = dataDirs && *dataDirs ? dataDirs : "/usr/local/share:/usr/share";
    size_t            start    = 0;
    while (start <= list.size()) {
      size_t end = list.find(':', start);
      if (end == std::string::npos)
        end = list.size();
      if (end > start)
        dirs.emplace_back(fs::path(list.substr(start, end - start)) / subdir);
      start = end + 1;
    }

    return dirs;
  }

  std::vector<fs::path> applicationDirs() {
    return dataDirs("applications");
  }

  // An icon name as a file: the scalable app icon first, which is where
  // Omarchy's icon sets put theirs, then the largest fixed size, then pixmaps.
  std::string iconFile(const std::string& icon) {
    if (icon.empty())
      return "";

    std::error_code ec;
    if (icon.starts_with('/'))
      return fs::is_regular_file(icon, ec) ? icon : "";

    const auto bases = dataDirs("icons");
    for (const auto& base : bases) {
      const auto file = base / "hicolor/scalable/apps" / (icon + ".svg");
      if (fs::is_regular_file(file, ec))
        return file.string();
    }
    for (const auto& base : bases) {
      for (const char* size : {"512x512", "256x256", "128x128", "96x96", "64x64", "48x48", "32x32"}) {
        const auto file = base / "hicolor" / size / "apps" / (icon + ".png");
        if (fs::is_regular_file(file, ec))
          return file.string();
      }
    }
    for (const char* extension : {".svg", ".png"}) {
      const auto file = fs::path("/usr/share/pixmaps") / (icon + extension);
      if (fs::is_regular_file(file, ec))
        return file.string();
    }

    return "";
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
    g_resolvedIcons.clear();

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
  // Reread the entries when a directory holding them has changed, checked at
  // most every couple of seconds.
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

  // The entry whose StartupWMClass, or else whose file name, is the class.
  const SEntry* entryFor(const std::string& windowClass) {
    for (const auto& entry : g_entries) {
      if (entry.wmClass == windowClass)
        return &entry;
    }
    const auto wanted = lower(windowClass);
    for (const auto& entry : g_entries) {
      if (lower(entry.id) == wanted)
        return &entry;
    }
    return nullptr;
  }
}

std::string omarchyAppName(const std::string& windowClass) {
  if (windowClass.empty())
    return "";

  refresh();
  if (const auto cached = g_resolved.find(windowClass); cached != g_resolved.end())
    return cached->second;

  const auto        entry = entryFor(windowClass);
  const std::string name  = entry ? entry->name : "";

  g_resolved[windowClass] = name;
  return name;
}

std::string omarchyAppIcon(const std::string& windowClass) {
  if (windowClass.empty())
    return "";

  refresh();
  if (const auto cached = g_resolvedIcons.find(windowClass); cached != g_resolvedIcons.end())
    return cached->second;

  const auto  entry = entryFor(windowClass);
  std::string file  = iconFile(entry ? entry->icon : "");
  // A window no entry claims may still have an icon named after its class.
  if (file.empty())
    file = iconFile(windowClass);
  if (file.empty())
    file = iconFile(lower(windowClass));

  g_resolvedIcons[windowClass] = file;
  return file;
}
