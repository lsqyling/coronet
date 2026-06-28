# ============================================================
# Install rules
# ============================================================

include(GNUInstallDirs)

install(TARGETS coronet
    EXPORT coronet_targets
    ARCHIVE  DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY  DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME  DESTINATION ${CMAKE_INSTALL_BINDIR})

install(DIRECTORY include/coronet
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    FILES_MATCHING PATTERN "*.hpp")

install(EXPORT coronet_targets
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/coronet)
