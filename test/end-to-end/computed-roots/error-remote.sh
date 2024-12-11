#!/bin/sh
# Copyright 2024 Huawei Cloud Computing Technology Co., Ltd.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

set -e

readonly ROOT="$(pwd)"
readonly LBRA="$TMPDIR/local-build-root-A"
readonly LBRB="$TMPDIR/local-build-root-B"
readonly JUST="${ROOT}/bin/tool-under-test"
readonly OUT="${ROOT}/out"
mkdir -p "${OUT}"

REMOTE_ARGS="-r ${REMOTE_EXECUTION_ADDRESS}"
if [ -n "${COMPATIBLE:-}" ]
then
    REMOTE_ARGS="${REMOTE_ARGS} --compatible"
fi
echo Using ${REMOTE_ARGS}

readonly BASE_ROOT="${ROOT}/base"
mkdir -p "${BASE_ROOT}"
cd "${BASE_ROOT}"
cat > TARGETS <<'EOF'
{ "": {"type": "export", "target": "failing build"}
, "failing build":
  { "type": "generic"
  , "outs": ["TARGETS"]
  , "cmds": ["echo -n ThisWill", "echo FAIL", "false"]
  }
}
EOF
git init 2>&1
git branch -m stable-1.0 2>&1
git config user.email "nobody@example.org" 2>&1
git config user.name "Nobody" 2>&1
git add . 2>&1
git commit -m "Initial commit" 2>&1
GIT_TREE=$(git log -n 1 --format="%T")


mkdir -p "${ROOT}/main"
cd "${ROOT}/main"
cat > repo-config.json <<EOF
{ "repositories":
  { "base":
    {"workspace_root": ["git tree", "${GIT_TREE}", "${BASE_ROOT}/.git"]}
  , "": {"workspace_root": ["computed", "base", "", "", {}]}
  }
}
EOF
cat repo-config.json


echo
echo 'Build depending on (failing) computed root'
echo
"${JUST}" build -C repo-config.json ${REMOTE_ARGS} \
          --local-build-root "${LBRA}" \
          -f "${OUT}/build.log" 2>&1 && exit 1 || :
LOG_BLOB=$(grep 'see [a-zA-Z0-9]* for details' "${OUT}/build.log" | sed 's/.*see //' | sed 's/ for details.*//')
echo
echo "Blob of log is ${LOG_BLOB}"

echo
echo 'Verifying that the blob can be downloaded from a different client'
echo
"${JUST}" install-cas ${REMOTE_ARGS} \
          --local-build-root "${LBRB}" \
          -o "${TMPDIR}/log" "${LOG_BLOB}" 2>&1
echo
grep ThisWillFAIL "${TMPDIR}/log"
echo

echo OK