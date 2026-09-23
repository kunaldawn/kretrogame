// The shelf: kretro's big-picture interface.
//
// Four screens and no settings screen, because everything configurable is
// per-game data. Navigable entirely by gamepad or by keyboard, and you can
// still type to filter, because that instinct is the right one.
#include "app.h"

#include "../config/config.h"
#include "../config/scaling.h"
#include "../disc/disc.h"
#include "../install/discs.h"
#include "../install/share.h"

#include <SDL.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <functional>
#include <mutex>
#include <thread>

#include "../gpu/probe.h"
#include "../install/install.h"
#include "../install/iso.h"
#include "../session/session.h"
#include "../util/hash.h"
#include "../util/paths.h"
#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_sdlrenderer2.h"
#include "bundles.h"
#include "library.h"
#include "wizard.h"
#include "stage.h"
#include "widgets.h"

namespace kg::gui {
namespace fs = std::filesystem;

namespace {

enum class Screen { Shelf, Game, Create, Import, Doctor, Library, Settings, Timeline, Bundles };

// Where a file browser opens. The person's home, because a pack somebody sent
// you is in Downloads; not games_dir(), because a pack already there is a game
// already installed.
fs::path home_of_the_person() {
  if (const char* h = std::getenv("HOME"); h && *h) return fs::path(h);
  return fs::path("/");
}

class App {
 public:
  App(const rt::Env& e, SDL_Renderer* r) : env_(e), textures_(r), wizard_(e, r), bundles_(e) { reload(); }

  ~App() {
    // Nothing joined this before. A job on the worker outlived the window and
    // went on writing into the members of a half-destroyed App, and a Build on
    // it went on holding gigabytes of staging tree.
    if (worker_.joinable()) worker_.join();
  }

  void reload() {
    entries_ = scan(env_);
    if (selected_ >= entries_.size()) selected_ = entries_.empty() ? 0 : entries_.size() - 1;
  }

  void frame(SDL_Window* win) {
    win_ = win;
    switch (screen_) {
      case Screen::Shelf: shelf(); break;
      case Screen::Game: game(); break;
      case Screen::Create:
        if (!wizard_.draw(win_)) {
          std::string s = wizard_.status();
          screen_ = Screen::Shelf;
          reload();
          if (s.rfind("play:", 0) == 0) { open(s.substr(5)); play(s.substr(5)); }
          else status_ = s;
          if (quit_after_wizard_) quit_ = true;
        } else if (wizard_.take_library_request()) {
          library_scanned_ = false;
          screen_ = Screen::Library;
        }
        break;
      case Screen::Import: import_page(); break;
      case Screen::Doctor: doctor(); break;
      case Screen::Library: library(); break;
      case Screen::Settings: settings(); break;
      case Screen::Timeline: timeline(); break;
      case Screen::Bundles: bundles_.draw(); break;
    }
    if (busy_) modal();
  }

  bool quit() const { return quit_; }

  // Closing the window during an install must not take a running installer and
  // a multi-gigabyte staging tree down without a word. The wizard asks first.
  void request_quit() {
    if (screen_ == Screen::Create && wizard_.busy()) {
      quit_after_wizard_ = true;
      wizard_.ask_abandon();
      return;
    }
    quit_ = true;
  }

  // A page that threw in the middle of drawing itself. Nothing here can put
  // the window down: the message goes where every other failure goes, the
  // status line, and the person is left with a program to read it in.
  void page_failed(const std::string& what) { status_ = what; }

  void back() {
    // The wizard is eight screens inside one Screen, so Escape has to ask it
    // first: dropping to the Shelf from the install step would walk away from
    // an installer that is halfway through writing a game.
    if (screen_ == Screen::Create && wizard_.back()) return;
    if (screen_ == Screen::Bundles && bundles_.back()) return;
    if (busy_) return;   // a job of our own is up; the modal owns the screen
    if (screen_ != Screen::Shelf) screen_ = Screen::Shelf;
    else quit_ = true;
  }

 private:
  // ---- textures -------------------------------------------------------
  const Texture* texture(const fs::path& p) { return textures_.file(p); }

  void forget_textures_for(const std::string& id) { textures_.forget_matching("/" + id + "/"); }

  // ---- shared chrome --------------------------------------------------
  void begin_page(const char* title, const char* hint) { gui::begin_page(title, hint, big_); }

  // ---- shelf ----------------------------------------------------------
  void shelf() {
    begin_page("kretro",
               "arrows move   Enter play   A add a game   I import   L library   S settings   "
               "D doctor   B bundles   Esc quit");

    if (!filter_.empty()) {
      ImGui::TextDisabled("filter: %s", filter_.c_str());
      ImGui::Spacing();
    }

    std::vector<const Entry*> shown;
    for (const Entry& e : entries_) {
      if (filter_.empty() || contains_ci(e.name, filter_) || contains_ci(e.id, filter_)) {
        shown.push_back(&e);
      }
    }

    if (shown.empty()) {
      ImGui::Spacing();
      ImGui::TextWrapped(
          entries_.empty()
              ? "No games yet, and no manifests found.\n\nPut a disc image in the iso directory and press A."
              : "Nothing matches that filter.");
      ImGui::End();
      return;
    }

    const float avail = ImGui::GetContentRegionAvail().x;
    const float tile_w = 300.0f, tile_h = 225.0f, pad = 18.0f;
    int cols = std::max(1, static_cast<int>((avail + pad) / (tile_w + pad)));

    ImGui::BeginChild("grid", ImVec2(0, 0), false);
    for (size_t i = 0; i < shown.size(); ++i) {
      if (i % cols) ImGui::SameLine(0, pad);
      tile(*shown[i], tile_w, tile_h, i);
    }
    ImGui::EndChild();
    ImGui::End();
  }

  static bool contains_ci(const std::string& hay, const std::string& needle) {
    auto it = std::search(hay.begin(), hay.end(), needle.begin(), needle.end(),
                          [](char a, char b) { return std::tolower(a) == std::tolower(b); });
    return it != hay.end();
  }

  void tile(const Entry& e, float w, float h, size_t index) {
    std::string sub;
    if (!e.installed) {
      sub = e.blocked ? e.blocked_reason : "not installed - press Enter";
    } else if (e.sessions) {
      sub = human_time(e.total_seconds) + "  \xc2\xb7  " + ago(e.last_played);
    } else {
      sub = e.year ? std::to_string(e.year) : "";
      sub += sub.empty() ? "never played" : "  \xc2\xb7  never played";
    }
    const Texture* art = texture(e.title_png.empty() ? e.last_png : e.title_png);
    const uint8_t dim = e.installed ? 0 : (e.blocked ? 150 : 90);
    bool focused = false;
    bool activated = gui::tile(e.id, e.name, sub, art, big_, w, h, dim, &focused);
    if (focused) focus_index_ = index;
    if (activated) {
      selected_ = index_of(e.id);
      screen_ = Screen::Game;
    }
  }

