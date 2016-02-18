cmake_minimum_required (VERSION 2.6)
include(CheckFunctionExists)
include(CheckIncludeFile)
project(raw_socket CXX C ASM)

macro (assert TEST COMMENT)                                                     
    if (NOT ${TEST})                                                            
        message(FATAL_ERROR "Assertion failed: ${COMMENT}")               
    endif ()                                                                    
endmacro (assert)

execute_process(COMMAND erl -eval "io:format(\"~s\", [lists:concat([code:root_dir(), \"/erts-\", erlang:system_info(version), \"/include\"])])" -s init stop -noshell OUTPUT_VARIABLE ERLANG_ROOT)

include_directories(${ERLANG_ROOT})
message("Erlang root path: " ${ERLANG_ROOT})

find_path(LIBEV_INCLUDE_DIR                                                     
   NAMES ev.h                                                                   
   PATHS /usr/include /usr/local/include)                                       
assert(LIBEV_INCLUDE_DIR "libev header file path not found")                    
include_directories( ${LIBEV_INCLUDE_DIR})                                      
find_library(LIBEV_LIB                                                          
   NAMES ev                                                                     
   PATHS /usr/lib /usr/local/lib)                                               
assert(LIBEV_LIB "libev not found")

set(CMAKE_CXX_FLAGS ${CMAKE_CXX_FLAGS} "-fpermissive")
set(SOURCES raw_socket.cpp)
add_library(raw_socket SHARED ${SOURCES})
set_target_properties(raw_socket PROPERTIES PREFIX "")
set_target_properties(raw_socket PROPERTIES LIBRARY_OUTPUT_DIRECTORY
	"${PROJECT_SOURCE_DIR}/../priv")
target_link_libraries(raw_socket ${LIBEV_LIB})

