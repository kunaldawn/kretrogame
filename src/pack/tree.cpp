#include "tree.h"

#include <sys/stat.h>

#include <algorithm>
#include <cstdio>
#include <stdexcept>

namespace kg {
namespace fs = std::filesystem;

namespace {

std::string hex32(uint32_t v) {
  char b[9];
  std::snprintf(b, sizeof(b), "%08x", v);
  return b;
}

std::string hex64(uint64_t v) {
  char b[17];
  std::snprintf(b, sizeof(b), "%016llx", static_cast<unsigned long long>(v));
  return b;
}

uint64_t parse_hex(std::string_view s) {
  uint64_t v = 0;
  for (char c : s) {
    int d;
    if (c >= '0' && c <= '9') d = c - '0';
    else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
    else throw std::runtime_error("manifest line has a non-hex field");
    v = (v << 4) | static_cast<uint64_t>(d);
  }
  return v;
}

}  // namespace

bool TreeEntry::is_dir() const { return S_ISDIR(mode); }
bool TreeEntry::is_symlink() const { return S_ISLNK(mode); }
bool TreeEntry::is_regular() const { return S_ISREG(mode); }

void Tree::add(TreeEntry e) { entries_.push_back(std::move(e)); }

void Tree::sort() {
  std::sort(entries_.begin(), entries_.end(),
            [](const TreeEntry& a, const TreeEntry& b) { return a.path < b.path; });
}

Tree Tree::from_directory(const fs::path& root) {
  Tree t;
  if (!fs::exists(root)) throw std::runtime_error("no such directory: " + root.string());

  fs::recursive_directory_iterator it(root, fs::directory_options::none);
  for (const fs::directory_entry& de : it) {
    std::string rel = fs::relative(de.path(), root).generic_string();
    if (rel.find('\n') != std::string::npos) {
      // The canonical form is line-based and puts the path last; a newline in a
      // name would make the manifest ambiguous. No real game disc has one.
      throw std::runtime_error("path contains a newline: " + rel);
    }

    struct stat st {};
    if (::lstat(de.path().c_str(), &st) != 0) {
      throw std::runtime_error("cannot stat " + de.path().string());
    }

    TreeEntry e;
    e.path = rel;
    e.mode = static_cast<uint32_t>(st.st_mode);

    if (S_ISDIR(st.st_mode)) {
      e.size = 0;
      e.hash = hash_string("");
    } else if (S_ISLNK(st.st_mode)) {
      std::string target = fs::read_symlink(de.path()).generic_string();
      e.size = target.size();
      e.hash = hash_string(target);
    } else if (S_ISREG(st.st_mode)) {
      e.size = static_cast<uint64_t>(st.st_size);
      e.hash = hash_file(de.path());
    } else {
      throw std::runtime_error("refusing to pack a non-file entry: " + rel);
    }
    t.entries_.push_back(std::move(e));
  }

  t.sort();
  return t;
}

std::string Tree::canonical() const {
  std::string out;
  // 8 + 1 + 16 + 1 + 64 + 1 + path + newline
  out.reserve(entries_.size() * 96);
  for (const TreeEntry& e : entries_) {
    out += hex32(e.mode);
    out += ' ';
    out += hex64(e.size);
    out += ' ';
    out += to_hex(e.hash);
    out += ' ';
    out += e.path;
    out += '\n';
  }
  return out;
}

Tree Tree::from_canonical(std::string_view text) {
  Tree t;
  size_t pos = 0;
  while (pos < text.size()) {
    size_t nl = text.find('\n', pos);
    if (nl == std::string_view::npos) throw std::runtime_error("manifest does not end in a newline");
    std::string_view line = text.substr(pos, nl - pos);
    pos = nl + 1;
    if (line.empty()) continue;
    // 8 + 1 + 16 + 1 + 64 + 1 = 91 characters before the path
    if (line.size() < 92) throw std::runtime_error("manifest line is too short");
    if (line[8] != ' ' || line[25] != ' ' || line[90] != ' ') {
      throw std::runtime_error("manifest line is misaligned");
    }
    TreeEntry e;
    e.mode = static_cast<uint32_t>(parse_hex(line.substr(0, 8)));
    e.size = parse_hex(line.substr(9, 16));
    e.hash = from_hex(line.substr(26, 64));
    e.path = std::string(line.substr(91));
    t.entries_.push_back(std::move(e));
  }
  t.sort();
  return t;
}

Hash Tree::root() const {
  std::string c = canonical();
  return hash_bytes(c.data(), c.size());
}

uint64_t Tree::total_bytes() const {
  uint64_t n = 0;
  for (const TreeEntry& e : entries_) {
    if (e.is_regular()) n += e.size;
  }
  return n;
}

Tree::Diff Tree::diff_to(const Tree& newer) const {
  Diff d;
  size_t a = 0, b = 0;
  const auto& A = entries_;
  const auto& B = newer.entries_;
  while (a < A.size() && b < B.size()) {
    if (A[a].path == B[b].path) {
      if (A[a].hash != B[b].hash || A[a].mode != B[b].mode) d.changed.push_back(B[b]);
      ++a;
      ++b;
    } else if (A[a].path < B[b].path) {
      d.removed.push_back(A[a++]);
    } else {
      d.added.push_back(B[b++]);
    }
  }
  while (a < A.size()) d.removed.push_back(A[a++]);
  while (b < B.size()) d.added.push_back(B[b++]);
  return d;
}

}  // namespace kg