  size_t index_of(const std::string& id) const {
    for (size_t i = 0; i < entries_.size(); ++i) if (entries_[i].id == id) return i;
    return 0;
  }

  // ---- one game -------------------------------------------------------
  void game() {
    if (entries_.empty()) { screen_ = Screen::Shelf; return; }
    const Entry& e = entries_[std::min(selected_, entries_.size() - 1)];
    begin_page(e.name.c_str(), "Esc back");

    ImGui::BeginChild("left", ImVec2(ImGui::GetContentRegionAvail().x * 0.58f, 0), false);
    const Texture* hero = texture(e.last_png.empty() ? e.title_png : e.last_png);
    if (hero) {
      float w = ImGui::GetContentRegionAvail().x;
      float h = w * static_cast<float>(hero->h) / static_cast<float>(hero->w);
      ImGui::Image(reinterpret_cast<ImTextureID>(hero->tex), ImVec2(w, h));
      ImGui::TextDisabled("where you left off");
    } else if (e.installed) {
      ImGui::TextDisabled("No screenshot yet. One is taken automatically while you play.");
    }
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("right", ImVec2(0, 0), false);

    if (e.installed) {
      ImGui::PushFont(big_);
      if (ImGui::Button("Play", ImVec2(-1, 60))) play(e.id);
      ImGui::PopFont();
      ImGui::Spacing();
      if (ImGui::Button("Timeline")) go(Screen::Timeline);
      ImGui::SameLine();
      if (ImGui::Button("Settings")) go(Screen::Settings);
      ImGui::SameLine();
      if (ImGui::Button("Uninstall")) confirm_uninstall_ = true;
      ImGui::Spacing();
      uninstall_modal(e);

      if (e.sessions) {
        ImGui::Text("%zu session%s, %s in total", e.sessions, e.sessions == 1 ? "" : "s",
                    human_time(e.total_seconds).c_str());
        ImGui::TextDisabled("last played %s", ago(e.last_played).c_str());
        if (!e.last_note.empty()) {
          ImGui::Spacing();
          ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.86f, 0.55f, 1.0f));
          ImGui::TextWrapped("\"%s\"", e.last_note.c_str());
          ImGui::PopStyleColor();
        }
      } else {
        ImGui::TextDisabled("never played");
      }

      ImGui::Spacing();
      ImGui::Separator();
      ImGui::TextDisabled("%zu files, %s, packed to %s", e.files, human_size(e.tree_bytes).c_str(),
                          human_size(e.pack_bytes).c_str());
      ImGui::TextDisabled("one file: %s", e.pack.filename().c_str());

      // The state Meta.discs has been able to describe since the format
      // existed, finally said out loud. Play still works - most of these games
      // only check for the disc when they start a new campaign or play their
      // video - so this is a fact about the pack, not a refusal, and it sits
      // with the other facts about the pack rather than beside the Play button.
      if (!e.absent_discs.empty()) {
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.86f, 0.55f, 1.0f));
        if (e.flat_body) {
          ImGui::TextWrapped("Needs the original disc. This pack was made before packs carried "
                             "their discs - rebuild it to include them.");
        } else if (e.absent_discs.size() == 1) {
          ImGui::TextWrapped("Needs the original disc: this pack names %s but does not carry it.",
                             e.absent_discs[0].c_str());
        } else {
          ImGui::TextWrapped("Needs the original discs: this pack names %zu it does not carry.",
                             e.absent_discs.size());
        }
        ImGui::PopStyleColor();
        for (const std::string& d : e.absent_discs) ImGui::TextDisabled("  %s", d.c_str());
      }

      ImGui::Spacing();
      std::vector<std::string> gens = session::generations(e.id);
      if (!gens.empty()) {
        ImGui::Separator();
        ImGui::Text("snapshots");
        ImGui::TextDisabled("every session that wrote something left one");
        ImGui::BeginChild("gens", ImVec2(0, 140), true);
        for (auto it = gens.rbegin(); it != gens.rend(); ++it) {
          ImGui::PushID(it->c_str());
          ImGui::TextUnformatted(it->c_str());
          ImGui::SameLine(120);
          if (ImGui::SmallButton("restore")) {
            try { session::restore(e.id, *it); status_ = "restored " + *it; }
            catch (const std::exception& ex) { status_ = ex.what(); }
          }
          ImGui::PopID();
        }
        ImGui::EndChild();
      }

