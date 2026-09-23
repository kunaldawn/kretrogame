#include "install.h"

#include "../disc/disc.h"
#include "../disc/drive.h"
#include "../session/session.h"
#include "build.h"
#include "discs.h"
#include "keys.h"
#include "registry.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>

#include "../util/paths.h"
#include "../util/toml.h"
#include "iso.h"

namespace kg::install {
namespace fs = std::filesystem;

namespace {

std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
  return s;
}

const char* env(const char* k) {
  const char* v = std::getenv(k);
  return (v && *v) ? v : nullptr;
}

// Case-insensitive existence check against a real directory listing, because
// what a disc calls GAME.EXE a manifest may call Game.exe.
bool exists_ci(const fs::path& root, const std::string& want) {
  std::error_code ec;
  fs::path cur = root;
  std::string w = want;
  std::replace(w.begin(), w.end(), '\\', '/');
  std::stringstream ss(w);
  std::string part;
  while (std::getline(ss, part, '/')) {
    if (part.empty()) continue;
    bool found = false;
    for (const fs::directory_entry& de : fs::directory_iterator(cur, ec)) {
      if (lower(de.path().filename().string()) == lower(part)) {
        cur = de.path();
        found = true;
        break;
      }
    }
    if (!found) return false;
  }
  return true;
}

// Moves a tree, and falls back to a hardlink farm across a filesystem boundary.
// Neither costs the bytes; a copy would, twice, for a disc set.
void move_or_link(const fs::path& from, const fs::path& to) {
  std::error_code ec;
  fs::create_directories(to.parent_path(), ec);
  fs::rename(from, to, ec);
  if (!ec) return;

  // A single file - an audio track is one - has no tree to walk, and
  // recursive_directory_iterator over a file yields nothing, which would drop
  // the track silently. Link or copy it directly instead.
  if (!fs::is_directory(from, ec)) {
    std::error_code one;
    fs::create_hard_link(from, to, one);
    if (one) {
      one.clear();
      fs::copy_file(from, to, fs::copy_options::overwrite_existing, one);
      if (one) throw std::runtime_error("cannot place " + to.string() + ": " + one.message());
    }
    fs::remove(from, ec);
    return;
  }

  ec.clear();
  fs::create_directories(to, ec);
  std::error_code walk;
  for (const fs::directory_entry& de : fs::recursive_directory_iterator(from, walk)) {
    std::error_code one;
    fs::path dst = to / fs::relative(de.path(), from, one);
    if (de.is_symlink(one)) {
      fs::create_directories(dst.parent_path(), one);
      fs::copy_symlink(de.path(), dst, one);
      continue;
    }
    if (de.is_directory(one)) {
      fs::create_directories(dst, one);
      continue;
    }
    fs::create_directories(dst.parent_path(), one);
    fs::create_hard_link(de.path(), dst, one);
    if (one) {
      one.clear();
      fs::copy_file(de.path(), dst, fs::copy_options::overwrite_existing, one);
      if (one) throw std::runtime_error("cannot place " + dst.string() + ": " + one.message());
    }
  }
  if (walk) throw std::runtime_error("cannot read " + from.string() + ": " + walk.message());
  fs::remove_all(from, ec);
}

}  // namespace

