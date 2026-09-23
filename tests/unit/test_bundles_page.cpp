// The Bundles page drawn with no window: every step of a remembered bundle,
// several frames each, against a scratch shelf.
//
// ImGui needs no renderer to lay a page out, only a font atlas and a display
// size, so this draws the real page code into nothing. What it catches is what
// a person would otherwise find by opening the page: an ImGui assertion - an
// unbalanced Begin, PushID or disabled block, an ID used twice - which in
// this build aborts kretro rather than drawing a red tooltip. It does not
// click anything; the decisions behind the buttons are tests/unit/test_builder.cpp's.
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "bundle/builder.h"
#include "gui/bundles.h"
#include "gui/widgets.h"
#include "pack/kgpack.h"
#include "util/paths.h"
#include "imgui.h"

namespace fs = std::filesystem;
using namespace kg;

static void write_file(const fs::path& p, const std::string& content) {
  fs::create_directories(p.parent_path());
  std::ofstream f(p, std::ios::binary | std::ios::trunc);
  f << content;
}

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
  write_pack(games_dir() / "fixture.kgpack", m, WriteOptions{Kind::Game, tmp / "body", false});

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
  ImFont* font = io.Fonts->AddFontDefault();
  unsigned char* px = nullptr;
  int w = 0, h = 0;
  io.Fonts->GetTexDataAsRGBA32(&px, &w, &h);

  int frames = 0;
  {
    rt::Env env;
    gui::Bundles page(env);
    page.set_fonts(font, font);
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
  return still && texture_kept ? 0 : 1;
}
