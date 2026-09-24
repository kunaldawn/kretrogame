// The Bundles screen: the Bundles page, which does its own drawing and its
// own going back.
#include "pages.h"

namespace kg::gui::shelf {

void BundlesPage::open() {
  bundles_.open();
  ctx_.go(Screen::Bundles);
}

}  // namespace kg::gui::shelf
