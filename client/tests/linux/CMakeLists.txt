cmake_minimum_required(VERSION 3.25)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(AmneziaDnsUtilsLinuxTest VERSION 1.0 LANGUAGES CXX)
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_AUTOMOC ON)
    include(CTest)

    set(CLIENT_APPLICATION_NAME "AmneziaVPN")
    set(CLIENT_SERVICE_NAME "AmneziaVPN-service")
    set(CLIENT_ORGANIZATION_NAME "AmneziaVPN.ORG")
    set(APP_MAJOR_VERSION "1.0.0")
    configure_file(
        ${CMAKE_CURRENT_LIST_DIR}/../../../version.h.in
        ${CMAKE_CURRENT_BINARY_DIR}/version.h
    )
endif()

find_package(Qt6 REQUIRED COMPONENTS Core Core5Compat DBus Gui Network Test)

set(AMNEZIA_REPO_ROOT ${CMAKE_CURRENT_LIST_DIR}/../../..)

add_executable(fake_resolve1
    fakeResolve1.cpp
)

target_link_libraries(fake_resolve1 PRIVATE
    Qt6::Core
    Qt6::DBus
    Qt6::Network
)

target_include_directories(fake_resolve1 PRIVATE
    ${AMNEZIA_REPO_ROOT}/client
)

add_executable(test_dns_utils_linux
    testDnsUtilsLinux.cpp
    ${AMNEZIA_REPO_ROOT}/client/platforms/linux/daemon/dnsutilslinux.cpp
    ${AMNEZIA_REPO_ROOT}/client/platforms/linux/daemon/dnsutilslinux.h
    ${AMNEZIA_REPO_ROOT}/client/platforms/linux/daemon/dbustypeslinux.h
    ${AMNEZIA_REPO_ROOT}/client/daemon/dnsutils.h
    ${AMNEZIA_REPO_ROOT}/client/mozilla/shared/leakdetector.cpp
    ${AMNEZIA_REPO_ROOT}/client/mozilla/shared/leakdetector.h
    ${AMNEZIA_REPO_ROOT}/common/logger/logger.cpp
    ${AMNEZIA_REPO_ROOT}/common/logger/logger.h
)

target_link_libraries(test_dns_utils_linux PRIVATE
    Qt6::Core
    Qt6::DBus
    Qt6::Gui
    Qt6::Network
    Qt6::Test
)

target_include_directories(test_dns_utils_linux PRIVATE
    ${AMNEZIA_REPO_ROOT}/client
    ${AMNEZIA_REPO_ROOT}/client/mozilla/shared
    ${AMNEZIA_REPO_ROOT}/common/logger
    ${CMAKE_CURRENT_BINARY_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}/../..
)

add_test(NAME DnsUtilsLinuxPrivateDbusTest
    COMMAND bash
        ${CMAKE_CURRENT_LIST_DIR}/runPrivateDbusDnsTest.sh
        $<TARGET_FILE:test_dns_utils_linux>
        $<TARGET_FILE:fake_resolve1>
)

add_executable(test_linux_netlink
    testLinuxRouteMonitor.cpp
    ${AMNEZIA_REPO_ROOT}/client/platforms/linux/daemon/iputilslinux.cpp
    ${AMNEZIA_REPO_ROOT}/client/platforms/linux/daemon/iputilslinux.h
    ${AMNEZIA_REPO_ROOT}/client/platforms/linux/daemon/linuxroutemonitor.cpp
    ${AMNEZIA_REPO_ROOT}/client/platforms/linux/daemon/linuxroutemonitor.h
    ${AMNEZIA_REPO_ROOT}/client/core/utils/networkUtilities.cpp
    ${AMNEZIA_REPO_ROOT}/client/core/utils/networkUtilities.h
    ${AMNEZIA_REPO_ROOT}/client/daemon/interfaceconfig.h
    ${AMNEZIA_REPO_ROOT}/client/daemon/iputils.h
    ${AMNEZIA_REPO_ROOT}/client/mozilla/shared/ipaddress.cpp
    ${AMNEZIA_REPO_ROOT}/client/mozilla/shared/ipaddress.h
    ${AMNEZIA_REPO_ROOT}/client/mozilla/shared/leakdetector.cpp
    ${AMNEZIA_REPO_ROOT}/client/mozilla/shared/leakdetector.h
    ${AMNEZIA_REPO_ROOT}/client/mozilla/shared/loglevel.h
    ${AMNEZIA_REPO_ROOT}/common/logger/logger.cpp
    ${AMNEZIA_REPO_ROOT}/common/logger/logger.h
)

target_link_libraries(test_linux_netlink PRIVATE
    Qt6::Core
    Qt6::Core5Compat
    Qt6::Gui
    Qt6::Network
    Qt6::Test
)

target_include_directories(test_linux_netlink PRIVATE
    ${AMNEZIA_REPO_ROOT}/client
    ${AMNEZIA_REPO_ROOT}/client/mozilla/shared
    ${AMNEZIA_REPO_ROOT}/common/logger
    ${CMAKE_CURRENT_BINARY_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}/../..
)

add_library(wireguardutils_linux_compile OBJECT
    ${AMNEZIA_REPO_ROOT}/client/platforms/linux/daemon/wireguardutilslinux.cpp
    ${AMNEZIA_REPO_ROOT}/client/platforms/linux/daemon/wireguardutilslinux.h
)

target_link_libraries(wireguardutils_linux_compile PRIVATE
    Qt6::Core
    Qt6::Core5Compat
    Qt6::Gui
    Qt6::Network
)

target_include_directories(wireguardutils_linux_compile PRIVATE
    ${AMNEZIA_REPO_ROOT}/client
    ${AMNEZIA_REPO_ROOT}/client/mozilla/shared
    ${AMNEZIA_REPO_ROOT}/common/logger
    ${CMAKE_CURRENT_BINARY_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}/../..
)

option(AMNEZIA_ENABLE_PRIVILEGED_LINUX_TESTS
    "Register isolated Linux tests that require NET_ADMIN"
    OFF
)

if(AMNEZIA_ENABLE_PRIVILEGED_LINUX_TESTS)
    add_test(NAME LinuxNetlinkPrivateNamespaceTest
        COMMAND $<TARGET_FILE:test_linux_netlink>
    )
    set_tests_properties(LinuxNetlinkPrivateNamespaceTest PROPERTIES
        LABELS "linux;privileged;network-namespace"
        TIMEOUT 30
    )
endif()
