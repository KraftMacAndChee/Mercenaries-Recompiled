if(NOT EXISTS "${DESTINATION}")
    configure_file("${SOURCE}" "${DESTINATION}" COPYONLY)
endif()
