# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# "Trim" the build. Include the minimal set of components, main, and anything it depends on.
idf_build_set_property(MINIMAL_BUILD ON)
# 固件版本号：OTA 通过它与 GitHub Release 的 tag（vX.Y.Z）比较判断是否升级
# 发布新版本请修改此处并运行 scripts/release.sh
set(PROJECT_VER "1.0.6")
project(sms)
