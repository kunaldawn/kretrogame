// The Bundles page drawn with no window: every step of a remembered bundle,
// several frames each, against a scratch shelf.
//
// ImGui needs no renderer to lay a page out, only a font atlas and a display
// size, so this draws the real page code into nothing. What it catches is what
// a person would otherwise find by opening the page: an ImGui assertion - an
// unbalanced Begin, PushID or disabled block, an ID used twice - which in
// this build aborts kretro rather than drawing a red tooltip. It does not
// click anything; the decisions behind the buttons are tests/unit/test_builder.cpp's.
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "bundle/builder.h"
#include "gui/bundles/bundles_page.h"
#include "gui/scale.h"
#include "gui/texture.h"
#include "gui/widgets.h"
#include "gui/window.h"
#include "pack/kgpack.h"
#include "util/paths.h"
#include "imgui.h"
#include "support/files.h"

namespace fs = std::filesystem;
using namespace kg;

using kgtest::write_file;

int main() {
  fs::path tmp = fs::temp_directory_path() / "kretro-test-bundles-page";
  fs::remove_all(tmp);
  // Before anything asks where the state is: state_dir() is resolved once.
  setenv("KRETRO_STATE", (tmp / "state").c_str(), 1);
  setenv("HOME", (tmp / "home").c_str(), 1);
  unsetenv("KRETRO_SELF");
  unsetenv("KRETRO_PLAYER_BASE");
  fs::create_directories(tmp / "home");

  // One game on the shelf, and one remembered bundle carrying it and a game
  // that has since left the shelf.
  write_file(tmp / "tree" / "GAME.EXE", "MZ");
  write_file(tmp / "body", std::string(5000, 'b'));
  Meta m;
  m.id = "fixture";
  m.name = "Fixture";
  m.year = 1999;
  m.run.exe = "GAME.EXE";
  m.registry.fragment = "REGEDIT4\n\n[HKEY_LOCAL_MACHINE\\Software\\F]\n\"CDKey\"=\"ABCD-1234-EFGH\"\n";
  m.tree = Tree::from_directory(tmp / "tree");
  fs::create_directories(games_dir());
  write_pack(games_dir() / "fixture.kgpack", m, WriteOptions{PackKind::Game, tmp / "body", false});

  bundle::Draft d;
  d.title = "Page smoke";
  d.id = "page-smoke";
  d.out_dir = tmp.string();
  d.games.push_back(bundle::game_from_pack(bundle::read_pack_facts(games_dir() / "fixture.kgpack")));
  bundle::DraftGame gone;
  gone.id = "gone";
  gone.name = "Gone";
  gone.embed_key = true;
  gone.extra_dlls = {{"dgVoodoo.conf", "x"}};
  d.games.push_back(gone);
  d.last_built = (tmp / "page-smoke-1.0.run").string();
  bundle::save_draft(bundle::bundles_dir(), d);

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  io.DisplaySize = ImVec2(1280, 800);
  io.DeltaTime = 1.0f / 60.0f;
  io.Fonts->AddFontDefault();
  unsigned char* px = nullptr;
  int w = 0, h = 0;
  io.Fonts->GetTexDataAsRGBA32(&px, &w, &h);

  int frames = 0;
  {
    rt::Env env;
    gui::Bundles page(env, gui::Fonts{});
    page.open();
    auto frame = [&] {
      ImGui::NewFrame();
      page.draw();
      ImGui::Render();
      ++frames;
    };
    for (int i = 0; i < 3; ++i) frame();  // the list
    using S = gui::Bundles::Step;
    if (!page.open_bundle("page-smoke", S::Identity)) {
      std::fprintf(stderr, "  FAIL the remembered bundle did not open\n");
      return 1;
    }
    for (S s : {S::Identity, S::Games, S::PerGame, S::Check, S::Size, S::Rights, S::Build, S::Preview}) {
      page.open_bundle("page-smoke", s);
      for (int i = 0; i < 4; ++i) frame();
    }
    while (page.back()) frame();  // back to the list, and remembered on the way
    frame();
    // The same page laid out at twice the size, as on a television: nothing
    // in it may assume the scale is 1.
    gui::set_ui_scale(2.0f);
    page.open_bundle("page-smoke", S::Identity);
    for (int i = 0; i < 3; ++i) frame();
    while (page.back()) frame();
    frame();
    gui::set_ui_scale(1.0f);
  }

  // The scale's arithmetic, which no window is needed for.
  bool scale_ok = true;
  auto expect = [&](bool ok, const char* what) {
    if (!ok) {
      std::fprintf(stderr, "  FAIL %s\n", what);
      scale_ok = false;
    }
  };
  expect(gui::quantize_ui_scale(1.03f) == 1.0f, "a scale is rounded to sixteenths");
  expect(gui::quantize_ui_scale(1.25f) == 1.25f, "a sixteenth stays as it is");
  expect(gui::quantize_ui_scale(40.0f) == 4.0f, "a scale is capped");
  expect(gui::auto_ui_scale(1280, 800, 1, 96) == 1.0f, "the design size is scale 1");
  expect(gui::auto_ui_scale(1024, 600, 1, 96) == 0.8f, "a small window is scaled down");
  expect(gui::auto_ui_scale(800, 480, 1, 96) == 0.75f, "a very small window is scaled down to a floor");
  expect(gui::auto_ui_scale(1366, 768, 1, 96) == 1.0f, "a window a little short of the design stays at 100%");
  expect(gui::auto_ui_scale(3840, 2160, 1, 96) == 2.7f, "a 4K screen fits the design by its height");
  expect(gui::auto_ui_scale(1024, 600, 1, 192) == 0.9375f, "a dense screen keeps text larger in a small window");
  expect(gui::auto_ui_scale(1280, 800, 1, 215) == 1.25f, "a dense small screen still holds the design's pages");
  expect(gui::auto_ui_scale(1024, 600, 2, 192) == 0.8f, "a scaled framebuffer does not count density twice");
  gui::zoom_reset();
  for (int i = 0; i < 10; ++i) gui::zoom_in();
  expect(gui::ui_zoom() == 2.0f, "ten steps of zoom are exactly 200%");
  for (int i = 0; i < 40; ++i) gui::zoom_out();
  expect(gui::ui_zoom() == 0.5f, "the zoom stops at 50%");
  gui::zoom_reset();
  // On a 4K screen the scale is already near its limit of 4, so the zoom
  // stops where the scale does rather than climbing on unseen.
  gui::set_zoom_base(2.6875f);
  for (int i = 0; i < 20; ++i) gui::zoom_in();
  expect(gui::ui_zoom() == 1.4f, "the zoom stops where the scale does");
  gui::zoom_out();
  expect(gui::ui_zoom() == 1.3f, "one step back is one step");
  gui::set_zoom_base(1.0f);
  gui::zoom_reset();
  gui::Grid g = gui::tile_grid(1000, 300, 0.75f, 20);
  expect(g.cols == 3 && g.w == 320 && g.h == 240, "tiles share a row evenly");
  expect(g.cols * g.w + (g.cols - 1) * g.gap <= 1000, "a row of tiles fits its width");
  g = gui::tile_grid(100, 300, 0.75f, 20);
  expect(g.cols == 1 && g.w == 100, "a row narrower than a tile still has one");
  gui::set_ui_scale(2.0f);
  expect(gui::px(10) == 20.0f && gui::px(3, 4).y == 8.0f, "px() is the design length at the scale");
  gui::set_ui_scale(1.0f);

  // Motion: the easings, and a step that is the same at any frame rate.
  auto near = [](float a, float b, float eps = 1e-3f) { return std::fabs(a - b) <= eps; };
  expect(gui::ease_out_cubic(0) == 0 && gui::ease_out_cubic(1) == 1 && gui::ease_out_cubic(0.5f) == 0.875f,
         "ease-out runs 0 to 1, fast then slow");
  expect(gui::ease_in_out_cubic(0.5f) == 0.5f && gui::ease_in_out_cubic(2) == 1, "ease-in-out is symmetric and clamped");
  {
    float a = 0, b = 0;
    for (int i = 0; i < 6; ++i) a = gui::approach(a, 100, 0.01f, 0.045f);
    for (int i = 0; i < 3; ++i) b = gui::approach(b, 100, 0.02f, 0.045f);
    expect(near(a, b, 0.01f), "an approach covers the same ground at 100 and at 50 frames a second");
    expect(near(gui::approach(0, 100, 0.045f, 0.045f), 63.21f, 0.01f), "an approach covers 63% in one tau");
    expect(gui::approach(99.9995f, 100, 0.001f, 0.045f) == 100, "an approach lands on its target");
    expect(gui::approach(3, 100, 0, 0.045f) == 3 && gui::approach(3, 100, 0.01f, 0) == 100,
           "no time is no step, and no tau is a snap");
    float x = 0, v = 0;
    float peak = 0;
    for (int i = 0; i < 200; ++i) {
      x = gui::spring(x, 1, v, 1.0f / 60.0f, 0.1f);
      peak = std::max(peak, x);
    }
    expect(near(x, 1, 1e-3f) && peak <= 1.0001f, "a spring arrives without overshooting");
  }

  // The size classes, in design pixels.
  using BP = gui::Breakpoint;
  expect(gui::breakpoint_for(1280) == BP::Regular && gui::breakpoint_for(1099) == BP::Compact &&
             gui::breakpoint_for(2201) == BP::Wide && gui::breakpoint_for(2200) == BP::Regular,
         "breakpoints fall at 1100 and 2200");
  expect(gui::breakpoint_for(1280.0f / 1.5f) == BP::Compact, "a 1280 window at 150% is compact");
  using C = gui::Content;
  expect(gui::content_max_w_for(C::Form, BP::Compact, 900, 1) == 900, "a compact form takes the room");
  expect(gui::content_max_w_for(C::Form, BP::Regular, 2000, 1) == 1040, "a regular form stops at 1040");
  expect(gui::content_max_w_for(C::Reading, BP::Wide, 3000, 2) == 2400, "a wide reading column is 1200 design pixels");
  expect(gui::content_max_w_for(C::Grid, BP::Regular, 1800, 1) == 1800 &&
             gui::content_max_w_for(C::Grid, BP::Wide, 3000, 1) == 2000,
         "a grid is full width until wide");
  expect(gui::content_max_w_for(C::Dialog, BP::Compact, 400, 1) == 368, "a compact dialog keeps a margin");
  expect(gui::content_max_w_for(C::Form, BP::Regular, 500, 1) == 500, "a column is never wider than the room");
  expect(gui::hero_height_for(BP::Regular, 800, 1) == 300 && gui::hero_height_for(BP::Wide, 2160, 1) == 340,
         "a hero band is 300 design pixels, 340 on wide");
  expect(gui::hero_height_for(BP::Compact, 1000, 1) == 220, "a compact hero band is 220 on a tall window");
  expect(gui::hero_height_for(BP::Compact, 480, 0.75f) == 144, "a short compact window keeps its band under a third");
  expect(gui::hero_height_for(BP::Compact, 300, 1) == 150, "and never below what its title needs");

  // Scrolling's arithmetic: a wheel notch, a glide, and ImGui's targets.
  expect(gui::wheel_step(17, 1000) == 85 && gui::wheel_step(17, 60) == 40, "a notch is five lines, at most 2/3 of the view");
  {
    const float first = gui::scroll_glide(0, 100, 1.0f / 60.0f);
    expect(first > 25 && first < 40, "a glide moves on the very first frame");
    float y = 0;
    for (int i = 0; i < 9; ++i) y = gui::scroll_glide(y, 100, 1.0f / 60.0f);
    expect(y > 94, "a glide is nearly there in 150 ms");
    expect(gui::scroll_glide(99.7f, 100, 0.001f) == 100, "a glide within half a pixel lands");
  }
  expect(gui::scroll_from_target(500, 0, 0, 300, 2000, 1700) == 500, "a target at the top of the view is the scroll");
  expect(gui::scroll_from_target(500, 0.5f, 0, 300, 2000, 1700) == 350, "a centred target is half a view less");
  expect(gui::scroll_from_target(1900, 1, 0, 300, 2000, 1700) == 1600, "a target at the foot of the view");
  expect(gui::scroll_from_target(5000, 0, 0, 300, 2000, 1700) == 1700 &&
             gui::scroll_from_target(-5, 0, 0, 300, 2000, 1700) == 0,
         "a target is clamped to the scroll's range");
  expect(gui::scroll_from_target(10, 0, 20, 300, 2000, 1700) == 0, "a target near the top snaps to it");
  expect(gui::scroll_from_target(1995, 1, 20, 300, 2000, 1700) == 1700, "a target near the end snaps to it");

  // A backdrop: scaled down to at most the width asked, and a flat picture
  // stays flat however it is blurred.
  {
    std::vector<unsigned char> flat(640 * 360 * 4);
    for (size_t i = 0; i < flat.size(); ++i) flat[i] = static_cast<unsigned char>(i % 4 == 3 ? 255 : 90);
    int bw = 0, bh = 0;
    const std::vector<unsigned char> b = gui::backdrop_pixels(flat.data(), 640, 360, 160, 3, &bw, &bh);
    expect(bw == 160 && bh == 90 && b.size() == 160u * 90u * 4u, "a backdrop is at most 160 pixels wide");
    bool same = true;
    for (size_t i = 0; i < b.size(); ++i) same = same && b[i] == (i % 4 == 3 ? 255 : 90);
    expect(same, "a flat picture blurs to itself");
    std::vector<unsigned char> dot(9 * 9 * 4, 0);
    dot[(4 * 9 + 4) * 4] = 255;
    const std::vector<unsigned char> s = gui::backdrop_pixels(dot.data(), 9, 9, 160, 1, &bw, &bh);
    expect(bw == 9 && s[(4 * 9 + 4) * 4] < 255 && s[(4 * 9 + 5) * 4] > 0, "a blur spreads a point");
    expect(gui::backdrop_pixels(nullptr, 0, 0, 160, 3, &bw, &bh).empty() && bw == 0, "no picture, no backdrop");
  }

  // Every shared piece drawn into no window, twice over (ImGui hides a new
  // popup's first frame), and a dialog open over them: an unbalanced stack or
  // a clipped child that is never ended is an assertion, which aborts.
  {
    gui::set_reduce_motion(true);
    const char* steps[] = {"sources", "name", "what"};
    const gui::NavEntry places[] = {{"library", "L", ">", true, 8},
                                    {"import", "I", "+", false, -1},
                                    {"all", nullptr, "o", false, 3, "collections", true}};
    for (int f = 0; f < 3; ++f) {
      ImGui::NewFrame();
      gui::begin_page("helpers", "arrows move   Enter play   A add   Esc back", nullptr);
      gui::set_input_mode(f == 1 ? gui::InputMode::Pad : gui::InputMode::Keyboard);
      gui::nav_section_begin("sidebar");
      gui::nav_sidebar("side", places, 3, 0, 200);
      gui::nav_section_end();
      ImGui::SameLine();
      gui::begin_scroll("main", ImVec2(0, 0));
      gui::hero(gui::HeroSpec{nullptr, nullptr, "example-game", "Example Game", "1996 · example-game"});
      gui::action_bar_begin("actions");
      gui::play_button("Play", gui::PlayKind::Play);
      ImGui::SameLine();
      gui::play_button("Install", gui::PlayKind::Install);
      ImGui::SameLine();
      gui::stat("last played", "3 days ago");
      ImGui::SameLine();
      gui::ghost_button("Timeline");
      gui::action_bar_end();
      gui::nav_section_begin("recent");
      if (gui::carousel_begin("recent", 120)) {
        for (int i = 0; i < 6; ++i) {
          gui::TileSpec t;
          t.id = "demo-" + std::to_string(i);
          t.name = "Demo " + std::to_string(i);
          t.w = 160;
          t.h = 120;
          t.rest_bright = 0.82f;
          gui::carousel_item(t);
        }
      }
      gui::carousel_end();
      gui::nav_section_end();
      gui::grid_begin();
      for (int i = 0; i < 4; ++i) {
        if (i) ImGui::SameLine();
        gui::tile("grid-" + std::to_string(i), "Game", "30m", nullptr, nullptr, 120, 90);
      }
      gui::grid_end();
      auto info = [](int i) { return gui::StepInfo{i == 0 ? gui::StepState::Done : gui::StepState::Warn, 3}; };
      gui::stepper("flow", steps, 3, 1, info, [](int i) { return i < 2; });
      // A step that wants the room for itself folds the steps to one line,
      // with nothing on it to press.
      expect(gui::stepper("flow-line", steps, 3, 2, info, [](int) { return true; }, true) == -1, "a folded stepper has nothing to press");
      gui::step_heading("what runs the install?");
      gui::card_begin("card", "section");
      gui::centre_column(gui::content_max_w(gui::Content::Form));
      ImGui::TextWrapped("A sentence inside a card inside a centred column.");
      gui::pad_glyph("A");
      gui::end_centre_column();
      gui::card_end();
      gui::section("checks", gui::BadgeKind::Ok);
      gui::section("problems", gui::BadgeKind::Warn, "3");
      gui::SnapshotCard snap;
      snap.gen = "0004";
      snap.when = "3 days ago";
      snap.played = "1h 31m";
      snap.files = "48 files written";
      snap.note = "left off at the second checkpoint, with a note long enough to wrap onto a line of its own";
      snap.id = "example-game";
      snap.first = true;
      for (const char* gen : {"0004", "0003"}) {
        ImGui::PushID(gen);
        gui::snapshot_card(snap);
        ImGui::PopID();
        snap.gen = "0003";
        snap.first = false;
        snap.last = true;
        snap.note.clear();
        snap.width = 400;
      }
      int choice = 1;
      const char* choices[] = {"integer", "fit", "native"};
      gui::setting_begin("scaling", "how a game's pixels reach the screen");
      gui::step_combo("##scaling", &choice, choices, 3);
      gui::setting_end();
      gui::setting_begin("/example/folder", nullptr, gui::kCyan);
      gui::ghost_button("remove");
      gui::setting_end();
      gui::centred_card_begin("message", 560);
      ImGui::TextWrapped("A card alone in the middle of the page.");
      gui::centred_card_end();
      gui::flow_footer(gui::FlowFooter{"Back", "Next: name it", false, "name it first", "Skip"});
      expect(gui::flow_footer(gui::FlowFooter{"Cancel", "Build", true, nullptr, nullptr, "saved", true}) ==
                 gui::FlowAction::None,
             "a footer nobody pressed does nothing");
      gui::end_scroll();
      ImGui::OpenPopup("question?");
      if (gui::dialog_begin("question?", 560, "A question?")) {
        ImGui::TextWrapped("Body text.");
        gui::dialog_footer();
        gui::dialog_button("Go", gui::DialogButton::Primary);
        gui::dialog_button("Throw it away", gui::DialogButton::Danger);
        gui::dialog_button("Keep it", gui::DialogButton::Secondary, true);
        gui::dialog_end();
      }
      gui::page_focus_end();
      ImGui::End();
      gui::draw_focus_glow();
      ImGui::Render();
    }
    gui::set_input_mode(gui::InputMode::Keyboard);
    gui::set_reduce_motion(false);
  }
  // A combo on a settings row steps with Left and Right while the keys are on
  // it, as a console's settings do, stopping at either end, and the focus
  // stays on it rather than moving to whatever is beside it.
  {
    ImGuiIO& kio = ImGui::GetIO();
    const ImGuiConfigFlags flags_were = kio.ConfigFlags;
    kio.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    const char* choices[] = {"integer", "fit", "native"};
    int choice = 0, changes = 0;
    bool kept = true;
    auto row = [&](bool focus, ImGuiKey key) {
      if (key != ImGuiKey_None) kio.AddKeyEvent(key, true);
      ImGui::NewFrame();
      ImGui::Begin("rows");
      gui::setting_begin("scaling");
      if (focus) ImGui::SetKeyboardFocusHere();
      if (gui::step_combo("##scaling", &choice, choices, 3)) ++changes;
      if (key != ImGuiKey_None) kept = kept && ImGui::IsItemFocused();
      gui::setting_end();
      ImGui::End();
      ImGui::Render();
      if (key != ImGuiKey_None) kio.AddKeyEvent(key, false);
    };
    row(true, ImGuiKey_None);
    row(false, ImGuiKey_None);
    ImGui::SetNavCursorVisible(true);
    for (ImGuiKey k : {ImGuiKey_RightArrow, ImGuiKey_RightArrow, ImGuiKey_RightArrow, ImGuiKey_LeftArrow}) {
      row(false, k);
      row(false, ImGuiKey_None);
    }
    expect(choice == 1 && changes == 3, "Right and Left step a settings combo, and stop at its last choice");
    expect(kept, "the focus stays on a combo stepped by Left and Right");
    kio.ConfigFlags = flags_were;
  }
  {
    ImGui::NewFrame();
    ImFont* f = ImGui::GetFont();
    const std::string long_name = "an example game whose name is far too long to fit";
    const float limit = f->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, 0, "an example").x;
    const std::string cut = gui::elide(f, long_name, limit);
    expect(cut.size() < long_name.size() && cut.substr(cut.size() - 3) == "...", "a long name is cut short with an ellipsis");
    expect(f->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, 0, cut.c_str()).x <= limit, "a cut name fits");
    expect(gui::elide(f, "short", 1000) == "short", "a name that fits is left alone");
    ImGui::Render();
  }

  // A picture forgotten in the middle of a frame - a game's cover, when Play
  // comes back from the game - is still in that frame's draw list, which is
  // drawn when the frame ends. It has to outlive the frame, and go after.
  bool texture_kept = true;
  {
    SDL_Surface* surf = SDL_CreateRGBSurfaceWithFormat(0, 16, 16, 32, SDL_PIXELFORMAT_RGBA8888);
    SDL_Renderer* ren = surf ? SDL_CreateSoftwareRenderer(surf) : nullptr;
    if (!ren) {
      std::fprintf(stderr, "  (no software renderer: %s; the texture check is skipped)\n", SDL_GetError());
    } else {
      const std::string png(
          "\x89\x50\x4e\x47\x0d\x0a\x1a\x0a\x00\x00\x00\x0d\x49\x48\x44\x52\x00\x00\x00\x01\x00\x00\x00\x01"
          "\x08\x06\x00\x00\x00\x1f\x15\xc4\x89\x00\x00\x00\x0d\x49\x44\x41\x54\x78\x9c\x63\xf8\xcf\xc0\xf0"
          "\x1f\x00\x05\x00\x01\xff\x89\x99\x3d\x1d\x00\x00\x00\x00\x49\x45\x4e\x44\xae\x42\x60\x82",
          70);
      gui::Textures textures(ren);
      ImGui::NewFrame();
      const gui::Texture* t = textures.png("/example/title.png", png);
      SDL_Texture* drawn = t ? t->tex : nullptr;
      if (!drawn) {
        std::fprintf(stderr, "  FAIL a 1x1 PNG did not become a texture\n");
        texture_kept = false;
      } else {
        ImGui::Image(reinterpret_cast<ImTextureID>(drawn), ImVec2(1, 1));
        textures.forget_matching("/example/");
        // Still in the cache, it would not be loaded again; still alive, the
        // frame's draw list can draw it.
        int w = 0;
        if (SDL_QueryTexture(drawn, nullptr, nullptr, &w, nullptr) != 0 || w != 1) {
          std::fprintf(stderr, "  FAIL a texture forgotten mid-frame was destroyed before the frame was drawn\n");
          texture_kept = false;
        }
        ImGui::Render();
        ImGui::NewFrame();
        const gui::Texture* again = textures.png("/example/title.png", png);
        if (!again || !again->tex) {
          std::fprintf(stderr, "  FAIL a forgotten picture was not loaded again\n");
          texture_kept = false;
        }
        ImGui::Render();
      }
    }
    if (ren) SDL_DestroyRenderer(ren);
    if (surf) SDL_FreeSurface(surf);
  }
  ImGui::DestroyContext();

  bool still = fs::exists(bundle::bundles_dir() / "page-smoke.cbor");
  fs::remove_all(tmp);
  std::fprintf(stderr, "%d frames over every step of the Bundles page%s\n", frames,
               still ? "" : " - but the remembered bundle went missing");
  return still && texture_kept && scale_ok ? 0 : 1;
}
