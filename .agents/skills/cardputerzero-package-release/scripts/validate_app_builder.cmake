cmake_minimum_required(VERSION 3.19)

if(NOT DEFINED PROJECT_ROOT OR PROJECT_ROOT STREQUAL "")
    get_filename_component(
        PROJECT_ROOT
        "${CMAKE_CURRENT_LIST_DIR}/../../../.."
        ABSOLUTE
    )
endif()

if(NOT DEFINED APP_BUILDER_FILE OR APP_BUILDER_FILE STREQUAL "")
    set(APP_BUILDER_FILE "${PROJECT_ROOT}/app-builder.json")
endif()

set(validation_errors)

macro(add_validation_error text)
    list(APPEND validation_errors "${text}")
endmacro()

macro(json_get output)
    string(JSON _json_value ERROR_VARIABLE _json_error GET "${app_builder_json}" ${ARGN})
    if(NOT _json_error STREQUAL "NOTFOUND")
        add_validation_error("Missing or invalid JSON field '${ARGN}': ${_json_error}")
        set(${output} "")
    else()
        set(${output} "${_json_value}")
    endif()
endmacro()

if(NOT EXISTS "${APP_BUILDER_FILE}")
    message(FATAL_ERROR "app-builder manifest not found: ${APP_BUILDER_FILE}")
endif()

if(NOT EXISTS "${PROJECT_ROOT}/CMakeLists.txt")
    message(FATAL_ERROR "CMakeLists.txt not found under project root: ${PROJECT_ROOT}")
endif()

file(READ "${APP_BUILDER_FILE}" app_builder_json)
file(READ "${PROJECT_ROOT}/CMakeLists.txt" project_cmake)
file(READ "${PROJECT_ROOT}/cmake/cm0-package.cmake" package_cmake)

string(
    REGEX MATCH
    "project\\([ \t\r\n]*([A-Za-z0-9_.+-]+)[ \t\r\n]+VERSION[ \t\r\n]+([0-9]+(\\.[0-9]+)+)"
    project_match
    "${project_cmake}"
)
if(NOT project_match)
    add_validation_error("Unable to read project name and version from CMakeLists.txt")
else()
    set(cmake_project_version "${CMAKE_MATCH_2}")
endif()
string(
    REGEX MATCH
    "set\\(APP_DEBIAN_PACKAGE_NAME[ \t]+\"([a-z0-9.+-]+)\""
    package_name_match
    "${package_cmake}"
)
if(NOT package_name_match)
    add_validation_error("Unable to read APP_DEBIAN_PACKAGE_NAME from cmake/cm0-package.cmake")
else()
    set(debian_package_name "${CMAKE_MATCH_1}")
endif()

# Top-level packaging fields, plus the "store" section that czdev publish and
# the store's CI validate (see AppBuilder docs/APP_BUILDER_JSON.md).
foreach(required_field IN ITEMS
        app_name
        package_name
        version
        store)
    string(JSON field_type ERROR_VARIABLE field_error TYPE "${app_builder_json}" "${required_field}")
    if(NOT field_error STREQUAL "NOTFOUND")
        add_validation_error("Required field '${required_field}' is missing")
    endif()
endforeach()

foreach(required_field IN ITEMS
        summary
        categories
        screenshots
        icon
        license
        source_repo
        author
        share_code
        permissions)
    string(JSON field_type ERROR_VARIABLE field_error TYPE "${app_builder_json}" store "${required_field}")
    if(NOT field_error STREQUAL "NOTFOUND")
        add_validation_error("Required field 'store.${required_field}' is missing")
    endif()
endforeach()

string(JSON author_name_type ERROR_VARIABLE author_name_error TYPE "${app_builder_json}" store author display_name)
if(NOT author_name_error STREQUAL "NOTFOUND")
    add_validation_error("Required field 'store.author.display_name' is missing")
endif()

json_get(manifest_package_name package_name)
json_get(manifest_version version)
json_get(manifest_icon store icon)
json_get(manifest_share_code store share_code)

if(NOT manifest_package_name MATCHES "^[a-z0-9][a-z0-9.+-]*$")
    add_validation_error(
        "package_name '${manifest_package_name}' is not a valid lowercase Debian package name"
    )
endif()
if(DEFINED debian_package_name AND
   NOT manifest_package_name STREQUAL debian_package_name)
    add_validation_error(
        "package_name '${manifest_package_name}' does not match Debian package '${debian_package_name}'"
    )
