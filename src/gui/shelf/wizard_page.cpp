// The Create screen: the wizard draws itself, and says when it is done, when
// it wants a game played, and when it wants the Library screen.
#include <exception>
#include <string>

#include "../../install/manifest.h"
#include "pages.h"

namespace kg::gui::shelf {

WizardPage::WizardPage(ShelfContext& ctx, LibraryPage& library, SDL_Renderer* ren)
    : ctx_(ctx), library_(library), wizard_(ctx.env, ren, ctx.fonts) {}

void WizardPage::draw() {
  WizardOutcome out = wizard_.draw();
  switch (out.kind) {
    case WizardOutcome::Closed:
    case WizardOutcome::Play:
      ctx_.go(Screen::Shelf);
      ctx_.reload();
      if (out.kind == WizardOutcome::Play) {
        ctx_.open_game(out.game_id);
        ctx_.play(out.game_id);
      } else ctx_.status = out.status;
      if (ctx_.quit_after_wizard) ctx_.quit = true;
      break;
    case WizardOutcome::WantsLibrary:
      library_.rescan_next();
      ctx_.go(Screen::Library);
      break;
    case WizardOutcome::Still: break;
  }
}

void WizardPage::create() {
  if (shelf_busy()) return;
  wizard_.begin();
  ctx_.go(Screen::Create);
}

void WizardPage::open_for(const std::string& id) {
  if (shelf_busy()) return;
  try {
    Meta m = install::load_manifest(install::find_manifest(ctx_.env, id));
    wizard_.begin_from(install::draft_from_meta(m));
  } catch (const std::exception& ex) {
    ctx_.status = ex.what();
    wizard_.begin();
  }
  ctx_.go(Screen::Create);
}

void WizardPage::begin_from_recipe(const install::Prefill& p, const Hash& root) {
  wizard_.begin_from_recipe(p, root);
}

}  // namespace kg::gui::shelf
