// Copyright 2023 Huawei Cloud Computing Technology Co., Ltd.
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

#ifndef BOOTSTRAP_BUILD_TOOL

#include "src/buildtool/serve_api/serve_service/source_tree.hpp"

#include <algorithm>
#include <functional>
#include <vector>

#include "fmt/core.h"
#include "google/protobuf/repeated_ptr_field.h"
#include "nlohmann/json.hpp"
#include "src/buildtool/common/artifact.hpp"
#include "src/buildtool/common/artifact_digest.hpp"
#include "src/buildtool/common/artifact_digest_factory.hpp"
#include "src/buildtool/common/bazel_types.hpp"
#include "src/buildtool/common/protocol_traits.hpp"
#include "src/buildtool/common/repository_config.hpp"
#include "src/buildtool/crypto/hash_function.hpp"
#include "src/buildtool/execution_api/common/execution_api.hpp"
#include "src/buildtool/execution_api/serve/mr_git_api.hpp"
#include "src/buildtool/execution_api/utils/rehash_utils.hpp"
#include "src/buildtool/file_system/file_system_manager.hpp"
#include "src/buildtool/file_system/git_cas.hpp"
#include "src/buildtool/file_system/git_repo.hpp"
#include "src/buildtool/logging/log_level.hpp"
#include "src/buildtool/multithreading/async_map_utils.hpp"
#include "src/buildtool/multithreading/task_system.hpp"
#include "src/buildtool/storage/config.hpp"
#include "src/buildtool/storage/fs_utils.hpp"
#include "src/buildtool/storage/garbage_collector.hpp"
#include "src/buildtool/storage/repository_garbage_collector.hpp"
#include "src/buildtool/storage/storage.hpp"
#include "src/buildtool/tree_structure/tree_structure_utils.hpp"
#include "src/utils/archive/archive_ops.hpp"
#include "src/utils/cpp/expected.hpp"
#include "src/utils/cpp/file_locking.hpp"
#include "src/utils/cpp/hex_string.hpp"
#include "src/utils/cpp/tmp_dir.hpp"

namespace {

auto ArchiveTypeToString(
    ::justbuild::just_serve::ServeArchiveTreeRequest_ArchiveType const& type)
    -> std::string {
    using ServeArchiveType =
        ::justbuild::just_serve::ServeArchiveTreeRequest_ArchiveType;
    switch (type) {
        case ServeArchiveType::ServeArchiveTreeRequest_ArchiveType_ZIP: {
            return "zip";
        }
        case ServeArchiveType::ServeArchiveTreeRequest_ArchiveType_TAR:
        default:
            return "archive";  //  default to .tar archive
    }
}

auto SymlinksResolveToPragmaSpecial(
    ::justbuild::just_serve::ServeArchiveTreeRequest_SymlinksResolve const&
        resolve) -> std::optional<PragmaSpecial> {
    using ServeSymlinksResolve =
        ::justbuild::just_serve::ServeArchiveTreeRequest_SymlinksResolve;
    switch (resolve) {
        case ServeSymlinksResolve::
            ServeArchiveTreeRequest_SymlinksResolve_IGNORE: {
            return PragmaSpecial::Ignore;
        }
        case ServeSymlinksResolve::
            ServeArchiveTreeRequest_SymlinksResolve_PARTIAL: {
            return PragmaSpecial::ResolvePartially;
        }
        case ServeSymlinksResolve::
            ServeArchiveTreeRequest_SymlinksResolve_COMPLETE: {
            return PragmaSpecial::ResolveCompletely;
        }
        case ServeSymlinksResolve::ServeArchiveTreeRequest_SymlinksResolve_NONE:
        default:
            return std::nullopt;  // default to NONE
    }
}

/// \brief Extracts the archive of given type into the destination directory
/// provided. Returns nullopt on success, or error string on failure.
[[nodiscard]] auto ExtractArchive(std::filesystem::path const& archive,
                                  std::string const& repo_type,
                                  std::filesystem::path const& dst_dir) noexcept
    -> std::optional<std::string> {
    if (repo_type == "archive") {
        return ArchiveOps::ExtractArchive(
            ArchiveType::TarAuto, archive, dst_dir);
    }
    if (repo_type == "zip") {
        return ArchiveOps::ExtractArchive(
            ArchiveType::ZipAuto, archive, dst_dir);
    }
    return "unrecognized archive type";
}

}  // namespace

auto SourceTreeService::EnsureGitCacheRoot()
    -> expected<std::monostate, std::string> {
    // make sure the git root directory is properly initialized
    if (not FileSystemManager::CreateDirectory(
            native_context_->storage_config->GitRoot())) {
        return unexpected{
            fmt::format("Could not create Git cache root {}",
                        native_context_->storage_config->GitRoot().string())};
    }
    if (not GitRepo::InitAndOpen(native_context_->storage_config->GitRoot(),
                                 true)) {
        return unexpected{
            fmt::format("Could not initialize Git cache repository {}",
                        native_context_->storage_config->GitRoot().string())};
    }
    return std::monostate{};
}

auto SourceTreeService::GetSubtreeFromCommit(
    std::filesystem::path const& repo_path,
    std::string const& commit,
    std::string const& subdir,
    std::shared_ptr<Logger> const& logger)
    -> expected<std::string, GitLookupError> {
    if (auto git_cas = GitCAS::Open(repo_path)) {
        if (auto repo = GitRepo::Open(git_cas)) {
            // wrap logger for GitRepo call
            auto wrapped_logger = std::make_shared<GitRepo::anon_logger_t>(
                [logger, repo_path, commit, subdir](auto const& msg,
                                                    bool fatal) {
                    if (fatal) {
                        logger->Emit(LogLevel::Debug,
                                     "While retrieving subtree {} of commit {} "
                                     "from repository {}:\n{}",
                                     subdir,
                                     commit,
                                     repo_path.string(),
                                     msg);
                    }
                });
            return repo->GetSubtreeFromCommit(commit, subdir, wrapped_logger);
        }
    }
    return unexpected{GitLookupError::Fatal};
}

auto SourceTreeService::GetSubtreeFromTree(
    std::filesystem::path const& repo_path,
    std::string const& tree_id,
    std::string const& subdir,
    std::shared_ptr<Logger> const& logger)
    -> expected<std::string, GitLookupError> {
    if (auto git_cas = GitCAS::Open(repo_path)) {
        if (auto repo = GitRepo::Open(git_cas)) {
            // wrap logger for GitRepo call
            auto wrapped_logger = std::make_shared<GitRepo::anon_logger_t>(
                [logger, repo_path, tree_id, subdir](auto const& msg,
                                                     bool fatal) {
                    if (fatal) {
                        logger->Emit(LogLevel::Debug,
                                     "While retrieving subtree {} of tree {} "
                                     "from repository {}:\n{}",
                                     subdir,
                                     tree_id,
                                     repo_path.string(),
                                     msg);
                    }
                });
            if (auto subtree_id =
                    repo->GetSubtreeFromTree(tree_id, subdir, wrapped_logger)) {
                return *subtree_id;
            }
            return unexpected{GitLookupError::NotFound};  // non-fatal failure
        }
    }
    return unexpected{GitLookupError::Fatal};
}

auto SourceTreeService::GetBlobFromRepo(std::filesystem::path const& repo_path,
                                        std::string const& blob_id,
                                        std::shared_ptr<Logger> const& logger)
    -> expected<std::string, GitLookupError> {
    if (auto git_cas = GitCAS::Open(repo_path)) {
        if (auto repo = GitRepo::Open(git_cas)) {
            // wrap logger for GitRepo call
            auto wrapped_logger = std::make_shared<GitRepo::anon_logger_t>(
                [logger, repo_path, blob_id](auto const& msg, bool fatal) {
                    if (fatal) {
                        logger->Emit(LogLevel::Debug,
                                     "While checking existence of blob {} in "
                                     "repository {}:\n{}",
                                     blob_id,
                                     repo_path.string(),
                                     msg);
                    }
                });
            auto res = repo->TryReadBlob(blob_id, wrapped_logger);
            if (not res.first) {
                return unexpected{GitLookupError::Fatal};
            }
            if (not res.second) {
                logger->Emit(LogLevel::Debug,
                             "Blob {} not found in repository {}",
                             blob_id,
                             repo_path.string());
                return unexpected{
                    GitLookupError::NotFound};  // non-fatal failure
            }
            return res.second.value();
        }
    }
    // failed to open repository
    logger->Emit(
        LogLevel::Debug, "Failed to open repository {}", repo_path.string());
    return unexpected{GitLookupError::Fatal};
}

