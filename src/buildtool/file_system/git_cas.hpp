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

#ifndef INCLUDED_SRC_BUILDTOOL_FILE_SYSTEM_GIT_CAS_HPP
#define INCLUDED_SRC_BUILDTOOL_FILE_SYSTEM_GIT_CAS_HPP

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "gsl/gsl"
#include "src/buildtool/file_system/git_utils.hpp"
#include "src/buildtool/file_system/object_type.hpp"
#include "src/buildtool/logging/log_level.hpp"

class GitCAS;
using GitCASPtr = std::shared_ptr<GitCAS const>;

/// \brief Git CAS that maintains its Git context.
class GitCAS {
  public:
    [[nodiscard]] static auto Open(
        std::filesystem::path const& repo_path,
        LogLevel log_failure = LogLevel::Warning) noexcept -> GitCASPtr;

    [[nodiscard]] static auto CreateEmpty() noexcept -> GitCASPtr;

    GitCAS() noexcept;
    ~GitCAS() noexcept = default;

    // prohibit moves and copies
    GitCAS(GitCAS const&) = delete;
    GitCAS(GitCAS&& other) = delete;
    auto operator=(GitCAS const&) = delete;
    auto operator=(GitCAS&& other) = delete;

    [[nodiscard]] auto GetODB() const noexcept -> gsl::not_null<git_odb*> {
        return odb_.get();
    }

    [[nodiscard]] auto GetRepository() const noexcept
        -> gsl::not_null<git_repository*> {
        return repo_.get();
    }

    [[nodiscard]] auto GetPath() const noexcept
        -> std::filesystem::path const& {
        return git_path_;
    }

    /// \brief Read object from CAS.
    /// \param id          The object id.
    /// \param is_hex_id   Specify whether `id` is hex string or raw.
    /// \param as_valid_symlink Specify whether to treat any read blob as a
    ///                         valid symlink and check for non-upwardness.
    /// \param log_failure Log level at which to log failures accessing the
    /// object.
    [[nodiscard]] auto ReadObject(std::string const& id,
                                  bool is_hex_id,
                                  bool as_valid_symlink = false,
                                  LogLevel log_failure = LogLevel::Warning)
        const noexcept -> std::optional<std::string>;

    /// \brief Read object header from CAS.
    /// \param id         The object id.
    /// \param is_hex_id  Specify whether `id` is hex string or raw.
    // Use with care. Quote from git2/odb.h:138:
    //    Note that most backends do not support reading only the header of an
    //    object, so the whole object will be read and then the header will be
    //    returned.
    [[nodiscard]] auto ReadHeader(std::string const& id, bool is_hex_id)
        const noexcept -> std::optional<std::pair<std::size_t, ObjectType>>;

  private:
    std::unique_ptr<git_odb, decltype(&odb_closer)> odb_{nullptr, odb_closer};
    std::unique_ptr<git_repository, decltype(&repository_closer)> repo_{
        nullptr,
        repository_closer};
    // git folder path of repo
    std::filesystem::path git_path_;
};

#endif  // INCLUDED_SRC_BUILDTOOL_FILE_SYSTEM_GIT_CAS_HPP
