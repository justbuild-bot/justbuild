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
#include "src/buildtool/build_engine/target_map/absent_target_map.hpp"

#ifndef BOOTSTRAP_BUILD_TOOL
#include <filesystem>
#include <memory>
#include <optional>
#include <set>
#include <unordered_set>
#include <utility>  // std::move
#include <variant>

#include "fmt/core.h"
#include "src/buildtool/build_engine/analysed_target/target_graph_information.hpp"
#include "src/buildtool/build_engine/base_maps/entity_name_data.hpp"
#include "src/buildtool/build_engine/expression/configuration.hpp"
#include "src/buildtool/common/action_description.hpp"
#include "src/buildtool/common/artifact.hpp"
#include "src/buildtool/common/artifact_digest.hpp"
#include "src/buildtool/common/repository_config.hpp"
#include "src/buildtool/common/statistics.hpp"
#include "src/buildtool/common/tree.hpp"
#include "src/buildtool/common/tree_overlay.hpp"
#include "src/buildtool/logging/log_level.hpp"
#include "src/buildtool/logging/logger.hpp"
#include "src/buildtool/progress_reporting/progress.hpp"
#include "src/buildtool/progress_reporting/task_tracker.hpp"
#include "src/buildtool/serve_api/remote/serve_api.hpp"
#include "src/buildtool/storage/storage.hpp"
#include "src/buildtool/storage/target_cache_entry.hpp"
#include "src/buildtool/storage/target_cache_key.hpp"
#include "src/utils/cpp/json.hpp"
#endif  // BOOTSTRAP_BUILD_TOOL

#ifndef BOOTSTRAP_BUILD_TOOL
namespace {
void WithFlexibleVariables(
    const gsl::not_null<AnalyseContext*>& context,
    const BuildMaps::Target::ConfiguredTarget& key,
    std::vector<std::string> flexible_vars,
    const BuildMaps::Target::AbsentTargetMap::SubCallerPtr& subcaller,
    const BuildMaps::Target::AbsentTargetMap::SetterPtr& setter,
    const BuildMaps::Target::AbsentTargetMap::LoggerPtr& logger,
    const gsl::not_null<BuildMaps::Target::ResultTargetMap*> result_map,
    BuildMaps::Target::ServeFailureLogReporter* serve_failure_reporter) {
    auto effective_config = key.config.Prune(flexible_vars);
    if (key.config != effective_config) {
        (*subcaller)(
            {BuildMaps::Target::ConfiguredTarget{.target = key.target,
                                                 .config = effective_config}},
            [setter](auto const& values) {
                AnalysedTargetPtr result = *(values[0]);
                (*setter)(std::move(result));
            },
            logger);
        return;
    }

    // TODO(asartori): avoid code duplication in export.cpp
    context->statistics->IncrementExportsFoundCounter();
    auto target_name = key.target.GetNamedTarget();
    auto repo_key = context->repo_config->RepositoryKey(*context->storage,
                                                        target_name.repository);
    if (not repo_key) {
        (*logger)(fmt::format("Failed to obtain repository key for repo \"{}\"",
                              target_name.repository),
                  /*fatal=*/true);
        return;
    }
    auto target_cache_key = context->storage->TargetCache().ComputeKey(
        *repo_key, target_name, effective_config);
    if (not target_cache_key) {
        (*logger)(fmt::format("Could not produce cache key for target {}",
                              key.target.ToString()),
                  /*fatal=*/true);
        return;
    }
    std::optional<std::pair<TargetCacheEntry, Artifact::ObjectInfo>>
        target_cache_value{std::nullopt};
    target_cache_value =
        context->storage->TargetCache().Read(*target_cache_key);
    bool from_just_serve = false;
    if (not target_cache_value and context->serve != nullptr) {
        auto task = fmt::format("[{},{}]",
                                key.target.ToString(),
                                PruneJson(effective_config.ToJson()).dump());
        Logger::Log(
            LogLevel::Debug,
            "Querying serve endpoint for absent export target {} with key {}",
            task,
            key.target.ToString());
        context->progress->TaskTracker().Start(task);
        auto res = context->serve->ServeTarget(*target_cache_key, *repo_key);
        // process response from serve endpoint
        if (not res) {
            // report target not found
            (*logger)(fmt::format("Absent target {} was not found on serve "
                                  "endpoint",
                                  key.target.ToString()),
                      /*fatal=*/true);
            return;
        }
        switch (auto const& ind = res->index(); ind) {
            case 0: {
                if (serve_failure_reporter != nullptr) {
                    (*serve_failure_reporter)(key, std::get<0>(*res));
                }
                // target found but failed to analyse/build: log it as fatal
                (*logger)(
                    fmt::format("Failure to remotely analyse or build absent "
                                "target {}\nDetailed log available on the "
                                "remote-execution endpoint as blob {}",
                                key.target.ToString(),
                                std::get<0>(*res)),
                    /*fatal=*/true);
                return;
            }
            case 1:  // fallthrough
            case 2: {
                // Other errors, including INTERNAL: log it as fatal
                (*logger)(fmt::format(
                              "While querying serve endpoint for absent export "
                              "target {}:\n{}",
                              key.target.ToString(),
                              ind == 1 ? std::get<1>(*res) : std::get<2>(*res)),
                          /*fatal=*/true);
                return;
            }
            default: {
                // index == 3
                target_cache_value = std::get<3>(*res);
                context->progress->TaskTracker().Stop(task);
                from_just_serve = true;
            }
        }
    }

    if (not target_cache_value) {
        (*logger)(fmt::format("Could not get target cache value for key {}",
                              target_cache_key->Id().ToString()),
                  /*fatal=*/true);
        return;
    }
    auto const& [entry, info] = *target_cache_value;
    if (auto result = entry.ToResult()) {
        auto deps_info = TargetGraphInformation{
            std::make_shared<BuildMaps::Target::ConfiguredTarget>(
                BuildMaps::Target::ConfiguredTarget{
                    .target = key.target, .config = effective_config}),
            {},
            {},
            {}};

        auto analysis_result = std::make_shared<AnalysedTarget const>(
            *result,
            std::vector<ActionDescription::Ptr>{},
            std::vector<std::string>{},
            std::vector<Tree::Ptr>{},
            std::vector<TreeOverlay::Ptr>{},
            std::unordered_set<std::string>{flexible_vars.begin(),
                                            flexible_vars.end()},
            std::set<std::string>{},
            entry.ToImplied(),
            deps_info);

        analysis_result = result_map->Add(
            key.target, effective_config, analysis_result, std::nullopt, true);

        Logger::Log(LogLevel::Performance,
                    "Absent export target {} taken from {}: {} -> {}",
                    key.target.ToString(),
                    (from_just_serve ? "serve endpoint" : "cache"),
                    target_cache_key->Id().ToString(),
                    info.ToString());

        (*setter)(std::move(analysis_result));
        if (from_just_serve) {
            context->statistics->IncrementExportsServedCounter();
        }
        else {
            context->statistics->IncrementExportsCachedCounter();
        }
        return;
    }
    (*logger)(fmt::format("Reading target entry for key {} failed",
                          target_cache_key->Id().ToString()),
              /*fatal=*/true);
}

}  // namespace

