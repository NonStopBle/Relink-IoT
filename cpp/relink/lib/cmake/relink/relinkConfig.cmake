# relink 1.0.2, precompiled for linux-x86_64
include(CMakeFindDependencyMacro)
find_dependency(OpenSSL)
find_dependency(ZLIB)
find_dependency(Threads)
get_filename_component(_relink_root "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
option(RELINK_STATIC "Link librelink.a instead of librelink.so" OFF)
if(NOT TARGET relink::relink)
  if(RELINK_STATIC)
    add_library(relink::relink STATIC IMPORTED)
    set_target_properties(relink::relink PROPERTIES IMPORTED_LOCATION "${_relink_root}/lib/librelink.a")
  else()
    add_library(relink::relink SHARED IMPORTED)
    set_target_properties(relink::relink PROPERTIES IMPORTED_LOCATION "${_relink_root}/lib/librelink.so")
  endif()
  set_target_properties(relink::relink PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${_relink_root}/include"
    INTERFACE_LINK_LIBRARIES "OpenSSL::Crypto;ZLIB::ZLIB;Threads::Threads"
    INTERFACE_COMPILE_FEATURES cxx_std_17)
endif()
set(relink_VERSION 1.0.2)
