cmake_minimum_required(VERSION 2.8.3)
project(rosbag)
set(VERSION_MAJOR "1")
set(VERSION_MINOR "5")
set(VERSION_PATCH "1")
set(VERSION "${VERSION_MAJOR}.${VERSION_MINOR}.${VERSION_PATCH}")
find_package(catkin REQUIRED COMPONENTS rosbag_storage rosconsole roscpp topic_tools xmlrpcpp)
find_package(Boost REQUIRED COMPONENTS date_time regex program_options filesystem python)
find_package(BZip2 REQUIRED)

option(BUILD_SHARED_LIBS "Build Shared Libraries" OFF)

if(BUILD_SHARED_LIBS)
	set(LABEL_SUFFIX "shared")
else()
	set(LABEL_SUFFIX "static")
endif()

if(CMAKE_COMPILER_IS_GNUCXX)
    set(CMAKE_CXX_FLAGS "-g -pipe -W -Wall -fPIC -O2 -Wno-parentheses -rdynamic -std=c++11 -ffast-math")
    set(CMAKE_EXE_LINKER_FLAGS "-lpthread -lcrypto -lrt -lz -export-dynamic")
endif()

#catkin_python_setup()
set(LIBRARY_OUTPUT_PATH ${PROJECT_SOURCE_DIR}/lib)

# Support large bags (>2GB) on 32-bit systems
add_definitions(-D_FILE_OFFSET_BITS=64)

include_directories(include ${catkin_INCLUDE_DIRS} ${Boost_INCLUDE_DIRS} /usr/include/python2.7/)

catkin_package(
  LIBRARIES rosbag_recorder
  INCLUDE_DIRS include
  CATKIN_DEPENDS rosbag_storage rosconsole roscpp topic_tools xmlrpcpp)

add_library(rosbag_recorder SHARED
  src/recorder.cpp
)
add_library(rosbag_recorder_static STATIC
  src/recorder.cpp
)

set_target_properties(rosbag_recorder_static PROPERTIES OUTPUT_NAME "rosbag_recorder")
set_target_properties(rosbag_recorder PROPERTIES CLEAN_DIRECT_OUTPUT 1)
set_target_properties(rosbag_recorder_static PROPERTIES CLEAN_DIRECT_OUTPUT 1)
set_target_properties(rosbag_recorder PROPERTIES
	VERSION "${VERSION}"
	SOVERSION "${VERSION_MAJOR}.${VERSION_MINOR}"
	PROJECT_LABEL "rosbag_recorder ${LABEL_SUFFIX}"
)
target_link_libraries(rosbag_recorder ${catkin_LIBRARIES} ${Boost_LIBRARIES} python2.7 )
