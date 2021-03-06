
find_package(PkgConfig REQUIRED)

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

if(BUILD_WITH_STATIC)
    find_library(LIBGFLAGS libgflags.a)
    find_library(LIBEV libev.a)
    find_library(LIBGLOG libglog.a)
    find_library(LIBMBEDCRYPT libmbedcrypto.a)
else(BUILD_WITH_STATIC)
    find_library(LIBGFLAGS gflags)
    find_library(LIBEV ev)
    find_library(LIBGLOG glog)
    find_library(LIBMBEDCRYPT mbedcrypto)
endif(BUILD_WITH_STATIC)

pkg_search_module(LIBNFQUEUE REQUIRED libnetfilter_queue)

list(APPEND DEPS
    ${LIBPTHREAD}
    ${LIBGFLAGS}
    ${LIBEV}
    ${LIBGLOG}
    ${LIBMBEDCRYPT}
    ${LIBNFQUEUE_LIBRARIES}
    )

list(APPEND SOURCES
    myipsec.cpp
    nfq.cpp
    nfqueue.cpp
    conf.cpp
    transer.cpp
    filter.cpp
    )

add_custom_target(scripts)

add_custom_command(TARGET scripts PRE_BUILD
                   COMMAND ${CMAKE_COMMAND} -E
                        copy ${CMAKE_CURRENT_SOURCE_DIR}/iptables_setup.sh
                             ${CMAKE_CURRENT_BINARY_DIR}/iptables_setup.sh)

add_executable(myipsec ${SOURCES})
add_dependencies(myipsec scripts)
target_link_libraries(myipsec ${DEPS} Threads::Threads)

