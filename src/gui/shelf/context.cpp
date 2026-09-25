#include "context.h"

#include <algorithm>
#include <cmath>
#include <exception>
#include <utility>

#include "../../session/play.h"
#include "../format.h"
#include "../screen.h"
#include "../widgets.h"
#include "imgui.h"
#include "pages.h"

namespace kg::gui::shelf {

ShelfContext::ShelfContext(const rt::Env& e, SDL_Window* w, SDL_Renderer* r, Fonts f)
    : env(e), win(w), fonts(f), textures(r) {}

void ShelfContext::reload() {
  ++generation;
  entries = gui::scan(env);
  if (selected >= entries.size()) selected = entries.empty() ? 0 : entries.size() - 1;
}

size_t ShelfContext::index_of(const std::string& id) const {
  for (size_t i = 0; i < entries.size(); ++i)
    if (entries[i].id == id) return i;
  return 0;
}

void ShelfContext::play(std::string id) {
  if (job.running()) return;
  // Measured while our window is still up, as the launcher does. Without it
  // the session knows no panel, and every game from the shelf would come up
  // at 1x in a window.
  session::PlayRequest req;
  req.id = id;
  const PanelSize ps = desktop_size(false);
  req.display.panel_w = ps.w;
  req.display.panel_h = ps.h;
  req.display.usable_w = ps.usable_w;
  req.display.usable_h = ps.usable_h;
  req.hooks.say = [this](const std::string& line) { job.log(line); };
  req.hooks.screen_up = [this] { screen_up_ = true; };
  req.hooks.stop_requested = [this] { return job.cancelled(); };

  const size_t at = index_of(id);
  const std::string name = at < entries.size() && entries[at].id == id && !entries[at].name.empty()
                               ? entries[at].name
                               : id;
  playing_ = id;
  screen_up_ = false;
  job.locked([this] { played_.clear(); });
  job.clear_cancel();
  job.start("Playing " + name, [this, req] {
    session::Outcome out = session::play(env, req);
    std::string said = "played for " + human_time(out.seconds);
    if (!out.generation.empty()) said += ", snapshot " + out.generation.filename().string();
    job.locked([this, &said] { played_ = said; });
  });
}

void ShelfContext::hide_for_game() {
  // The game gets its own screen; ours would only be in the way.
  if (screen_up_.exchange(false) && !playing_.empty() && !hidden_) {
    SDL_HideWindow(win);
    hidden_ = true;
  }
}

void ShelfContext::after_play() {
  if (playing_.empty() || !job.finished()) return;
  if (hidden_) {
    SDL_ShowWindow(win);
    SDL_RaiseWindow(win);
    hidden_ = false;
  }
  screen_up_ = false;
  const std::string id = std::move(playing_);
  playing_.clear();
  textures.forget_matching("/" + id + "/");
  const std::string error = job.error();
  if (!error.empty()) {
    // The modal stays up with the session's lines and what went wrong; its
    // Close joins the worker and reads the collection again.
    status = error;
    return;
  }
  status = job.locked([this] { return played_; });
  ImGui::CloseCurrentPopup();
  job.join();
  reload();
}

void ShelfContext::open_game(const std::string& id) {
  selected = index_of(id);
  go(Screen::Game);
}

// A stop asked of the last job is not one asked of this.
void ShelfContext::run_job(const std::string& title, std::function<void()> fn) {
  if (job.running()) return;
  job.clear_cancel();
  job.start(title, std::move(fn));
}

float hairline() { return std::max(1.0f, std::round(px(1))); }

void outline(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 col, float t) {
  if (t <= 0) t = hairline();
  dl->AddRectFilled(a, ImVec2(b.x, a.y + t), col);
  dl->AddRectFilled(ImVec2(a.x, b.y - t), b, col);
  dl->AddRectFilled(ImVec2(a.x, a.y + t), ImVec2(a.x + t, b.y - t), col);
  dl->AddRectFilled(ImVec2(b.x - t, a.y + t), ImVec2(b.x, b.y - t), col);
}

// small_button pads its label by round(px(2)) above and below; text given the
// same frame padding sits on the same baseline.
void align_to_small_button() {
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x, std::round(px(2))));
  ImGui::AlignTextToFramePadding();
  ImGui::PopStyleVar();
}

float small_button_height() { return ImGui::GetTextLineHeight() + std::round(px(2)) * 2; }

}  // namespace kg::gui::shelf