endif()

if(DEFINED cmake_project_version AND
   NOT manifest_version STREQUAL cmake_project_version)
    add_validation_error(
        "version '${manifest_version}' does not match CMake project version '${cmake_project_version}'"
    )
endif()
if(NOT manifest_share_code MATCHES "^[A-Z0-9][A-Z0-9][A-Z0-9][A-Z0-9]$")
    add_validation_error("store.share_code must be exactly four uppercase letters or digits")
endif()

if(IS_ABSOLUTE "${manifest_icon}")
    set(manifest_icon_path "${manifest_icon}")
else()
    set(manifest_icon_path "${PROJECT_ROOT}/${manifest_icon}")
endif()
if(manifest_icon STREQUAL "" OR NOT EXISTS "${manifest_icon_path}")
    add_validation_error("Icon does not exist: ${manifest_icon}")
else()
    file(READ "${manifest_icon_path}" icon_header OFFSET 0 LIMIT 24 HEX)
    string(TOLOWER "${icon_header}" icon_header)
    if(NOT icon_header MATCHES "^89504e470d0a1a0a0000000d49484452")
        add_validation_error("Icon is not a valid PNG: ${manifest_icon}")
    else()
        string(SUBSTRING "${icon_header}" 32 8 icon_width_hex)
        string(SUBSTRING "${icon_header}" 40 8 icon_height_hex)
        math(EXPR icon_width "0x${icon_width_hex}")
        math(EXPR icon_height "0x${icon_height_hex}")
        if(icon_width LESS 128 OR icon_width GREATER 512 OR
           NOT icon_width EQUAL icon_height)
            add_validation_error(
                "Icon must be square and 128-512 pixels per side: ${manifest_icon} (${icon_width}x${icon_height})"
            )
        endif()
    endif()
endif()

string(JSON screenshot_count ERROR_VARIABLE screenshot_error LENGTH "${app_builder_json}" store screenshots)
if(NOT screenshot_error STREQUAL "NOTFOUND")
    add_validation_error("store.screenshots must be an array")
elseif(screenshot_count LESS 1)
    add_validation_error("store.screenshots must contain at least one file")
else()
    math(EXPR screenshot_last "${screenshot_count} - 1")
    foreach(index RANGE 0 ${screenshot_last})
        json_get(screenshot_path store screenshots ${index})
        if(IS_ABSOLUTE "${screenshot_path}")
            set(screenshot_absolute "${screenshot_path}")
        else()
            set(screenshot_absolute "${PROJECT_ROOT}/${screenshot_path}")
        endif()
        if(screenshot_path STREQUAL "" OR NOT EXISTS "${screenshot_absolute}")
            add_validation_error("Screenshot does not exist: ${screenshot_path}")
        else()
            file(READ "${screenshot_absolute}" screenshot_header OFFSET 0 LIMIT 24 HEX)
            string(TOLOWER "${screenshot_header}" screenshot_header)
            if(NOT screenshot_header MATCHES "^89504e470d0a1a0a0000000d49484452")
                add_validation_error("Screenshot is not a valid PNG: ${screenshot_path}")
            else()
                string(SUBSTRING "${screenshot_header}" 32 8 screenshot_width_hex)
                string(SUBSTRING "${screenshot_header}" 40 8 screenshot_height_hex)
                math(EXPR screenshot_width "0x${screenshot_width_hex}")
                math(EXPR screenshot_height "0x${screenshot_height_hex}")
                if(NOT screenshot_width EQUAL 320 OR NOT screenshot_height EQUAL 170)
                    add_validation_error(
                        "Screenshot must be exactly 320x170 pixels: ${screenshot_path} (${screenshot_width}x${screenshot_height})"
                    )
                endif()
            endif()
        endif()
    endforeach()
endif()

string(JSON category_count ERROR_VARIABLE category_error LENGTH "${app_builder_json}" store categories)
if(NOT category_error STREQUAL "NOTFOUND" OR category_count LESS 1)
    add_validation_error("store.categories must be a non-empty array")
elseif(category_count GREATER 2)
    add_validation_error("store.categories must contain at most 2 entries")
