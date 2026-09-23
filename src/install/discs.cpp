#include "discs.h"

#include <cctype>
#include <map>
#include <stdexcept>

#include "install.h"

namespace fs = std::filesystem;

namespace kg::install {
namespace {

std::string lower(std::string_view s) {
  std::string o(s);
  for (char& c : o) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return o;
}

bool all_digits(std::string_view s) {
  if (s.empty()) return false;
  for (char c : s) {
    if (!std::isdigit(static_cast<unsigned char>(c))) return false;
  }
  return true;
}

}  // namespace

DiscRef parse_disc_ref(std::string_view s) {
  DiscRef r;
  size_t hash = s.find('#');
  if (hash == std::string_view::npos) {
    r.archive = std::string(s);
    return r;
  }
  r.archive = std::string(s.substr(0, hash));
  std::string_view rest = s.substr(hash + 1);
  if (all_digits(rest)) r.index = std::stoi(std::string(rest));
  else r.label = std::string(rest);
  return r;
}

int pick_disc(const std::vector<disc::Disc>& discs, const DiscRef& ref) {
  if (!ref.label.empty()) {
    std::string want = lower(ref.label);
    for (size_t i = 0; i < discs.size(); ++i) {
      if (lower(discs[i].label) == want) return static_cast<int>(i);
    }
    return -1;
  }
  if (ref.index > 0) {
    if (static_cast<size_t>(ref.index) > discs.size()) return -1;
    return ref.index - 1;
  }
  return discs.empty() ? -1 : 0;
}

std::vector<disc::Disc> resolve_discs(const rt::Env& e, const std::vector<std::string>& refs,
                                      const fs::path& work,
                                      const std::function<void(const std::string&)>& say) {
  std::map<std::string, std::vector<disc::Disc>> opened;
  std::vector<disc::Disc> out;
  std::error_code ec;

  for (size_t n = 0; n < refs.size(); ++n) {
    DiscRef ref = parse_disc_ref(refs[n]);
    auto it = opened.find(ref.archive);
    if (it == opened.end()) {
      fs::path file = find_iso(ref.archive);
      if (file.empty()) {
        throw std::runtime_error("missing disc: " + ref.archive + "\n  looked in " +
                                 iso_dir().string());
      }
      say("opening " + file.filename().string());
      std::vector<disc::Candidate> cands = disc::probe(e, file);
      std::vector<disc::Disc> discs;
      int i = 0;
      for (const disc::Candidate& c : cands) {
        fs::path w = work / ("disc-" + std::to_string(opened.size()) + "-" + std::to_string(i++));
        fs::create_directories(w, ec);
        discs.push_back(disc::open(e, c, w, /*rip_audio=*/true));
      }
      disc::DiscSet set = disc::assemble(disc::set_id_from(file.stem().string()),
                                         file.stem().string(), std::move(discs));
      it = opened.emplace(ref.archive, std::move(set.discs)).first;
      for (const disc::Disc& d : it->second) say("  " + d.label);
    }

    int which = pick_disc(it->second, ref);
    if (which < 0) {
      std::string have;
      for (const disc::Disc& d : it->second) have += "\n    " + d.label;
      throw std::runtime_error("the manifest asks for " + refs[n] + ", which is not in " +
                               ref.archive + ".\n  It holds:" + have);
    }
    out.push_back(it->second[static_cast<size_t>(which)]);
  }
  return out;
}

}  // namespace kg::install
