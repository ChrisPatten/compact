# Writes fw_version.h (FW_VERSION = git describe --always --dirty) on every build, touching the file only when the
# text changes. Run by main/CMakeLists.txt. IDF's own PROJECT_VER is fixed at configure time, so it went stale
# (a commit made without a reconfigure kept reporting the previous version in /info, Nov 19).
execute_process(COMMAND git describe --always --dirty --abbrev=7
                WORKING_DIRECTORY "${SRC}" OUTPUT_VARIABLE V OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT V)
  set(V "unknown")
endif()
file(WRITE "${OUT}.tmp" "#pragma once\n#define FW_VERSION \"${V}\"\n")
execute_process(COMMAND ${CMAKE_COMMAND} -E copy_if_different "${OUT}.tmp" "${OUT}")
file(REMOVE "${OUT}.tmp")