endif()
if(category_error STREQUAL "NOTFOUND" AND category_count GREATER 0)
    set(valid_categories
        "System Tools"
        "Development"
        "Hardware & IoT"
        "Security"
        "Radio & Comms"
        "AI"
        "Creative & Office"
        "Media"
        "Games"
        "Emulators"
        "Education"
        "Lifestyle"
        "Other"
    )
    math(EXPR category_last "${category_count} - 1")
    foreach(index RANGE 0 ${category_last})
        json_get(category store categories ${index})
        list(FIND valid_categories "${category}" category_index)
        if(category_index EQUAL -1)
            add_validation_error("Invalid store category '${category}'")
        endif()
    endforeach()
endif()

foreach(permission IN ITEMS
        camera
        microphone
        imu
        network
        additional_hardware
        background_service
        external_display)
    string(
        JSON permission_type
        ERROR_VARIABLE permission_error
        TYPE "${app_builder_json}" store permissions "${permission}"
    )
    if(NOT permission_error STREQUAL "NOTFOUND")
        add_validation_error("Permission 'store.permissions.${permission}' is missing")
    elseif(NOT permission_type STREQUAL "BOOLEAN")
        add_validation_error("Permission 'store.permissions.${permission}' must be boolean")
    endif()
endforeach()

if(DEFINED PACKAGE_FILE AND NOT PACKAGE_FILE STREQUAL "")
    if(IS_ABSOLUTE "${PACKAGE_FILE}")
        set(package_path "${PACKAGE_FILE}")
    else()
        set(package_path "${PROJECT_ROOT}/${PACKAGE_FILE}")
    endif()

    if(NOT EXISTS "${package_path}")
        add_validation_error("Package file does not exist: ${PACKAGE_FILE}")
    else()
        get_filename_component(package_filename "${package_path}" NAME)
        set(package_prefix "${manifest_package_name}_${manifest_version}-")
        string(FIND "${package_filename}" "${package_prefix}" package_prefix_index)
        string(LENGTH "${package_filename}" package_filename_length)
        string(LENGTH "${package_prefix}" package_prefix_length)
        if(package_filename_length GREATER_EQUAL 10)
            math(EXPR package_suffix_offset "${package_filename_length} - 10")
            string(SUBSTRING "${package_filename}" ${package_suffix_offset} 10 package_suffix)
        else()
            set(package_suffix "")
        endif()
        math(EXPR minimum_filename_length "${package_prefix_length} + 10")
        if(NOT package_prefix_index EQUAL 0 OR
           NOT package_suffix STREQUAL "_arm64.deb" OR
           package_filename_length LESS_EQUAL minimum_filename_length)
            add_validation_error(
                "Package filename must be <package_name>_<version>-<revision>_arm64.deb: ${package_filename}"
            )
        endif()

        find_program(DPKG_DEB_EXECUTABLE dpkg-deb)
        if(DPKG_DEB_EXECUTABLE)
            execute_process(
                COMMAND "${DPKG_DEB_EXECUTABLE}" -f "${package_path}" Version
                RESULT_VARIABLE version_result
                OUTPUT_VARIABLE package_version
                ERROR_VARIABLE version_error
                OUTPUT_STRIP_TRAILING_WHITESPACE
            )
            execute_process(
                COMMAND "${DPKG_DEB_EXECUTABLE}" -f "${package_path}" Architecture
                RESULT_VARIABLE architecture_result
                OUTPUT_VARIABLE package_architecture
                ERROR_VARIABLE architecture_error
                OUTPUT_STRIP_TRAILING_WHITESPACE
            )
            if(NOT version_result EQUAL 0)
                add_validation_error("dpkg-deb could not read package version: ${version_error}")
            else()
                set(package_revision_prefix "${manifest_version}-")
                string(FIND "${package_version}" "${package_revision_prefix}" revision_prefix_index)
                if(NOT package_version STREQUAL manifest_version AND
                   NOT revision_prefix_index EQUAL 0)
                    add_validation_error(
                        "Debian version '${package_version}' does not match manifest '${manifest_version}'"
                    )
                endif()
            endif()
            if(NOT architecture_result EQUAL 0)
                add_validation_error(
                    "dpkg-deb could not read package architecture: ${architecture_error}"
                )
            elseif(NOT package_architecture STREQUAL "arm64")
                add_validation_error(
                    "Debian architecture '${package_architecture}' is not arm64"
                )
            endif()
        else()
            message(STATUS "dpkg-deb not found; skipped package control-field validation")
        endif()
    endif()
endif()

if(validation_errors)
    list(JOIN validation_errors "\n  - " validation_error_text)
    message(FATAL_ERROR "app-builder/package validation failed:\n  - ${validation_error_text}")
endif()

message(STATUS "app-builder/package validation passed")
