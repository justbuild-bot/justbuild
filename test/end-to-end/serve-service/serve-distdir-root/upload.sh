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


###
# This test checks that an absent distdir root can be successfully computed
# locally and then uploaded to a serve endpoint that does not know the root.
##

set -eu

env

readonly JUST="${PWD}/bin/tool-under-test"
readonly JUST_MR="${PWD}/bin/mr-tool-under-test"
readonly DISTDIR="${TEST_TMPDIR}/distfiles"
readonly LBR="${TEST_TMPDIR}/local-build-root"

mkdir -p "${DISTDIR}"

COMPAT=""
if [ "${COMPATIBLE:-}" = "YES" ]; then
  COMPAT="--compatible"
fi

ENDPOINT_ARGS="-r ${REMOTE_EXECUTION_ADDRESS} -R ${SERVE} ${COMPAT}"

###
# Setup sample repos config with present and absent repos
##

cp src.tar "${DISTDIR}"
SRC_HASH=$(git hash-object src.tar)
cp src-syms.tar "${DISTDIR}"
SRC_SYMS_HASH=$(git hash-object src-syms.tar)

mkdir work
cd work

touch ROOT
cat > repos.json <<EOF
{ "repositories":
  { "unresolved":
    { "repository":
      { "type": "archive"
      , "content": "$SRC_HASH"
      , "fetch": "http://example.org/src.tar"
      , "subdir": "repo"
      }
    }
  , "resolved":
    { "repository":
      { "type": "archive"
      , "content": "$SRC_SYMS_HASH"
      , "fetch": "http://example.org/src-syms.tar"
      , "subdir": "repo"
      , "pragma": {"special": "resolve-completely"}
      }
    }
  , "present":
    { "repository":
      { "type": "distdir"
      , "repositories": ["unresolved", "resolved"]
      }
    }
  , "absent":
    { "repository":
      { "type": "distdir"
      , "repositories": ["unresolved", "resolved"]
      , "pragma": {"absent": true}
      }
    }
  }
}
EOF

###
# Run the checks
##

# Compute present root locally from scratch (via distfile)
CONF=$("${JUST_MR}" --norc -C repos.json \
                    --just "${JUST}" \
                    --local-build-root "${LBR}" \
                    --distdir "${DISTDIR}" \
                    --log-limit 6 \
                    setup present)
cat "${CONF}"
echo
TREE=$(jq -r '.repositories.present.workspace_root[1]' "${CONF}")

# Remove the distdir
rm -rf "${DISTDIR}"

# While keeping the file association, ask serve endpoint to provide the root as
# absent. For a serve endpoint that does not have the archive blob available,
# this will require uploading the locally-known root tree to remote CAS, from
# where the serve endpoint will pick it up.
${JUST} gc --local-build-root ${LBR} 2>&1
${JUST} gc --local-build-root ${LBR} 2>&1

CONF=$("${JUST_MR}" --norc -C repos.json \
                    --just "${JUST}" \
                    --local-build-root "${LBR}" \
                    --log-limit 6 \
                    ${ENDPOINT_ARGS} setup absent)
cat "${CONF}"
echo
test $(jq -r '.repositories.absent.workspace_root[1]' "${CONF}") = "${TREE}"

echo OK