      ImGui::Spacing();
      std::vector<session::Record> j = session::journal(e.id);
      if (!j.empty()) {
        ImGui::Separator();
        ImGui::Text("timeline");
        ImGui::BeginChild("journal", ImVec2(0, 160), true);
        for (const session::Record& r : j) {
          char when[64];
          std::strftime(when, sizeof(when), "%Y-%m-%d %H:%M", std::localtime(&r.started));
          ImGui::Text("%s  %s", when,
                      human_time(static_cast<double>(r.ended - r.started)).c_str());
          if (!r.note.empty()) ImGui::TextDisabled("  \"%s\"", r.note.c_str());
        }
        ImGui::EndChild();
      }
    } else {
      // A game we have a manifest for and have not installed. When the discs
      // it names are not in the collection this page used to be a dead end -
      // no button, and an instruction to put a file in iso_dir() - which was
      // true while the manifest was the gate. It is not the gate any more and
      // the wizard never reads that directory as a requirement: you hand it
      // the files. So what we cannot see is a note above the same button.
      if (!e.missing_discs.empty()) {
        std::string what = e.missing_discs[0];
        for (size_t i = 1; i < e.missing_discs.size(); ++i) what += ", " + e.missing_discs[i];
        ImGui::TextWrapped("Not installed. Nothing in %s answers to %s.",
                           install::iso_dir().c_str(), what.c_str());
        ImGui::Spacing();
        ImGui::TextDisabled(
            "That directory is where the shelf looks, and it is the only place the shelf "
            "looks. The wizard takes the files themselves - an .iso, a .bin and its .cue, a "
            "zip, a folder, a bare setup .exe - from anywhere on this machine, or dropped on "
            "this window. What the manifest knows is offered either way.");
      } else if (e.blocked) {
        ImGui::TextWrapped("Not installed, and this game's manifest does not load: %s",
                           e.blocked_reason.c_str());
        ImGui::Spacing();
        ImGui::TextDisabled(
            "The wizard opens with nothing prefilled, which is how it opens for a disc "
            "nobody has written a manifest for.");
      } else {
        ImGui::TextWrapped("Not installed yet.");
      }
      ImGui::Spacing();
      ImGui::PushFont(big_);
      // The manifest is not a gate any more, it is a head start: everything
      // it knows about this game becomes the wizard's first answer, and every
      // one of those answers is still editable.
      if (ImGui::Button("Install", ImVec2(-1, 60))) open_wizard_for(e.id);
      ImGui::PopFont();
    }

    if (!status_.empty()) {
      ImGui::Spacing();
      ImGui::TextWrapped("%s", status_.c_str());
    }
    ImGui::EndChild();
    ImGui::End();
  }

  // Everything the Import page knows about one file, worked out once.
  //
  // The page used to open the pack, read its meta and - for a recipe - resolve
  // every disc it names against the collection, on every frame it drew.
  // Resolving is find_iso_by_fingerprint, which hashes the head of every image
  // in the collection: at sixty frames a second, on a collection of .iso files,
  // that is a disk that never stops and a page that never becomes responsive.
  // None of it changes while the same file is on screen.
  struct Inspected {
    fs::path of;                 // the file this describes; empty means nothing
    std::string trouble;         // why it could not be read, when it could not
    Meta meta;
    bool has_body = false;
    Hash root{};
    uint64_t bytes = 0;
    install::Prefill prefill;    // the recipe's discs, resolved; empty for a capsule
  };
  Inspected import_;

  void inspect_for_import(const fs::path& f) {
    if (import_.of == f) return;
    import_ = Inspected{};
    import_.of = f;
    std::error_code ec;
    import_.bytes = fs::file_size(f, ec);
    try {
      Pack p = Pack::open(f);
      import_.meta = p.meta();
      import_.has_body = p.has_body();
      import_.root = p.header().blake3_root;
      // A capsule carries its discs, so there is nothing to go looking for.
      if (!import_.has_body) import_.prefill = install::draft_from_meta(env_, import_.meta);
    } catch (const std::exception& ex) {
      import_.trouble = ex.what();
    }
  }

  // A list of directories and .kgpack files, the same plain walk over
  // std::filesystem the wizard's step 1 uses. Dropping a file on the window
  // was the only way into this page, and a drop is not something a person
  // sitting in front of a shelf with a gamepad can do - nor anyone whose file
  // manager is a terminal. A dialog is out for the same reason it is out of
  // the wizard: another toolkit, another theme, and nothing a pad can drive.
  void import_browser() {
    std::error_code ec;
    ImGui::BeginChild("import-browse", ImVec2(0, 260), true);
    ImGui::TextDisabled("%s", import_dir_.c_str());
    if (ImGui::Selectable("..")) import_dir_ = import_dir_.parent_path();
    std::vector<fs::path> dirs, packs;
    for (const fs::directory_entry& de : fs::directory_iterator(import_dir_, ec)) {
      if (de.is_directory(ec)) { dirs.push_back(de.path()); continue; }
      if (!de.is_regular_file(ec)) continue;
      if (lower_of(de.path().extension().string()) == ".kgpack") packs.push_back(de.path());
    }
    std::sort(dirs.begin(), dirs.end());
    std::sort(packs.begin(), packs.end());
    for (const fs::path& d : dirs) {
      ImGui::PushID(d.c_str());
      if (ImGui::Selectable((d.filename().string() + "/").c_str())) import_dir_ = d;
      ImGui::PopID();
    }
    // Only .kgpack, because this page can read nothing else: a disc belongs to
    // the Library screen, and listing anything else here would be offering
    // sentences that say no.
    for (const fs::path& f : packs) {
      ImGui::PushID(f.c_str());
      if (ImGui::Selectable(f.filename().c_str())) {
        drop_path_ = f;
        import_replace_ = false;
        import_browsing_ = false;
      }
      ImGui::SameLine(ImGui::GetWindowWidth() - 130);
      ImGui::TextDisabled("%s", human_size(fs::file_size(f, ec)).c_str());
      ImGui::PopID();
    }
    if (dirs.empty() && packs.empty()) ImGui::TextDisabled("no .kgpack here, and nowhere to go");
    ImGui::EndChild();
  }

  // A modal that could say only "Import X?" could not say what X is: a capsule
  // or a recipe, which game, how large, whose root. So it is a page.
  void import_page() {
    begin_page("import", "Esc back");

    if (drop_path_.empty()) {
      ImGui::TextWrapped(
          "Nothing to import yet. Pick a .kgpack below, or drop one on this window - a "
          "capsule carries the game itself, a recipe carries only the knowledge and rebuilds "
          "from your own disc.");
      ImGui::Spacing();
      import_browser();
      ImGui::End();
      return;
    }

    std::error_code ec;
    inspect_for_import(drop_path_);
    const Inspected& in = import_;
    ImGui::Text("%s", drop_path_.filename().c_str());
    ImGui::SameLine(ImGui::GetWindowWidth() - 200);
    ImGui::TextDisabled("%s", human_size(in.bytes).c_str());
    ImGui::Spacing();

    // A different file, from the same page: whatever is on screen, including a
    // file that turned out not to be a pack at all.
    if (ImGui::SmallButton(import_browsing_ ? "close the browser" : "pick another file")) {
      import_browsing_ = !import_browsing_;
    }
    if (import_browsing_) import_browser();
    ImGui::Spacing();

    if (!in.trouble.empty()) {
      ImGui::TextWrapped("I cannot read that: %s", in.trouble.c_str());
      ImGui::End();
      return;
    }
    const Meta& m = in.meta;
    const bool has_body = in.has_body;
    const Hash& root = in.root;

    ImGui::Indent(24);
    ImGui::Text("%s", m.name.empty() ? m.id.c_str() : m.name.c_str());
    ImGui::SameLine();
    if (m.year) ImGui::TextDisabled("%u", m.year);
    if (has_body) {
      std::string what = "a capsule: the game, its registry";
      size_t embedded = 0;
      for (const Meta::Disc& d : m.discs) if (d.embedded) ++embedded;
      what += embedded ? " and its " + std::to_string(embedded) +
                             (embedded == 1 ? " disc" : " discs")
                       : ", without its discs";
      ImGui::TextDisabled("%s", what.c_str());
    } else {
      // Not "proves the result": clicking through an installer twice need not
      // produce the same bytes, so what the rebuild gets is compared with the
      // root below and the difference reported, which is a different promise.
      ImGui::TextDisabled(
          "a recipe: it rebuilds from your own disc, and says how the tree compares");
    }
    ImGui::TextDisabled("root  %s", to_hex(root).substr(0, 16).c_str());
    ImGui::Unindent(24);
    ImGui::Spacing();

    bool installed = fs::exists(games_dir() / (m.id + ".kgpack"), ec);
    if (installed) {
      double seconds = 0;
      for (const session::Record& r : session::journal(m.id)) {
        seconds += static_cast<double>(r.ended - r.started);
      }
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.6f, 0.4f, 1.0f));
      ImGui::TextWrapped("%s is already installed, with %s played against it.", m.id.c_str(),
                         human_time(seconds).c_str());
      ImGui::PopStyleColor();
      ImGui::TextWrapped(
          "Importing over it replaces the pack. The saves and the snapshots stay where they "
          "are, because they are not in the pack.");
      ImGui::Checkbox("replace it", &import_replace_);
    }

    if (!has_body) {
      // A recipe's discs are resolved by fingerprint before anything starts, so
      // a set that is short a disc says which one now rather than failing at
      // install time. Resolved once, in inspect_for_import: matching by
      // fingerprint hashes the head of every image in the collection, and this
      // page is redrawn sixty times a second.
      const install::Prefill& pre = in.prefill;
      for (const std::string& missing : pre.missing) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.6f, 0.4f, 1.0f));
        ImGui::TextWrapped("this recipe wants %s, and your collection does not have it",
                           missing.c_str());
        ImGui::PopStyleColor();
      }
      ImGui::Spacing();
      ImGui::PushFont(big_);
      if (pre.missing.empty()) {
        if (ImGui::Button("Rebuild it from your disc", ImVec2(-1, 60))) {
          // By value: the next line puts the cached inspection out of date, and
          // `pre` is a reference into it.
          install::Prefill taken = pre;
          wizard_.begin_from_recipe(taken, root);
          drop_path_.clear();
          screen_ = Screen::Create;
        }
      } else {
        ImGui::BeginDisabled();
        ImGui::Button("Rebuild it from your disc", ImVec2(-1, 60));
        ImGui::EndDisabled();
      }
      ImGui::PopFont();
      ImGui::End();
      return;
    }

    ImGui::Spacing();
    ImGui::PushFont(big_);
    bool blocked = installed && !import_replace_;
    if (blocked) ImGui::BeginDisabled();
    if (ImGui::Button("Import it", ImVec2(-1, 60))) {
      fs::path f = drop_path_;
      bool replace = import_replace_;
      run_job("Importing " + f.filename().string(), [this, f, replace]() {
        install::ImportResult r =
            install::import_pack(env_, f, replace, [this](const std::string& l) { log_line(l); });
        log_line(r.name + " is in your library");
      });
      drop_path_.clear();
      screen_ = Screen::Shelf;
    }
    if (blocked) ImGui::EndDisabled();
    ImGui::PopFont();
    ImGui::End();
  }

  // ---- doctor ---------------------------------------------------------
  void doctor() {
    begin_page("Doctor", "Esc back");
    if (!probed_) { report_ = gpu::probe(); gpu::materialize(report_); probed_ = true; }

    ImGui::Text("display");
    if (report_.wayland) ImGui::TextDisabled("  wayland   %s", report_.wayland_display.c_str());
    if (report_.x11) ImGui::TextDisabled("  x11       %s", report_.x_display.c_str());
    ImGui::Spacing();
    ImGui::Text("graphics");
    for (const gpu::Device& d : report_.devices) {
      ImGui::TextDisabled("  %-22s %-8s %s", d.node.c_str(), gpu::vendor_name(d.vendor),
                          d.readable ? "accessible" : "NOT ACCESSIBLE");
    }
    if (report_.nvidia_present) {
      ImGui::Spacing();
      ImGui::Text("nvidia");
      ImGui::TextDisabled("  driver %s, %zu libraries linked", report_.nvidia_version.c_str(),
                          report_.nvidia_libs.size());
    }
    ImGui::Spacing();
    ImGui::Text("runtime");
    ImGui::TextDisabled("  %s", env_.root.c_str());
    ImGui::Spacing();
    if (report_.problems.empty()) {
      ImGui::TextColored(ImVec4(0.5f, 0.9f, 0.5f, 1.0f), "no problems found");
    } else {
      for (const std::string& p : report_.problems) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.6f, 0.4f, 1.0f));
        ImGui::TextWrapped("%s", p.c_str());
        ImGui::PopStyleColor();
      }
    }
    ImGui::End();
  }

  // A game can be reinstalled from its disc in minutes; a save cannot be
  // reinstalled at all. So the saves are kept unless you say otherwise, and the
  // dialog says what will be freed before it frees it.
  void uninstall_modal(const Entry& e) {
    if (confirm_uninstall_) {
      ImGui::OpenPopup("Uninstall?");
      confirm_uninstall_ = false;
      also_saves_ = false;
    }
    if (!ImGui::BeginPopupModal("Uninstall?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;

    std::error_code ec;
    uint64_t prefix_bytes = 0;
    fs::path prefix = prefixes_dir() / e.id;
    for (const auto& de : fs::recursive_directory_iterator(prefix, ec)) {
      if (de.is_regular_file(ec)) prefix_bytes += de.file_size(ec);
    }
    uint64_t save_bytes = 0;
    fs::path saves = saves_dir() / e.id;
    for (const auto& de : fs::recursive_directory_iterator(saves, ec)) {
      if (de.is_regular_file(ec)) save_bytes += de.file_size(ec);
    }

    ImGui::Text("Uninstall %s?", e.name.c_str());
    ImGui::Spacing();
    ImGui::TextDisabled("  the game            %s", human_size(e.pack_bytes).c_str());
    ImGui::TextDisabled("  its Wine prefix     %s", human_size(prefix_bytes).c_str());
    ImGui::TextDisabled("  saves and snapshots %s", human_size(save_bytes).c_str());
    ImGui::Checkbox("delete the saves too", &also_saves_);
    ImGui::Spacing();
    if (ImGui::Button("Uninstall")) {
      fs::remove(e.pack, ec);
      fs::remove_all(prefix, ec);
      if (also_saves_) fs::remove_all(saves, ec);
      status_ = e.name + " uninstalled";
      reload();
      screen_ = Screen::Shelf;
      ImGui::CloseCurrentPopup();
      ImGui::EndPopup();
      return;
    }
    ImGui::SameLine();
    if (ImGui::Button("Keep it")) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }

  // ---- library --------------------------------------------------------
  //
  // The front door: every disc in your folders, what it is, and whether it can
  // be installed. A multi-disc game whose first disc is not in any folder
  // appears here as a set missing its first disc, which is the honest answer
  // and more use than an install that fails a minute in.
  void library() {
    begin_page("Library", "Esc back");

    config::Config cfg = config::load(config::config_file());
    std::vector<std::string> where = cfg.library_paths;
    if (where.empty()) where.push_back(install::iso_dir().string());

    for (const std::string& w : where) ImGui::TextDisabled("  %s", w.c_str());
    ImGui::Spacing();

    if (busy_) {
      ImGui::TextDisabled("scanning...");
      ImGui::End();
      return;
    }
    if (ImGui::Button(library_scanned_ ? "Scan again" : "Scan")) {
      scan_library(where);
    }
    ImGui::SameLine();
    // Import is reachable from the shelf, from here, and by dropping a pack on
    // the window. A pack somebody handed you is part of a collection, and this
    // is the screen where a collection is looked at; with nothing dropped yet
    // the page says so and asks for one.
    if (ImGui::Button("Import...")) screen_ = Screen::Import;
    if (!drop_note_.empty()) {
      ImGui::SameLine();
      ImGui::TextDisabled("%s", drop_note_.c_str());
    }
    ImGui::Spacing();

    if (!library_scanned_) {
      ImGui::TextWrapped(
          "Nothing scanned yet. Scanning reads every archive in those folders and "
          "works out what discs are inside; on a large collection that takes a "
          "minute or two.");
      ImGui::End();
      return;
    }

    if (ImGui::BeginTable("discs", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
      ImGui::TableSetupColumn("file");
      ImGui::TableSetupColumn("disc");
      ImGui::TableSetupColumn("size");
      ImGui::TableSetupColumn("known as");
      ImGui::TableSetupColumn("");
      ImGui::TableHeadersRow();
      for (const DiscRow& r : library_) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextDisabled("%s", r.archive.c_str());
        ImGui::TableNextColumn();
        ImGui::Text("%s", r.label.empty() ? "-" : r.label.c_str());
        ImGui::TableNextColumn();
        ImGui::TextDisabled("%s", human_size(r.bytes).c_str());
        ImGui::TableNextColumn();
        ImGui::TextDisabled("%s", r.known_as.empty() ? "not in the database" : r.known_as.c_str());
        ImGui::TableNextColumn();
        if (r.state == "ready" && !r.game_id.empty()) {
          ImGui::PushID(r.game_id.c_str());
          if (ImGui::SmallButton("Install")) open_wizard_for(r.game_id);
          ImGui::PopID();
        } else {
          ImGui::TextDisabled("%s", r.state.c_str());
        }
      }
      ImGui::EndTable();
    }
    ImGui::End();
  }

  void scan_library(const std::vector<std::string>& where) {
    // A scan of a real collection takes minutes, so it runs where every other
    // long job in this program runs: on the worker, with the log modal up.
    library_.clear();
    std::vector<fs::path> files;
    std::error_code ec;
    for (const std::string& w : where) {
      for (const auto& de : fs::directory_iterator(w, ec)) {
        if (de.is_regular_file(ec)) files.push_back(de.path());
      }
    }
    std::sort(files.begin(), files.end());
    if (!library_only_.empty()) {
      files.clear();
      files.push_back(library_only_);
    }

    run_job("Scanning your collection", [this, files]() {
      std::vector<iso::Known> db = iso::load_database(env_);
      std::vector<std::string> manifests = install::known_games(env_);
      std::vector<DiscRow> rows;
      for (const fs::path& f : files) {
        // A scan of a real collection is minutes of reading archives. Nothing
        // here was interruptible before, so the only way out was to close the
        // window and lose the scan.
        if (cancel_) { log_line("stopped"); break; }
        std::vector<disc::Candidate> cands;
        try {
          cands = disc::probe(env_, f);
        } catch (const std::exception&) { continue; }
        if (cands.empty()) continue;
        log_line(f.filename().string());

        fs::path work = fs::temp_directory_path() / "kretro-gui-scan";
        std::error_code e2;
        std::vector<disc::Disc> opened;
        for (size_t i = 0; i < cands.size(); ++i) {
          try {
            opened.push_back(disc::open(env_, cands[i], work / std::to_string(i), false));
          } catch (const std::exception&) {}
          fs::remove_all(work / std::to_string(i), e2);
        }
        disc::DiscSet set = disc::assemble(disc::set_id_from(f.stem().string()),
                                           f.stem().string(), std::move(opened));
        for (const disc::Disc& d : set.discs) {
          DiscRow r;
          r.archive = f.filename().string();
          r.label = d.label;
          r.bytes = d.info.size;
          iso::Match m = iso::identify(db, d.info);
          if (m.entry) {
            r.known_as = m.entry->name;
            if (m.suspect_bad_dump) r.known_as += "  (bytes differ - a bad dump?)";
          }
          r.state = "no manifest wants this";
          for (const std::string& id : manifests) {
            fs::path mp = install::find_manifest(env_, id);
            if (mp.empty()) continue;
            Meta mm;
            try { mm = install::load_manifest(mp); } catch (const std::exception&) { continue; }
            bool wants = false;
            for (const std::string& ref : mm.recipe.discs) {
              install::DiscRef dr = install::parse_disc_ref(ref);
              if (dr.archive != r.archive) continue;
              if (dr.label.empty() || lower_of(dr.label) == lower_of(r.label)) wants = true;
            }
            if (!wants) continue;
            r.game_id = id;
            r.state = fs::exists(games_dir() / (id + ".kgpack"), e2) ? "installed" : "ready";
            break;
          }
          rows.push_back(r);
        }
        fs::remove_all(work, e2);
      }
      std::lock_guard<std::mutex> lk(log_mutex_);
      library_ = rows;
      library_scanned_ = true;
    });
  }

  // ---- settings -------------------------------------------------------
  //
  // No save button. Every change is written as it is made: a settings screen
  // with a save button is a settings screen you can lose work in.
  void settings() {
    begin_page("Settings", "Esc back");
    config::Config cfg = config::load(config::config_file());
    bool dirty = false;

    ImGui::Text("window");
    const char* wmodes[] = {"windowed", "borderless", "fullscreen"};
    int wm = static_cast<int>(cfg.window.mode);
    if (ImGui::Combo("mode", &wm, wmodes, 3)) {
      cfg.window.mode = static_cast<config::WindowMode>(wm);
      dirty = true;
    }
    if (ImGui::Checkbox("remember where the window was", &cfg.window.remember_geometry)) dirty = true;

    ImGui::Spacing();
    ImGui::Text("scaling");
    const char* smodes[] = {"integer", "fit", "native"};
    int sm = static_cast<int>(cfg.display.mode);
    if (ImGui::Combo("how", &sm, smodes, 3)) {
      cfg.display.mode = static_cast<config::ScaleMode>(sm);
      dirty = true;
    }
    int sc = static_cast<int>(cfg.display.scale);
    if (ImGui::SliderInt("factor", &sc, 0, 6, sc == 0 ? "automatic" : "%dx")) {
      cfg.display.scale = static_cast<uint32_t>(sc);
      dirty = true;
    }
    ImGui::TextDisabled(
        "integer keeps every game pixel an exact square. fit is the largest whole");
    ImGui::TextDisabled(
        "number that fits, fullscreen. native asks the game for the screen's own size.");

    // What that means, for the game you were last looking at.
    if (!entries_.empty()) {
      const Entry& en = entries_[selected_ < entries_.size() ? selected_ : 0];
      uint32_t pw = 0, ph = 0;
      SDL_DisplayMode dm;
      if (SDL_GetDesktopDisplayMode(0, &dm) == 0) { pw = dm.w; ph = dm.h; }
      uint32_t gw = 640, gh = 480;
      std::error_code e2;
      if (fs::exists(en.pack, e2)) {
        try {
          Meta mm = Pack::open(en.pack).meta();
          if (mm.run.width) { gw = mm.run.width; gh = mm.run.height; }
        } catch (const std::exception&) {}
      }
      config::Geometry g =
          config::compute_geometry(gw, gh, pw, ph, config::for_game(cfg, en.id));
      ImGui::Spacing();
      ImGui::Text("%s would run at %ux%u at %ux -> %ux%u%s", en.name.c_str(), gw, gh, g.scale,
                  g.logical_w * g.scale, g.logical_h * g.scale,
                  g.fullscreen ? ", fullscreen" : "");
      ImGui::SameLine();
      if (ImGui::SmallButton("use these for this game only")) {
        cfg.per_game[en.id] = cfg.display;
        dirty = true;
      }
      if (cfg.per_game.count(en.id)) {
        ImGui::SameLine();
        if (ImGui::SmallButton("clear its override")) {
          cfg.per_game.erase(en.id);
          dirty = true;
        }
      }
    }

    ImGui::Spacing();
    ImGui::Text("library folders");
    int remove_at = -1;
    for (size_t i = 0; i < cfg.library_paths.size(); ++i) {
      ImGui::TextDisabled("  %s", cfg.library_paths[i].c_str());
      ImGui::SameLine();
      ImGui::PushID(static_cast<int>(i));
      if (ImGui::SmallButton("remove")) remove_at = static_cast<int>(i);
      ImGui::PopID();
    }
    if (remove_at >= 0) {
      cfg.library_paths.erase(cfg.library_paths.begin() + remove_at);
      dirty = true;
    }
    ImGui::TextDisabled("  drop a folder on this window to add one");

    if (dirty) config::save(config::config_file(), cfg);
    ImGui::End();
  }

  // ---- timeline -------------------------------------------------------
  //
  // The overlay already captured every write and the harvester already
  // photographed every session. Putting them on one axis is a rewind button for
  // games that never had one.
  void timeline() {
    if (entries_.empty()) { screen_ = Screen::Shelf; return; }
    const Entry& en = entries_[selected_ < entries_.size() ? selected_ : 0];
    begin_page((en.name + " - timeline").c_str(), "Esc back");

    std::vector<std::string> gens = session::generations(en.id);
    std::vector<session::Record> recs = session::journal(en.id);
    if (gens.empty()) {
      ImGui::TextWrapped(
          "Nothing to go back to yet. Every time you play, whatever the game writes is kept "
          "as a snapshot, and they appear here newest first - when it was, how long you "
          "played, what it wrote - with the last picture taken of the game beside the "
          "newest of them.");
      ImGui::End();
      return;
    }

    for (size_t i = gens.size(); i-- > 0;) {
      const std::string& g = gens[i];
      ImGui::PushID(g.c_str());
      const session::Record* rec = nullptr;
      for (const session::Record& r : recs) if (r.generation == g) rec = &r;

      if (const Texture* t = texture(en.last_png); t && i + 1 == gens.size()) {
        ImGui::Image(reinterpret_cast<ImTextureID>(t->tex), ImVec2(160, 120));
        ImGui::SameLine();
      }
      ImGui::BeginGroup();
      ImGui::Text("snapshot %s", g.c_str());
      if (rec) {
        ImGui::TextDisabled("%s, played %s, %zu files written", ago(rec->ended).c_str(),
                            human_time(static_cast<double>(rec->ended - rec->started)).c_str(),
                            rec->files_written);
        if (!rec->note.empty()) ImGui::TextDisabled("\"%s\"", rec->note.c_str());
      }
      if (ImGui::SmallButton("Restore")) {
        restore_gen_ = g;
        confirm_restore_ = true;
      }
      ImGui::EndGroup();
      ImGui::Separator();
      ImGui::PopID();
    }

    if (confirm_restore_) {
      ImGui::OpenPopup("Restore?");
      confirm_restore_ = false;
    }
    if (ImGui::BeginPopupModal("Restore?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
      ImGui::Text("Restore snapshot %s?", restore_gen_.c_str());
      ImGui::TextWrapped(
          "Everything the game has written since then is replaced by what it had "
          "written by then. The current state is kept as a new snapshot first, so "
          "this is reversible.");
      ImGui::Spacing();
      if (ImGui::Button("Restore")) {
        try {
          session::restore(en.id, restore_gen_);
          status_ = "restored " + restore_gen_;
        } catch (const std::exception& ex) {
          status_ = std::string("could not restore: ") + ex.what();
        }
        ImGui::CloseCurrentPopup();
      }
      ImGui::SameLine();
      if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
      ImGui::EndPopup();
    }
    ImGui::End();
  }

  static std::string lower_of(const std::string& v) {
    std::string o = v;
    for (char& c : o) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return o;
  }

  // ---- actions --------------------------------------------------------
  void play(const std::string& id) {
    // The game gets its own screen; ours would only be in the way.
    SDL_HideWindow(win_);
    session::Options o;
    try {
      session::Outcome out = session::play(env_, id, o);
      status_ = "played for " + human_time(out.seconds);
      if (!out.generation.empty()) status_ += ", snapshot " + out.generation.filename().string();
    } catch (const std::exception& ex) {
      status_ = ex.what();
    }
    SDL_ShowWindow(win_);
    SDL_RaiseWindow(win_);
    forget_textures_for(id);
    reload();
  }

  // A job on a thread, its output in the log modal, so the window keeps
  // drawing through the minutes it may take.
  void log_line(const std::string& l) {
    std::lock_guard<std::mutex> lk(log_mutex_);
    log_.push_back(l);
  }


  void run_job(const std::string& title, std::function<void()> job) {
    if (busy_) return;
    busy_ = true;
    busy_title_ = title;
    cancel_ = false;
    { std::lock_guard<std::mutex> lk(log_mutex_); log_.clear(); }
    worker_ = std::thread([this, job] {
      try {
        job();
      } catch (const std::exception& ex) {
        log_line(std::string("failed: ") + ex.what());
      }
      done_ = true;
    });
  }

  void modal() {
    ImGui::OpenPopup("busy");
    ImVec2 c = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(c, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(720, 380));
    if (ImGui::BeginPopupModal("busy", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove)) {
      ImGui::PushFont(big_);
      ImGui::TextUnformatted(busy_title_.c_str());
      ImGui::PopFont();
      ImGui::Separator();
      ImGui::BeginChild("log", ImVec2(0, 260), true);
      {
        std::lock_guard<std::mutex> lk(log_mutex_);
        for (const std::string& l : log_) ImGui::TextWrapped("%s", l.c_str());
      }
      if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 40) ImGui::SetScrollHereY(1.0f);
      ImGui::EndChild();

      if (done_) {
        if (ImGui::Button("Close", ImVec2(-1, 44))) {
          if (worker_.joinable()) worker_.join();
          busy_ = false;
          done_ = false;
          reload();
          ImGui::CloseCurrentPopup();
        }
      } else {
        ImGui::TextDisabled("working...");
        ImGui::SameLine();
        if (ImGui::SmallButton("stop")) cancel_ = true;
      }
      ImGui::EndPopup();
    }
  }

 public:
  void set_fonts(ImFont* body, ImFont* big) {
    body_ = body;
    big_ = big;
    wizard_.set_fonts(body, big);
    bundles_.set_fonts(body, big);
  }
  void open_bundles() { bundles_.open(); screen_ = Screen::Bundles; }
  void type(char c) {
    if (screen_ != Screen::Shelf) return;
    if (c == '\b') { if (!filter_.empty()) filter_.pop_back(); }
    else if (c >= 32 && c < 127) filter_.push_back(c);
  }
  void clear_filter() { filter_.clear(); }
  bool filtering() const { return !filter_.empty(); }
  void go(Screen s) { screen_ = s; }
  void create() { if (is_busy()) return; wizard_.begin(); screen_ = Screen::Create; }

  void open_wizard_for(const std::string& id) {
    if (is_busy()) return;
    try {
      Meta m = install::load_manifest(install::find_manifest(env_, id));
      wizard_.begin_from(install::draft_from_meta(env_, m));
    } catch (const std::exception& ex) {
      status_ = ex.what();
      wizard_.begin();
    }
    screen_ = Screen::Create;
  }

  // Dropping something on the shelf should do the obvious thing with it, and
  // say what it decided rather than acting silently. A program that ignores
  // what you hand it teaches you not to hand it anything.
  void dropped(const std::string& path) {
    std::error_code ec;
    fs::path p = path;
    // While the wizard's first step is up, anything dropped is a source for
    // it. Sending a disc off to the Library screen at that moment would throw
    // away the thing the user was in the middle of doing.
    if (screen_ == Screen::Create) { wizard_.add_source(p); return; }
    if (fs::is_directory(p, ec)) {
      config::Config c = config::load(config::config_file());
      if (std::find(c.library_paths.begin(), c.library_paths.end(), p.string()) ==
          c.library_paths.end()) {
        c.library_paths.push_back(p.string());
        config::save(config::config_file(), c);
      }
      drop_note_ = p.filename().string() + " added to your library folders";
      library_scanned_ = false;
      go(Screen::Library);
      return;
    }
    std::string ext = p.extension().string();
    for (char& ch : ext) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    if (ext == ".kgpack") {
      drop_path_ = p;
      screen_ = Screen::Import;
      return;
    }
    if (ext == ".iso" || ext == ".cue" || ext == ".bin" || ext == ".zip" || ext == ".7z" ||
        ext == ".exe" || ext == ".img" || ext == ".mdf") {
      // library_only_ is what the Library screen reads; drop_path_ belongs to
      // the Import page and nothing else. Setting it here set nothing looking
      // at it and left an .iso behind for the next visit to Import, which
      // opens on whatever drop_path_ holds and can only say it cannot read it.
      drop_note_ = "dropped: " + p.filename().string();
      library_only_ = p;
      library_scanned_ = false;
      go(Screen::Library);
      return;
    }
    drop_note_ = "I do not know what to do with " + p.filename().string();
  }
  void open(const std::string& id) {
    selected_ = index_of(id);
    screen_ = Screen::Game;
  }
  Screen screen() const { return screen_; }
  bool is_busy() const { return busy_ || wizard_.busy(); }

 private:
  const rt::Env& env_;
  SDL_Window* win_ = nullptr;
  std::vector<Entry> entries_;
  Textures textures_;
  size_t selected_ = 0, focus_index_ = 0;
  Screen screen_ = Screen::Shelf;

  // Drag and drop, and the library.
  struct DiscRow {
    std::string archive, label, known_as, game_id, state;
    uint64_t bytes = 0;
  };
  std::vector<DiscRow> library_;
  std::string restore_gen_;
  bool confirm_restore_ = false;
  bool confirm_uninstall_ = false;
  bool also_saves_ = false;
  fs::path drop_path_;
  bool import_replace_ = false;
  // The Import page's own file list, and where it is looking. Started at the
  // user's home rather than at games_dir(): a pack somebody sent you is in
  // Downloads, and a pack already in games_dir() is already installed.
  bool import_browsing_ = false;
  fs::path import_dir_ = home_of_the_person();
  fs::path library_only_;          // set when a single file was dropped
  std::string drop_note_;
  bool library_scanned_ = false;
  std::string filter_, status_;
  bool quit_ = false;

  ImFont *body_ = nullptr, *big_ = nullptr;

  gpu::Report report_;
  bool probed_ = false;

  std::thread worker_;
  std::mutex log_mutex_;
  std::vector<std::string> log_;
  std::atomic<bool> done_{false};
  std::atomic<bool> cancel_{false};
  bool busy_ = false;
  std::string busy_title_;

  Wizard wizard_;
  bool quit_after_wizard_ = false;
  Bundles bundles_;
};

}  // namespace

