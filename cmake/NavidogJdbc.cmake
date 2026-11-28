# A runtime image is specific to its platform and CPU architecture.
set(navidog_jdbc_native ON)
if(CMAKE_CROSSCOMPILING OR NAVIDOG_WIN7 OR NAVIDOG_HARMONYOS)
    set(navidog_jdbc_native OFF)
endif()

if(navidog_jdbc_native)
    find_package(Java 25 QUIET COMPONENTS Development)
endif()
set(navidog_jdbc_default OFF)
if(navidog_jdbc_native AND Java_FOUND AND Java_VERSION_MAJOR EQUAL 25)
    set(navidog_jdbc_default ON)
endif()
option(NAVIDOG_WITH_JDBC "Build the JDBC bridge using JDK 25" ${navidog_jdbc_default})
if(NOT NAVIDOG_WITH_JDBC)
    return()
endif()
if(NOT navidog_jdbc_native OR NOT Java_FOUND OR NOT Java_VERSION_MAJOR EQUAL 25)
    message(FATAL_ERROR "NAVIDOG_WITH_JDBC requires a supported native build and JDK 25 (set JAVA_HOME).")
endif()

get_filename_component(navidog_javac "${Java_JAVAC_EXECUTABLE}" REALPATH)
get_filename_component(navidog_java_bin "${navidog_javac}" DIRECTORY)
find_program(NAVIDOG_JLINK NAMES jlink HINTS "${navidog_java_bin}" REQUIRED NO_DEFAULT_PATH)
set(navidog_jdbc_dir "${CMAKE_CURRENT_BINARY_DIR}/jdbc")
set(navidog_jdbc_jar "${navidog_jdbc_dir}/navidog-jdbc-bridge.jar")
set(navidog_jdbc_stamp "${navidog_jdbc_dir}/runtime/release")
set(navidog_jdbc_classes "${CMAKE_CURRENT_BINARY_DIR}/jdbc-classes")

add_custom_command(OUTPUT "${navidog_jdbc_jar}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${navidog_jdbc_dir}" "${navidog_jdbc_classes}"
    COMMAND "${Java_JAVAC_EXECUTABLE}" -encoding UTF-8 -d "${navidog_jdbc_classes}"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/drivers/jdbc/Bridge.java"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/drivers/jdbc/DamengTls.java"
    COMMAND "${Java_JAR_EXECUTABLE}" --create --file "${navidog_jdbc_jar}" -C "${navidog_jdbc_classes}" .
    DEPENDS src/drivers/jdbc/Bridge.java src/drivers/jdbc/DamengTls.java "${Java_JAVAC_EXECUTABLE}"
    VERBATIM)
add_custom_target(navidog-jdbc DEPENDS "${navidog_jdbc_jar}")
add_dependencies(navidog navidog-jdbc)
if(NOT APPLE OR NOT CMAKE_OSX_ARCHITECTURES OR CMAKE_OSX_ARCHITECTURES STREQUAL CMAKE_HOST_SYSTEM_PROCESSOR)
    add_custom_command(OUTPUT "${navidog_jdbc_stamp}"
        COMMAND "${CMAKE_COMMAND}" -E rm -rf "${navidog_jdbc_dir}/runtime"
        COMMAND "${NAVIDOG_JLINK}" --add-modules java.sql.rowset,java.naming,java.management,java.desktop,java.security.jgss,jdk.crypto.ec,jdk.charsets,jdk.unsupported
            --strip-debug --no-header-files --no-man-pages --compress=zip-6 --output "${navidog_jdbc_dir}/runtime"
        DEPENDS "${NAVIDOG_JLINK}" "${CMAKE_CURRENT_SOURCE_DIR}/cmake/NavidogJdbc.cmake"
        VERBATIM)
    add_custom_target(navidog-jre DEPENDS "${navidog_jdbc_stamp}")
    add_dependencies(navidog-jdbc navidog-jre)
endif()
target_sources(navidog_core PRIVATE src/drivers/jdbc_bridge.cpp src/drivers/dameng.cpp src/drivers/jdbc_products.cpp)
target_compile_definitions(navidog_core PRIVATE NAVIDOG_HAS_JDBC=1)
if(APPLE)
    set(navidog_app_directory "${CMAKE_CURRENT_BINARY_DIR}")
    if(CMAKE_CONFIGURATION_TYPES)
        string(APPEND navidog_app_directory "/$<CONFIG>")
    endif()
    set(navidog_jdbc_app "${navidog_app_directory}/navidog.app/Contents/Resources/jdbc")
    add_custom_command(OUTPUT "${navidog_jdbc_app}/.bundled"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${navidog_jdbc_app}"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${navidog_jdbc_jar}" "${navidog_jdbc_app}"
        COMMAND "${CMAKE_COMMAND}" -E touch "${navidog_jdbc_app}/.bundled"
        DEPENDS "${navidog_jdbc_jar}"
        VERBATIM)
    add_custom_target(navidog-jdbc-app DEPENDS "${navidog_jdbc_app}/.bundled")
    add_dependencies(navidog-jdbc-app navidog-jdbc)
    add_dependencies(navidog navidog-jdbc-app)
endif()
if(APPLE)
    set(navidog_jdbc_install navidog.app/Contents/Resources/jdbc)
elseif(WIN32)
    set(navidog_jdbc_install bin/jdbc)
else()
    set(navidog_jdbc_install lib/navidog/jdbc)
endif()
install(FILES "${navidog_jdbc_jar}" DESTINATION "${navidog_jdbc_install}")

if(BUILD_TESTING AND NAVIDOG_BUILD_TESTS)
    add_executable(navidog-jdbc-tests tests/jdbc_test.cpp)
    target_include_directories(navidog-jdbc-tests PRIVATE src/drivers)
    target_link_libraries(navidog-jdbc-tests PRIVATE navidog_core ${NAVIDOG_QT}::Test)
    add_dependencies(navidog-jdbc-tests navidog-jdbc)
    add_test(NAME navidog-jdbc COMMAND navidog-jdbc-tests)
    set_tests_properties(navidog-jdbc PROPERTIES TIMEOUT 90)
endif()
