#include "launcher.h"

#include <SDL.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <functional>
#include <mutex>
#include <sstream>
#include <thread>

#include "../player/desktop.h"
#include "../util/paths.h"
#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "library.h"
#include "widgets.h"

namespace kg::gui {
namespace fs = std::filesystem;

namespace {

enum class Page { Checking, Blocked, Grid, Game, Saves, Display, Controls, Bundle, Licenses, About };

const char* env(const char* k) {
  const char* v = std::getenv(k);
  return (v && *v) ? v : nullptr;
}

std::string gb(uint64_t n) {
  char b[32];
  std::snprintf(b, sizeof(b), "%.1f GB", static_cast<double>(n) / 1e9);
  return b;
}

const ImVec4 kWarm(1.0f, 0.86f, 0.55f, 1.0f);
const ImVec4 kBad(1.0f, 0.6f, 0.4f, 1.0f);

class Launcher {
 public:
  Launcher(const player::Player& p, const Window& w)
      : p_(p), w_(w), textures_(w.ren), state_(player::load_launcher(p.launcher_file())) {
    for (const bundle::GameMeta& g : p_.bundle().meta.games) ids_.push_back(g.id);
    selected_ = ids_.front();
    begin_check();
  }

  ~Launcher() {
    if (worker_.joinable()) worker_.join();
  }

  bool quit() const { return quit_; }
  void request_quit() {
    if (busy_) return;  // a check or an unpack is writing; it finishes first
    quit_ = true;
  }

  void back() {
    if (busy_) return;
    switch (page_) {
      case Page::Saves: case Page::Display: case Page::Controls: page_ = Page::Game; break;
      case Page::Licenses: case Page::About: page_ = Page::Bundle; break;
      case Page::Game: case Page::Bundle:
        if (ids_.size() > 1) page_ = Page::Grid; else quit_ = true;
        break;
      default: quit_ = true;
    }
  }

  void page_failed(const std::string& what) { status_ = what; }

  void frame() {
    finish_job();
    switch (page_) {
      case Page::Checking: checking(); break;
      case Page::Blocked: blocked(); break;
      case Page::Grid: grid(); break;
      case Page::Game: game(); break;
      case Page::Saves: saves(); break;
      case Page::Display: display(); break;
      case Page::Controls: controls(); break;
      case Page::Bundle: bundle_page(); break;
      case Page::Licenses: licenses(); break;
      case Page::About: about(); break;
    }
    if (busy_) busy_modal();
    else if (!notes_.empty()) notes_modal();
    else if (offer_desktop_) desktop_modal();
    else if (consent_) consent_modal();
  }

  // What the window was, for launcher.toml on the way out.
  void remember_window(SDL_Window* win) {
    Uint32 f = SDL_GetWindowFlags(win);
    state_.window_fullscreen = (f & SDL_WINDOW_FULLSCREEN_DESKTOP) != 0;
    if (!state_.window_fullscreen) {
      int ww = 0, wh = 0;
      SDL_GetWindowSize(win, &ww, &wh);
      if (ww > 0 && wh > 0) {
        state_.window_w = static_cast<uint32_t>(ww);
        state_.window_h = static_cast<uint32_t>(wh);
      }
    }
    save_state();
  }

 private:
  // ---- the worker -----------------------------------------------------------
  //
  // Checking the machine, hashing a pack and unpacking one take seconds to
  // minutes; they run here while the window keeps drawing. `then` runs on the
  // UI thread once the job is over, with what it threw, if anything.
  void run_job(const std::string& title, std::function<void()> job,
               std::function<void(const std::string& error)> then) {
    if (busy_) return;
    busy_ = true;
    job_title_ = title;
    set_progress("");
    job_error_.clear();
    job_then_ = std::move(then);
    done_ = false;
    worker_ = std::thread([this, job] {
      try {
        job();
      } catch (const std::exception& ex) {
        std::lock_guard<std::mutex> lk(mu_);
        job_error_ = ex.what();
      }
      done_ = true;
    });
  }

