cmake_minimum_required(VERSION 3.3)
project(GetFileServer)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -pthread")

set(SOURCE_FILES gfserver.c gfserver.h gfserver_main.c content.c content.h handler.c steque.h steque.c)
add_executable(GetFileServer ${SOURCE_FILES})