auto SourceTreeService::ServeCommitTree(
    ::grpc::ServerContext* /* context */,
    const ::justbuild::just_serve::ServeCommitTreeRequest* request,
    ServeCommitTreeResponse* response) -> ::grpc::Status {
    logger_->Emit(
        LogLevel::Debug, "ServeCommitTree({}, ...)", request->commit());
    // get lock for Git cache
    auto repo_lock = RepositoryGarbageCollector::SharedLock(
        *native_context_->storage_config);
    if (not repo_lock) {
        logger_->Emit(LogLevel::Error, "Could not acquire repo gc SharedLock");
        response->set_status(ServeCommitTreeResponse::INTERNAL_ERROR);
        return ::grpc::Status::OK;
    }
    // ensure Git cache exists
    if (auto done = EnsureGitCacheRoot(); not done) {
        logger_->Emit(LogLevel::Error, std::move(done).error());
        response->set_status(ServeCommitTreeResponse::INTERNAL_ERROR);
        return ::grpc::Status::OK;
    }

    auto const& commit{request->commit()};
    auto const& subdir{request->subdir()};
    // try in local build root Git cache
    auto res = GetSubtreeFromCommit(
        native_context_->storage_config->GitRoot(), commit, subdir, logger_);
    if (res) {
        auto const tree_id = *std::move(res);
        auto status = ServeCommitTreeResponse::OK;
        if (request->sync_tree()) {
            status =
                SyncGitEntryToCas<ObjectType::Tree, ServeCommitTreeResponse>(
                    tree_id, native_context_->storage_config->GitRoot());
            if (status == ServeCommitTreeResponse::OK) {
                status = SetDigestInResponse<ServeCommitTreeResponse>(
                    response, tree_id, /*is_tree=*/true, /*from_git=*/true);
            }
        }
        *(response->mutable_tree()) = tree_id;
        response->set_status(status);
        return ::grpc::Status::OK;
    }
    // report fatal failure
    if (res.error() == GitLookupError::Fatal) {
        logger_->Emit(LogLevel::Error,
                      "Failed while retrieving subtree {} of commit {} from "
                      "repository {}",
                      subdir,
                      commit,
                      native_context_->storage_config->GitRoot().string());
        response->set_status(ServeCommitTreeResponse::INTERNAL_ERROR);
        return ::grpc::Status::OK;
    }
    // try given extra repositories, in order
    for (auto const& path : serve_config_.known_repositories) {
        auto res = GetSubtreeFromCommit(path, commit, subdir, logger_);
        if (res) {
            auto const tree_id = *std::move(res);
            auto status = ServeCommitTreeResponse::OK;
            if (request->sync_tree()) {
                status =
                    SyncGitEntryToCas<ObjectType::Tree,
                                      ServeCommitTreeResponse>(tree_id, path);
                if (status == ServeCommitTreeResponse::OK) {
                    status = SetDigestInResponse<ServeCommitTreeResponse>(
                        response, tree_id, /*is_tree=*/true, /*from_git=*/true);
                }
            }
            *(response->mutable_tree()) = tree_id;
            response->set_status(status);
            return ::grpc::Status::OK;
        }
        // report fatal failure
        if (res.error() == GitLookupError::Fatal) {
            logger_->Emit(LogLevel::Error,
                          "Failed while retrieving subtree {} of commit {} "
                          "from repository {}",
                          subdir,
                          commit,
                          path.string());
            response->set_status(ServeCommitTreeResponse::INTERNAL_ERROR);
            return ::grpc::Status::OK;
        }
    }
    // commit not found
    response->set_status(ServeCommitTreeResponse::NOT_FOUND);
    return ::grpc::Status::OK;
}

auto SourceTreeService::SyncArchive(std::string const& tree_id,
                                    std::filesystem::path const& repo_path,
                                    bool sync_tree,
                                    ServeArchiveTreeResponse* response)
    -> ::grpc::Status {
    auto status = ServeArchiveTreeResponse::OK;
    if (sync_tree) {
        status = SyncGitEntryToCas<ObjectType::Tree, ServeArchiveTreeResponse>(
            tree_id, repo_path);
        if (status == ServeArchiveTreeResponse::OK) {
            status = SetDigestInResponse<ServeArchiveTreeResponse>(
                response, tree_id, /*is_tree=*/true, /*from_git=*/true);
        }
    }
    *(response->mutable_tree()) = tree_id;
    response->set_status(status);
    return ::grpc::Status::OK;
}

template <ObjectType kType, typename TResponse>
auto SourceTreeService::SyncGitEntryToCas(
    std::string const& object_hash,
    std::filesystem::path const& repo_path) const noexcept
    -> std::remove_cvref_t<decltype(TResponse::OK)> {
    // get gc locks for the local storages
    auto native_lock =
        GarbageCollector::SharedLock(*native_context_->storage_config);
    if (not native_lock) {
        logger_->Emit(LogLevel::Error, "Could not acquire gc SharedLock");
        return TResponse::INTERNAL_ERROR;
    }
    std::optional<LockFile> compat_lock = std::nullopt;
    if (compat_context_ != nullptr) {
        compat_lock =
            GarbageCollector::SharedLock(*compat_context_->storage_config);
        if (not compat_lock) {
            logger_->Emit(LogLevel::Error, "Could not acquire gc SharedLock");
            return TResponse::INTERNAL_ERROR;
        }
    }

    auto const hash_type =
        native_context_->storage_config->hash_function.GetType();
    if (IsTreeObject(kType) and not ProtocolTraits::IsTreeAllowed(hash_type)) {
        logger_->Emit(LogLevel::Error,
                      "Cannot sync tree {} from repository {} with "
                      "the remote in compatible mode",
                      object_hash,
                      repo_path.string());
        return TResponse::SYNC_ERROR;
    }

    auto repo = RepositoryConfig{};
    if (not repo.SetGitCAS(repo_path, native_context_->storage_config)) {
        logger_->Emit(
            LogLevel::Error, "Failed to SetGitCAS at {}", repo_path.string());
        return TResponse::INTERNAL_ERROR;
    }
    auto const digest = ArtifactDigestFactory::Create(
        hash_type, object_hash, 0, IsTreeObject(kType));
    if (not digest) {
        logger_->Emit(LogLevel::Error, "SyncGitEntryToCas: {}", digest.error());
        return TResponse::INTERNAL_ERROR;
    }

    auto const is_compat = compat_context_ != nullptr;
    auto git_api =
        MRGitApi{&repo,
                 native_context_->storage_config,
                 is_compat ? &*compat_context_->storage_config : nullptr,
                 is_compat ? &*apis_.local : nullptr};
    if (not git_api.RetrieveToCas(
            {Artifact::ObjectInfo{.digest = *digest, .type = kType}},
            *apis_.remote)) {
        logger_->Emit(LogLevel::Error,
                      "Failed to sync object {} from repository {}",
                      object_hash,
                      repo_path.string());
        return TResponse::SYNC_ERROR;
    }
    return TResponse::OK;
}

template <typename TResponse>
auto SourceTreeService::SetDigestInResponse(
    gsl::not_null<TResponse*> const& response,
    std::string const& object_hash,
    bool is_tree,
    bool from_git) const noexcept
    -> std::remove_cvref_t<decltype(TResponse::OK)> {
    // set digest in response
    auto native_digest = ArtifactDigestFactory::Create(
        native_context_->storage_config->hash_function.GetType(),
        object_hash,
        /*size is unknown*/ 0,
        is_tree);
    if (not native_digest) {
        logger_->Emit(LogLevel::Error,
                      "SetDigestInResponse: {}",
                      std::move(native_digest).error());
        return TResponse::INTERNAL_ERROR;
    }
    // in native mode, set the native digest in response
    if (ProtocolTraits::IsNative(apis_.remote->GetHashType())) {
        *(response->mutable_digest()) =
            ArtifactDigestFactory::ToBazel(*std::move(native_digest));
    }
    else {
        // in compatible mode, we need to respond with a compatible digest
        if (compat_context_ == nullptr) {
            // sanity check
            logger_->Emit(LogLevel::Error,
                          "Compatible storage not available as required");
            return TResponse::INTERNAL_ERROR;
        }

        // get gc locks for the local storages
        auto native_lock =
            GarbageCollector::SharedLock(*native_context_->storage_config);
        if (not native_lock) {
            logger_->Emit(LogLevel::Error, "Could not acquire gc SharedLock");
            return TResponse::INTERNAL_ERROR;
        }
        auto compat_lock =
            GarbageCollector::SharedLock(*compat_context_->storage_config);
        if (not compat_lock) {
            logger_->Emit(LogLevel::Error, "Could not acquire gc SharedLock");
            return TResponse::INTERNAL_ERROR;
        }

        // get the compatible digest from the mapping that was created during
        // upload from Git cache
        auto const cached_obj =
            RehashUtils::ReadRehashedDigest(*native_digest,
                                            *native_context_->storage_config,
                                            *compat_context_->storage_config,
                                            from_git);
        if (not cached_obj) {
            logger_->Emit(
                LogLevel::Error, "SetDigestInResponse: {}", cached_obj.error());
            return TResponse::INTERNAL_ERROR;
        }
        if (not *cached_obj) {
            logger_->Emit(
                LogLevel::Error,
                "Cached compatible object for native digest {} not found",
                native_digest->hash());
            return TResponse::INTERNAL_ERROR;
        }
        // set compatible digest in response
        *(response->mutable_digest()) =
            ArtifactDigestFactory::ToBazel(cached_obj->value().digest);
    }
    return TResponse::OK;
}

