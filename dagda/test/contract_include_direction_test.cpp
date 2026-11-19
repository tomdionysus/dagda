//
// Dagda - The Distributed Object Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Seam 7's include direction (requirements step 8a): nothing under
// dagda/ledger or dagda/contract includes anything but Dagda's, Dannan's and
// the system's headers, or names an owner's type: a catalogue, a
// filesystem (the FS, not std::filesystem), a namespace tree or a metadata
// record. Comments and string literals are not code: the gates keep Macha's
// trace words. The scanner is checked against planted sources first, so a
// rule that matches nothing fails here.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <regex>
#include <string>
#include <vector>

namespace dagda {
namespace {

// The code of a C++ source: comments and string and character literals
// blanked.
std::string code_of(const std::string& source) {
  std::string out;
  out.reserve(source.size());
  for (size_t i = 0; i < source.size();) {
    if (source.compare(i, 2, "//") == 0) {
      while (i < source.size() && source[i] != '\n') ++i;
    } else if (source.compare(i, 2, "/*") == 0) {
      const auto end = source.find("*/", i + 2);
      i = end == std::string::npos ? source.size() : end + 2;
    } else if (source[i] == '"' || source[i] == '\'') {
      const char quote = source[i++];
      out += quote;
      while (i < source.size() && source[i] != quote) i += source[i] == '\\' ? 2 : 1;
      out += quote;
      ++i;
    } else {
      out += source[i++];
    }
  }
  return out;
}

// Every rule the source breaks, by line.
std::vector<std::string> violations(const std::string& source) {
  static const std::regex include(R"(^\s*#\s*include\s*(["<])([^">]+)[">])");
  static const std::regex owner_type(
      R"(\b(\w*[Cc]atalogue\w*|FileSystem\w*|Filesystem\w*|Fs[A-Z]\w*|Namespace[A-Z]\w*|Metadata[A-Z]\w*|GarbageRef|TombstoneBatch)\b)");
  std::vector<std::string> out;
  size_t from = 0;
  int line = 0;
  while (from <= source.size()) {
    const auto end = std::min(source.find('\n', from), source.size());
    const auto text = source.substr(from, end - from);
    ++line;
    std::smatch match;
    if (std::regex_search(text, match, include)) {
      const auto path = match[2].str();
      const bool ours = match[1] == "\"" && (path.starts_with("dagda/") || path.starts_with("dannan/"));
      // A system header (the standard library's, POSIX's), unless it names
      // an owner's directory.
      static const std::regex owner_path(R"((^|/)(catalogue|filesystem|metadata)/)");
      const bool standard = match[1] == "<" && !std::regex_search(path, owner_path);
      if (!ours && !standard) out.push_back(std::to_string(line) + ": includes " + path);
    }
    const auto code = code_of(text);
    if (std::regex_search(code, match, owner_type)) out.push_back(std::to_string(line) + ": names " + match[1].str());
    from = end + 1;
  }
  return out;
}

std::string read(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

TEST(ContractIncludeDirection, TheScannerFindsWhatItLooksFor) {
  EXPECT_EQ(violations("#include \"catalogue/catalogue.hpp\"\n").size(), 1u);
  EXPECT_EQ(violations("#include \"filesystem/filesystem.hpp\"\n").size(), 1u);
  EXPECT_EQ(violations("#include \"metadata/metadata.hpp\"\n").size(), 1u);
  EXPECT_FALSE(violations("#include <catalogue/catalogue.hpp>\n").empty());
  EXPECT_EQ(violations("#include \"storage/retention.hpp\"\n").size(), 1u);
  EXPECT_EQ(violations("  CatalogueManager& catalogue_;\n").size(), 1u);
  EXPECT_EQ(violations("void f(FileSystem&);\n").size(), 1u);
  EXPECT_EQ(violations("const FsEntry& entry;\n").size(), 1u);
  EXPECT_EQ(violations("MetadataSnapshotView head;\n").size(), 1u);
  EXPECT_EQ(violations("std::vector<GarbageRef> g;\n").size(), 1u);
  EXPECT_EQ(violations("bool catalogue_complete() const;\n").size(), 1u);
  // What is allowed: our headers, the standard library's, std::filesystem,
  // and the words in comments and strings.
  EXPECT_TRUE(violations("#include <filesystem>\n#include \"dagda/ledger/object_trie.h\"\n#include \"dannan/net/codec.h\"\n").empty());
  EXPECT_TRUE(violations("std::filesystem::path p;  // the catalogue's\nconst char* s = \"catalogue inventory incomplete\";\n").empty());
}

TEST(ContractIncludeDirection, TheLedgerAndTheContractKnowNoOwnersTypes) {
  const std::filesystem::path root = DAGDA_SOURCE_DIR;
  size_t scanned = 0;
  std::vector<std::string> found;
  for (const auto* dir : {"dagda/ledger", "dagda/contract", "dagda/heads"}) {
    for (const auto& entry : std::filesystem::directory_iterator(root / dir)) {
      const auto extension = entry.path().extension();
      if (extension != ".h" && extension != ".cpp") continue;
      ++scanned;
      for (const auto& violation : violations(read(entry.path()))) found.push_back(entry.path().filename().string() + ":" + violation);
    }
  }
  EXPECT_GT(scanned, 20u);
  EXPECT_TRUE(found.empty()) << [&] {
    std::string all;
    for (const auto& line : found) all += line + "\n";
    return all;
  }();
}

// Ingest is Macha's, not the store's (Tom, 2026-11-04: "Dagda doesn't need
// ingest, that will be handled by macha"): nothing in the library names
// ingest or a torrent, in code or in comments.
bool names_ingest(const std::string& source) {
  static const std::regex ingest(R"(torrent|ingest)", std::regex::icase);
  return std::regex_search(source, ingest);
}

TEST(ContractIncludeDirection, TheLibraryNamesNoIngest) {
  EXPECT_TRUE(names_ingest("struct TorrentRequest;"));
  EXPECT_TRUE(names_ingest("// a torrent's claim"));
  EXPECT_TRUE(names_ingest("std::string ingest_job_id;"));
  EXPECT_FALSE(names_ingest("struct Lease;"));
  const std::filesystem::path root = DAGDA_SOURCE_DIR;
  size_t scanned = 0;
  std::vector<std::string> found;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(root / "dagda")) {
    if (!entry.is_regular_file()) continue;
    const auto relative = std::filesystem::relative(entry.path(), root / "dagda");
    if (*relative.begin() == "test") continue;
    ++scanned;
    if (names_ingest(read(entry.path()))) found.push_back(relative.string());
  }
  EXPECT_GE(scanned, 80u);
  EXPECT_TRUE(found.empty()) << [&] {
    std::string all;
    for (const auto& name : found) all += name + "\n";
    return all;
  }();
}

}  // namespace
}  // namespace dagda
