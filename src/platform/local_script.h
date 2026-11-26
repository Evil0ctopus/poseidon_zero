#pragma once

#include <atomic>
#include <filesystem>
#include <string>

namespace platform {

struct LocalScript {
    std::string name;
    std::string text;
    bool python{false};
    std::string error;
};

struct ScriptResult {
    int exit_code{-1};
    bool truncated{false};
    std::string output;
    std::string error;
};

LocalScript read_local_script(const std::filesystem::path& directory,
                              const std::string& filename);
ScriptResult run_local_script(const LocalScript& script, const std::atomic_bool& cancelled);

} // namespace platform