  void finish_job() {
    if (!busy_ || !done_) return;
    worker_.join();
    busy_ = false;
    std::string err;
    {
      std::lock_guard<std::mutex> lk(mu_);
      err = job_error_;
    }
    auto then = std::move(job_then_);
    job_then_ = nullptr;
    if (then) then(err);
  }

  void set_progress(const std::string& s) {
    std::lock_guard<std::mutex> lk(mu_);
    progress_ = s;
  }

  void busy_modal() {
    ImGui::OpenPopup("working");
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(640, 0));
    if (ImGui::BeginPopupModal("working", nullptr,
                               ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar)) {
      ImGui::PushFont(w_.big);
      ImGui::TextWrapped("%s", job_title_.c_str());
      ImGui::PopFont();
      std::string p;
      {
        std::lock_guard<std::mutex> lk(mu_);
        p = progress_;
      }
      ImGui::TextDisabled("%s", p.empty() ? "working..." : p.c_str());
      ImGui::EndPopup();
    }
  }

  // ---- first run --------------------------------------------------------------

  void begin_check() {
    page_ = Page::Checking;
    run_job("Checking this machine", [this] { report_ = p_.doctor_report(); },
            [this](const std::string& err) {
              if (!err.empty()) {
                blocking_ = {"The check of this machine failed: " + err};
                page_ = Page::Blocked;
                return;
              }
              checked_ = true;
              for (const gpu::Problem& pr : report_.problems) {
                if (pr.blocking()) blocking_.push_back(pr.line());
              }
              for (const std::string& w : player::unseen_warnings(report_, state_)) notes_.push_back(w);
              if (!p_.state().refused_portable.empty() && !state_.portable_fallback_said) {
                notes_.push_back(p_.state().refused_portable.string() + " is there, but it " +
                                 p_.state().refused_why + ", so this player keeps its saves in " +
                                 state_dir().string() + " instead.");
                state_.portable_fallback_said = true;
              }
              if (!blocking_.empty()) {
                page_ = Page::Blocked;
                return;
              }
              offer_desktop_ = !state_.desktop_offered;
              save_state();
              if (ids_.size() == 1) {
                page_ = Page::Game;
                // Started once the notes are read and the menu entry is
                // answered - and at once when there is neither, which is every
                // launch after the first: waiting for a modal that will never
                // open left a one-game player on its page instead of playing.
                auto_play_ = true;
                maybe_auto_play();
              } else {
                page_ = Page::Grid;
              }
            });
  }

  void checking() {
    begin_page(p_.bundle().meta.title.c_str(), "", w_.big);
    ImGui::TextDisabled("checking this machine...");
    ImGui::End();
  }

  void blocked() {
    begin_page(p_.bundle().meta.title.c_str(), "Esc quit", w_.big);
    ImGui::PushFont(w_.big);
    ImGui::TextWrapped("This machine cannot play these games yet.");
    ImGui::PopFont();
    ImGui::Spacing();
    for (const std::string& b : blocking_) {
      ImGui::PushStyleColor(ImGuiCol_Text, kBad);
      ImGui::TextWrapped("%s", b.c_str());
      ImGui::PopStyleColor();
      ImGui::Spacing();
    }
    ImGui::Spacing();
    if (ImGui::Button("Show the full report")) page_ = Page::About;
    ImGui::SameLine();
    if (ImGui::Button("Quit")) quit_ = true;
    ImGui::End();
  }

  void notes_modal() {
    ImGui::OpenPopup("worth knowing");
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(720, 0));
    if (ImGui::BeginPopupModal("worth knowing", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove)) {
      ImGui::TextWrapped("Worth knowing about this machine. You will not be told again.");
      ImGui::Spacing();
      for (const std::string& n : notes_) {
        ImGui::PushStyleColor(ImGuiCol_Text, kWarm);
        ImGui::TextWrapped("%s", n.c_str());
        ImGui::PopStyleColor();
      }
      ImGui::Spacing();
      if (ImGui::Button("OK", ImVec2(-1, 44))) {
        notes_.clear();
        ImGui::CloseCurrentPopup();
        maybe_auto_play();
      }
      ImGui::EndPopup();
    }
  }

