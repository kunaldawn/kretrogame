# Formatting and static analysis, both in the GUI builder so every machine
# runs the same clang-format and clang-tidy. Images built before these tools
# were added to runtime/Dockerfile.guibuilder are rebuilt the first time. And
# check-generic, which runs on the host against local data.

HAVE_CLANG_TOOLS = $(RUN_GUI) sh -c 'command -v clang-format && command -v clang-tidy' >/dev/null 2>&1 || \
	docker build -q -t $(GUIBUILDER) -f runtime/Dockerfile.guibuilder runtime/ >/dev/null

# Rewrites the files in FORMAT_CLEAN to .clang-format's style. Everything
# else is formatted only on the lines an edit changes (git clang-format).
format: images
	@$(HAVE_CLANG_TOOLS)
	@$(RUN_GUI) clang-format -i $(FORMAT_CLEAN)

# Fails, naming each place, if a file in FORMAT_CLEAN is not in
# .clang-format's style. CI runs this one.
format-check: images
	@$(HAVE_CLANG_TOOLS)
	@$(RUN_GUI) clang-format --dry-run --Werror $(FORMAT_CLEAN)

# The whole tree. format-all rewrites every file, breaking up the deliberate
# multi-statement lines the older code keeps, so it is not for routine use.
# format-check-all lists every place out of style and never fails: it is for
# reading, not a gate.
format-all: images
	@$(HAVE_CLANG_TOOLS)
	@$(RUN_GUI) clang-format -i $(FORMAT_SRC)

format-check-all: images
	@$(HAVE_CLANG_TOOLS)
	@$(RUN_GUI) clang-format --dry-run $(FORMAT_SRC)

# clang-tidy over src/ with the checks .clang-tidy chooses, compiled with the
# flags the build uses. One file per process, as many at a time as there are
# cores. lint-run is the part inside the image, where pkg-config finds SDL.
#
# LINT_CHECKS, when set, is passed to clang-tidy as --checks, on top of
# .clang-tidy's list, so that one check can be measured without editing the
# file: make lint LINT_CHECKS='-*,performance-unnecessary-value-param'
LINT_CHECKS ?=

lint: images
	@$(HAVE_CLANG_TOOLS)
	@$(RUN_GUI) make -s lint-run LINT_CHECKS='$(LINT_CHECKS)'

# The "N warnings generated." lines are about headers outside our filter, and
# are dropped; what is left is what failed.
lint-run:
	@out=$$(printf '%s\n' $(LINT_SRC) | \
	  xargs -P "$$(nproc)" -I{} clang-tidy --quiet $(if $(LINT_CHECKS),--checks='$(LINT_CHECKS)') {} -- -std=c++20 $(INCLUDES) 2>&1); rc=$$?; \
	 [ -z "$$out" ] || printf '%s\n' "$$out" | grep -v 'generated\.$$' || true; exit $$rc

# Fails, naming each place, if any tracked file names something from the game
# collection on this machine (games/, db/, iso/, tests/local/). Local only: CI
# has no collection to look for.
check-generic:
	@scripts/check-generic.sh
