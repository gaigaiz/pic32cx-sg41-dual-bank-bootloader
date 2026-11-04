# The following variables contains the files used by the different stages of the build process.
set(APP_default_default_XC32_FILE_TYPE_assemble)
set_source_files_properties(${APP_default_default_XC32_FILE_TYPE_assemble} PROPERTIES LANGUAGE ASM)

# For assembly files, add "." to the include path for each file so that .include with a relative path works
foreach(source_file ${APP_default_default_XC32_FILE_TYPE_assemble})
        set_source_files_properties(${source_file} PROPERTIES INCLUDE_DIRECTORIES "$<PATH:NORMAL_PATH,$<PATH:REMOVE_FILENAME,${source_file}>>")
endforeach()

set(APP_default_default_XC32_FILE_TYPE_assembleWithPreprocess)
set_source_files_properties(${APP_default_default_XC32_FILE_TYPE_assembleWithPreprocess} PROPERTIES LANGUAGE ASM)

# For assembly files, add "." to the include path for each file so that .include with a relative path works
foreach(source_file ${APP_default_default_XC32_FILE_TYPE_assembleWithPreprocess})
        set_source_files_properties(${source_file} PROPERTIES INCLUDE_DIRECTORIES "$<PATH:NORMAL_PATH,$<PATH:REMOVE_FILENAME,${source_file}>>")
endforeach()

set(APP_default_default_XC32_FILE_TYPE_compile
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../config.mcc/src/config/default/exceptions.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../config.mcc/src/config/default/initialization.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../config.mcc/src/config/default/interrupts.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../config.mcc/src/config/default/libc_syscalls.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../config.mcc/src/config/default/peripheral/clock/plib_clock.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../config.mcc/src/config/default/peripheral/cmcc/plib_cmcc.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../config.mcc/src/config/default/peripheral/evsys/plib_evsys.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../config.mcc/src/config/default/peripheral/nvic/plib_nvic.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../config.mcc/src/config/default/peripheral/nvmctrl/plib_nvmctrl.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../config.mcc/src/config/default/peripheral/port/plib_port.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../config.mcc/src/config/default/peripheral/sercom/usart/plib_sercom4_usart.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../config.mcc/src/config/default/peripheral/wdt/plib_wdt.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../config.mcc/src/config/default/startup_xc32.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../config.mcc/src/config/default/stdio/xc32_monitor.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../config.mcc/src/main.c")
set_source_files_properties(${APP_default_default_XC32_FILE_TYPE_compile} PROPERTIES LANGUAGE C)
set(APP_default_default_XC32_FILE_TYPE_compile_cpp)
set_source_files_properties(${APP_default_default_XC32_FILE_TYPE_compile_cpp} PROPERTIES LANGUAGE CXX)
set(APP_default_default_XC32_FILE_TYPE_link)

# The linker script used for the build.
set(APP_default_LINKER_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/../../../config.mcc/src/config/default/PIC32CX1025SG41128.ld")
set(APP_default_image_name "default.elf")
set(APP_default_image_base_name "default")

# The output directory of the final image.
set(APP_default_output_dir "${CMAKE_CURRENT_SOURCE_DIR}/../../../out/APP")

# The full path to the final image.
set(APP_default_full_path_to_image ${APP_default_output_dir}/${APP_default_image_name})