auto SourceTreeService::ResolveContentTree(
    std::string const& tree_id,
    std::filesystem::path const& repo_path,
    bool repo_is_git_cache,
    std::optional<PragmaSpecial> const& resolve_special,
    bool sync_tree,
    ServeArchiveTreeResponse* response) -> ::grpc::Status {
    if (resolve_special) {
        // get the resolved tree
        auto tree_id_file = StorageUtils::GetResolvedTreeIDFile(
            *native_context_->storage_config, tree_id, *resolve_special);
        if (FileSystemManager::Exists(tree_id_file)) {
            // read resolved tree id
            auto resolved_tree_id = FileSystemManager::ReadFile(tree_id_file);
            if (not resolved_tree_id) {
                logger_->Emit(LogLevel::Error,
                              "Failed to read resolved tree id from file {}",
                              tree_id_file.string());
                response->set_status(ServeArchiveTreeResponse::INTERNAL_ERROR);
                return ::grpc::Status::OK;
            }
            return SyncArchive(
                *resolved_tree_id, repo_path, sync_tree, response);
        }
        // resolve tree; target repository is always the Git cache
        auto target_cas =
            GitCAS::Open(native_context_->storage_config->GitRoot());
        if (not target_cas) {
            logger_->Emit(LogLevel::Error,
                          "Failed to open Git ODB at {}",
                          native_context_->storage_config->GitRoot().string());
            response->set_status(ServeArchiveTreeResponse::INTERNAL_ERROR);
            return ::grpc::Status::OK;
        }
        auto source_cas = target_cas;
        if (not repo_is_git_cache) {
            source_cas = GitCAS::Open(repo_path);
            if (not source_cas) {
                logger_->Emit(LogLevel::Error,
                              "Failed to open Git ODB at {}",
                              repo_path.string());
                response->set_status(ServeArchiveTreeResponse::INTERNAL_ERROR);
                return ::grpc::Status::OK;
            }
        }
        std::optional<ResolvedGitObject> resolved_tree = std::nullopt;
        bool failed{false};
        {
            TaskSystem ts{serve_config_.jobs};
            resolve_symlinks_map_.ConsumeAfterKeysReady(
                &ts,
                {GitObjectToResolve{tree_id,
                                    ".",
                                    *resolve_special,
                                    /*known_info=*/std::nullopt,
                                    source_cas,
                                    target_cas}},
                [&resolved_tree](auto hashes) { resolved_tree = *hashes[0]; },
                [logger = logger_, tree_id, &failed](auto const& msg,
                                                     bool fatal) {
                    logger->Emit(LogLevel::Error,
                                 "While resolving tree {}:\n{}",
                                 tree_id,
                                 msg);
                    failed = failed or fatal;
                });
        }
        if (failed) {
            logger_->Emit(
                LogLevel::Error, "Failed to resolve tree id {}", tree_id);
            response->set_status(ServeArchiveTreeResponse::RESOLVE_ERROR);
            return ::grpc::Status::OK;
        }
        // check if we have a value
        if (not resolved_tree) {
            // check for cycles
            if (auto error = DetectAndReportCycle(
                    fmt::format("resolving symlinks in tree {}", tree_id),
                    resolve_symlinks_map_,
                    kGitObjectToResolvePrinter)) {
                logger_->Emit(LogLevel::Error, *error);
                response->set_status(ServeArchiveTreeResponse::RESOLVE_ERROR);
                return ::grpc::Status::OK;
            }
            logger_->Emit(LogLevel::Error,
                          "Unknown error while resolving tree id {}",
                          tree_id);
            response->set_status(ServeArchiveTreeResponse::INTERNAL_ERROR);
            return ::grpc::Status::OK;
        }
        // keep tree alive in the Git cache via a tagged commit
        auto wrapped_logger = std::make_shared<GitRepo::anon_logger_t>(
            [logger = logger_,
             storage_config = native_context_->storage_config,
             resolved_tree](auto const& msg, bool fatal) {
                if (fatal) {
                    logger->Emit(LogLevel::Error,
                                 "While keeping tree {} in repository {}:\n{}",
                                 resolved_tree->id,
                                 storage_config->GitRoot().string(),
                                 msg);
                }
            });
        {
            // this is a non-thread-safe Git operation, so it must be guarded!
            std::unique_lock slock{*lock_};
            // open real repository at Git CAS location
            auto git_repo =
                GitRepo::Open(native_context_->storage_config->GitRoot());
            if (not git_repo) {
                logger_->Emit(
                    LogLevel::Error,
                    "Failed to open Git CAS repository {}",
                    native_context_->storage_config->GitRoot().string());
                response->set_status(ServeArchiveTreeResponse::RESOLVE_ERROR);
                return ::grpc::Status::OK;
            }
            // Important: message must be consistent with just-mr!
            if (not git_repo->KeepTree(resolved_tree->id,
                                       "Keep referenced tree alive",  // message
                                       wrapped_logger)) {
                response->set_status(ServeArchiveTreeResponse::RESOLVE_ERROR);
                return ::grpc::Status::OK;
            }
        }
        // cache the resolved tree association
        if (not StorageUtils::WriteTreeIDFile(tree_id_file,
                                              resolved_tree->id)) {
            logger_->Emit(LogLevel::Error,
                          "Failed to write resolved tree id to file {}",
                          tree_id_file.string());
            response->set_status(ServeArchiveTreeResponse::RESOLVE_ERROR);
            return ::grpc::Status::OK;
        }
        return SyncArchive(resolved_tree->id, repo_path, sync_tree, response);
    }
    // if no special handling of symlinks, use given tree as-is
    return SyncArchive(tree_id, repo_path, sync_tree, response);
}

auto SourceTreeService::ArchiveImportToGit(
    std::filesystem::path const& unpack_path,
    std::filesystem::path const& archive_tree_id_file,
    std::string const& content,
    std::string const& archive_type,
    std::string const& subdir,
    std::optional<PragmaSpecial> const& resolve_special,
    bool sync_tree,
    ServeArchiveTreeResponse* response) -> ::grpc::Status {
    // Important: commit message must match that in just-mr!
    auto res = GitRepo::ImportToGit(
        *native_context_->storage_config,
        unpack_path,
        /*commit_message=*/
        fmt::format("Content of {} {}", archive_type, content),
        lock_);
    if (not res) {
        // report the error
        logger_->Emit(LogLevel::Error, "{}", res.error());
        response->set_status(ServeArchiveTreeResponse::INTERNAL_ERROR);
        return ::grpc::Status::OK;
    }
    auto const& tree_id = *res;
    // write to tree id file
    if (not StorageUtils::WriteTreeIDFile(archive_tree_id_file, tree_id)) {
        logger_->Emit(LogLevel::Error,
                      "Failed to write tree id to file {}",
                      archive_tree_id_file.string());
        response->set_status(ServeArchiveTreeResponse::INTERNAL_ERROR);
        return ::grpc::Status::OK;
    }
    // open the Git CAS repo
    auto just_git_cas =
        GitCAS::Open(native_context_->storage_config->GitRoot());
    if (not just_git_cas) {
        logger_->Emit(LogLevel::Error,
                      "Failed to open Git ODB at {}",
                      native_context_->storage_config->GitRoot().string());
        response->set_status(ServeArchiveTreeResponse::INTERNAL_ERROR);
        return ::grpc::Status::OK;
    }
    auto just_git_repo = GitRepo::Open(just_git_cas);
    if (not just_git_repo) {
        logger_->Emit(LogLevel::Error,
                      "Failed to open Git repository {}",
                      native_context_->storage_config->GitRoot().string());
        response->set_status(ServeArchiveTreeResponse::INTERNAL_ERROR);
        return ::grpc::Status::OK;
    }
    // wrap logger for GitRepo call
    std::string err;
    auto wrapped_logger = std::make_shared<GitRepo::anon_logger_t>(
        [&err, subdir, tree_id](auto const& msg, bool fatal) {
            if (fatal) {
                err = fmt::format("While retrieving subtree {} of tree {}:\n{}",
                                  subdir,
                                  tree_id,
                                  msg);
            }
        });
    // get the subtree id; this is thread-safe
    auto subtree_id =
        just_git_repo->GetSubtreeFromTree(tree_id, subdir, wrapped_logger);
    if (not subtree_id) {
        logger_->Emit(LogLevel::Error, "{}", err);
        response->set_status(ServeArchiveTreeResponse::INTERNAL_ERROR);
        return ::grpc::Status::OK;
    }
    return ResolveContentTree(*subtree_id,
                              native_context_->storage_config->GitRoot(),
                              /*repo_is_git_cache=*/true,
                              resolve_special,
                              sync_tree,
                              response);
}

