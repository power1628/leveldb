

find_path(Lmdb_INCLUDE_DIR
  NAMES lmdb.h
  PATHS "/usr/local/include"
  PATH_SUFFIXES lmdb
)
find_library(Lmdb_LIBRARY
  NAMES lmdb
  PATHS "/usr/local/lib"
)

set(Lmdb_VERSION 0.0.0)


include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Lmdb
  FOUND_VAR Lmdb_FOUND
  REQUIRED_VARS
    Lmdb_LIBRARY
    Lmdb_INCLUDE_DIR
  VERSION_VAR Lmdb_VERSION
)

if(Lmdb_FOUND)
  set(Lmdb_LIBRARIES ${Lmdb_LIBRARY})
  set(Lmdb_INCLUDE_DIRS ${Lmdb_INCLUDE_DIR})
  message(STATUS "LMDB FOUND ${Lmdb_LIBRARIES} ${Lmdb_INCLUDE_DIRS}")
endif()
