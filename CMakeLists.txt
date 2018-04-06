cmake_minimum_required(VERSION 3.5)

Project(graft_server)

set(CMAKE_VERBOSE_MAKEFILE on)

find_package (Threads)

#find_package(PCRE REQUIRED)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_BUILD_TYPE Debug)

add_definitions(-DGN_ENABLE_EVENTFD=1 -DMG_USE_READ_WRITE)

add_executable(graft_server
	graft_server.cpp
	router.cpp
	thread_pool.cpp
	mongoose/mongoose.c
)

target_include_directories(graft_server PRIVATE 
	${PROJECT_SOURCE_DIR}/mongoose
	${PROJECT_SOURCE_DIR}/r3/include
	${PROJECT_SOURCE_DIR}/thread-pool-cpp/include
)

target_link_libraries(graft_server
	m
	${CMAKE_THREAD_LIBS_INIT}
	${PROJECT_SOURCE_DIR}/libr3.a
	pcre
	${PROJECT_SOURCE_DIR}/liblib3rdparty.a
)

add_subdirectory(r3)
add_subdirectory(thread-pool-cpp)
