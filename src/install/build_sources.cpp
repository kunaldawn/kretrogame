// Build's first step: every source the person handed over, opened and
// assembled into one disc set.
#include "build.h"

#include <exception>
#include <mutex>
#include <string>
#include <utility>


namespace kg::install {
namespace fs = std::filesystem;

void Build::open_sources(const std::vector<fs::path>& sources) {
  sources_.clear();
  discs_.clear();
  std::error_code ec;

  // Probing several gigabytes is slow, so this runs on the worker, says which
  // source it is on, and gives up between sources when asked to.
  std::vector<disc::Disc> opened;
  size_t n = 0;
  for (const fs::path& p : sources) {
    if (cancelled_) return;
    ++n;
    Source s = classify_source(p);
    say_("reading " + p.filename().string() + "  (" + std::to_string(n) + " of " +
         std::to_string(sources.size()) + ")");
    try {
      switch (s.kind) {
        case Source::Kind::Directory:
          opened.push_back(disc::open_directory(p));
          break;
        case Source::Kind::DiscImage:
        case Source::Kind::Archive: {
          // probe finds every disc inside this one file, archives inside
          // archives included; open materialises and normalises each.
          std::vector<disc::Candidate> cands = disc::probe(env_, p);
          if (cands.empty()) {
            s.kind = Source::Kind::Unreadable;
            s.trouble = "no disc image inside it";
            break;
          }
          int i = 0;
          for (const disc::Candidate& c : cands) {
            if (cancelled_) return;
            fs::path w = work_ / "media" /
                         ("source-" + std::to_string(n) + "-" + std::to_string(i++));
            fs::create_directories(w, ec);
            opened.push_back(disc::open(env_, c, w, /*rip_audio=*/true));
          }
          break;
        }
        case Source::Kind::BareExe:
          // Not a disc at all. It selects the installer_exe method, and the
          // set can be empty.
          break;
        case Source::Kind::Unreadable:
          break;
      }
    } catch (const std::exception& ex) {
      // One bad zip in three does not cost the other two.
      s.kind = Source::Kind::Unreadable;
      s.trouble = ex.what();
    }
    sources_.push_back(s);
  }

  // One assemble over everything, and this is the whole of what is new here:
  // assemble traverses nothing, so the only way three separately-handed-over
  // dumps of the same disc become one disc and two alternates is to give it
  // the concatenation rather than one source at a time.
  std::string name = sources.empty() ? "" : sources[0].stem().string();
  disc::DiscSet set = disc::assemble(disc::set_id_from(name), name, std::move(opened));
  {
    // mounted() is read on the UI thread from the frame the worker starts, so
    // the one assignment that gives this Build its discs happens under the
    // same lock as everything else the drive row reads.
    std::lock_guard<std::mutex> lk(mounts_mu_);
    discs_ = std::move(set.discs);
  }
  for (const disc::Disc& a : set.alternates) {
    say_("  also: " + a.label + " (alternate dump, bytes differ)");
  }
  say_(std::to_string(discs_.size()) + (discs_.size() == 1 ? " disc" : " discs") + ", one game");
}

}  // namespace kg::install
