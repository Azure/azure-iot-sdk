# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.

# Patch cmocka's CMakeLists.txt to remove the unconditional symlink creation
# for compile_commands.json. On Windows without Developer Mode this fails with
# "A required privilege is not held by the client."
#
# This script runs as PATCH_COMMAND inside FetchContent_Declare, so the working
# directory is the cmocka source directory.

file(READ "CMakeLists.txt" _content)
string(REGEX REPLACE
    "# Link combile database for clangd\nexecute_process\\(COMMAND cmake -E create_symlink\n[^\n]*\n[^\n]*\\)"
    "# (symlink for compile_commands.json removed — fails on Windows without Developer Mode)"
    _content "${_content}")
file(WRITE "CMakeLists.txt" "${_content}")
