#
# PIXA PORJECT
#
set(SRCS "")
set(DIRS "")

list(APPEND SRCS "main.c")
list(APPEND DIRS "inc/")

idf_component_register(SRCS ${SRCS} INCLUDE_DIRS "." ${DIRS} REQUIRES driver)