auto SourceTreeService::ServeArchiveTree(
    ::grpc::ServerContext* /* context */,
    const ::justbuild::just_serve::ServeArchiveTreeRequest* request,
    ServeArchiveTreeResponse* response) -> ::grpc::Status {
    logger_->Emit(
        LogLevel::Debug, "ServeArchiveTree({}, ...)", request->content());
    // get gc lock for Git cache
    auto repo_lock = RepositoryGarbageCollector::SharedLock(
        *native_context_->storage_config);
    if (not repo_lock) {
        logger_->Emit(LogLevel::Error, "Could not acquire repo gc SharedLock");
        response->set_status(ServeArchiveTreeResponse::INTERNAL_ERROR);
        return ::grpc::Status::OK;
    }
    // ensure Git cache exists
    if (auto done = EnsureGitCacheRoot(); not done) {
        logger_->Emit(LogLevel::Error, std::move(done).error());
        response->set_status(ServeArchiveTreeResponse::INTERNAL_ERROR);
        return ::grpc::Status::OK;
    }

    auto const& content{request->content()};
    auto archive_type = ArchiveTypeToString(request->archive_type());
    auto const& subdir{request->subdir()};
    auto resolve_special =
        SymlinksResolveToPragmaSpecial(request->resolve_symlinks());

    // check for archive_tree_id_file
    auto archive_tree_id_file = StorageUtils::GetArchiveTreeIDFile(
        *native_context_->storage_config, archive_type, content);
    if (FileSystemManager::Exists(archive_tree_id_file)) {
        // read archive_tree_id from file tree_id_file
        auto archive_tree_id =
            FileSystemManager::ReadFile(archive_tree_id_file);
        if (not archive_tree_id) {
            logger_->Emit(LogLevel::Error,
                          "Failed to read tree id from file {}",
                          archive_tree_id_file.string());
            response->set_status(ServeArchiveTreeResponse::INTERNAL_ERROR);
            return ::grpc::Status::OK;
        }
        // check local build root Git cache
        auto res =
            GetSubtreeFromTree(native_context_->storage_config->GitRoot(),
                               *archive_tree_id,
                               subdir,
                               logger_);
        if (res) {
            return ResolveContentTree(
                *res,  // tree_id
                native_context_->storage_config->GitRoot(),
                /*repo_is_git_cache=*/true,
                resolve_special,
                request->sync_tree(),
                response);
        }
        // check for fatal error
        if (res.error() == GitLookupError::Fatal) {
            logger_->Emit(LogLevel::Error,
                          "Failed to open repository {}",
                          native_context_->storage_config->GitRoot().string());
            response->set_status(ServeArchiveTreeResponse::INTERNAL_ERROR);
            return ::grpc::Status::OK;
        }
        // check known repositories
        for (auto const& path : serve_config_.known_repositories) {
            auto res =
                GetSubtreeFromTree(path, *archive_tree_id, subdir, logger_);
            if (res) {
                return ResolveContentTree(*res,  // tree_id
                                          path,
                                          /*repo_is_git_cache=*/false,
                                          resolve_special,
                                          request->sync_tree(),
                                          response);
            }
            // check for fatal error
            if (res.error() == GitLookupError::Fatal) {
                logger_->Emit(LogLevel::Error,
                              "Failed to open repository {}",
                              path.string());
                response->set_status(ServeArchiveTreeResponse::INTERNAL_ERROR);
                return ::grpc::Status::OK;
            }
        }
        // report error for missing tree specified in id file
        logger_->Emit(LogLevel::Error,
                      "Failed while retrieving subtree {} of known tree {}",
                      subdir,
                      *archive_tree_id);
        response->set_status(ServeArchiveTreeResponse::INTERNAL_ERROR);
        return ::grpc::Status::OK;
    }
    // acquire lock for native CAS
    auto lock = GarbageCollector::SharedLock(*native_context_->storage_config);
    if (not lock) {
        logger_->Emit(LogLevel::Error, "Could not acquire gc SharedLock");
        response->set_status(ServeArchiveTreeResponse::INTERNAL_ERROR);
        return ::grpc::Status::OK;
    }
    // check if content is in native local CAS already
    auto const digest = ArtifactDigestFactory::Create(
        native_context_->storage_config->hash_function.GetType(),
        content,
        0,
        /*is_tree=*/false);
    auto const& native_cas = native_context_->storage->CAS();
    auto content_cas_path =
        digest ? native_cas.BlobPath(*digest, /*is_executable=*/false)
               : std::nullopt;
    if (not content_cas_path) {
        // check if content blob is in Git cache
        auto res = GetBlobFromRepo(
            native_context_->storage_config->GitRoot(), content, logger_);
        if (res) {
            // add to native CAS
            content_cas_path =
                StorageUtils::AddToCAS(*native_context_->storage, *res);
        }
        if (res.error() == GitLookupError::Fatal) {
            logger_->Emit(
                LogLevel::Error,
                "Failed while trying to retrieve content {} from repository {}",
                content,
                native_context_->storage_config->GitRoot().string());
            response->set_status(ServeArchiveTreeResponse::INTERNAL_ERROR);
            return ::grpc::Status::OK;
        }
    }
    if (not content_cas_path) {
        // check if content blob is in a known repository
        for (auto const& path : serve_config_.known_repositories) {
            auto res = GetBlobFromRepo(path, content, logger_);
            if (res) {
                // add to native CAS
                content_cas_path =
                    StorageUtils::AddToCAS(*native_context_->storage, *res);
                if (content_cas_path) {
                    break;
                }
            }
            if (res.error() == GitLookupError::Fatal) {
                logger_->Emit(LogLevel::Error,
                              "Failed while trying to retrieve content {} from "
                              "repository {}",
                              content,
                              path.string());
                response->set_status(ServeArchiveTreeResponse::INTERNAL_ERROR);
                return ::grpc::Status::OK;
            }
        }
    }
    if (digest and not content_cas_path) {
        // try to retrieve it from remote CAS
        if (not(apis_.remote->IsAvailable(*digest) and
                apis_.remote->RetrieveToCas(
                    {Artifact::ObjectInfo{.digest = *digest,
                                          .type = ObjectType::File}},
                    *apis_.local))) {
            // content could not be found
            response->set_status(ServeArchiveTreeResponse::NOT_FOUND);
            return ::grpc::Status::OK;
        }
        // content should now be in native CAS
        content_cas_path =
            native_cas.BlobPath(*digest, /*is_executable=*/false);
        if (not content_cas_path) {
            logger_->Emit(
                LogLevel::Error,
                "Retrieving content {} from native CAS failed unexpectedly",
                content);
            response->set_status(ServeArchiveTreeResponse::INTERNAL_ERROR);
            return ::grpc::Status::OK;
        }
    }
    // extract archive
    auto tmp_dir =
        native_context_->storage_config->CreateTypedTmpDir(archive_type);
    if (not tmp_dir) {
        logger_->Emit(
            LogLevel::Error,
            "Failed to create tmp path for {} archive with content {}",
            archive_type,
            content);
        response->set_status(ServeArchiveTreeResponse::INTERNAL_ERROR);
        return ::grpc::Status::OK;
    }
    auto res =
        ExtractArchive(*content_cas_path, archive_type, tmp_dir->GetPath());
    if (res != std::nullopt) {
        logger_->Emit(LogLevel::Error,
                      "Failed to extract archive {} from native CAS:\n{}",
                      content_cas_path->string(),
                      *res);
        response->set_status(ServeArchiveTreeResponse::UNPACK_ERROR);
        return ::grpc::Status::OK;
    }
    // import to git
    return ArchiveImportToGit(tmp_dir->GetPath(),
                              archive_tree_id_file,
                              content,
                              archive_type,
                              subdir,
                              resolve_special,
                              request->sync_tree(),
                              response);
}

