// Copyright 2022 Huawei Cloud Computing Technology Co., Ltd.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <unordered_set>

#include "catch2/catch_test_macros.hpp"
#include "fmt/core.h"
#include "src/buildtool/file_system/file_root.hpp"
#include "src/buildtool/file_system/file_system_manager.hpp"
#include "test/utils/shell_quoting.hpp"

namespace {

auto const kBundlePath =
    std::string{"test/buildtool/file_system/data/test_repo.bundle"};

auto const kBundleSymPath =
    std::string{"test/buildtool/file_system/data/test_repo_symlinks.bundle"};

[[nodiscard]] auto GetTestDir() -> std::filesystem::path {
    auto* tmp_dir = std::getenv("TEST_TMPDIR");
    if (tmp_dir != nullptr) {
        return tmp_dir;
    }
    return FileSystemManager::GetCurrentDirectory() /
           "test/buildtool/file_system";
}

[[nodiscard]] auto CreateTestRepo(bool do_checkout = false)
    -> std::optional<std::filesystem::path> {
    static std::atomic<int> counter{};
    auto repo_path =
        GetTestDir() / "test_repo" /
        std::filesystem::path{std::to_string(counter++)}.filename();
    auto cmd = fmt::format("git clone {}{} {}",
                           do_checkout ? "--branch master " : "",
                           QuoteForShell(kBundlePath),
                           QuoteForShell(repo_path.string()));
    if (std::system(cmd.c_str()) == 0) {
        return repo_path;
    }
    return std::nullopt;
}

[[nodiscard]] auto CreateTestRepoSymlinks(bool do_checkout = false)
    -> std::optional<std::filesystem::path> {
    static std::atomic<int> counter{};
    auto repo_path =
        GetTestDir() / "test_repo_symlinks" /
        std::filesystem::path{std::to_string(counter++)}.filename();
    auto cmd = fmt::format("git clone {}{} {}",
                           do_checkout ? "--branch master " : "",
                           QuoteForShell(kBundleSymPath),
                           QuoteForShell(repo_path.string()));
    if (std::system(cmd.c_str()) == 0) {
        return repo_path;
    }
    return std::nullopt;
}

}  // namespace

TEST_CASE("Get entries of a directory", "[directory_entries]") {
    const std::unordered_set<std::string> reference_entries{
        "test_repo.bundle",
        "test_repo_symlinks.bundle",
        "empty_executable",
        "subdir",
        "example_file"};

    const auto dir = std::filesystem::path("test/buildtool/file_system/data");

    auto fs_root = FileRoot();
    auto dir_entries = fs_root.ReadDirectory(dir);
    REQUIRE(dir_entries.has_value());
    CHECK(dir_entries->ContainsBlob("test_repo.bundle"));
    CHECK(dir_entries->ContainsBlob("test_repo_symlinks.bundle"));
    CHECK(dir_entries->ContainsBlob("empty_executable"));
    CHECK(dir_entries->ContainsBlob("example_file"));
    {
        // all the entries returned by FilesIterator are files,
        // are contained in reference_entries,
        // and are 4
        auto counter = 0;
        for (const auto& x : dir_entries->FilesIterator()) {
            REQUIRE(reference_entries.contains(x));
            CHECK(dir_entries->ContainsBlob(x));
            ++counter;
        }

        CHECK(counter == 4);
    }
    {
        // all the entries returned by DirectoriesIterator are not files (e.g.,
        // trees),
        // are contained in reference_entries,
        // and are 1

        auto counter = 0;
        for (const auto& x : dir_entries->DirectoriesIterator()) {
            REQUIRE(reference_entries.contains(x));
            CHECK_FALSE(dir_entries->ContainsBlob(x));
            ++counter;
        }

        CHECK(counter == 1);
    }
}

TEST_CASE("Get entries of a git tree", "[directory_entries]") {
    auto reference_entries =
        std::unordered_set<std::string>{"foo", "bar", "baz", ".git"};
    auto repo = *CreateTestRepo(true);
    auto fs_root = FileRoot();
    auto dir_entries = fs_root.ReadDirectory(repo);
    REQUIRE(dir_entries.has_value());
    CHECK(dir_entries->ContainsBlob("bar"));
    CHECK(dir_entries->ContainsBlob("foo"));
    CHECK_FALSE(dir_entries->ContainsBlob("baz"));
    {
        // all the entries returned by FilesIterator are files,
        // are contained in reference_entries,
        // and are 2 (foo, and bar)
        auto counter = 0;
        for (const auto& x : dir_entries->FilesIterator()) {
            REQUIRE(reference_entries.contains(x));
            CHECK(dir_entries->ContainsBlob(x));
            ++counter;
        }

        CHECK(counter == 2);
    }
    {
        // all the entries returned by DirectoriesIterator are not files (e.g.,
        // trees),
        // are contained in reference_entries,
        // and are 2 (baz, and .git)
        auto counter = 0;
        for (const auto& x : dir_entries->DirectoriesIterator()) {
            REQUIRE(reference_entries.contains(x));
            CHECK_FALSE(dir_entries->ContainsBlob(x));
            ++counter;
        }

        CHECK(counter == 2);
    }
}

