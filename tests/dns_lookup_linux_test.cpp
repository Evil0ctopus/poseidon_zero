#include "linux_network.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {
struct Fixture {
    std::filesystem::path directory;
    ~Fixture() {
        std::error_code error;
        std::filesystem::remove(directory / "timeout", error);
        std::filesystem::remove(directory / "getent", error);
        std::filesystem::remove(directory / "curl", error);
        std::filesystem::remove(directory, error);
    }
};
}

int main() {
    const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    Fixture fixture{std::filesystem::temp_directory_path() /
                    ("poseidon-dns-fixture-" + std::to_string(suffix))};
    if (!std::filesystem::create_directory(fixture.directory)) return 1;
    {
        std::ofstream timeout(fixture.directory / "timeout");
        timeout << "#!/bin/sh\nshift\nexec \"$@\"\n";
        std::ofstream getent(fixture.directory / "getent");
        getent << "#!/bin/sh\n"
                  "case \"$POSEIDON_DNS_FIXTURE\" in\n"
                  "success) printf '192.0.2.1 STREAM example.test\\n';;\n"
                  "ipv6) printf '2001:db8::1 STREAM example.test\\n';;\n"
                  "empty) exit 0;;\n"
                  "invalid) printf 'not-an-address\\n';;\n"
                  "timeout) exit 124;;\n"
                  "failed) exit 127;;\n"
                  "large) i=0; while [ \"$i\" -lt 500 ]; do "
                  "printf '192.0.2.1 STREAM example.test\\n'; i=$((i+1)); done;;\n"
                  "*) exit 1;;\n"
                  "esac\n";
        std::ofstream curl(fixture.directory / "curl");
        curl << "#!/bin/sh\n"
                "case \" $* \" in *' --disable '*' --noproxy '*' --proto '*' --head '*) ;; *) exit 98;; esac\n"
                "for arg in \"$@\"; do last=\"$arg\"; done\n"
                "[ \"$last\" = 'https://example.test/' ] || exit 99\n"
                "case \"$POSEIDON_TLS_FIXTURE\" in\n"
                "success) printf 200;;\n"
                "http-error) printf 404;;\n"
                "untrusted) exit 60;;\n"
                "timeout) exit 28;;\n"
                "process-timeout) exit 124;;\n"
                "missing-ca) exit 77;;\n"
                "invalid) printf 000;;\n"
                "large) i=0; while [ \"$i\" -lt 600 ]; do printf x; i=$((i+1)); done;;\n"
                "*) exit 1;;\n"
                "esac\n";
        if (!timeout || !getent || !curl) return 1;
    }
    const auto permissions = std::filesystem::perms::owner_read |
                             std::filesystem::perms::owner_write |
                             std::filesystem::perms::owner_exec;
    std::filesystem::permissions(fixture.directory / "timeout", permissions);
    std::filesystem::permissions(fixture.directory / "getent", permissions);
    std::filesystem::permissions(fixture.directory / "curl", permissions);
    if (setenv("PATH", fixture.directory.c_str(), 1) != 0) return 1;

    for (const char* mode : {"success", "ipv6", "empty", "invalid", "timeout", "failed", "large"}) {
        if (setenv("POSEIDON_DNS_FIXTURE", mode, 1) != 0) return 1;
        const auto result = platform::dns_lookup("example.test");
        const std::string name(mode);
        const bool success = name == "success" || name == "ipv6";
        if ((success && (!result.error.empty() || result.output.empty())) ||
            (!success && (result.error.empty() || !result.output.empty())) ||
            (name == "timeout" && result.error.find("timed out") == std::string::npos) ||
            (name == "failed" && result.error.find("127") == std::string::npos) ||
            (name == "large" && result.error.find("limit") == std::string::npos)) {
            std::cerr << "Unexpected " << mode << " lookup result: " << result.error << '\n';
            return 1;
        }
    }
    if (platform::dns_lookup("bad;host").error != "Invalid host") {
        std::cerr << "Invalid host accepted\n";
        return 1;
    }
    for (const char* mode : {"success", "http-error", "untrusted", "timeout", "process-timeout",
                             "missing-ca", "invalid", "large"}) {
        if (setenv("POSEIDON_TLS_FIXTURE", mode, 1) != 0) return 1;
        const auto result = platform::check_https_certificate("example.test");
        const std::string name(mode);
        const bool success = name == "success" || name == "http-error";
        if (result.verified != success || result.detail.empty() ||
            (name == "untrusted" && result.detail.find("verification failed") == std::string::npos) ||
            ((name == "timeout" || name == "process-timeout") &&
             result.detail.find("timed out") == std::string::npos) ||
            (name == "missing-ca" && result.detail.find("77") == std::string::npos)) {
            std::cerr << "Unexpected " << mode << " HTTPS result: " << result.detail << '\n';
            return 1;
        }
    }
    if (platform::valid_https_host("bad;host") ||
        platform::valid_https_host(std::string(45, 'a')) ||
        platform::check_https_certificate("bad;host").detail != "Invalid HTTPS hostname") {
        std::cerr << "Invalid HTTPS host accepted\n";
        return 1;
    }
    return 0;
}