auto SourceTreeService::DistdirImportToGit(
    std::string const& distdir_tree_id,
    std::string const& content_id,
    std::unordered_map<std::string, std::pair<std::string, bool>> const&
        content_list,
    bool sync_tree,
    ServeDistdirTreeResponse* response) -> ::grpc::Status {
    // create tmp directory for the distdir
    auto distdir_tmp_dir =
        native_context_->storage_config->CreateTypedTmpDir("distdir");
    if (not distdir_tmp_dir) {
        logger_->Emit(LogLevel::Error,
                      "Failed to create tmp path for distdir target {}",
                      content_id);
        response->set_status(ServeDistdirTreeResponse::INTERNAL_ERROR);
        return ::grpc::Status::OK;
    }
    auto const& tmp_path = distdir_tmp_dir->GetPath();
    // link the native CAS blobs into the tmp dir
    auto const& native_cas = native_context_->storage->CAS();
    if (not std::all_of(
            content_list.begin(),
            content_list.end(),
            [&native_cas, tmp_path](auto const& kv) {
                auto const digest = ArtifactDigestFactory::Create(
                    native_cas.GetHashFunction().GetType(),
                    kv.second.first,
                    0,
                    /*is_tree=*/false);
                if (not digest) {
                    return false;
                }
                auto content_path =
                    native_cas.BlobPath(*digest, kv.second.second);
                if (content_path) {
                    return FileSystemManager::CreateFileHardlink(
                               *content_path,  // from: cas_path/content_id
                               tmp_path / kv.first)  // to: tmp_path/name
                        .has_value();
                }
                return false;
            })) {
        logger_->Emit(LogLevel::Error,
                      "Failed to create links to native CAS content {}",
                      content_id);
        response->set_status(ServeDistdirTreeResponse::INTERNAL_ERROR);
        return ::grpc::Status::OK;
    }
    // Important: commit message must match that in just-mr!
    auto res = GitRepo::ImportToGit(
        *native_context_->storage_config,
        tmp_path,
        /*commit_message=*/fmt::format("Content of distdir {}", content_id),
        lock_);
    if (not res) {
        // report the error
        logger_->Emit(LogLevel::Error, "{}", res.error());
        response->set_status(ServeDistdirTreeResponse::INTERNAL_ERROR);
        return ::grpc::Status::OK;
    }
    auto tree_id = *std::move(res);
    // check the committed tree matches what we expect
    if (tree_id != distdir_tree_id) {
        // something is very wrong...
        logger_->Emit(LogLevel::Error,
                      "Unexpected mismatch for tree of committed "
                      "distdir:\nexpected {} but got {}",
                      distdir_tree_id,
                      tree_id);
        response->set_status(ServeDistdirTreeResponse::INTERNAL_ERROR);
        return ::grpc::Status::OK;
    }
    // if asked, sync tree (and implicitly all blobs) with remote CAS
    auto status = ServeDistdirTreeResponse::OK;
    if (sync_tree) {
        status = SyncGitEntryToCas<ObjectType::Tree, ServeDistdirTreeResponse>(
            tree_id, native_context_->storage_config->GitRoot());
        if (status == ServeDistdirTreeResponse::OK) {
            status = SetDigestInResponse<ServeDistdirTreeResponse>(
                response, tree_id, /*is_tree=*/true, /*from_git=*/true);
        }
    }
    // set response on success
    *(response->mutable_tree()) = std::move(tree_id);
    response->set_status(status);
    return ::grpc::Status::OK;
}

auto SourceTreeService::ServeDistdirTree(
    ::grpc::ServerContext* /* context */,
    const ::justbuild::just_serve::ServeDistdirTreeRequest* request,
    ServeDistdirTreeResponse* response) -> ::grpc::Status {
    logger_->Emit(LogLevel::Debug, "ServeDistdirTree(...)");
    // get gc lock for Git cache
    auto repo_lock = RepositoryGarbageCollector::SharedLock(
        *native_context_->storage_config);
    if (not repo_lock) {
        logger_->Emit(LogLevel::Error, "Could not acquire repo gc SharedLock");
        response->set_status(ServeDistdirTreeResponse::INTERNAL_ERROR);
        return ::grpc::Status::OK;
    }
    // ensure Git cache exists
    if (auto done = EnsureGitCacheRoot(); not done) {
        logger_->Emit(LogLevel::Error, std::move(done).error());
        response->set_status(ServeDistdirTreeResponse::INTERNAL_ERROR);
        return ::grpc::Status::OK;
    }
    // acquire lock for native CAS
    auto lock = GarbageCollector::SharedLock(*native_context_->storage_config);
    if (not lock) {
        logger_->Emit(LogLevel::Error, "Could not acquire gc SharedLock");
        response->set_status(ServeDistdirTreeResponse::INTERNAL_ERROR);
        return ::grpc::Status::OK;
    }
    // create in-memory tree from distfiles map
    GitRepo::tree_entries_t entries{};
    entries.reserve(request->distfiles().size());

    auto const& native_cas = native_context_->storage->CAS();
    std::unordered_map<std::string, std::pair<std::string, bool>>
        content_list{};
    content_list.reserve(request->distfiles().size());

    bool const is_native =
        ProtocolTraits::IsNative(apis_.remote->GetHashType());
    for (auto const& kv : request->distfiles()) {
        bool blob_found{};
        std::string blob_digest;  // The digest of the requested distfile, taken
                                  // by the hash applicable for our CAS; this
                                  // might be different from content, if our CAS
                                  // is not based on git blob identifiers
                                  // (i.e., if we're not in native mode).
        auto const& content = kv.content();
        // check content blob is known
        // first check the native local CAS itself, provided it uses the same
        // type of identifier
        auto const digest = ArtifactDigestFactory::Create(
            native_context_->storage_config->hash_function.GetType(),
            content,
            0,
            /*is_tree=*/false);

        if (is_native) {
            blob_found =
                digest and native_cas.BlobPath(*digest, kv.executable());
        }
        if (blob_found) {
            blob_digest = content;
        }
        else {
            // check local Git cache
            auto res = GetBlobFromRepo(
                native_context_->storage_config->GitRoot(), content, logger_);
            if (res) {
                // add content to native local CAS
                auto stored_blob = native_cas.StoreBlob(*res, kv.executable());
                if (not stored_blob) {
                    logger_->Emit(LogLevel::Error,
                                  "Failed to store content {} from local Git "
                                  "cache to native local CAS",
                                  content);
                    response->set_status(
                        ServeDistdirTreeResponse::INTERNAL_ERROR);
                    return ::grpc::Status::OK;
                }
                blob_found = true;
                blob_digest = stored_blob->hash();
            }
            else {
                if (res.error() == GitLookupError::Fatal) {
                    logger_->Emit(
                        LogLevel::Error,
                        "Failed while trying to retrieve content {} from "
                        "repository {}",
                        content,
                        native_context_->storage_config->GitRoot().string());
                    response->set_status(
                        ServeDistdirTreeResponse::INTERNAL_ERROR);
                    return ::grpc::Status::OK;
                }
                // check known repositories
                for (auto const& path : serve_config_.known_repositories) {
                    auto res = GetBlobFromRepo(path, content, logger_);
                    if (res) {
                        // add content to native local CAS
                        auto stored_blob =
                            native_cas.StoreBlob(*res, kv.executable());
                        if (not stored_blob) {
                            logger_->Emit(
                                LogLevel::Error,
                                "Failed to store content {} from known "
                                "repository {} to native local CAS",
                                path.string(),
                                content);
                            response->set_status(
                                ServeDistdirTreeResponse::INTERNAL_ERROR);
                            return ::grpc::Status::OK;
                        }
                        blob_found = true;
                        blob_digest = stored_blob->hash();
                        break;
                    }
                    if (res.error() == GitLookupError::Fatal) {
                        logger_->Emit(
                            LogLevel::Error,
                            "Failed while trying to retrieve content {} from "
                            "repository {}",
                            content,
                            path.string());
                        response->set_status(
                            ServeDistdirTreeResponse::INTERNAL_ERROR);
                        return ::grpc::Status::OK;
                    }
                }
                if (not blob_found) {
                    // check remote CAS
                    if (is_native and digest and
                        apis_.remote->IsAvailable(*digest)) {
                        // retrieve content to native local CAS
                        if (not apis_.remote->RetrieveToCas(
                                {Artifact::ObjectInfo{
                                    .digest = *digest,
                                    .type = kv.executable()
                                                ? ObjectType::Executable
                                                : ObjectType::File}},
                                *apis_.local)) {
                            logger_->Emit(LogLevel::Error,
                                          "Failed to retrieve content {} from "
                                          "remote to native local CAS",
                                          content);
                            response->set_status(
                                ServeDistdirTreeResponse::INTERNAL_ERROR);
                            return ::grpc::Status::OK;
                        }
                        blob_found = true;
                        blob_digest = content;
                    }
                }
            }
        }
        // error out if blob is not known
        if (not blob_found) {
            logger_->Emit(LogLevel::Error, "Content {} is not known", content);
            response->set_status(ServeDistdirTreeResponse::NOT_FOUND);
            return ::grpc::Status::OK;
        }
        // store content blob to the entries list, using the expected raw id
        if (auto raw_id = FromHexString(content)) {
            entries[*raw_id].emplace_back(
                kv.name(),
                kv.executable() ? ObjectType::Executable : ObjectType::File);
        }
        else {
            logger_->Emit(
                LogLevel::Error,
                "Conversion of content {} to raw id failed unexpectedly",
                content);
            response->set_status(ServeDistdirTreeResponse::INTERNAL_ERROR);
            return ::grpc::Status::OK;
        }
        // store to content_list for import-to-git hardlinking
        content_list.insert_or_assign(
            kv.name(), std::make_pair(blob_digest, kv.executable()));
    }
    // get hash of distdir content; this must match with that in just-mr
    auto content_id = HashFunction{HashFunction::Type::GitSHA1}
                          .HashBlobData(nlohmann::json(content_list).dump())
                          .HexString();
    // create in-memory tree of the distdir, now that we know we have all blobs
    auto tree = GitRepo::CreateShallowTree(entries);
    if (not tree) {
        logger_->Emit(LogLevel::Error,
                      "Failed to construct in-memory tree for distdir");
        response->set_status(ServeDistdirTreeResponse::INTERNAL_ERROR);
        return ::grpc::Status::OK;
    }
    // get hash from raw_id
    auto tree_id = ToHexString(tree->first);
    // add tree to native local CAS
    if (not native_cas.StoreTree(tree->second)) {
        logger_->Emit(LogLevel::Error,
                      "Failed to store distdir tree {} to native local CAS",
                      tree_id);
        response->set_status(ServeDistdirTreeResponse::INTERNAL_ERROR);
        return ::grpc::Status::OK;
    }
    // check if tree is already in Git cache
    auto has_tree = GitRepo::IsTreeInRepo(
        native_context_->storage_config->GitRoot(), tree_id);
    if (not has_tree) {
        logger_->Emit(LogLevel::Error,
                      "Failed while checking for tree {} in repository {}:\n{}",
                      tree_id,
                      native_context_->storage_config->GitRoot().string(),
                      std::move(has_tree).error());
        response->set_status(ServeDistdirTreeResponse::INTERNAL_ERROR);
        return ::grpc::Status::OK;
    }
    if (*has_tree) {
        // if asked, sync tree and all blobs with remote CAS
        auto status = ServeDistdirTreeResponse::OK;
        if (request->sync_tree()) {
            status =
                SyncGitEntryToCas<ObjectType::Tree, ServeDistdirTreeResponse>(
                    tree_id, native_context_->storage_config->GitRoot());
            if (status == ServeDistdirTreeResponse::OK) {
                status = SetDigestInResponse<ServeDistdirTreeResponse>(
                    response, tree_id, /*is_tree=*/true, /*from_git=*/true);
            }
        }
        // set response on success
        *(response->mutable_tree()) = std::move(tree_id);
        response->set_status(status);
        return ::grpc::Status::OK;
    }
    // check if tree is in a known repository
    for (auto const& path : serve_config_.known_repositories) {
        auto has_tree = GitRepo::IsTreeInRepo(path, tree_id);
        if (not has_tree) {
            logger_->Emit(
                LogLevel::Error,
                "Failed while checking for tree {} in repository {}:\n{}",
                tree_id,
                path.string(),
                *std::move(has_tree));
            response->set_status(ServeDistdirTreeResponse::INTERNAL_ERROR);
            return ::grpc::Status::OK;
        }
        if (*has_tree) {
            // if asked, sync tree and all blobs with remote CAS
            auto status = ServeDistdirTreeResponse::OK;
            if (request->sync_tree()) {
                status =
                    SyncGitEntryToCas<ObjectType::Tree,
                                      ServeDistdirTreeResponse>(tree_id, path);
                if (status == ServeDistdirTreeResponse::OK) {
                    status = SetDigestInResponse<ServeDistdirTreeResponse>(
                        response, tree_id, /*is_tree=*/true, /*from_git=*/true);
                }
            }
            // set response on success
            *(response->mutable_tree()) = std::move(tree_id);
            response->set_status(status);
            return ::grpc::Status::OK;
        }
    }
    // otherwise, we import the tree from native local CAS ourselves
    return DistdirImportToGit(
        tree_id, content_id, content_list, request->sync_tree(), response);
}

