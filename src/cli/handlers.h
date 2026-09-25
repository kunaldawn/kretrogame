// One function per command, each in its group's file. The table in
// commands.cpp decides when each runs and with how many arguments; a handler
// may assume the count is within its row's arity. Internal to src/cli.
#pragma once

#include <string>
#include <vector>

#include "../rt/env.h"

namespace kg::cli {

// system.cpp
int cmd_info(const rt::Env& e, std::vector<std::string>& a);
int cmd_doctor(const rt::Env& e, std::vector<std::string>& a);
int cmd_wine(const rt::Env& e, std::vector<std::string>& a);
int cmd_exec(const rt::Env& e, std::vector<std::string>& a);
int cmd_stage_probe(const rt::Env& e, std::vector<std::string>& a);

// catalogue.cpp
int cmd_games(const rt::Env& e, std::vector<std::string>& a);
int cmd_identify(const rt::Env& e, std::vector<std::string>& a);
int cmd_scan(const rt::Env& e, std::vector<std::string>& a);
int cmd_contents(const rt::Env& e, std::vector<std::string>& a);
int cmd_key(const rt::Env& e, std::vector<std::string>& a);

// install.cpp
int cmd_create(const rt::Env& e, std::vector<std::string>& a);
int cmd_install(const rt::Env& e, std::vector<std::string>& a);
int cmd_swap(const rt::Env& e, std::vector<std::string>& a);

// library.cpp
int cmd_list(const rt::Env& e, std::vector<std::string>& a);
int cmd_verify(const rt::Env& e, std::vector<std::string>& a);
int cmd_uninstall(const rt::Env& e, std::vector<std::string>& a);
int cmd_display(const rt::Env& e, std::vector<std::string>& a);
int cmd_panel(const rt::Env& e, std::vector<std::string>& a);
int cmd_show(const rt::Env& e, std::vector<std::string>& a);

// play.cpp
int cmd_play(const rt::Env& e, std::vector<std::string>& a);
int cmd_compare(const rt::Env& e, std::vector<std::string>& a);
int cmd_input(const rt::Env& e, std::vector<std::string>& a);

// saves.cpp
int cmd_journal(const rt::Env& e, std::vector<std::string>& a);
int cmd_saves(const rt::Env& e, std::vector<std::string>& a);
int cmd_restore(const rt::Env& e, std::vector<std::string>& a);
int cmd_export_saves(const rt::Env& e, std::vector<std::string>& a);
int cmd_import_saves(const rt::Env& e, std::vector<std::string>& a);

// share.cpp
int cmd_export(const rt::Env& e, std::vector<std::string>& a);
int cmd_import(const rt::Env& e, std::vector<std::string>& a);

// bundle.cpp
int cmd_bundle(const rt::Env& e, std::vector<std::string>& a);
int cmd_bundle_build(const rt::Env& e, std::vector<std::string>& a);

}  // namespace kg::cli
