message(STATUS "Creating XBOX ISO directory: ${XBOX_ISO_DIR}")
file(MAKE_DIRECTORY ${XBOX_ISO_DIR})

message(STATUS "Copying assets to XBOX ISO directory")
file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/assets/"
     DESTINATION "${XBOX_ISO_DIR}")

#Convert the exe to xbe
add_custom_target(cxbe_convert ALL
    COMMENT "CXBE Conversion: [EXE -> XBE]"
    VERBATIM COMMAND "${CMAKE_COMMAND}" -E env ${NXDK_DIR}/tools/cxbe/cxbe
                        -OUT:${CMAKE_CURRENT_BINARY_DIR}/default.xbe
                        -TITLE:${XBE_TITLE} ${CMAKE_CURRENT_BINARY_DIR}/${XBE_TITLE}.exe > NUL 2>&1
)
add_dependencies(cxbe_convert sdl3-test)

#Generate the Xbox ISO
add_custom_target(xbe_iso ALL
    COMMENT "XISO Conversion: [XBE -> XISO]"
    COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_BINARY_DIR}/default.xbe" "${XBOX_ISO_DIR}/default.xbe"
    WORKING_DIRECTORY ${XBOX_ISO_DIR}
    VERBATIM COMMAND "${CMAKE_COMMAND}" -E env ${NXDK_DIR}/tools/extract-xiso/build/extract-xiso -c ${XBOX_ISO_DIR} ${CMAKE_CURRENT_BINARY_DIR}/${XBE_TITLE}.iso
)
add_dependencies(xbe_iso cxbe_convert)

set_target_properties(cxbe_convert PROPERTIES OUTPUT_QUIET ON)
set_target_properties(xbe_iso PROPERTIES OUTPUT_QUIET ON)