auto SourceTreeService::ServeContent(
    ::grpc::ServerContext* /* context */,
    const ::justbuild::just_serve::ServeContentRequest* request,
    ServeContentResponse* response) -> ::grpc::Status {
    auto const& content{request->content()};
    logger_->Emit(LogLevel::Debug, "ServeContent({})", content);
    // get gc lock for Git cache
    auto repo_lock = RepositoryGarbageCollector::SharedLock(
        *native_context_->storage_config);
    if (not repo_lock) {
        logger_->Emit(LogLevel::Error, "Could not acquire repo gc SharedLock");
        response->set_status(ServeContentResponse::INTERNAL_ERROR);
        return ::grpc::Status::OK;
    }
    // ensure Git cache exists
    if (auto done = EnsureGitCacheRoot(); not done) {
        logger_->Emit(LogLevel::Error, std::move(done).error());
        response->set_status(ServeContentResponse::INTERNAL_ERROR);
        return ::grpc::Status::OK;
    }
    // get gc lock for native storage
    auto lock = GarbageCollector::SharedLock(*native_context_->storage_config);
    if (not lock) {
        logger_->Emit(LogLevel::Error, "Could not acquire gc SharedLock");
        response->set_status(ServeContentResponse::INTERNAL_ERROR);
        return ::grpc::Status::OK;
    }

    // check if content blob is in Git cache
    auto res = GetBlobFromRepo(
        native_context_->storage_config->GitRoot(), content, logger_);
    if (res) {
        auto status = SyncGitEntryToCas<ObjectType::File, ServeContentResponse>(
            content, native_context_->storage_config->GitRoot());
        if (status == ServeContentResponse::OK) {
            status = SetDigestInResponse<ServeContentResponse>(
                response, content, /*is_tree=*/false, /*from_git=*/true);
        }
        response->set_status(status);
        return ::grpc::Status::OK;
    }
    if (res.error() == GitLookupError::Fatal) {
        logger_->Emit(LogLevel::Error,
                      "Failed while checking for content {} in repository {}",
                      content,
                      native_context_->storage_config->GitRoot().string());
        response->set_status(ServeContentResponse::INTERNAL_ERROR);
        return ::grpc::Status::OK;
    }
    // check if content blob is in a known repository
    for (auto const& path : serve_config_.known_repositories) {
        auto res = GetBlobFromRepo(path, content, logger_);
        if (res) {
            // upload blob to remote CAS
            auto status =
                SyncGitEntryToCas<ObjectType::File, ServeContentResponse>(
                    content, path);
            if (status == ServeContentResponse::OK) {
                status = SetDigestInResponse<ServeContentResponse>(
                    response, content, /*is_tree=*/false, /*from_git=*/true);
            }
            response->set_status(status);
            return ::grpc::Status::OK;
        }
        if (res.error() == GitLookupError::Fatal) {
            auto str = fmt::format(
                "Failed while checking for content {} in repository {}",
                content,
                path.string());
            logger_->Emit(LogLevel::Error, str);
            response->set_status(ServeContentResponse::INTERNAL_ERROR);
            return ::grpc::Status::OK;
        }
    }

    // check also in the native local CAS
    auto const native_digest = ArtifactDigestFactory::Create(
        native_context_->storage_config->hash_function.GetType(),
        content,
        /*size is unknown*/ 0,
        /*is_tree=*/false);
    if (not native_digest) {
        logger_->Emit(LogLevel::Error, "Failed to create digest object");
        response->set_status(ServeContentResponse::INTERNAL_ERROR);
        return ::grpc::Status::OK;
    }
    if (apis_.local->IsAvailable(*native_digest)) {
        // upload blob to remote CAS
        if (not apis_.local->RetrieveToCas(
                {Artifact::ObjectInfo{.digest = *native_digest,
                                      .type = ObjectType::File}},
                *apis_.remote)) {
            logger_->Emit(LogLevel::Error,
                          "Failed to sync content {} from local native CAS",
                          content);
            response->set_status(ServeContentResponse::SYNC_ERROR);
            return ::grpc::Status::OK;
        }
        auto const status = SetDigestInResponse<ServeContentResponse>(
            response, content, /*is_tree=*/false, /*from_git=*/false);
        response->set_status(status);
        return ::grpc::Status::OK;
    }
    // content blob not known
    response->set_status(ServeContentResponse::NOT_FOUND);
    return ::grpc::Status::OK;
}

