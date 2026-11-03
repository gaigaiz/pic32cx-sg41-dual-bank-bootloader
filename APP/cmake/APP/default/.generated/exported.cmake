set(DEPENDENT_MP_BIN2HEXAPP_default_4rzmqGyB "c:/Program Files/Microchip/xc32/v5.10/bin/xc32-bin2hex.exe")
set(DEPENDENT_DEPENDENT_TARGET_ELFAPP_default_4rzmqGyB ${CMAKE_CURRENT_LIST_DIR}/../../../../out/APP/default.elf)
set(DEPENDENT_TARGET_DIRAPP_default_4rzmqGyB ${CMAKE_CURRENT_LIST_DIR}/../../../../out/APP)
set(DEPENDENT_BYPRODUCTSAPP_default_4rzmqGyB ${DEPENDENT_TARGET_DIRAPP_default_4rzmqGyB}/${sourceFileNameAPP_default_4rzmqGyB}.c)
add_custom_command(
    OUTPUT ${DEPENDENT_TARGET_DIRAPP_default_4rzmqGyB}/${sourceFileNameAPP_default_4rzmqGyB}.c
    COMMAND ${DEPENDENT_MP_BIN2HEXAPP_default_4rzmqGyB} --image ${DEPENDENT_DEPENDENT_TARGET_ELFAPP_default_4rzmqGyB} --image-generated-c ${sourceFileNameAPP_default_4rzmqGyB}.c --image-generated-h ${sourceFileNameAPP_default_4rzmqGyB}.h --image-copy-mode ${modeAPP_default_4rzmqGyB} --image-offset ${addressAPP_default_4rzmqGyB} 
    WORKING_DIRECTORY ${DEPENDENT_TARGET_DIRAPP_default_4rzmqGyB}
    DEPENDS ${DEPENDENT_DEPENDENT_TARGET_ELFAPP_default_4rzmqGyB})
add_custom_target(
    dependent_produced_source_artifactAPP_default_4rzmqGyB 
    DEPENDS ${DEPENDENT_TARGET_DIRAPP_default_4rzmqGyB}/${sourceFileNameAPP_default_4rzmqGyB}.c
    )
