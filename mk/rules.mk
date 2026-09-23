# Compiling and linking. Every rule here runs inside an image: the C++ in
# kretro-guibuilder, the bootstrap and anything musl in kretro-builder.

# --- compilation -----------------------------------------------------------
$(BUILD)/%.o: %.cpp
	@mkdir -p $(dir $@)
	@$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS) $(B3_FLAGS) -c $< -o $@

$(BUILD)/musl/%.o: %.c
	@mkdir -p $(dir $@)
	@$(MUSL_CC) $(MUSL_FLAGS) $(B3_FLAGS) -c $< -o $@

# --- programs --------------------------------------------------------------
$(BUILD)/kretro-gui: $(BUILD)/src/main_kretro.o $(LIB_OBJ) $(GUI_OBJ) $(B3_OBJ)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LIBS)

$(BUILD)/kgpack: $(BUILD)/src/pack/main_kgpack.o $(LIB_OBJ) $(B3_OBJ)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $^ -o $@ -lzstd -lpthread

# The player links the library as an archive, so it takes only the objects
# something in it reaches: the disc scanner, the recipe resolver and the
# wizard's engine stay in kretro, unless session or the saves export needs
# them.
$(BUILD)/libkg.a: $(LIB_OBJ) $(B3_OBJ)
	@mkdir -p $(dir $@)
	@rm -f $@
	@ar rcs $@ $^

$(BUILD)/kretro-player: $(BUILD)/src/main_player.o $(PLAYER_GUI_OBJ) $(BUILD)/libkg.a
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LIBS)

# The linker's BLAKE3. Built from source in one step rather than from the
# objects above, and static, because link runs in the musl builder and the
# tests in the GUI builder, and a binary from either has to run in both.
$(BUILD)/kretro-b3: scripts/kretro-b3.c $(B3_SRC)
	@mkdir -p $(dir $@)
	@$(CC) -static -O2 $(B3_FLAGS) -I$(B3) $^ -o $@

$(BUILD)/bootstrap: boot/bootstrap.c $(BOOT_B3_OBJ)
	@mkdir -p $(dir $@)
	@$(MUSL_CC) $(MUSL_FLAGS) -I$(B3) $^ -o $@
	@printf '  bootstrap: %s, ' "$$(du -h $@ | cut -f1)"
	@if ldd $@ 2>&1 | grep -q 'not a dynamic executable'; then echo 'static'; \
	 else echo 'NOT STATIC - this will not run on Alpine'; exit 1; fi

# --- unit tests ------------------------------------------------------------
$(UNIT_TEST_BIN): $(BUILD)/test_%: $(BUILD)/tests/unit/test_%.o $(LIB_OBJ) $(B3_OBJ)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $^ -o $@ -lzstd -lpthread

$(GUI_TEST_BIN): $(BUILD)/tests/unit/test_bundles_page.o $(LIB_OBJ) $(GUI_OBJ) $(B3_OBJ)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LIBS)

# Header dependencies, written by -MMD. Missing on a clean tree, which is why
# the dash: make must not complain about them the first time.
-include $(sort $(LIB_OBJ:.o=.d) $(GUI_OBJ:.o=.d) $(PLAYER_GUI_OBJ:.o=.d) $(MAIN_OBJ:.o=.d) $(TEST_OBJ:.o=.d))
