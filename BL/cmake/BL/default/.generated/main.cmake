include("${CMAKE_CURRENT_LIST_DIR}/rule.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/file.cmake")

set(BL_default_library_list )

set(OUTPUT_SUFFIX elf)
# Handle files with suffix s, for group default-XC32
if(BL_default_default_XC32_FILE_TYPE_assemble)
add_library(BL_default_default_XC32_assemble OBJECT ${BL_default_default_XC32_FILE_TYPE_assemble})
    BL_default_default_XC32_assemble_rule(BL_default_default_XC32_assemble)
    list(APPEND BL_default_library_list "$<TARGET_OBJECTS:BL_default_default_XC32_assemble>")

endif()

# Handle files with suffix S, for group default-XC32
if(BL_default_default_XC32_FILE_TYPE_assembleWithPreprocess)
add_library(BL_default_default_XC32_assembleWithPreprocess OBJECT ${BL_default_default_XC32_FILE_TYPE_assembleWithPreprocess})
    BL_default_default_XC32_assembleWithPreprocess_rule(BL_default_default_XC32_assembleWithPreprocess)
    list(APPEND BL_default_library_list "$<TARGET_OBJECTS:BL_default_default_XC32_assembleWithPreprocess>")

endif()

# Handle files with suffix [cC], for group default-XC32
if(BL_default_default_XC32_FILE_TYPE_compile)
add_library(BL_default_default_XC32_compile OBJECT ${BL_default_default_XC32_FILE_TYPE_compile})
    BL_default_default_XC32_compile_rule(BL_default_default_XC32_compile)
    list(APPEND BL_default_library_list "$<TARGET_OBJECTS:BL_default_default_XC32_compile>")

endif()

# Handle files with suffix cpp, for group default-XC32
if(BL_default_default_XC32_FILE_TYPE_compile_cpp)
add_library(BL_default_default_XC32_compile_cpp OBJECT ${BL_default_default_XC32_FILE_TYPE_compile_cpp})
    BL_default_default_XC32_compile_cpp_rule(BL_default_default_XC32_compile_cpp)
    list(APPEND BL_default_library_list "$<TARGET_OBJECTS:BL_default_default_XC32_compile_cpp>")

endif()

# Handle files with suffix [cC], for group default-XC32
if(BL_default_default_XC32_FILE_TYPE_dependentObject)
add_library(BL_default_default_XC32_dependentObject OBJECT ${BL_default_default_XC32_FILE_TYPE_dependentObject})
    BL_default_default_XC32_dependentObject_rule(BL_default_default_XC32_dependentObject)
    list(APPEND BL_default_library_list "$<TARGET_OBJECTS:BL_default_default_XC32_dependentObject>")

endif()


# Main target for this project
add_executable(BL_default_image_hY4iUbUP ${BL_default_library_list})

set_target_properties(BL_default_image_hY4iUbUP PROPERTIES
    OUTPUT_NAME "default"
    SUFFIX ".elf"
    RUNTIME_OUTPUT_DIRECTORY "${BL_default_output_dir}")
target_link_libraries(BL_default_image_hY4iUbUP PRIVATE ${BL_default_default_XC32_FILE_TYPE_link})
# Add the link options from the rule file.
BL_default_link_rule( BL_default_image_hY4iUbUP)

# Add bin2hex target for converting built file to a .hex file.
string(REGEX REPLACE [.]elf$ .hex BL_default_image_name_hex ${BL_default_image_name})
add_custom_target(BL_default_Bin2Hex ALL
    COMMAND ${MP_BIN2HEX} \"${BL_default_output_dir}/${BL_default_image_name}\"
    DEPENDS "${BL_default_output_dir}/${BL_default_image_name}"
    BYPRODUCTS ${BL_default_output_dir}/${BL_default_image_name_hex}
    COMMENT "Convert built file to .hex")
add_dependencies(BL_default_Bin2Hex BL_default_image_hY4iUbUP)