bool is_outside_the_game(const std::string& path, const std::string& install_dir) {
  std::string p = lower(path);
  std::replace(p.begin(), p.end(), '\\', '/');
  while (!p.empty() && p.front() == '/') p.erase(p.begin());
  if (p.empty()) return false;

  // The scratch, first: an installer that unpacked itself into a Temp folder
  // has left behind something no game reads, and it is often the largest thing
  // in the diff.
  static const char* const kScratch[] = {"windows/temp/", "temp/", "tmp/"};
  for (const char* s : kScratch) {
    if (p.rfind(s, 0) == 0) return false;
  }
  // users/<whoever>/Temp and users/<whoever>/Local Settings/Temp, without
  // knowing what the account on this machine is called.
  if (p.rfind("users/", 0) == 0) {
    size_t slash = p.find('/', 6);
    if (slash != std::string::npos) {
      std::string rest = p.substr(slash + 1);
      if (rest.rfind("temp/", 0) == 0 || rest.rfind("local settings/temp/", 0) == 0 ||
          rest.rfind("appdata/local/temp/", 0) == 0) {
        return false;
      }
    }
  }

  std::string dir = lower(install_dir);
  std::replace(dir.begin(), dir.end(), '\\', '/');
  while (!dir.empty() && dir.front() == '/') dir.erase(dir.begin());
  while (!dir.empty() && dir.back() == '/') dir.pop_back();
  if (dir.empty()) return true;   // nothing is the game's, so everything travels
  if (p == dir) return false;
  return p.rfind(dir + "/", 0) != 0;
}

SystemFiles gather_system_files(const fs::path& drive_c, const std::vector<std::string>& paths,
                                const fs::path& into) {
  SystemFiles out;
  std::error_code ec;
  for (const std::string& rel : paths) {
    fs::path from = drive_c / rel;
    fs::file_status st = fs::symlink_status(from, ec);
    if (ec || !fs::exists(st)) { ec.clear(); continue; }
    fs::path to = into / rel;
    fs::create_directories(to.parent_path(), ec);
    ec.clear();
    if (fs::is_symlink(st)) {
      fs::remove(to, ec);
      ec.clear();
      fs::copy_symlink(from, to, ec);
      if (!ec) ++out.files;
      ec.clear();
      continue;
    }
    if (fs::is_directory(st)) {
      // A directory the installer made and left empty still has to exist: a
      // game that writes its config into C:\GameData on first run wants the
      // folder there. Nothing is counted for it - it is no bytes and no file.
      fs::create_directories(to, ec);
      ec.clear();
      continue;
    }
    if (!fs::is_regular_file(st)) continue;
    uint64_t sz = fs::file_size(from, ec);
    if (ec) { ec.clear(); sz = 0; }
    // A hardlink where the filesystem allows it: the staging prefix is thrown
    // away when the Build is destroyed, so these bytes need never be copied.
    fs::remove(to, ec);
    ec.clear();
    fs::create_hard_link(from, to, ec);
    if (ec) {
      ec.clear();
      fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
      if (ec) { ec.clear(); continue; }
    }
    ++out.files;
    out.bytes += sz;
  }
  return out;
}

size_t restore_system_files(const fs::path& system, const fs::path& drive_c) {
  std::error_code ec;
  if (!fs::is_directory(system, ec)) return 0;
  size_t placed = 0;
  std::error_code walk;
  for (const fs::directory_entry& de : fs::recursive_directory_iterator(system, walk)) {
    std::error_code one;
    fs::path rel = fs::relative(de.path(), system, one);
    if (one || rel.empty()) continue;
    fs::path dst = drive_c / rel;
    if (de.is_symlink(one)) {
      fs::create_directories(dst.parent_path(), one);
      fs::remove(dst, one);
      one.clear();
      fs::copy_symlink(de.path(), dst, one);
      if (!one) ++placed;
      continue;
    }
    if (de.is_directory(one)) {
      fs::create_directories(dst, one);
      continue;
    }
    fs::create_directories(dst.parent_path(), one);
    // A real copy, not a link: the body is a read-only mount and the prefix is
    // a place Wine writes.
    fs::remove(dst, one);
    one.clear();
    fs::copy_file(de.path(), dst, fs::copy_options::overwrite_existing, one);
    if (!one) ++placed;
  }
  return placed;
}

