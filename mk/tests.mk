# `make test`: the unit tests, the Bundles page drawn into no window, and the
# bootstrap run against stand-ins. None of it needs iso/ or the runtime; the
# tests that do are the scripts in tests/integration/, run by hand.

test: images
	@$(RUN_GUI) make -s $(UNIT_TEST_BIN) $(GUI_TEST_BIN) $(BUILD)/kretro-b3
	@set -e; for t in $(UNIT_TEST_BIN); do $$t; done
	@$(RUN_GUI) $(GUI_TEST_BIN)
	@$(DOCKER) --tmpfs /nx:rw,noexec,mode=1777 $(BUILDER) make -s boot-test

# tests/integration/test_boot.sh runs the real bootstrap against files it
# builds around a stand-in loader and app. `make test` runs this in the musl
# builder, with a noexec tmpfs at /nx for the checks that need one.
BOOT_TEST_BIN = $(BUILD)/boot-test/stub_loader $(BUILD)/boot-test/stub_app $(BUILD)/boot-test/b3sum

boot-test: $(BUILD)/bootstrap $(BOOT_TEST_BIN)
	@tests/integration/test_boot.sh

$(BUILD)/boot-test/b3sum: tests/fixtures/boot/b3sum.c $(BOOT_B3_OBJ)
	@mkdir -p $(dir $@)
	@$(MUSL_CC) $(MUSL_FLAGS) -I$(B3) $^ -o $@

$(BUILD)/boot-test/%: tests/fixtures/boot/%.c
	@mkdir -p $(dir $@)
	@$(MUSL_CC) $(MUSL_FLAGS) $< -o $@

# Opens both runtimes and checks what each must and must not carry, then starts
# 64-bit and 32-bit Wine from them. Needs `make runtime` first; slow.
test-runtime:
	@BUILD=$(BUILD) ./tests/integration/runtime.sh