auto SourceTreeService::ServeTree(
    ::grpc::ServerContext* /* context */,
    const ::justbuild::just_serve::ServeTreeRequest* request,
    ServeTreeResponse* response) -> ::grpc::Status {
    auto const& tree_id{request->tree()};
    // get gc lock for Git cache
    auto repo_lock = RepositoryGarbageCollector::SharedLock(
        *native_context_->storage_config);
    if (not repo_lock) {
        logger_->Emit(LogLevel::Error, "Could not acquire repo gc SharedLock");
        response->set_status(ServeTreeResponse::INTERNAL_ERROR);
        return ::grpc::Status::OK;
    }
    // ensure Git cache exists
    if (auto done = EnsureGitCacheRoot(); not done) {
        logger_->Emit(LogLevel::Error, std::move(done).error());
        response->set_status(ServeTreeResponse::INTERNAL_ERROR);
        return ::grpc::Status::OK;
    }
    // get gc lock for native storage
    auto lock = GarbageCollector::SharedLock(*native_context_->storage_config);
    if (not lock) {
        logger_->Emit(LogLevel::Error, "Could not acquire gc SharedLock");
        response->set_status(ServeTreeResponse::INTERNAL_ERROR);
        return ::grpc::Status::OK;
    }

    // check if tree is in Git cache
    auto has_tree = GitRepo::IsTreeInRepo(
        native_context_->storage_config->GitRoot(), tree_id);
    if (not has_tree) {
        logger_->Emit(LogLevel::Error,
                      "Failed while checking for tree {} in repository {}:\n{}",
                      tree_id,
                      native_context_->storage_config->GitRoot().string(),
                      std::move(has_tree).error());
        response->set_status(ServeTreeResponse::INTERNAL_ERROR);
        return ::grpc::Status::OK;
    }
    if (*has_tree) {
        // upload tree to remote CAS
        auto status = SyncGitEntryToCas<ObjectType::Tree, ServeTreeResponse>(
            tree_id, native_context_->storage_config->GitRoot());
        if (status == ServeTreeResponse::OK) {
            status = SetDigestInResponse<ServeTreeResponse>(
                response, tree_id, /*is_tree=*/true, /*from_git=*/true);
        }
        response->set_status(status);
        return ::grpc::Status::OK;
    }
    // check if tree is in a known repository
    for (auto const& path : serve_config_.known_repositories) {
        auto has_tree = GitRepo::IsTreeInRepo(path, tree_id);
        if (not has_tree) {
            logger_->Emit(
                LogLevel::Error,
                "Failed while checking for tree {} in repository {}:\n{}",
                tree_id,
                path.string(),
                std::move(has_tree).error());
            response->set_status(ServeTreeResponse::INTERNAL_ERROR);
            return ::grpc::Status::OK;
        }
        if (*has_tree) {
            // upload blob to remote CAS
            auto status =
                SyncGitEntryToCas<ObjectType::Tree, ServeTreeResponse>(tree_id,
                                                                       path);
            if (status == ServeTreeResponse::OK) {
                status = SetDigestInResponse<ServeTreeResponse>(
                    response, tree_id, /*is_tree=*/true, /*from_git=*/true);
            }
            response->set_status(status);
            return ::grpc::Status::OK;
        }
    }

    // check also in the native local CAS
    auto const native_digest = ArtifactDigestFactory::Create(
        native_context_->storage_config->hash_function.GetType(),
        tree_id,
        /*size is unknown*/ 0,
        /*is_tree=*/true);
    if (not native_digest) {
        logger_->Emit(LogLevel::Error, "Failed to create digest object");
        response->set_status(ServeTreeResponse::INTERNAL_ERROR);
        return ::grpc::Status::OK;
    }
    if (apis_.local->IsAvailable(*native_digest)) {
        // upload tree to remote CAS
        if (not apis_.local->RetrieveToCas(
                {Artifact::ObjectInfo{.digest = *native_digest,
                                      .type = ObjectType::Tree}},
                *apis_.remote)) {
            logger_->Emit(LogLevel::Error,
                          "Failed to sync tree {} from native local CAS",
                          tree_id);
            response->set_status(ServeTreeResponse::SYNC_ERROR);
            return ::grpc::Status::OK;
        }
        auto const status = SetDigestInResponse<ServeTreeResponse>(
            response, tree_id, /*is_tree=*/true, /*from_git=*/false);
        response->set_status(status);
        return ::grpc::Status::OK;
    }
    // tree not known
    response->set_status(ServeTreeResponse::NOT_FOUND);
    return ::grpc::Status::OK;
}

auto SourceTreeService::CheckRootTree(
    ::grpc::ServerContext* /* context */,
    const ::justbuild::just_serve::CheckRootTreeRequest* request,
    CheckRootTreeResponse* response) -> ::grpc::Status {
    auto const& tree_id{request->tree()};
    logger_->Emit(LogLevel::Debug, "CheckRootTree({})", tree_id);
    // get gc lock for Git cache
    auto repo_lock = RepositoryGarbageCollector::SharedLock(
        *native_context_->storage_config);
    if (not repo_lock) {
        logger_->Emit(LogLevel::Error, "Could not acquire repo gc SharedLock");
        response->set_status(CheckRootTreeResponse::INTERNAL_ERROR);
        return ::grpc::Status::OK;
    }
    // ensure Git cache exists
    if (auto done = EnsureGitCacheRoot(); not done) {
        logger_->Emit(LogLevel::Error, std::move(done).error());
        response->set_status(CheckRootTreeResponse::INTERNAL_ERROR);
        return ::grpc::Status::OK;
    }
    // get gc lock for native storage
    auto lock = GarbageCollector::SharedLock(*native_context_->storage_config);
    if (not lock) {
        logger_->Emit(LogLevel::Error, "Could not acquire gc SharedLock");
        response->set_status(CheckRootTreeResponse::INTERNAL_ERROR);
        return ::grpc::Status::OK;
    }

    // check first in the Git cache
    auto has_tree = GitRepo::IsTreeInRepo(
        native_context_->storage_config->GitRoot(), tree_id);
    if (not has_tree) {
        logger_->Emit(LogLevel::Error,
                      "Failed while checking for tree {} in repository {}:\n{}",
                      tree_id,
                      native_context_->storage_config->GitRoot().string(),
                      std::move(has_tree).error());
        response->set_status(CheckRootTreeResponse::INTERNAL_ERROR);
        return ::grpc::Status::OK;
    }
    if (*has_tree) {
        // success!
        response->set_status(CheckRootTreeResponse::OK);
        return ::grpc::Status::OK;
    }
    // check if tree is in a known repository
    for (auto const& path : serve_config_.known_repositories) {
        auto has_tree = GitRepo::IsTreeInRepo(path, tree_id);
        if (not has_tree) {
            logger_->Emit(
                LogLevel::Error,
                "Failed while checking for tree {} in repository {}:\n{}",
                tree_id,
                path.string(),
                std::move(has_tree).error());
            response->set_status(CheckRootTreeResponse::INTERNAL_ERROR);
            return ::grpc::Status::OK;
        }
        if (*has_tree) {
            // success!
            response->set_status(CheckRootTreeResponse::OK);
            return ::grpc::Status::OK;
        }
    }

    // now check in the native local CAS
    auto const native_digest = ArtifactDigestFactory::Create(
        native_context_->storage_config->hash_function.GetType(),
        tree_id,
        0,
        /*is_tree=*/true);
    if (not native_digest) {
        logger_->Emit(LogLevel::Error, "Failed to create digest object");
        response->set_status(CheckRootTreeResponse::INTERNAL_ERROR);
        return ::grpc::Status::OK;
    }
    if (native_context_->storage->CAS().TreePath(*native_digest)) {
        // As we currently build only against roots in Git repositories, we need
        // to move the tree from CAS to local Git storage
        auto tmp_dir = native_context_->storage_config->CreateTypedTmpDir(
            "source-tree-check-root-tree");
        if (not tmp_dir) {
            logger_->Emit(LogLevel::Error,
                          "Failed to create tmp directory for copying git-tree "
                          "{} from remote CAS",
                          tree_id);
            response->set_status(CheckRootTreeResponse::INTERNAL_ERROR);
            return ::grpc::Status::OK;
        }
        if (not apis_.local->RetrieveToPaths(
                {Artifact::ObjectInfo{.digest = *native_digest,
                                      .type = ObjectType::Tree}},
                {tmp_dir->GetPath()})) {
            logger_->Emit(LogLevel::Error,
                          "Failed to copy git-tree {} to {}",
                          tree_id,
                          tmp_dir->GetPath().string());
            response->set_status(CheckRootTreeResponse::INTERNAL_ERROR);
            return ::grpc::Status::OK;
        }
        // Import from tmp dir to Git cache
        auto res = GitRepo::ImportToGit(
            *native_context_->storage_config,
            tmp_dir->GetPath(),
            /*commit_message=*/fmt::format("Content of tree {}", tree_id),
            lock_);
        if (not res) {
            // report the error
            logger_->Emit(LogLevel::Error, "{}", res.error());
            response->set_status(CheckRootTreeResponse::INTERNAL_ERROR);
            return ::grpc::Status::OK;
        }
        auto const& imported_tree_id = *res;
        // sanity check
        if (imported_tree_id != tree_id) {
            logger_->Emit(
                LogLevel::Error,
                "Unexpected mismatch in imported tree:\nexpected {} but got {}",
                tree_id,
                imported_tree_id);
            response->set_status(CheckRootTreeResponse::INTERNAL_ERROR);
            return ::grpc::Status::OK;
        }
        // success!
        response->set_status(CheckRootTreeResponse::OK);
        return ::grpc::Status::OK;
    }
    // tree not known
    response->set_status(CheckRootTreeResponse::NOT_FOUND);
    return ::grpc::Status::OK;
}