void lay_out_body(const fs::path& root, const fs::path& tree, const std::vector<BodyDisc>& discs,
                  const std::string& registry_fragment, const fs::path& system) {
  std::error_code ec;
  fs::remove_all(root, ec);
  fs::create_directories(root, ec);

  move_or_link(tree, root / "game");

  // Beside the game, never inside it: `tree` is hashed into Meta.tree and the
  // Merkle root has to keep meaning the identity of the installed game. A DLL
  // the installer left in system32 is part of the pack, not part of the game
  // directory, and putting it under game/ would change the root of every
  // install that happened to touch C:.
  if (!system.empty() && fs::exists(system, ec) && !fs::is_empty(system, ec)) {
    move_or_link(system, root / "system");
  }

  // Numbered from 1, because that is what a person calls disc 1, and because
  // Meta.discs[i] is disc i+1 for every reader of this layout.
  for (size_t i = 0; i < discs.size(); ++i) {
    fs::path dst = root / "discs" / std::to_string(i + 1);
    move_or_link(discs[i].tree, dst);
    // The two files that make a directory-backed drive answer a CD check. They
    // are written now, once, while the tree is still writable; at play time it
    // is a read-only DwarFS mount and this would throw.
    disc::write_drive_metadata(dst, discs[i].label, discs[i].serial);
    if (discs[i].audio.empty()) continue;
    fs::path adir = dst / "audio";
    fs::create_directories(adir, ec);
    for (const fs::path& t : discs[i].audio) {
      if (!fs::exists(t, ec)) continue;
      move_or_link(t, adir / t.filename());
    }
  }

  // The same text Meta.registry.fragment carries. It is here as well so that a
  // person who mounts the image can read what the installer wrote without
  // decoding CBOR, and so a future reader of the body needs no metadata at all.
  if (!registry_fragment.empty()) {
    std::ofstream f(root / "registry.reg", std::ios::trunc);
    if (!f) throw std::runtime_error("cannot write the registry fragment into the body");
    f << registry_fragment;
  }
}

std::vector<fs::path> manifest_dirs(const rt::Env& e) {
  std::vector<fs::path> dirs;
  if (const char* m = env("KRETRO_MANIFESTS")) dirs.push_back(m);
  if (e.valid()) dirs.push_back(e.root / "usr/share/kretro/games");
  dirs.push_back(state_dir() / "manifests");
  dirs.push_back("games");  // running from a checkout
  return dirs;
}

fs::path find_manifest(const rt::Env& e, const std::string& id) {
  std::error_code ec;
  for (const fs::path& d : manifest_dirs(e)) {
    fs::path p = d / (id + ".toml");
    if (fs::exists(p, ec)) return p;
  }
  return {};
}

std::vector<std::string> known_games(const rt::Env& e) {
  std::vector<std::string> out;
  std::error_code ec;
  for (const fs::path& d : manifest_dirs(e)) {
    if (!fs::exists(d, ec)) continue;
    for (const fs::directory_entry& de : fs::directory_iterator(d, ec)) {
      if (de.path().extension() == ".toml") {
        std::string id = de.path().stem().string();
        if (std::find(out.begin(), out.end(), id) == out.end()) out.push_back(id);
      }
    }
  }
  std::sort(out.begin(), out.end());
  return out;
}

fs::path iso_dir() {
  if (const char* d = env("KRETRO_ISO_DIR")) return d;
  fs::path in_state = state_dir() / "iso";
  std::error_code ec;
  if (fs::exists(in_state, ec)) return in_state;
  return "iso";
}

bool is_collection_name(const std::string& name) {
  if (name.empty() || name.size() > 1024) return false;
  fs::path p(name);
  // `dir / name` throws `dir` away when `name` is absolute, which is the whole
  // of the hole: a recipe naming /usr/bin/anything is a recipe that chose a
  // file on the importer's machine rather than one in his collection, and for
  // installer_exe that file is then run under Wine. ".." is the same trick with
  // one more step.
  if (p.is_absolute() || p.has_root_name() || p.has_root_directory()) return false;
  for (const fs::path& part : p.lexically_normal()) {
    if (part == "..") return false;
  }
  return true;
}

