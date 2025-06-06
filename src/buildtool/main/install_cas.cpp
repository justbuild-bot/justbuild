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

#include "src/buildtool/main/install_cas.hpp"

#ifdef __unix__
#include <unistd.h>
#else
#error "Non-unix is not supported yet"
#endif

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <sstream>
#include <utility>
#include <vector>

#include "gsl/gsl"
#include "src/buildtool/common/artifact_digest.hpp"
#include "src/buildtool/common/artifact_digest_factory.hpp"
#include "src/buildtool/file_system/file_system_manager.hpp"
#include "src/buildtool/file_system/object_type.hpp"
#include "src/buildtool/logging/log_level.hpp"
#include "src/buildtool/logging/logger.hpp"
#include "src/utils/cpp/expected.hpp"
#ifndef BOOTSTRAP_BUILD_TOOL
#include "src/buildtool/common/remote/remote_common.hpp"
#include "src/buildtool/execution_api/common/execution_api.hpp"
#include "src/buildtool/execution_api/remote/config.hpp"
#include "src/buildtool/execution_api/utils/subobject.hpp"
#include "src/buildtool/main/archive.hpp"
#endif

namespace {

[[nodiscard]] auto InvalidSizeString(HashFunction::Type hash_type,
                                     std::string const& size_str,
                                     std::string const& hash,
                                     bool has_remote) noexcept -> bool {
    // Only in compatible mode the size is checked, so an empty SHA256 hash is
    // needed.
    static auto const kEmptyHash =
        HashFunction{HashFunction::Type::PlainSHA256}.HashBlobData("");
    return hash_type ==
               HashFunction::Type::PlainSHA256 and    // native mode is fine
           (size_str == "0" or size_str.empty()) and  // not "0" or "" is fine
           kEmptyHash.HexString() != hash and         // empty hash is fine
           has_remote;                                // local is fine
}

}  // namespace

[[nodiscard]] auto ObjectInfoFromLiberalString(HashFunction::Type hash_type,
                                               std::string const& s,
                                               bool has_remote) noexcept
    -> std::optional<Artifact::ObjectInfo> {
    std::istringstream iss(s);
    std::string id{};
    std::string size_str{};
    std::string type{"f"};
    if (iss.peek() == '[') {
        (void)iss.get();
    }
    std::getline(iss, id, ':');
    if (not iss.eof()) {
        std::getline(iss, size_str, ':');
    }
    if (not iss.eof()) {
        std::getline(iss, type, ']');
    }
    if (InvalidSizeString(hash_type, size_str, id, has_remote)) {
        Logger::Log(
            LogLevel::Warning,
            "{} size in object-id is not supported in compatiblity mode.",
            size_str.empty() ? "omitting the" : "zero");
    }
    auto size = static_cast<std::size_t>(
        size_str.empty() ? 0 : std::atol(size_str.c_str()));
    auto const& object_type = FromChar(*type.c_str());
    auto digest = ArtifactDigestFactory::Create(
        hash_type, id, size, IsTreeObject(object_type));
    if (not digest) {
        return std::nullopt;
    }
    return Artifact::ObjectInfo{.digest = *std::move(digest),
                                .type = object_type};
}

#ifndef BOOTSTRAP_BUILD_TOOL
auto FetchAndInstallArtifacts(ApiBundle const& apis,
                              FetchArguments const& clargs,
                              RemoteContext const& remote_context) -> bool {
    auto object_info = ObjectInfoFromLiberalString(
        apis.remote->GetHashType(),
        clargs.object_id,
        remote_context.exec_config->remote_address.has_value());
    if (not object_info) {
        return false;
    }

    if (clargs.remember) {
        if (not apis.remote->ParallelRetrieveToCas(
                {*object_info}, *apis.local, 1, true)) {
            Logger::Log(LogLevel::Warning,
                        "Failed to copy artifact {} to local CAS",
                        object_info->ToString());
        }
    }

    if (clargs.sub_path) {
        auto new_object_info =
            RetrieveSubPathId(*object_info, apis, *clargs.sub_path);
        if (new_object_info) {
            object_info = new_object_info;
        }
        else {
            return false;
        }
    }

    std::optional<std::filesystem::path> out{};
    if (clargs.output_path) {
        // Compute output location and create parent directories
        auto output_path = (*clargs.output_path / "").parent_path();
        if (FileSystemManager::IsDirectory(output_path)) {
            output_path /= object_info->digest.hash();
        }

        if (not FileSystemManager::CreateDirectory(output_path.parent_path())) {
            Logger::Log(LogLevel::Error,
                        "failed to create parent directory {}.",
                        output_path.parent_path().string());
            return false;
        }
        out = output_path;
    }

    if (clargs.archive) {
        if (object_info->type != ObjectType::Tree) {
            Logger::Log(LogLevel::Error,
                        "Archive requested on non-tree {}",
                        object_info->ToString());
            return false;
        }
        return GenerateArchive(*apis.remote, *object_info, out);
    }

    if (out) {
        if (not apis.remote->RetrieveToPaths(
                {*object_info}, {*out}, &*apis.local)) {
            Logger::Log(LogLevel::Error, "failed to retrieve artifact.");
            return false;
        }

        Logger::Log(LogLevel::Info,
                    "artifact {} was installed to {}",
                    object_info->ToString(),
                    out->string());
    }
    else {  // dump to stdout
        if (not apis.remote->RetrieveToFds({*object_info},
                                           {dup(fileno(stdout))},
                                           clargs.raw_tree,
                                           &*apis.local)) {
            Logger::Log(LogLevel::Error, "failed to dump artifact.");
            return false;
        }
    }

    return true;
}
#endif
