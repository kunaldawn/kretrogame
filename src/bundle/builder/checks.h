// The Bundles page's check list: what it warns about, what stops a build
// outright, and whether Build is offered.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "../../util/pe.h"
#include "draft.h"

namespace kg::bundle {

// pack_facts.h holds GameFacts in DraftFacts, so it includes this header and
// not the other way round.
struct PackFacts;

// ---- the check list -----------------------------------------------------------------

struct GameFacts {
  const PackFacts* pack = nullptr;
  std::optional<pe::Imports> imports;  // absent until read
  std::string vault_key;               // empty when the vault has none
};

struct Check {
  std::string id;     // stable, so an acknowledgement survives a restart
  std::string game;   // empty for the bundle as a whole
  std::string text;   // what was found, in a sentence
  std::string fix;    // what fixing it would be; empty when only acknowledging makes sense
};

// SafeDisc or SecuROM, the author's key and registry.reg, dgVoodoo, a Glide-only
// renderer, no cover art. `games` is in draft order, one per draft game.
std::vector<Check> run_checks(const Draft& d, const std::vector<GameFacts>& games);

// What stops a build outright, whatever is acknowledged: nothing to build, a
// field bundle.meta would refuse, a key to embed that the vault does not have.
std::vector<std::string> blockers(const Draft& d, const std::vector<GameFacts>& games);

// Build is offered when there are no blockers, every check is acknowledged,
// and the rights box is ticked.
bool ready_to_build(const Draft& d, const std::vector<Check>& checks,
                    const std::vector<std::string>& blocking);

}  // namespace kg::bundle