TEST_CASE("Get entries of an empty directory", "[directory_entries]") {
    // CreateDirectoryEntriesMap returns
    // FileRoot::DirectoryEntries{FileRoot::DirectoryEntries::pairs_t{}} in case
    // of a missing directory, which represents an empty directory

    auto dir_entries =
        FileRoot::DirectoryEntries{FileRoot::DirectoryEntries::pairs_t{}};
    // of course, no files should be there
    CHECK_FALSE(dir_entries.ContainsBlob("test_repo.bundle"));
    {
        // FilesIterator should be an empty range
        auto counter = 0;
        for (const auto& x : dir_entries.FilesIterator()) {
            CHECK_FALSE(dir_entries.ContainsBlob(x));  // should never be called
            ++counter;
        }

        CHECK(counter == 0);
    }
    {
        // DirectoriesIterator should be an empty range
        auto counter = 0;
        for (const auto& x : dir_entries.DirectoriesIterator()) {
            CHECK(dir_entries.ContainsBlob(x));  // should never be called
            ++counter;
        }

        CHECK(counter == 0);
    }
}

TEST_CASE("Get ignore-special entries of a git tree with symlinks",
          "[directory_entries]") {
    auto reference_entries =
        std::unordered_set<std::string>{"foo", "bar", "baz", ".git"};
    auto repo = *CreateTestRepoSymlinks(true);
    auto fs_root = FileRoot(/*ignore_special=*/true);
    auto dir_entries = fs_root.ReadDirectory(repo);
    REQUIRE(dir_entries.has_value());
    CHECK(dir_entries->ContainsBlob("bar"));
    CHECK(dir_entries->ContainsBlob("foo"));
    CHECK_FALSE(dir_entries->ContainsBlob("baz"));
    {
        // all the entries returned by FilesIterator are files,
        // are contained in reference_entries,
        // and are 2 (foo, and bar)
        auto counter = 0;
        for (const auto& x : dir_entries->FilesIterator()) {
            REQUIRE(reference_entries.contains(x));
            CHECK(dir_entries->ContainsBlob(x));
            ++counter;
        }

        CHECK(counter == 2);
    }
    {
        // all the entries returned by DirectoriesIterator are not files (e.g.,
        // trees),
        // are contained in reference_entries,
        // and are 2 (baz, and .git)
        auto counter = 0;
        for (const auto& x : dir_entries->DirectoriesIterator()) {
            REQUIRE(reference_entries.contains(x));
            CHECK_FALSE(dir_entries->ContainsBlob(x));
            ++counter;
        }

        CHECK(counter == 2);
    }
}

TEST_CASE("Get entries of a git tree with symlinks", "[directory_entries]") {
    auto reference_entries = std::unordered_set<std::string>{
        "foo", "bar", "baz", "foo_l", "baz_l", ".git"};
    auto repo = *CreateTestRepoSymlinks(true);
    auto fs_root = FileRoot(/*ignore_special=*/false);
    auto dir_entries = fs_root.ReadDirectory(repo);
    REQUIRE(dir_entries.has_value());
    CHECK(dir_entries->ContainsBlob("bar"));
    CHECK(dir_entries->ContainsBlob("foo"));
    CHECK_FALSE(dir_entries->ContainsBlob("baz"));
    CHECK(dir_entries->ContainsBlob("foo_l"));
    CHECK(dir_entries->ContainsBlob("baz_l"));
    {
        // all the entries returned by FilesIterator are files,
        // are contained in reference_entries,
        // and are 2 (foo, and bar)
        auto counter = 0;
        for (const auto& x : dir_entries->FilesIterator()) {
            REQUIRE(reference_entries.contains(x));
            CHECK(dir_entries->ContainsBlob(x));
            ++counter;
        }

        CHECK(counter == 2);
    }
    {
        // all the entries returned by SymlinksIterator are symlinks,
        // are contained in reference_entries,
        // and are 2 (foo_l, and baz_l)
        auto counter = 0;
        for (const auto& x : dir_entries->SymlinksIterator()) {
            REQUIRE(reference_entries.contains(x));
            CHECK(dir_entries->ContainsBlob(x));
            ++counter;
        }

        CHECK(counter == 2);
    }
    {
        // all the entries returned by DirectoriesIterator are not files (e.g.,
        // trees),
        // are contained in reference_entries,
        // and are 2 (baz, and .git)
        auto counter = 0;
        for (const auto& x : dir_entries->DirectoriesIterator()) {
            REQUIRE(reference_entries.contains(x));
            CHECK_FALSE(dir_entries->ContainsBlob(x));
            ++counter;
        }

        CHECK(counter == 2);
    }
}