#endif  // BOOTSTRAP_BUILD_TOOL

auto BuildMaps::Target::CreateAbsentTargetVariablesMap(
    const gsl::not_null<AnalyseContext*>& context,
    std::size_t jobs) -> AbsentTargetVariablesMap {
#ifdef BOOTSTRAP_BUILD_TOOL
    auto target_variables = [](auto /*ts*/,
                               auto /*setter*/,
                               auto /*logger*/,
                               auto /*subcaller*/,
                               auto /*key*/) {};
#else
    auto target_variables = [context](auto /*ts*/,
                                      auto setter,
                                      auto logger,
                                      auto /*subcaller*/,
                                      auto key) {
        std::optional<std::vector<std::string>> vars;
        if (context->serve != nullptr) {
            vars = context->serve->ServeTargetVariables(
                key.target_root_id, key.target_file, key.target);
        }

        if (not vars) {
            (*logger)(fmt::format("Failed to obtain flexible config variables "
                                  "for absent target {}",
                                  key.target),
                      /*fatal=*/true);
            return;
        }
        (*setter)(std::move(vars.value()));
    };
#endif
    return AbsentTargetVariablesMap{target_variables, jobs};
}

auto BuildMaps::Target::CreateAbsentTargetMap(
    const gsl::not_null<AnalyseContext*>& context,
    const gsl::not_null<ResultTargetMap*>& result_map,
    const gsl::not_null<AbsentTargetVariablesMap*>& absent_variables,
    std::size_t jobs,
    BuildMaps::Target::ServeFailureLogReporter* serve_failure_reporter)
    -> AbsentTargetMap {
#ifndef BOOTSTRAP_BUILD_TOOL
    auto target_reader =
        [context, result_map, absent_variables, serve_failure_reporter](
            auto ts, auto setter, auto logger, auto subcaller, auto key) {
            // assumptions:
            // - target with absent targets file requested
            // - ServeApi correctly configured
            auto const& repo_name = key.target.ToModule().repository;
            auto target_root_id =
                context->repo_config->TargetRoot(repo_name)->GetAbsentTreeId();
            if (not target_root_id) {
                (*logger)(fmt::format("Failed to get the target root id for "
                                      "repository \"{}\"",
                                      repo_name),
                          /*fatal=*/true);
                return;
            }
            std::filesystem::path module{key.target.ToModule().module};
            auto vars_request = AbsentTargetDescription{
                .target_root_id = *target_root_id,
                .target_file = (module / *(context->repo_config->TargetFileName(
                                             repo_name)))
                                   .string(),
                .target = key.target.GetNamedTarget().name};
            absent_variables->ConsumeAfterKeysReady(
                ts,
                {vars_request},
                [context,
                 key,
                 setter,
                 logger,
                 serve_failure_reporter,
                 result_map,
                 subcaller](auto const& values) {
                    WithFlexibleVariables(context,
                                          key,
                                          *(values[0]),
                                          subcaller,
                                          setter,
                                          logger,
                                          result_map,
                                          serve_failure_reporter);
                },
                [logger, target = key.target](auto const& msg, auto fatal) {
                    (*logger)(fmt::format("While requested the flexible "
                                          "variables of {}:\n{}",
                                          target.ToString(),
                                          msg),
                              fatal);
                });
        };
#else
    auto target_reader = [](auto /*ts*/,
                            auto /*setter*/,
                            auto /*logger*/,
                            auto /*subcaller*/,
                            auto /*key*/) {};
#endif
    return AbsentTargetMap{target_reader, jobs};
}