  void desktop_modal() {
    ImGui::OpenPopup("applications menu");
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(640, 0));
    if (ImGui::BeginPopupModal("applications menu", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove)) {
      ImGui::TextWrapped("Add %s to your applications menu?", p_.bundle().meta.title.c_str());
      ImGui::TextDisabled("It points at this file, where it is now. Bundle settings removes it.");
      ImGui::Spacing();
      bool answered = false;
      if (ImGui::Button("Add it")) {
        add_desktop_entry();
        answered = true;
      }
      ImGui::SameLine();
      if (ImGui::Button("No thanks")) answered = true;
      if (answered) {
        state_.desktop_offered = true;
        offer_desktop_ = false;
        save_state();
        ImGui::CloseCurrentPopup();
        maybe_auto_play();
      }
      ImGui::EndPopup();
    }
  }

  void maybe_auto_play() {
    if (auto_play_ && notes_.empty() && !offer_desktop_) {
      auto_play_ = false;
      play(ids_.front());
    }
  }

  // ---- the grid ---------------------------------------------------------------

  void banner() {
    const bundle::BundleMeta& m = p_.bundle().meta;
    if (const Texture* b = textures_.png("banner", m.banner)) {
      float w = ImGui::GetContentRegionAvail().x;
      float h = std::min(220.0f, w * static_cast<float>(b->h) / static_cast<float>(b->w));
      ImGui::Image(reinterpret_cast<ImTextureID>(b->tex), ImVec2(h * b->w / b->h, h));
      ImGui::Spacing();
    }
  }

  const Texture* cover(const std::string& id) {
    const bundle::GameMeta& g = p_.game(id);
    if (const Texture* t = textures_.png("cover:" + id, g.cover)) return t;
    // No cover from the author: the game's own screen, once it has been played.
    return textures_.file(game_saves_dir(id) / "journal" / "title.png");
  }

  void grid() {
    const bundle::BundleMeta& m = p_.bundle().meta;
    begin_page(m.title.c_str(), "arrows move   Enter play   Esc quit", w_.big);
    banner();
    const float avail = ImGui::GetContentRegionAvail().x;
    const float tw = 300.0f, th = 225.0f, pad = 18.0f;
    const int cols = std::max(1, static_cast<int>((avail + pad) / (tw + pad)));
    ImGui::BeginChild("grid", ImVec2(0, -40), false);
    for (size_t i = 0; i < ids_.size(); ++i) {
      if (i % cols) ImGui::SameLine(0, pad);
      const bundle::GameMeta& g = p_.game(ids_[i]);
      std::string sub = g.year ? std::to_string(g.year) : "";
      if (tile(g.id, g.name, sub, cover(g.id), w_.big, tw, th)) {
        selected_ = g.id;
        status_.clear();
        page_ = Page::Game;
      }
    }
    ImGui::EndChild();
    if (ImGui::Button("Bundle settings")) page_ = Page::Bundle;
    status_line();
    ImGui::End();
  }

  void status_line() {
    if (status_.empty()) return;
    ImGui::SameLine();
    ImGui::TextWrapped("%s", status_.c_str());
  }

  // ---- one game -----------------------------------------------------------------

  void game() {
    const bundle::GameMeta& g = p_.game(selected_);
    begin_page(g.name.c_str(), ids_.size() > 1 ? "Esc back" : "Esc quit", w_.big);
    ImGui::BeginChild("left", ImVec2(ImGui::GetContentRegionAvail().x * 0.5f, 0), false);
    if (const Texture* t = cover(g.id)) {
      float w = ImGui::GetContentRegionAvail().x;
      ImGui::Image(reinterpret_cast<ImTextureID>(t->tex), ImVec2(w, w * t->h / t->w));
    }
    if (g.year) ImGui::TextDisabled("%u", g.year);
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("right", ImVec2(0, 0), false);
    ImGui::PushFont(w_.big);
    if (ImGui::Button("Play", ImVec2(-1, 60))) play(g.id);
    ImGui::PopFont();
    ImGui::Spacing();
    if (ImGui::Button("Saves")) page_ = Page::Saves;
    ImGui::SameLine();
    if (ImGui::Button("Display")) page_ = Page::Display;
    ImGui::SameLine();
    if (ImGui::Button("Controls")) page_ = Page::Controls;
    if (ids_.size() == 1) {
      ImGui::SameLine();
      if (ImGui::Button("Settings")) page_ = Page::Bundle;
    }
    ImGui::Spacing();

    std::vector<session::Record> j = session::journal(g.id);
    if (!j.empty()) {
      double total = 0;
      for (const session::Record& r : j) total += static_cast<double>(r.ended - r.started);
      ImGui::Text("%zu session%s, %s in total", j.size(), j.size() == 1 ? "" : "s", human_time(total).c_str());
      ImGui::TextDisabled("last played %s", ago(j.front().ended).c_str());
    } else {
      ImGui::TextDisabled("never played");
    }

    if (!failure_.empty() && failure_game_ == g.id) {
      ImGui::Spacing();
      ImGui::Separator();
      ImGui::PushStyleColor(ImGuiCol_Text, kBad);
      ImGui::TextWrapped("%s", failure_.c_str());
      ImGui::PopStyleColor();
      if (!failure_log_.empty()) {
        ImGui::BeginChild("log", ImVec2(0, 260), true, ImGuiWindowFlags_HorizontalScrollbar);
        ImGui::TextUnformatted(failure_log_.c_str());
        ImGui::EndChild();
      }
      if (ImGui::Button("Save diagnostics report")) save_report();
    }
    if (!status_.empty()) {
      ImGui::Spacing();
      ImGui::TextWrapped("%s", status_.c_str());
    }
    ImGui::EndChild();
    ImGui::End();
  }

  void fail(const std::string& id, const std::string& what, bool with_log) {
    failure_game_ = id;
    failure_ = what;
    failure_log_ = with_log ? p_.last_log(40) : "";
    selected_ = id;
    page_ = Page::Game;
  }

  // Step one of playing: the pack is checked the first time this version of
  // the bundle plays it. Hashing a pack that carries its disc is gigabytes, so
  // it is a job with its own progress, and it is only ever done once.
  void play(const std::string& id) {
    status_.clear();
    failure_.clear();
    if (!p_.verified(id)) {
      const std::string name = p_.game(id).name;
      run_job("Checking " + name + " (the first time only)",
              [this, id] {
                p_.verify(id, [this](uint64_t done, uint64_t total) {
                  set_progress(std::to_string(total ? done * 100 / total : 100) + "%");
                });
              },
              [this, id](const std::string& err) {
                if (!err.empty()) fail(id, err, false);
                else play_checked(id);
              });
      return;
    }
    play_checked(id);
  }

  // Step two: a machine with no FUSE plays an unpacked copy, which is asked
  // for with its size and its place.
  void play_checked(const std::string& id) {
    if (p_.no_fuse()) {
      player::UnpackPlan u = p_.unpack_plan(id);
      if (!u.ready) {
        consent_ = true;
        consent_game_ = id;
        consent_plan_ = u;
        return;
      }
    }
    play_now(id);
  }

  void consent_modal() {
    ImGui::OpenPopup("unpack");
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(720, 0));
    if (ImGui::BeginPopupModal("unpack", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove)) {
      const player::UnpackPlan& u = consent_plan_;
      const std::string name = p_.game(consent_game_).name;
      ImGui::TextWrapped("This machine cannot mount games (it has no FUSE), so %s has to be unpacked "
                         "to play.", name.c_str());
      ImGui::Spacing();
      ImGui::TextWrapped("Needs %s in %s to play without FUSE.", gb(u.need).c_str(),
                         u.where.parent_path().c_str());
      ImGui::TextDisabled("%s free there. It is done once; later launches use the copy.", gb(u.free).c_str());
      ImGui::Spacing();
      bool close = false;
      if (!u.fits()) {
        ImGui::PushStyleColor(ImGuiCol_Text, kBad);
        ImGui::TextWrapped("There is not enough room. Free some space there, or unpack it somewhere "
                           "else from a terminal: this file --extract-to DIR %s", consent_game_.c_str());
        ImGui::PopStyleColor();
        ImGui::BeginDisabled();
        ImGui::Button("Extract");
        ImGui::EndDisabled();
      } else if (ImGui::Button("Extract")) {
        const std::string id = consent_game_;
        run_job("Unpacking " + name, [this, id] { p_.unpack(id); },
                [this, id](const std::string& err) {
                  if (!err.empty()) fail(id, err, false);
                  else play_now(id);
                });
        close = true;
      }
      ImGui::SameLine();
      if (ImGui::Button(ids_.size() > 1 ? "Not now" : "Quit")) {
        if (ids_.size() == 1) quit_ = true;
        close = true;
      }
      if (close) {
        consent_ = false;
        ImGui::CloseCurrentPopup();
      }
      ImGui::EndPopup();
    }
  }

  // Step three. The game gets its own screen; ours would only be in the way.
  void play_now(const std::string& id) {
    player::PlayRequest req;
    SDL_DisplayMode dm;
    if (SDL_GetDesktopDisplayMode(0, &dm) == 0 && dm.w > 0 && dm.h > 0) {
      req.panel_w = static_cast<uint32_t>(dm.w);
      req.panel_h = static_cast<uint32_t>(dm.h);
    }
    SDL_HideWindow(w_.win);
    // A one-game player comes back to its game's page afterwards, as a grid's
    // game does: it started the game without being asked, and this page is the
    // only way to its saves, display and controls. Esc there quits.
    try {
      session::Outcome o = p_.play(id, req);
      // A game that died at once is a game that did not start, and the only
      // record of why is the compositor's log.
      if (o.status != 0 && o.seconds < 15) {
        fail(id, p_.game(id).name + " stopped as soon as it started (exit status " +
                     std::to_string(o.status) + "). The end of its log:",
             true);
      } else {
        status_ = "played for " + human_time(o.seconds);
        if (!o.generation.empty()) status_ += ", snapshot " + o.generation.filename().string();
      }
    } catch (const player::Damaged& ex) {
      fail(id, ex.what(), false);
    } catch (const session::NeedsUnpack&) {
      // The mount failed on a machine the bootstrap thought could mount.
      consent_ = true;
      consent_game_ = id;
      consent_plan_ = p_.unpack_plan(id);
    } catch (const std::exception& ex) {
      fail(id, ex.what(), true);
    }
    SDL_ShowWindow(w_.win);
    SDL_RaiseWindow(w_.win);
    textures_.forget_matching("/" + id + "/");
  }

  void save_report() {
    std::string text = player::doctor::redact(player::doctor::render(p_.doctor_report()));
    fs::path out = state_dir() / "report.txt";
    std::ofstream f(out, std::ios::trunc);
    f << text;
    status_ = f ? "saved to " + out.string() + ", without your home directory or user name"
                : "could not write " + out.string();
  }

  // ---- saves ----------------------------------------------------------------------

  void saves() {
    const bundle::GameMeta& g = p_.game(selected_);
    begin_page((g.name + " - saves").c_str(), "Esc back", w_.big);
    std::vector<std::string> gens = session::generations(g.id);
    std::vector<session::Record> recs = session::journal(g.id);

    ImGui::Text("export and import");
    if (export_path_.empty()) {
      export_path_ = (state_dir() / "exports" / (g.id + ".saves.kgpack")).string();
    }
    ImGui::SetNextItemWidth(-160);
    input_text("##export", export_path_);
    ImGui::SameLine();
    if (ImGui::Button("Export")) {
      try {
        std::error_code ec;
        fs::create_directories(fs::path(export_path_).parent_path(), ec);
        fs::path f = p_.export_saves(g.id, export_path_);
        status_ = "exported to " + f.string();
      } catch (const std::exception& ex) {
        status_ = ex.what();
      }
    }
    ImGui::SetNextItemWidth(-160);
    input_text("##import", import_path_);
    ImGui::SameLine();
    if (ImGui::Button("Import")) {
      try {
        p_.import_saves(g.id, import_path_);
        status_ = "imported; what was there before is a snapshot below";
      } catch (const std::exception& ex) {
        status_ = ex.what();
      }
    }
    ImGui::TextDisabled("importing keeps what is there now as a snapshot first");
    if (!status_.empty()) ImGui::TextWrapped("%s", status_.c_str());
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Text("timeline");
    if (gens.empty()) {
      ImGui::TextWrapped("Nothing to go back to yet. Every session that writes something leaves a "
                         "snapshot here, newest first.");
    }
    ImGui::BeginChild("gens", ImVec2(0, 0), false);
    for (size_t i = gens.size(); i-- > 0;) {
      const std::string& gen = gens[i];
      ImGui::PushID(gen.c_str());
      const session::Record* rec = nullptr;
      for (const session::Record& r : recs) {
        if (r.generation == gen) rec = &r;
      }
      ImGui::Text("snapshot %s", gen.c_str());
      if (rec) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s, played %s, %zu files written", ago(rec->ended).c_str(),
                            human_time(static_cast<double>(rec->ended - rec->started)).c_str(),
                            rec->files_written);
      }
      ImGui::SameLine();
      if (ImGui::SmallButton("Restore")) {
        restore_gen_ = gen;
        ImGui::OpenPopup("restore?");
      }
      if (ImGui::BeginPopupModal("restore?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Restore snapshot %s?", restore_gen_.c_str());
        ImGui::TextWrapped("What the game has written since is kept as a new snapshot first, so this "
                           "can be undone.");
        if (ImGui::Button("Restore")) {
          try {
            session::restore(g.id, restore_gen_);
            status_ = "restored " + restore_gen_;
          } catch (const std::exception& ex) {
            status_ = ex.what();
          }
          ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
      }
      ImGui::PopID();
    }
    ImGui::EndChild();
    ImGui::End();
  }

  static void input_text(const char* label, std::string& s) {
    char buf[1024];
    std::snprintf(buf, sizeof(buf), "%s", s.c_str());
    if (ImGui::InputText(label, buf, sizeof(buf))) s = buf;
  }

  // ---- display and controls ----------------------------------------------------------

  void display() {
    const bundle::GameMeta& g = p_.game(selected_);
    begin_page((g.name + " - display").c_str(), "Esc back", w_.big);
    player::GameSettings s = p_.settings(g.id);
    bool dirty = false;
    const char* modes[] = {"integer", "fit", "native"};
    int m = static_cast<int>(s.display.mode);
    if (ImGui::Combo("scaling", &m, modes, 3)) {
      s.display.mode = static_cast<config::ScaleMode>(m);
      dirty = true;
    }
    int sc = static_cast<int>(s.display.scale);
    if (ImGui::SliderInt("scale", &sc, 0, 6, sc == 0 ? "the largest that fits" : "%dx")) {
      s.display.scale = static_cast<uint32_t>(sc);
      dirty = true;
    }
    int fs_mode = s.fullscreen ? 1 : 0;
    const char* wmodes[] = {"windowed", "fullscreen"};
    if (ImGui::Combo("window", &fs_mode, wmodes, 2)) {
      s.fullscreen = fs_mode == 1;
      dirty = true;
    }
    ImGui::Spacing();
    ImGui::TextDisabled("integer keeps every game pixel an exact square. fit is the largest whole");
    ImGui::TextDisabled("number that fits, fullscreen. native asks the game for the screen's own size.");
    ImGui::TextDisabled("The author chose %s%s.", g.display.c_str(), g.fullscreen ? ", fullscreen" : "");
    if (dirty) p_.save_settings(g.id, s);
    ImGui::End();
  }

  void controls() {
    const bundle::GameMeta& g = p_.game(selected_);
    begin_page((g.name + " - controls").c_str(), "Esc back", w_.big);
    player::GameSettings s = p_.settings(g.id);
    int preset = s.gamepad == "kretro" ? 1 : 0;
    const char* presets[] = {"the author's map", "kretro's own map"};
    if (ImGui::Combo("gamepad", &preset, presets, 2)) {
      s.gamepad = preset == 1 ? "kretro" : "author";
      p_.save_settings(g.id, s);
    }
    ImGui::Spacing();
    if (g.gamepad.empty()) {
      ImGui::TextDisabled("The author set no map of their own, so both are kretro's: sticks and the");
      ImGui::TextDisabled("d-pad move, A is Enter, B is Escape, and the rest follow the keyboard.");
    } else {
      ImGui::TextDisabled("The author's map, over kretro's own:");
      ImGui::TextWrapped("%s", g.gamepad.c_str());
    }
    ImGui::End();
  }

  // ---- the bundle ---------------------------------------------------------------------

  player::DesktopPaths desktop_paths() const {
    return player::desktop_paths(p_.bundle().meta.id, env("XDG_DATA_HOME") ? env("XDG_DATA_HOME") : "",
                                 env("HOME") ? env("HOME") : "");
  }

  void add_desktop_entry() {
    try {
      player::install_desktop_entry(desktop_paths(), p_.bundle().meta.title, p_.bundle().meta.id,
                                    p_.bundle().self, p_.bundle().meta.icon);
      state_.desktop_installed = true;
      status_ = "added to your applications menu";
    } catch (const std::exception& ex) {
      status_ = ex.what();
    }
    save_state();
  }

  void bundle_page() {
    const bundle::BundleMeta& m = p_.bundle().meta;
    begin_page((m.title + " - settings").c_str(), "Esc back", w_.big);
    std::error_code ec;
    const bool installed = fs::exists(desktop_paths().entry, ec);
    if (installed) {
      if (ImGui::Button("Remove from the applications menu")) {
        player::remove_desktop_entry(desktop_paths());
        state_.desktop_installed = false;
        save_state();
        status_ = "removed from your applications menu";
      }
    } else if (ImGui::Button("Add to the applications menu")) {
      add_desktop_entry();
    }
    ImGui::Spacing();
    if (ImGui::Button("Open the data folder")) {
      std::string url = "file://" + state_dir().string();
      if (SDL_OpenURL(url.c_str()) != 0) status_ = "could not open it: " + std::string(SDL_GetError());
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%s", state_dir().c_str());
    ImGui::Spacing();
    if (ImGui::Button("Licences")) page_ = Page::Licenses;
    ImGui::SameLine();
    if (ImGui::Button("About and diagnostics")) page_ = Page::About;
    ImGui::Spacing();
    ImGui::TextDisabled("%s %s%s", m.id.c_str(), m.version.c_str(),
                        m.kretro_version.empty() ? "" : (", built by kretro " + m.kretro_version).c_str());
    if (!status_.empty()) ImGui::TextWrapped("%s", status_.c_str());
    ImGui::End();
  }

  void licenses() {
    begin_page("Licences", "Esc back", w_.big);
    if (licenses_text_.empty()) licenses_text_ = p_.licenses_text();
    ImGui::BeginChild("text", ImVec2(0, 0), false);
    ImGui::TextWrapped("%s", licenses_text_.c_str());
    ImGui::EndChild();
    ImGui::End();
  }

  void about() {
    begin_page("About and diagnostics", "Esc back", w_.big);
    if (!checked_ && !busy_) {
      ImGui::TextDisabled("not checked yet");
      ImGui::End();
      return;
    }
    if (report_text_.empty()) report_text_ = player::doctor::render(report_);
    if (ImGui::Button("Save report")) save_report();
    ImGui::SameLine();
    ImGui::TextDisabled("the saved copy has no home directory and no user name in it");
    if (!status_.empty()) ImGui::TextWrapped("%s", status_.c_str());
    ImGui::BeginChild("report", ImVec2(0, 0), true, ImGuiWindowFlags_HorizontalScrollbar);
    ImGui::TextUnformatted(report_text_.c_str());
    ImGui::EndChild();
    ImGui::End();
  }

  void save_state() {
    try {
      player::save_launcher(p_.launcher_file(), state_);
    } catch (const std::exception& ex) {
      status_ = ex.what();
    }
  }

  const player::Player& p_;
  const Window& w_;
  Textures textures_;
  player::LauncherState state_;
  std::vector<std::string> ids_;
  std::string selected_;
  Page page_ = Page::Checking;
  bool quit_ = false;
  std::string status_;

  player::doctor::Report report_;
  std::string report_text_, licenses_text_;
  bool checked_ = false;
  std::vector<std::string> blocking_, notes_;
  bool offer_desktop_ = false;
  bool auto_play_ = false;

  bool consent_ = false;
  std::string consent_game_;
  player::UnpackPlan consent_plan_;

  std::string failure_game_, failure_, failure_log_;
  std::string export_path_, import_path_, restore_gen_;

  std::thread worker_;
  std::mutex mu_;
  std::atomic<bool> done_{false};
  bool busy_ = false;
  std::string job_title_, progress_, job_error_;
  std::function<void(const std::string&)> job_then_;
};

}  // namespace

