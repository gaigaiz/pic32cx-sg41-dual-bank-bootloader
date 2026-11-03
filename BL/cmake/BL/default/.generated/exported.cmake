set(DEPENDENT_MP_BIN2HEXBL_default_hY4iUbUP "c:/Program Files/Microchip/xc32/v5.10/bin/xc32-bin2hex.exe")
set(DEPENDENT_DEPENDENT_TARGET_ELFBL_default_hY4iUbUP ${CMAKE_CURRENT_LIST_DIR}/../../../../out/BL/default.elf)
set(DEPENDENT_TARGET_DIRBL_default_hY4iUbUP ${CMAKE_CURRENT_LIST_DIR}/../../../../out/BL)
set(DEPENDENT_BYPRODUCTSBL_default_hY4iUbUP ${DEPENDENT_TARGET_DIRBL_default_hY4iUbUP}/${sourceFileNameBL_default_hY4iUbUP}.c)
add_custom_command(
    OUTPUT ${DEPENDENT_TARGET_DIRBL_default_hY4iUbUP}/${sourceFileNameBL_default_hY4iUbUP}.c
    COMMAND ${DEPENDENT_MP_BIN2HEXBL_default_hY4iUbUP} --image ${DEPENDENT_DEPENDENT_TARGET_ELFBL_default_hY4iUbUP} --image-generated-c ${sourceFileNameBL_default_hY4iUbUP}.c --image-generated-h ${sourceFileNameBL_default_hY4iUbUP}.h --image-copy-mode ${modeBL_default_hY4iUbUP} --image-offset ${addressBL_default_hY4iUbUP} 
    WORKING_DIRECTORY ${DEPENDENT_TARGET_DIRBL_default_hY4iUbUP}
    DEPENDS ${DEPENDENT_DEPENDENT_TARGET_ELFBL_default_hY4iUbUP})
add_custom_target(
    dependent_produced_source_artifactBL_default_hY4iUbUP 
    DEPENDS ${DEPENDENT_TARGET_DIRBL_default_hY4iUbUP}/${sourceFileNameBL_default_hY4iUbUP}.c
    )