fs::path find_iso(const std::string& name) {
  // A disc reference is a name in the collection directory. Nothing else is a
  // disc reference, however plausible it looks.
  if (!is_collection_name(name)) return {};
  std::error_code ec;
  fs::path dir = iso_dir();
  fs::path direct = dir / name;
  if (fs::exists(direct, ec)) return direct;
  if (!fs::exists(dir, ec)) return {};
  for (const fs::directory_entry& de : fs::directory_iterator(dir, ec)) {
    if (lower(de.path().filename().string()) == lower(name)) return de.path();
  }
  return {};
}

fs::path find_iso_by_fingerprint(const DiscFingerprint& want) {
  std::error_code ec;
  fs::path dir = iso_dir();
  if (!fs::exists(dir, ec)) return {};
  for (const fs::directory_entry& de : fs::directory_iterator(dir, ec)) {
    if (!de.is_regular_file(ec)) continue;
    // Size first: it costs a stat and rules out almost everything.
    if (want.size && fs::file_size(de.path(), ec) != want.size) continue;
    iso::Info info = iso::scan(de.path(), /*full=*/false);
    if (!want.volume_id.empty() && info.volume_id != want.volume_id) continue;
    if (want.size == 0) continue;
    // The recorded hash may be of the whole image or of the prefix, depending
    // on how the pack was made; a prefix match is enough to select a
    // candidate, and the tree hash is what actually proves the result.
    if (info.prefix == want.blake3) return de.path();
    if (hash_file(de.path()) == want.blake3) return de.path();
  }
  return {};
}

Meta load_manifest(const fs::path& p) {
  Toml t = Toml::parse_file(p);
  Meta m;
  m.id = t.str("id", p.stem().string());
  m.name = t.str("name", m.id);
  m.year = static_cast<uint32_t>(t.integer("year"));
  m.developer = t.str("developer");
  m.publisher = t.str("publisher");

  m.recipe.method = t.str("source.method");
  m.recipe.member = t.str("source.member");
  m.recipe.subdir = t.str("source.subdir");
  m.recipe.setup = t.str("source.setup");
  m.recipe.setup_ref = t.str("source.setup_ref");
  m.recipe.verify = t.array("source.verify");
  m.recipe.discs = t.array("source.discs");
  if (m.recipe.discs.empty() && t.has("source.iso")) {
    m.recipe.discs.push_back(t.str("source.iso"));
  }

  m.run.exe = t.str("run.exe");
  {
    std::vector<std::string> args = t.array("run.args");
    for (size_t i = 0; i < args.size(); ++i) m.run.args += (i ? " " : "") + args[i];
    // Manifests usually write this as a plain string - args = "-game mod
    // -window" - and reading it only as an array threw all of them away,
    // silently, since M1. Both forms are accepted.
    if (m.run.args.empty()) m.run.args = t.str("run.args");
  }
  m.run.width = static_cast<uint32_t>(t.integer("run.width", 640));
  m.run.height = static_cast<uint32_t>(t.integer("run.height", 480));
  m.run.windows_version = t.str("run.windows_version", "win98");

  m.present.dar = t.str("run.dar", "4:3");
  m.present.pause_on_blur = t.boolean("run.pause_on_blur", true);

  m.runtime.dgvoodoo = t.boolean("wine.dgvoodoo", false);

  // Per-game gamepad bindings. Toml has no table iteration, so a manifest lists
  // the buttons it rebinds, the way config.toml lists the games it overrides:
  //   [input]
  //   buttons = ["a", "start"]
  //   a       = "Return"
  //   start   = "F1"
  for (const std::string& b : t.array("input.buttons")) {
    std::string v = t.str("input." + b);
    if (!v.empty()) m.input[b] = v;
  }
  m.runtime.dlloverrides = t.str("wine.dlloverrides");
  m.runtime.winetricks = t.array("wine.winetricks");

  if (m.recipe.method.empty()) throw std::runtime_error(p.string() + ": source.method is required");
  if (m.run.exe.empty()) throw std::runtime_error(p.string() + ": run.exe is required");
  return m;
}

