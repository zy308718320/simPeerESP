#!/usr/bin/env bash
# 发布新固件版本到 GitHub Release（设备通过 OTA 自动升级）
#
# 用法: ./scripts/release.sh <版本号>
# 例如: ./scripts/release.sh 1.0.1
#
# 做的事情:
#   1. 把版本号写入顶层 CMakeLists.txt 的 PROJECT_VER 并提交
#   2. 全量构建固件
#   3. 创建 GitHub Release (tag vX.Y.Z) 并上传 build/sms.bin
#
# 依赖: gh (GitHub CLI, 已登录)、ESP-IDF v6.1 环境
# 注意: 本脚本为 macOS 语法 (sed -i '')

set -euo pipefail
cd "$(dirname "$0")/.."

VERSION="${1:?用法: ./scripts/release.sh <版本号>，如 1.0.1}"
TAG="v${VERSION}"

# 1. 写入版本号并提交
sed -i '' -E "s/^set\(PROJECT_VER .*/set(PROJECT_VER \"${VERSION}\")/" CMakeLists.txt
if ! git diff --quiet CMakeLists.txt; then
    git add CMakeLists.txt
    git commit -m "chore: 固件版本号更新到 ${VERSION}"
fi

# 2. 构建（分区表升级后首次构建需清理）
if [ ! -f build/partition_table/partition-table.bin ]; then
    rm -rf build
fi
. "${HOME}/.espressif/tools/activate_idf_v6.1.sh" >/dev/null
idf.py build

# 3. 发布 Release
if gh release view "${TAG}" >/dev/null 2>&1; then
    echo "Release ${TAG} 已存在，上传/覆盖固件..."
    gh release upload "${TAG}" build/sms.bin --clobber
else
    gh release create "${TAG}" build/sms.bin \
        --title "${TAG}" \
        --notes "固件 sms.bin。已部署的设备会在 24 小时内通过 OTA 自动检查并升级到本版本。"
fi

echo ""
echo "✓ 发布完成: ${TAG}"
echo "  设备端将在下次 OTA 检查时（最长 24 小时）自动升级"
