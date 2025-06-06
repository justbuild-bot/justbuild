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

#include "src/buildtool/file_system/git_cas.hpp"

#include <exception>
#include <mutex>

#include "src/buildtool/file_system/git_context.hpp"
#include "src/buildtool/logging/logger.hpp"
#include "src/utils/cpp/hex_string.hpp"
#include "src/utils/cpp/path.hpp"

#ifndef BOOTSTRAP_BUILD_TOOL

extern "C" {
#include <git2.h>
}

namespace {

[[nodiscard]] auto GitTypeToObjectType(git_object_t const& type) noexcept
    -> std::optional<ObjectType> {
    switch (type) {
        case GIT_OBJECT_BLOB:
            return ObjectType::File;
        case GIT_OBJECT_TREE:
            return ObjectType::Tree;
        default:
            Logger::Log(LogLevel::Debug,
                        "Unsupported git object type {}",
                        git_object_type2string(type));
            return std::nullopt;
    }
}

}  // namespace

#endif  // BOOTSTRAP_BUILD_TOOL

GitCAS::GitCAS() noexcept {
    GitContext::Create();
}

auto GitCAS::Open(std::filesystem::path const& repo_path,
                  LogLevel log_failure) noexcept -> GitCASPtr {
#ifdef BOOTSTRAP_BUILD_TOOL
    return nullptr;
#else
    try {
        auto result = std::make_shared<GitCAS>();

        // lock as git_repository API has no thread-safety guarantees
        static std::mutex repo_mutex{};
        std::unique_lock lock{repo_mutex};
        git_repository* repo_ptr = nullptr;
        if (git_repository_open_ext(&repo_ptr,
                                    repo_path.c_str(),
                                    GIT_REPOSITORY_OPEN_NO_SEARCH,
                                    nullptr) != 0 or
            repo_ptr == nullptr) {
            Logger::Log(log_failure,
                        "Opening git repository {} failed with:\n{}",
                        repo_path.string(),
                        GitLastError());
            return nullptr;
        }
        result->repo_.reset(repo_ptr);

        git_odb* odb_ptr = nullptr;
        if (git_repository_odb(&odb_ptr, result->repo_.get()) != 0 or
            odb_ptr == nullptr) {
            Logger::Log(log_failure,
                        "Obtaining git object database {} failed with:\n{}",
                        repo_path.string(),
                        GitLastError());
            return nullptr;
        }
        result->odb_.reset(odb_ptr);

        auto const git_path =
            git_repository_is_bare(result->repo_.get()) != 0
                ? ToNormalPath(git_repository_path(result->repo_.get()))
                : ToNormalPath(git_repository_workdir(result->repo_.get()));

        result->git_path_ = std::filesystem::absolute(git_path);
        return std::const_pointer_cast<GitCAS const>(result);
    } catch (std::exception const& e) {
        Logger::Log(log_failure,
                    "Unexpected failure opening git object database {}:\n{}",
                    repo_path.string(),
                    e.what());
        return nullptr;
    }
#endif
}

auto GitCAS::CreateEmpty() noexcept -> GitCASPtr {
#ifdef BOOTSTRAP_BUILD_TOOL
    return nullptr;
#else
    try {
        auto result = std::make_shared<GitCAS>();

        git_odb* odb_ptr{nullptr};
        if (git_odb_new(&odb_ptr) != 0 or odb_ptr == nullptr) {
            Logger::Log(LogLevel::Error,
                        "Creating an empty database failed with:\n{}",
                        GitLastError());
            return nullptr;
        }
        result->odb_.reset(odb_ptr);  // retain odb pointer

        git_repository* repo_ptr = nullptr;
        if (git_repository_wrap_odb(&repo_ptr, result->odb_.get()) != 0 or
            repo_ptr == nullptr) {
            Logger::Log(LogLevel::Error,
                        "Creating an empty repository failed with:\n{}",
                        GitLastError());
            return nullptr;
        }
        result->repo_.reset(repo_ptr);  // retain repo pointer
        return std::const_pointer_cast<GitCAS const>(result);
    } catch (std::exception const& e) {
        Logger::Log(LogLevel::Error,
                    "Unexpected failure creating empty repository:\n{}",
                    e.what());
        return nullptr;
    }
#endif
}

auto GitCAS::ReadObject(std::string const& id,
                        bool is_hex_id,
                        bool as_valid_symlink,
                        LogLevel log_failure) const noexcept
    -> std::optional<std::string> {
#ifdef BOOTSTRAP_BUILD_TOOL
    return std::nullopt;
#else
    try {
        if (not odb_) {
            return std::nullopt;
        }

        auto oid = GitObjectID(id, is_hex_id);
        if (not oid) {
            return std::nullopt;
        }

        git_odb_object* obj = nullptr;
        if (git_odb_read(&obj, odb_.get(), &oid.value()) != 0) {
            Logger::Log(log_failure,
                        "Reading git object {} from database failed with:\n{}",
                        is_hex_id ? id : ToHexString(id),
                        GitLastError());
            return std::nullopt;
        }

        std::string data(static_cast<char const*>(git_odb_object_data(obj)),
                         git_odb_object_size(obj));
        auto obj_type = GitTypeToObjectType(git_odb_object_type(obj));
        git_odb_object_free(obj);

        if (as_valid_symlink) {
            if (not obj_type) {
                return std::nullopt;
            }
            if (not IsTreeObject(*obj_type) and not PathIsNonUpwards(data)) {
                Logger::Log(log_failure,
                            "Invalid git object {}: upwards symlink",
                            is_hex_id ? id : ToHexString(id));
                return std::nullopt;
            }
        }

        return data;
    } catch (std::exception const& e) {
        Logger::Log(log_failure,
                    "Unexpected failure reading git object {}:\n{}",
                    is_hex_id ? id : ToHexString(id),
                    e.what());
        return std::nullopt;
    }
#endif
}

auto GitCAS::ReadHeader(std::string const& id, bool is_hex_id) const noexcept
    -> std::optional<std::pair<std::size_t, ObjectType>> {
#ifndef BOOTSTRAP_BUILD_TOOL
    try {
        if (not odb_) {
            return std::nullopt;
        }

        auto oid = GitObjectID(id, is_hex_id);
        if (not oid) {
            return std::nullopt;
        }

        std::size_t size{};
        git_object_t type{};
        if (git_odb_read_header(&size, &type, odb_.get(), &oid.value()) != 0) {
            Logger::Log(LogLevel::Debug,
                        "Reading git object header {} from database failed "
                        "with:\n{}",
                        is_hex_id ? id : ToHexString(id),
                        GitLastError());
            return std::nullopt;
        }

        if (auto obj_type = GitTypeToObjectType(type)) {
            return std::make_pair(size, *obj_type);
        }
    } catch (std::exception const& e) {
        Logger::Log(LogLevel::Debug,
                    "Unexpected failure reading git object header {}:\n{}",
                    is_hex_id ? id : ToHexString(id),
                    e.what());
    }
#endif
    return std::nullopt;
}
