#include "host.h"

#include <cstdlib>

namespace kg::gpu {
namespace fs = std::filesystem;

Host Host::system() {
  Host h;
  h.root = "/";
  h.getenv = [](const std::string& k) {
    const char* v = std::getenv(k.c_str());
    return std::string(v ? v : "");
  };
  return h;
}

Host Host::fixture(const fs::path& root, std::map<std::string, std::string> env) {
  Host h;
  h.root = root;
  h.getenv = [env = std::move(env)](const std::string& k) {
    auto it = env.find(k);
    return it == env.end() ? std::string() : it->second;
  };
  return h;
}

fs::path Host::at(const fs::path& abs) const {
  // relative_path drops the leading "/", so "/sys/x" under "/" is "/sys/x" and
  // under a fixture is "<fixture>/sys/x".
  return root / abs.relative_path();
}

}  // namespace kg::gpu
