#pragma once
// File-system locations and Unicode-safe file I/O.
//
// raylib's own file functions use narrow fopen(), which breaks on Windows
// when a path contains non-ASCII characters (e.g. a Japanese user name in
// %APPDATA%). All game I/O therefore goes through std::filesystem / fstream
// (wide paths on Windows) and hands raylib memory buffers.

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace rjc {

namespace fs = std::filesystem;

fs::path exeDir();
fs::path dataDir();  // <exe>/data (Android: inside the APK)
// Per-user writable directory: %APPDATA%\RealJapan (Windows) or
// $XDG_DATA_HOME/RealJapan (Linux). Portable mode (a file named
// "portable.txt" next to the executable) uses <exe>/userdata instead.
fs::path userDir();

// (On Android the data directory lives inside the APK: use these rather than std::filesystem or
// fstream for anything under dataDir().)
std::optional<std::vector<unsigned char>> readFile(const fs::path& p);
bool fileExists(const fs::path& p);
std::vector<fs::path> listFiles(const fs::path& dir);
std::optional<std::string> readText(const fs::path& p);
// Writes via a temporary file + rename so a crash never leaves a torn file.
bool writeFileAtomic(const fs::path& p, const std::string& contents);

std::string pathToUtf8(const fs::path& p);
std::string localTimestamp();  // "2026-09-26 16:20"

}  // namespace rjc