auto SourceTreeService::GetRemoteTree(
    ::grpc::ServerContext* /* context */,
    const ::justbuild::just_serve::GetRemoteTreeRequest* request,
    GetRemoteTreeResponse* response) -> ::grpc::Status {
    logger_->Emit(
        LogLevel::Debug, "GetRemoteTree({})", request->digest().hash());
    // get gc lock for Git cache
    auto repo_lock = RepositoryGarbageCollector::SharedLock(
        *native_context_->storage_config);
    if (not repo_lock) {
        logger_->Emit(LogLevel::Error, "Could not acquire repo gc SharedLock");
        response->set_status(GetRemoteTreeResponse::INTERNAL_ERROR);
        return ::grpc::Status::OK;
    }
    // ensure Git cache exists
    if (auto done = EnsureGitCacheRoot(); not done) {
        logger_->Emit(LogLevel::Error, std::move(done).error());
        response->set_status(GetRemoteTreeResponse::INTERNAL_ERROR);
        return ::grpc::Status::OK;
    }
    // get gc lock for native storage
    auto lock = GarbageCollector::SharedLock(*native_context_->storage_config);
    if (not lock) {
        logger_->Emit(LogLevel::Error, "Could not acquire gc SharedLock");
        response->set_status(GetRemoteTreeResponse::INTERNAL_ERROR);
        return ::grpc::Status::OK;
    }

    // get tree from remote CAS into tmp dir
    auto const remote_digest = ArtifactDigestFactory::FromBazel(
        apis_.remote->GetHashType(), request->digest());
    if (not remote_digest or not apis_.remote->IsAvailable(*remote_digest)) {
        logger_->Emit(LogLevel::Error,
                      "Remote CAS does not contain expected tree {}",
                      request->digest().hash());
        response->set_status(GetRemoteTreeResponse::FAILED_PRECONDITION);
        return ::grpc::Status::OK;
    }
    auto tmp_dir = native_context_->storage_config->CreateTypedTmpDir(
        "source-tree-get-remote-tree");
    if (not tmp_dir) {
        logger_->Emit(LogLevel::Error,
                      "Failed to create tmp directory for copying git-tree {} "
                      "from remote CAS",
                      remote_digest->hash());
        response->set_status(GetRemoteTreeResponse::INTERNAL_ERROR);
        return ::grpc::Status::OK;
    }
    if (not apis_.remote->ParallelRetrieveToCas(
            {Artifact::ObjectInfo{.digest = *remote_digest,
                                  .type = ObjectType::Tree}},
            *apis_.local,
            serve_config_.jobs,
            true)) {
        logger_->Emit(
            LogLevel::Error,
            "Failed to parallel retrieve tree {} from remote CAS to local CAS",
            remote_digest->hash());
        response->set_status(GetRemoteTreeResponse::FAILED_PRECONDITION);
        return ::grpc::Status::OK;
    }
    if (not apis_.local->RetrieveToPaths(
            {Artifact::ObjectInfo{.digest = *remote_digest,
                                  .type = ObjectType::Tree}},
            {tmp_dir->GetPath()})) {
        logger_->Emit(LogLevel::Error,
                      "Failed to install tree {} from local CAS",
                      remote_digest->hash());
        response->set_status(GetRemoteTreeResponse::FAILED_PRECONDITION);
        return ::grpc::Status::OK;
    }
    // Import from tmp dir to Git cache
    auto res = GitRepo::ImportToGit(
        *native_context_->storage_config,
        tmp_dir->GetPath(),
        /*commit_message=*/
        fmt::format("Content of tree {}", remote_digest->hash()),
        lock_);
    if (not res) {
        // report the error
        logger_->Emit(LogLevel::Error, "{}", res.error());
        response->set_status(GetRemoteTreeResponse::INTERNAL_ERROR);
        return ::grpc::Status::OK;
    }
    logger_->Emit(LogLevel::Debug,
                  "GetRemoteTree: imported tree {} to Git as {}",
                  remote_digest->hash(),
                  *res);
    // success!
    response->set_status(GetRemoteTreeResponse::OK);
    return ::grpc::Status::OK;
}

auto SourceTreeService::ComputeTreeStructure(
    ::grpc::ServerContext* /*context*/,
    const ::justbuild::just_serve::ComputeTreeStructureRequest* request,
    ComputeTreeStructureResponse* response) -> ::grpc::Status {
    logger_->Emit(LogLevel::Debug, "GetTreeStructure({})", request->tree());
    auto repo_lock = RepositoryGarbageCollector::SharedLock(
        *native_context_->storage_config);
    if (not repo_lock) {
        logger_->Emit(LogLevel::Error, "Could not acquire repo gc SharedLock");
        response->set_status(ComputeTreeStructureResponse::INTERNAL_ERROR);
        return ::grpc::Status::OK;
    }
    // ensure Git cache exists
    if (auto done = EnsureGitCacheRoot(); not done) {
        logger_->Emit(LogLevel::Error, std::move(done).error());
        response->set_status(ComputeTreeStructureResponse::INTERNAL_ERROR);
        return ::grpc::Status::OK;
    }
    // get gc lock for native storage
    auto lock = GarbageCollector::SharedLock(*native_context_->storage_config);
    if (not lock) {
        logger_->Emit(LogLevel::Error, "Could not acquire gc SharedLock");
        response->set_status(ComputeTreeStructureResponse::INTERNAL_ERROR);
        return ::grpc::Status::OK;
    }

    auto const tree_digest =
        ArtifactDigestFactory::Create(HashFunction::Type::GitSHA1,
                                      request->tree(),
                                      /*size_unknown=*/0,
                                      /*is_tree=*/true);
    if (not tree_digest) {
        logger_->Emit(LogLevel::Error, tree_digest.error());
        response->set_status(ComputeTreeStructureResponse::INTERNAL_ERROR);
        return ::grpc::Status::OK;
    }

    auto known_repositories = serve_config_.known_repositories;
    known_repositories.push_back(native_context_->storage_config->GitRoot());

    std::optional<ArtifactDigest> tree_structure;
    if (auto from_local = TreeStructureUtils::ComputeStructureLocally(
            *tree_digest,
            known_repositories,
            *native_context_->storage_config,
            lock_)) {
        tree_structure = std::move(from_local).value();
    }
    else {
        // A critical error occurred:
        logger_->Emit(LogLevel::Error, std::move(from_local).error());
        response->set_status(ComputeTreeStructureResponse::INTERNAL_ERROR);
        return ::grpc::Status::OK;
    }

    if (not tree_structure.has_value()) {
        logger_->Emit(
            LogLevel::Error, "Failed to find {}", tree_digest->hash());
        response->set_status(ComputeTreeStructureResponse::NOT_FOUND);
        return ::grpc::Status::OK;
    }

    response->set_tree_structure_hash(tree_structure->hash());
    response->set_status(ComputeTreeStructureResponse::OK);
    return ::grpc::Status::OK;
}

#endif  // BOOTSTRAP_BUILD_TOOL
