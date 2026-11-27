# relink 1.0.10, precompiled for linux-x86_64 (no dependencies beyond libc)
include(CMakeFindDependencyMacro)
find_dependency(Threads)
get_filename_component(_relink_root "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
option(RELINK_STATIC "Link librelink.a instead of librelink.so" OFF)
if(NOT TARGET relink::relink)
  if(RELINK_STATIC)
    add_library(relink::relink STATIC IMPORTED)
    set_target_properties(relink::relink PROPERTIES IMPORTED_LOCATION "${_relink_root}/lib/librelink.a"
      INTERFACE_LINK_LIBRARIES "Threads::Threads;${CMAKE_DL_LIBS}")
  else()
    add_library(relink::relink SHARED IMPORTED)
    set_target_properties(relink::relink PROPERTIES IMPORTED_LOCATION "${_relink_root}/lib/librelink.so"
      INTERFACE_LINK_LIBRARIES "Threads::Threads")
  endif()
  set_target_properties(relink::relink PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${_relink_root}/include"
    INTERFACE_COMPILE_FEATURES cxx_std_17)
endif()
set(relink_VERSION 1.0.10)