Result run(const rt::Env& e, Meta m, const Options& opt,
           const std::function<void(const std::string&)>& say) {
  if (!e.valid()) throw std::runtime_error("no runtime; install needs the bundled tools");
  ensure_state_dirs();

  fs::path out = games_dir() / (m.id + ".kgpack");
  std::error_code ec;
  if (fs::exists(out, ec) && !opt.force) {
    throw std::runtime_error(m.name + " is already installed. Use --force to reinstall.");
  }

  // This is no longer the GUI's install path. `kretro install <id>` opens the
  // wizard; what is left here is rebuilding a recipe - share.cpp's import_pack
  // calls it - and being the thing tests/unit/test_install.cpp can drive with no
  // display. It runs the same Build the wizard does, one step at a time, and
  // supplies from the manifest every answer the wizard would have asked for.
  Build b(e, staging_dir(m.id), say);
  b.keep_tree = opt.keep_tree;

  // A recipe that came from someone else names the disc as *they* had it filed.
  // Where the name does not resolve, fall back to matching by fingerprint, so
  // the same pressing under a different filename is still recognised. The
  // label or index part of the reference is kept either way.
  std::vector<std::string> refs = m.recipe.discs;
  for (size_t i = 0; i < refs.size(); ++i) {
    DiscRef r = parse_disc_ref(refs[i]);
    if (!find_iso(r.archive).empty()) continue;
    if (i >= m.recipe.fingerprints.size()) continue;
    fs::path p = find_iso_by_fingerprint(m.recipe.fingerprints[i]);
    if (p.empty()) continue;
    std::string suffix;
    if (!r.label.empty()) suffix = "#" + r.label;
    else if (r.index > 0) suffix = "#" + std::to_string(r.index);
    refs[i] = p.filename().string() + suffix;
  }

  // resolve_discs, not open_sources: a manifest names one disc *inside* an
  // archive by its volume label, which a list of whole files cannot say.
  std::vector<disc::Disc> media = resolve_discs(e, refs, staging_dir(m.id) / "media", say);
  if (media.empty() && m.recipe.method != "installer_exe") {
    throw std::runtime_error(m.id + ": the manifest names no disc");
  }

  if (!media.empty()) say("identifying " + media[0].label);
  m.recipe.fingerprints.clear();
  for (const disc::Disc& d : media) {
    iso::Info info = iso::scan(d.iso, /*full=*/true);
    if (!info.valid_iso9660) say("  warning: " + d.label + " has no ISO 9660 volume descriptor");
    say("  " + d.label + "  " + to_hex(info.whole).substr(0, 16));
    m.recipe.fingerprints.push_back(iso::fingerprint(info));
  }
  b.adopt_discs(media);

  if (m.recipe.method == "copy") {
    b.copy_from_disc(m.recipe.subdir);
  } else if (m.recipe.method == "unzip") {
    b.unzip_from_disc(m.recipe.member, m.recipe.subdir);
  } else if (m.recipe.method == "wine_setup" || m.recipe.method == "installer_exe") {
    b.prepare_prefix(m.run.windows_version);
    b.mount_discs();
    m.discs = b.drives();

    // With no auto-swap, a person changing discs mid-install is the normal
    // case rather than the fallback, so this is said every time.
    if (media.size() > 1) {
      say("  This set has " + std::to_string(media.size()) +
          " discs and they are all mounted at once, so most installers");
      say("  never ask. If this one does, run in another terminal:");
      for (size_t i = 0; i < media.size(); ++i) {
        say("    kretro swap " + m.id + " " + std::to_string(i + 1) + "    # " + media[i].label);
      }
    }

    fs::path setup;
    if (m.recipe.method == "installer_exe") {
      // This is the one method whose recipe names a file to execute rather than
      // a disc to read, and a recipe can arrive from anybody: share.cpp's
      // import_pack hands a stranger's Meta straight to this function. The
      // installer has to be in the collection, so that what runs is a file its
      // owner put there.
      if (!is_collection_name(m.recipe.setup)) {
        throw std::runtime_error("this recipe's installer, " + m.recipe.setup +
                                 ", is not in " + iso_dir().string() +
                                 ".\n  A pack only gets to name a file in the collection; put "
                                 "the installer there and try again.");
      }
      setup = find_iso(m.recipe.setup);
      if (setup.empty()) throw std::runtime_error("no installer called " + m.recipe.setup);
      // find_iso answers relative to the collection directory, and we run the
      // installer from its own folder - so a relative path would be resolved
      // twice and Wine would look for iso/iso/Setup.exe.
      setup = fs::absolute(setup);
    } else {
      DiscRef sr = parse_disc_ref(m.recipe.setup_ref.empty()
                                      ? (m.recipe.discs.empty() ? "" : m.recipe.discs[0])
                                      : m.recipe.setup_ref);
      int which = pick_disc(media, sr);
      if (which < 0) which = 0;
      fs::path drive = staging_dir(m.id) / (std::string("drive-") + static_cast<char>('d' + which));
      setup = resolve_path_ci_public(drive, m.recipe.setup);
      if (setup.empty()) {
        throw std::runtime_error(media[static_cast<size_t>(which)].label + " has no " +
                                 m.recipe.setup);
      }
    }

    std::string key = key_for(load_keys(keys_file()), m.id);
    if (!key.empty()) say("  a stored serial is available: " + key);

    b.snapshot_before();
    b.run_setup(setup, /*headless=*/false, [](const std::string&) {});
    b.diff_after();

    // Exactly detect_install_dir's rule, over a list that is now ranked rather
    // than sorted on the spot: the deepest directory holding every verify
    // entry at once. It fails the same way when none does.
    fs::path drive_c = staging_drive_c(staging_dir(m.id));
    fs::path installed;
    if (!m.recipe.verify.empty()) {
      for (const Build::Candidate& c : b.candidates()) {
        bool all = true;
        for (const std::string& v : m.recipe.verify) {
          if (!exists_ci(drive_c / c.dir, v)) { all = false; break; }
        }
        if (all) { installed = c.dir; break; }
      }
    }
    if (installed.empty()) {
      throw std::runtime_error(
          "cannot tell where the game was installed: nothing under C: holds " +
          (m.recipe.verify.empty() ? m.run.exe : m.recipe.verify[0]));
    }
    m.install.install_dir = installed.generic_string();
    say("  installed to C:\\" + m.install.install_dir);
  } else {
    throw std::runtime_error("unknown install method: " + m.recipe.method);
  }

  // The staging branch filled m.discs from the drive letters. Copy and unzip
  // mounted nothing, and their packs carry their discs too - write() reads
  // m.discs to decide whether the body carries them at all, so an empty list
  // here would be a rooted body with an empty discs/ and a Meta that admits
  // to no disc.
  if (m.discs.empty()) {
    for (size_t i = 0; i < media.size(); ++i) {
      Meta::Disc d;
      d.label = media[i].label;
      d.serial = media[i].serial;
      d.ref = i < m.recipe.discs.size() ? m.recipe.discs[i] : disc_ref_for(media[i]);
      d.source = fs::absolute(media[i].source, ec).lexically_normal().string();
      d.embedded = true;
      m.discs.push_back(d);
    }
  }

  // All discs or none - lay_out_body numbers them 1..n and every reader of the
  // rooted layout takes Meta.discs[i] to be disc i+1, so this is one decision
  // for the set, exactly as the wizard's single checkbox is.
  if (!opt.embed_discs) {
    for (Meta::Disc& d : m.discs) d.embedded = false;
  }

  Result res = b.write(m);

  if (opt.expect_root_set) {
    // Advisory, since Part 1: two people clicking through InstallShield need
    // not produce the same bytes, so a mismatch is reported rather than
    // refused, and root_matched says which happened.
    res.root_matched = res.root == opt.expect_root;
    if (res.root_matched) say("the tree matches the recipe");
    else say("the tree differs from the recipe: " + to_hex(res.root) + " not " +
             to_hex(opt.expect_root));
  }
  return res;
}

}  // namespace kg::install
