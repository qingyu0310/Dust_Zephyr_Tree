# 业务线程装配 — 门禁 TRD_XXX 保持原名（不加 DUST_ 前缀）

if(CONFIG_TRD_GPIO)
	target_sources(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/gpio/trd_gpio.cpp)
	target_include_directories(app PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}
		${CMAKE_CURRENT_SOURCE_DIR}/gpio
	)
endif()

if(CONFIG_TRD_CHASSIS)
	target_sources(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/chassis/trd_chassis.cpp)
	target_include_directories(app PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}
		${CMAKE_CURRENT_SOURCE_DIR}/chassis
	)
endif()

if(CONFIG_TRD_GIMBAL)
	target_sources(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/gimbal/trd_gimbal.cpp)
	target_include_directories(app PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}
		${CMAKE_CURRENT_SOURCE_DIR}/gimbal
	)
endif()

if(CONFIG_TRD_REMOTE)
	target_sources(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/remote/trd_remote.cpp)
	target_include_directories(app PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}
		${CMAKE_CURRENT_SOURCE_DIR}/remote
	)
endif()

if(CONFIG_TRD_IMU)
	target_sources(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/imu/trd_imu.cpp)
	target_include_directories(app PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}
		${CMAKE_CURRENT_SOURCE_DIR}/imu
	)
endif()

if(CONFIG_TRD_CAN_TX)
	target_sources(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/can/trd_can_tx.cpp)
	target_include_directories(app PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}
		${CMAKE_CURRENT_SOURCE_DIR}/can
	)
endif()

if(CONFIG_TRD_PC)
	target_sources(app PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}/pc/trd_pc.cpp
	)
	target_include_directories(app PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}
		${CMAKE_CURRENT_SOURCE_DIR}/pc
	)
endif()

if(CONFIG_TRD_TEST)
	target_sources(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/test/trd_test.cpp)
	target_include_directories(app PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}
		${CMAKE_CURRENT_SOURCE_DIR}/test
	)
endif()
