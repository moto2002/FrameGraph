file( GLOB_RECURSE SOURCES "*.*" )
add_library( "STL" STATIC ${SOURCES} )
source_group( TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${SOURCES} )
target_link_libraries( "STL" PUBLIC "ProjectTemplate" )
target_include_directories( "STL" PUBLIC ".." )
set_property( TARGET "STL" PROPERTY FOLDER "" )

if (TARGET "StdVariant-lib")
	target_link_libraries( "STL" PUBLIC "StdVariant-lib" )
endif()
	
if (TARGET "StdStringView-lib")
	target_link_libraries( "STL" PUBLIC "StdStringView-lib" )
endif()
	
if (TARGET "StdOptional-lib")
	target_link_libraries( "STL" PUBLIC "StdOptional-lib" )
endif()
	
if (TARGET "FileSystem-lib")
	target_link_libraries( "STL" PUBLIC "FileSystem-lib" )
endif()

if (UNIX)
	target_link_libraries( "STL" PUBLIC "dl;pthread" )
	target_compile_definitions( "STL" PUBLIC _LARGEFILE_SOURCE )
	target_compile_definitions( "STL" PUBLIC _LARGE_FILES )
	target_compile_definitions( "STL" PUBLIC _FILE_OFFSET_BITS=64 )
endif()

install( TARGETS "STL" ARCHIVE DESTINATION "libs/$<CONFIG>" )
install( DIRECTORY "." DESTINATION "include/STL" FILES_MATCHING PATTERN "*.h*" )
