#include "draft_store.h"

#include <algorithm>
#include <exception>
#include <fstream>
#include <stdexcept>

#include "../../util/file_io.h"
#include "../../util/paths.h"
#include "../../util/safe_names.h"

namespace kg::bundle {
namespace fs = std::filesystem;

namespace {

// The one file the bundle `id` is remembered in.
fs::path draft_file(const fs::path& dir, const std::string& id) { return dir / (id + ".cbor"); }

}  // namespace

fs::path bundles_dir() { return state_dir() / "bundles"; }

std::vector<Draft> load_drafts(const fs::path& dir, std::vector<std::string>* unreadable) {
  std::vector<Draft> out;
  std::error_code ec;
  for (const fs::directory_entry& de : fs::directory_iterator(dir, ec)) {
    if (de.path().extension() != ".cbor") continue;
    try {
      out.push_back(decode_draft(read_file_or_empty(de.path())));
    } catch (const std::exception& ex) {
      if (unreadable) unreadable->push_back(de.path().filename().string() + ": " + ex.what());
    }
  }
  std::sort(out.begin(), out.end(), [](const Draft& a, const Draft& b) {
    return a.title != b.title ? a.title < b.title : a.id < b.id;
  });
  return out;
}

void save_draft(const fs::path& dir, const Draft& d, const std::string& was) {
  // The file is named by the id, and the id is joined onto a directory: it is
  // held to the rule the id is held to everywhere else.
  if (!kg::id_is_safe(d.id)) throw std::runtime_error("the bundle id '" + d.id + "' is not a name a file can have");
  std::error_code ec;
  fs::create_directories(dir, ec);
  fs::path file = draft_file(dir, d.id);
  if (was != d.id && fs::exists(file, ec)) {
    throw std::runtime_error("another bundle is already remembered as '" + d.id +
                             "': give this one a different title or id");
  }
  fs::path tmp = file;
  tmp += ".new";
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    std::string bytes = encode_draft(d);
    f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    f.close();
    if (!f) {
      fs::remove(tmp, ec);
      throw std::runtime_error("cannot remember this bundle in " + dir.string());
    }
  }
  fs::rename(tmp, file, ec);
  if (ec) {
    fs::remove(tmp, ec);
    throw std::runtime_error("cannot remember this bundle in " + dir.string() + ": " + ec.message());
  }
  if (!was.empty() && was != d.id && kg::id_is_safe(was)) fs::remove(draft_file(dir, was), ec);
}

void forget_draft(const fs::path& dir, const std::string& id) {
  if (!kg::id_is_safe(id)) return;
  std::error_code ec;
  fs::remove(draft_file(dir, id), ec);
}

std::string unused_id(const fs::path& dir, const std::string& base) {
  std::error_code ec;
  std::string id = base;
  for (int n = 2; fs::exists(draft_file(dir, id), ec); ++n) id = base + "-" + std::to_string(n);
  return id;
}

bool stamp_built(const fs::path& dir, const std::string& id, const std::string& path, uint64_t size,
                 const std::string& when) {
  if (!kg::id_is_safe(id)) return false;
  fs::path file = draft_file(dir, id);
  std::error_code ec;
  if (!fs::exists(file, ec)) return false;
  Draft d;
  try {
    d = decode_draft(read_file_or_empty(file));
  } catch (const std::exception&) {
    return false;
  }
  d.last_built = path;
  d.last_size = size;
  d.last_built_at = when;
  save_draft(dir, d, d.id);
  return true;
}

bool remember_built(const fs::path& dir, Draft d, const std::vector<Check>& checks, const std::string& path,
                    uint64_t size, const std::string& when) {
  if (!kg::id_is_safe(d.id)) return false;
  std::error_code ec;
  if (fs::exists(draft_file(dir, d.id), ec)) return false;
  for (const Check& c : checks) {
    if (!d.acked(c.id)) d.acknowledged.push_back(c.id);
  }
  d.last_built = path;
  d.last_size = size;
  d.last_built_at = when;
  save_draft(dir, d);
  return true;
}

}  // namespace kg::bundle
