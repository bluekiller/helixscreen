// SPDX-License-Identifier: GPL-3.0-or-later
#include "helix_fs.h"
#include "text_io.h"

#include <cerrno>
#include <cstdlib>
#include <string>
#include <thread>

#include "../catch_amalgamated.hpp"

// A thread barred from storage (the ESP32 HTTP lane, whose PSRAM stack cannot
// survive a flash access) gets a refusal from every entry point instead of
// reaching the filesystem; every other thread is unaffected.
TEST_CASE("helix_fs: a thread barred from storage is refused, others are not",
          "[helix_fs][storage_guard]") {
    char tmpl[] = "/tmp/helix_storage_guard_XXXXXX";
    REQUIRE(mkdtemp(tmpl) != nullptr);
    const std::string dir = tmpl;
    const std::string file = dir + "/f.txt";
    REQUIRE(helix::text_io::write_file(file, "hello"));
    const std::string new_dir = dir + "/made/here";

    struct Seen {
        bool exists = true, is_dir = true, created = true, read = true, listed = true, sized = true,
             removed = true;
        int exists_errno = 0;
    } seen;
    std::thread barred([&] {
        helix::fs::forbid_storage_on_this_thread("test_lane");
        seen.exists = helix::fs::exists(file);
        seen.exists_errno = errno;
        seen.is_dir = helix::fs::is_directory(dir);
        seen.created = helix::fs::create_directories(new_dir);
        seen.read = helix::text_io::read_file(file).has_value();
        seen.listed = helix::fs::list_dir(dir).has_value();
        seen.sized = helix::text_io::file_size(file).has_value();
        seen.removed = helix::fs::remove(file);
    });
    barred.join();

    CHECK_FALSE(seen.exists);
    CHECK(seen.exists_errno == EPERM);
    CHECK_FALSE(seen.is_dir);
    CHECK_FALSE(seen.created);
    CHECK_FALSE(seen.read);
    CHECK_FALSE(seen.listed);
    CHECK_FALSE(seen.sized);
    CHECK_FALSE(seen.removed);

    // Nothing reached the disk, and this thread still can.
    CHECK_FALSE(helix::fs::exists(new_dir));
    CHECK(helix::fs::exists(file));
    CHECK(helix::text_io::read_file(file) == std::optional<std::string>("hello"));

    REQUIRE(helix::fs::remove(file));
    REQUIRE(helix::fs::remove(dir));
}