int run_launcher(const player::Player& p) {
  if (!have_display()) {
    std::fprintf(stderr,
                 "%s: no display to draw on.\n"
                 "  This machine has no Wayland or X11 session, so there is no launcher to show.\n"
                 "  From a terminal: --doctor, play <game>, --licenses. --help lists the rest.\n",
                 p.bundle().self.filename().c_str());
    return 1;
  }
  player::LauncherState ls = player::load_launcher(p.launcher_file());
  WindowSpec spec;
  spec.title = p.bundle().meta.title.c_str();
  spec.width = ls.window_w;
  spec.height = ls.window_h;
  spec.fullscreen = ls.window_fullscreen;
  std::string why;
  std::optional<Window> opened = open_window(p.env(), spec, &why);
  if (!opened) {
    std::fprintf(stderr, "%s: %s\n", p.bundle().self.filename().c_str(), why.c_str());
    return 1;
  }
  Window w = *opened;

  {
    Launcher app(p, w);
    while (!app.quit()) {
      SDL_Event ev;
      while (SDL_PollEvent(&ev)) {
        ImGui_ImplSDL2_ProcessEvent(&ev);
        if (ev.type == SDL_QUIT) app.request_quit();
        if (ev.type == SDL_CONTROLLERDEVICEADDED) SDL_GameControllerOpen(ev.cdevice.which);
        if (ev.type == SDL_KEYDOWN) {
          if (ev.key.keysym.sym == SDLK_ESCAPE) app.back();
          if (ev.key.keysym.sym == SDLK_F11) {
            Uint32 f = SDL_GetWindowFlags(w.win);
            SDL_SetWindowFullscreen(w.win, f & SDL_WINDOW_FULLSCREEN_DESKTOP ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
          }
        }
        if (ev.type == SDL_CONTROLLERBUTTONDOWN && ev.cbutton.button == SDL_CONTROLLER_BUTTON_B) app.back();
      }
      begin_frame(w);
      try {
        app.frame();
      } catch (const std::exception& ex) {
        app.page_failed(ex.what());
      }
      end_frame(w);
    }
    app.remember_window(w.win);
  }
  close_window(w);
  return 0;
}

}  // namespace kg::gui
