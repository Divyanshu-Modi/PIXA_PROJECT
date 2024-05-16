# PIXA PROJECT
cmake_minimum_required(VERSION 3.16)

include($ENV{ADF_PATH}/CMakeLists.txt)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(pixa)

target_add_binary_data(pixa.elf "main/isrgrootx1.pem" TEXT)
