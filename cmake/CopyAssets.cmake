# Copy the runtime asset tree into the build output.
#
#   cmake -DCOPY_SRC=<repo>/assets -DCOPY_DEST=<out>/assets -P cmake/CopyAssets.cmake
#
# `cmake -E copy_directory` has no way to skip files, and the source tree
# carries dev-only artwork (the icon master and the reference sheets, several
# MB each) that must not ship. So copy the pieces the game actually loads:
#
#   data/       stage/enemy/boss/weapon JSON (required)
#   icons/      window-icon PNG pack
#   sprites/    optional per-sprite PNG overrides
#   tiles/      optional tile PNG overrides
#   splash.png  loading-screen artwork
#
# Missing optional entries are skipped silently. file(COPY) preserves source
# timestamps, so the tree stays diffable.
if(NOT DEFINED COPY_SRC OR NOT DEFINED COPY_DEST)
    message(FATAL_ERROR "CopyAssets.cmake requires -DCOPY_SRC and -DCOPY_DEST")
endif()

file(MAKE_DIRECTORY "${COPY_DEST}")

foreach(item data icons sprites tiles splash.png)
    if(EXISTS "${COPY_SRC}/${item}")
        file(COPY "${COPY_SRC}/${item}" DESTINATION "${COPY_DEST}")
    endif()
endforeach()