int run(const rt::Env& e, const Startup& entry) {
  if (!std::getenv("DISPLAY") && !std::getenv("WAYLAND_DISPLAY")) {
    if (entry.create) {
      // Installing means watching an installer click by click; there is no
      // headless form of it left to fall back to, and saying so is better than
      // offering `kretro list` to somebody who asked to install something.
      std::fprintf(stderr,
                   "kretro: no display to draw on.\n"
                   "  Installing a game means watching its installer, and this machine has no\n"
                   "  Wayland or X11 session to watch it in. Run this from a desktop.\n");
    } else {
      std::fprintf(stderr,
                   "kretro: no display to draw on.\n"
                   "  This machine has no Wayland or X11 session, so there is no shelf to show.\n"
                   "  Everything still works from the terminal - try: kretro list\n");
    }
    return 1;
  }

  // Before SDL opens its own X connection, because SDL's X11 backend installs
  // an error handler during SDL_Init and chains to whatever it finds - and
  // because XInitThreads must come before every other Xlib call in the
  // process, this one included.
  install_x_error_handlers();

  // The window opens where it was left, in the mode it was left in.
  config::Config cfg = config::load(config::config_file());
  WindowSpec spec;
  spec.title = "kretro";
  spec.x = cfg.window.x;
  spec.y = cfg.window.y;
  spec.width = cfg.window.width;
  spec.height = cfg.window.height;
  spec.borderless = cfg.window.mode == config::WindowMode::Borderless;
  spec.fullscreen = cfg.window.mode == config::WindowMode::Fullscreen;
  std::string why;
  std::optional<Window> opened = open_window(e, spec, &why);
  if (!opened) {
    std::fprintf(stderr, "kretro: %s\n", why.c_str());
    return 1;
  }
  Window w = *opened;
  SDL_Window* win = w.win;
  ImFont* body = w.body;
  ImFont* big = w.big;

  // A scope of its own, closing before the shutdown sequence below.
  //
  // An App is a Wizard, a Stage and a cache of SDL textures. ~Wizard touches
  // ImGui::GetIO(), ~Stage destroys an SDL texture and ~App destroys the rest
  // of them - all of which need ImGui's context and SDL's video subsystem to
  // still exist. As a plain local of this function it was destroyed at the end
  // of the function, which is after ImGui::DestroyContext() and after
  // SDL_Quit(): every ordinary exit of the shelf ran three destructors over
  // libraries that had already been torn down.
  {
    App app(e, w.ren);
    app.set_fonts(body, big);
    // Three ways in, in the order the arguments are allowed to disagree: a game
    // to open beats a wizard to open, and a wizard with a manifest behind it is
    // open_wizard_for rather than create - the two are Milestone 5's, and the
    // only difference is whether the draft starts from draft_from_meta.
    if (!entry.game.empty()) app.open(entry.game);
    else if (entry.create && entry.preset.empty()) app.create();
    else if (entry.create) app.open_wizard_for(entry.preset);

    while (!app.quit()) {
      SDL_Event ev;
      while (SDL_PollEvent(&ev)) {
        ImGui_ImplSDL2_ProcessEvent(&ev);
        if (ev.type == SDL_QUIT) app.request_quit();
        if (ev.type == SDL_DROPFILE) {
          std::string dropped = ev.drop.file ? ev.drop.file : "";
          if (ev.drop.file) SDL_free(ev.drop.file);
          if (!dropped.empty()) app.dropped(dropped);
        }
        if (ev.type == SDL_CONTROLLERDEVICEADDED) SDL_GameControllerOpen(ev.cdevice.which);
        if (ev.type == SDL_KEYDOWN) {
          switch (ev.key.keysym.sym) {
            case SDLK_ESCAPE:
              // Escape reaches the app even while a job is running, because on
              // the wizard's install step it is the only way to abandon one.
              // back() knows which screens may act on it and which may not.
              if (app.filtering()) app.clear_filter(); else app.back();
              break;
            case SDLK_BACKSPACE: if (!app.is_busy()) app.type('\b'); break;
            case SDLK_F11: {
              Uint32 f = SDL_GetWindowFlags(win);
              SDL_SetWindowFullscreen(win, f & SDL_WINDOW_FULLSCREEN_DESKTOP ? 0
                                                                            : SDL_WINDOW_FULLSCREEN_DESKTOP);
              break;
            }
            default: break;
          }
        }
        // Typing filters the shelf, the way dmenu does. Letters that are also
        // shortcuts only act as shortcuts when nothing is being typed.
        if (ev.type == SDL_TEXTINPUT && !app.is_busy()) {
          char c = ev.text.text[0];
          if (app.screen() == Screen::Shelf && !app.filtering() && (c == 'a' || c == 'A')) {
            app.create();
          } else if (app.screen() == Screen::Shelf && !app.filtering() && (c == 'i' || c == 'I')) {
            app.go(Screen::Import);
          } else if (app.screen() == Screen::Shelf && !app.filtering() && (c == 'd' || c == 'D')) {
            app.go(Screen::Doctor);
          } else if (app.screen() == Screen::Shelf && !app.filtering() && (c == 'l' || c == 'L')) {
            app.go(Screen::Library);
          } else if (app.screen() == Screen::Shelf && !app.filtering() && (c == 's' || c == 'S')) {
            app.go(Screen::Settings);
          } else if (app.screen() == Screen::Shelf && !app.filtering() && (c == 'b' || c == 'B')) {
            app.open_bundles();
          } else {
            app.type(c);
          }
        }
      }

      begin_frame(w);
      // Every page draws from disk, and disk says no: a manifest that moved, a
      // pack whose body cannot be read, a directory that stopped being
      // readable between frames. main() catches what escapes here and exits 1
      // with the message, which is the right end for a program that cannot
      // start and the wrong one for a shelf somebody is standing in front of.
      try {
        app.frame(win);
      } catch (const std::exception& ex) {
        app.page_failed(ex.what());
      }
      end_frame(w);
    }
  }   // the App goes here, while ImGui and SDL are both still up

  // Written once, on the way out, rather than on every drag: a settings file
  // rewritten sixty times a second while somebody resizes is a settings file
  // waiting to be truncated by a crash.
  if (cfg.window.remember_geometry) {
    Uint32 f = SDL_GetWindowFlags(win);
    if (f & SDL_WINDOW_FULLSCREEN_DESKTOP) {
      cfg.window.mode = config::WindowMode::Fullscreen;
    } else {
      cfg.window.mode = (f & SDL_WINDOW_BORDERLESS) ? config::WindowMode::Borderless
                                                    : config::WindowMode::Windowed;
      int ww = 0, wh = 0, wx = 0, wy = 0;
      SDL_GetWindowSize(win, &ww, &wh);
      SDL_GetWindowPosition(win, &wx, &wy);
      if (ww > 0 && wh > 0) {
        cfg.window.width = static_cast<uint32_t>(ww);
        cfg.window.height = static_cast<uint32_t>(wh);
        cfg.window.x = wx;
        cfg.window.y = wy;
      }
    }
    config::save(config::config_file(), cfg);
  }

  close_window(w);
  return 0;
}

}  // namespace kg::gui
