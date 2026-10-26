message("Client android ${CMAKE_ANDROID_ARCH_ABI} build")

if(NOT DEFINED APP_ANDROID_MIN_SDK)
    set(APP_ANDROID_MIN_SDK 28)
endif()

set(ANDROID_PLATFORM "android-${APP_ANDROID_MIN_SDK}" CACHE STRING
    "The minimum API level supported by the application or library" FORCE)

# set QTP0002 policy: target properties that specify Android-specific paths may contain generator expressions
qt_policy(SET QTP0002 NEW)

set_target_properties(${PROJECT} PROPERTIES
    QT_ANDROID_VERSION_NAME ${LESHA_FULL_VERSION}
    QT_ANDROID_VERSION_CODE ${LESHA_ANDROID_VERSION_CODE}
    QT_ANDROID_MIN_SDK_VERSION ${APP_ANDROID_MIN_SDK}
    QT_ANDROID_TARGET_SDK_VERSION 36
    QT_ANDROID_SDK_BUILD_TOOLS_REVISION 36.0.0
)

# Fork: COORDINATOR_API_URL too, else the non-main ABI (armeabi-v7a) is built without the baked-in URL
set(QT_ANDROID_MULTI_ABI_FORWARD_VARS "QT_NO_GLOBAL_APK_TARGET_PART_OF_ALL;CMAKE_BUILD_TYPE;COORDINATOR_API_URL")

# We need to include qtprivate api's
# As QAndroidBinder is not yet implemented with a public api
# Check if Qt6::CorePrivate is available (may not be in all Qt versions/configurations)
if(TARGET Qt6::CorePrivate)
    set(LIBS ${LIBS} Qt6::CorePrivate)
endif()
set(LIBS ${LIBS} -ljnigraphics)

link_directories(${CMAKE_CURRENT_SOURCE_DIR}/platforms/android)

set(HEADERS ${HEADERS}
    ${CMAKE_CURRENT_SOURCE_DIR}/platforms/android/android_controller.h
    ${CMAKE_CURRENT_SOURCE_DIR}/platforms/android/android_utils.h
    ${CMAKE_CURRENT_SOURCE_DIR}/core/protocols/androidVpnProtocol.h
    ${CMAKE_CURRENT_SOURCE_DIR}/core/utils/installedAppsImageProvider.h
)

set(SOURCES ${SOURCES}
    ${CMAKE_CURRENT_SOURCE_DIR}/platforms/android/android_controller.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/platforms/android/android_utils.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/core/protocols/androidVpnProtocol.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/core/utils/installedAppsImageProvider.cpp
)


find_package(awg-android REQUIRED)
set(LIBS ${LIBS} amnezia::awg-android)
set_property(TARGET ${PROJECT} APPEND PROPERTY QT_ANDROID_EXTRA_LIBS ${AMNEZIA_ANDROID_LIBWG_PATH} ${AMNEZIA_ANDROID_LIBWG_QUICK_PATH})

set(APP_ANDROID_PACKAGE_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/android)

if(APP_ANDROID_MAX_SDK)
    set(APP_ANDROID_PACKAGE_SOURCE_DIR ${CMAKE_CURRENT_BINARY_DIR}/android-package-source)
    file(REMOVE_RECURSE ${APP_ANDROID_PACKAGE_SOURCE_DIR})
    file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/android/ DESTINATION ${APP_ANDROID_PACKAGE_SOURCE_DIR})

    set(manifest_path ${APP_ANDROID_PACKAGE_SOURCE_DIR}/AndroidManifest.xml)
    set(manifest_anchor "android:installLocation=\"auto\">")
    file(READ ${manifest_path} manifest_contents)
    string(REPLACE
        "${manifest_anchor}"
        "${manifest_anchor}\n\n    <uses-sdk android:maxSdkVersion=\"${APP_ANDROID_MAX_SDK}\" />"
        patched_contents "${manifest_contents}")
    if(patched_contents STREQUAL manifest_contents)
        message(FATAL_ERROR
            "Failed to set maxSdkVersion=${APP_ANDROID_MAX_SDK}: anchor '${manifest_anchor}' "
            "not found in ${CMAKE_CURRENT_SOURCE_DIR}/android/AndroidManifest.xml")
    endif()
    file(WRITE ${manifest_path} "${patched_contents}")
endif()

set_property(TARGET ${PROJECT} PROPERTY QT_ANDROID_PACKAGE_SOURCE_DIR ${APP_ANDROID_PACKAGE_SOURCE_DIR})

if(QT_USE_TARGET_ANDROID_BUILD_DIR)
    set(_android_build_dir "${CMAKE_CURRENT_BINARY_DIR}/android-build-${PROJECT}")
else()
    set(_android_build_dir "${CMAKE_CURRENT_BINARY_DIR}/android-build")
endif()

add_custom_target(android_gradle_clean
    COMMAND ./gradlew clean
    WORKING_DIRECTORY "${_android_build_dir}"
    COMMENT "Cleaning Android Gradle build cache"
)
