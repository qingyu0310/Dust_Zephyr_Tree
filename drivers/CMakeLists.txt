if(CONFIG_DUST_DEV_FSMC)
    target_sources(app PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/fsmc/fsmc.cpp
    )
    target_include_directories(app PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/fsmc
    )
endif()
