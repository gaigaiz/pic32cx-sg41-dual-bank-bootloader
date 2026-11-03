include("${CMAKE_CURRENT_LIST_DIR}/rule.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/file.cmake")

set(APP_default_library_list )

set(OUTPUT_SUFFIX elf)
# Handle files with suffix s, for group default-XC32
if(APP_default_default_XC32_FILE_TYPE_assemble)
add_library(APP_default_default_XC32_assemble OBJECT ${APP_default_default_XC32_FILE_TYPE_assemble})
    APP_default_default_XC32_assemble_rule(APP_default_default_XC32_assemble)
    list(APPEND APP_default_library_list "$<TARGET_OBJECTS:APP_default_default_XC32_assemble>")

endif()

# Handle files with suffix S, for group default-XC32
if(APP_default_default_XC32_FILE_TYPE_assembleWithPreprocess)
add_library(APP_default_default_XC32_assembleWithPreprocess OBJECT ${APP_default_default_XC32_FILE_TYPE_assembleWithPreprocess})
    APP_default_default_XC32_assembleWithPreprocess_rule(APP_default_default_XC32_assembleWithPreprocess)
    list(APPEND APP_default_library_list "$<TARGET_OBJECTS:APP_default_default_XC32_assembleWithPreprocess>")

endif()

# Handle files with suffix [cC], for group default-XC32
if(APP_default_default_XC32_FILE_TYPE_compile)
add_library(APP_default_default_XC32_compile OBJECT ${APP_default_default_XC32_FILE_TYPE_compile})
    APP_default_default_XC32_compile_rule(APP_default_default_XC32_compile)
    list(APPEND APP_default_library_list "$<TARGET_OBJECTS:APP_default_default_XC32_compile>")

endif()

# Handle files with suffix cpp, for group default-XC32
if(APP_default_default_XC32_FILE_TYPE_compile_cpp)
add_library(APP_default_default_XC32_compile_cpp OBJECT ${APP_default_default_XC32_FILE_TYPE_compile_cpp})
    APP_default_default_XC32_compile_cpp_rule(APP_default_default_XC32_compile_cpp)
    list(APPEND APP_default_library_list "$<TARGET_OBJECTS:APP_default_default_XC32_compile_cpp>")

endif()

# Handle files with suffix [cC], for group default-XC32
if(APP_default_default_XC32_FILE_TYPE_dependentObject)
add_library(APP_default_default_XC32_dependentObject OBJECT ${APP_default_default_XC32_FILE_TYPE_dependentObject})
    APP_default_default_XC32_dependentObject_rule(APP_default_default_XC32_dependentObject)
    list(APPEND APP_default_library_list "$<TARGET_OBJECTS:APP_default_default_XC32_dependentObject>")

endif()


# Main target for this project
add_executable(APP_default_image_4rzmqGyB ${APP_default_library_list})

set_target_properties(APP_default_image_4rzmqGyB PROPERTIES
    OUTPUT_NAME "default"
    SUFFIX ".elf"
    RUNTIME_OUTPUT_DIRECTORY "${APP_default_output_dir}")
target_link_libraries(APP_default_image_4rzmqGyB PRIVATE ${APP_default_default_XC32_FILE_TYPE_link})
# Add the link options from the rule file.
APP_default_link_rule( APP_default_image_4rzmqGyB)

# Add bin2hex target for converting built file to a .hex file.
string(REGEX REPLACE [.]elf$ .hex APP_default_image_name_hex ${APP_default_image_name})
add_custom_target(APP_default_Bin2Hex ALL
    COMMAND ${MP_BIN2HEX} \"${APP_default_output_dir}/${APP_default_image_name}\"
    DEPENDS "${APP_default_output_dir}/${APP_default_image_name}"
    BYPRODUCTS ${APP_default_output_dir}/${APP_default_image_name_hex}
    COMMENT "Convert built file to .hex")
add_dependencies(APP_default_Bin2Hex APP_default_image_4rzmqGyB)




