find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(SHADER_WARMUP_HEADER "${CMAKE_CURRENT_BINARY_DIR}/shader_warmup_catalog.inc")
add_custom_command(OUTPUT "${SHADER_WARMUP_HEADER}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/Generate-Shader-Warmup.py"
        "${CMAKE_CURRENT_LIST_DIR}/../data/shader-warmup.json" "${SHADER_WARMUP_HEADER}"
    DEPENDS "${CMAKE_CURRENT_LIST_DIR}/Generate-Shader-Warmup.py"
        "${CMAKE_CURRENT_LIST_DIR}/../data/shader-warmup.json"
    VERBATIM)
