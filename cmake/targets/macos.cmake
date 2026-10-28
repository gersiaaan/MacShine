# macos specific target definitions

if (SUNSHINE_BUILD_HOMEBREW)
    target_link_options(sunshine PRIVATE LINKER:-sectcreate,__TEXT,__info_plist,${APPLE_PLIST_FILE})
else()
    # .app build
    set_target_properties(sunshine PROPERTIES
            OUTPUT_NAME "${CMAKE_PROJECT_NAME}"
            MACOSX_BUNDLE_BUNDLE_NAME "${CMAKE_PROJECT_NAME}"
            MACOSX_BUNDLE_GUI_IDENTIFIER "${PROJECT_FQDN}"
            MACOSX_BUNDLE_INFO_PLIST "${APPLE_PLIST_FILE}"
            MACOSX_BUNDLE_ICON_FILE "sunshine.icns"
            MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
            MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}")

    # Populate bundle resources in the build tree for local runs.
    set(_bundle_resources_dir "$<TARGET_FILE_DIR:sunshine>/../Resources")
    add_custom_command(TARGET sunshine POST_BUILD
            COMMENT "Copying bundle resources to build tree"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${_bundle_resources_dir}"
            COMMAND "${CMAKE_COMMAND}" -E copy_directory "${CMAKE_BINARY_DIR}/assets" "${_bundle_resources_dir}/assets"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    "${PROJECT_SOURCE_DIR}/src_assets/macos/build/sunshine.icns"
                    "${_bundle_resources_dir}/sunshine.icns"
            VERBATIM)
endif()

# Tell linker to dynamically load these symbols at runtime, in case they're unavailable:
target_link_options(sunshine PRIVATE -Wl,-U,_CGPreflightScreenCaptureAccess -Wl,-U,_CGRequestScreenCaptureAccess)

# Virtual display helper: owns one CGVirtualDisplay per stream session (see src/platform/macos/vd_helper.m).
# It lives next to the sunshine executable, which is where Sunshine looks for it at runtime.
add_executable(sunshine_vd_helper "${CMAKE_SOURCE_DIR}/src/platform/macos/vd_helper.m")
target_compile_options(sunshine_vd_helper PRIVATE -fobjc-arc -Wall)
target_link_libraries(sunshine_vd_helper PRIVATE "-framework Foundation" "-framework CoreGraphics")
add_dependencies(sunshine sunshine_vd_helper)
add_custom_command(TARGET sunshine POST_BUILD
        COMMENT "Copying virtual display helper next to sunshine"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "$<TARGET_FILE:sunshine_vd_helper>"
                "$<TARGET_FILE_DIR:sunshine>/sunshine_vd_helper"
        VERBATIM)
