# 上层装配（framework/）—— 业务架构 + 类驱动
#
# 本层自包含：上层（project/CMakeLists.txt）只 add_subdirectory(framework)，
# 内部子目录由本文件负责。
#
# 子目录职责：
#   drivers/   类驱动（Pwm / Output / Input / Encoder / Can / Spi / UartDma / Rs485）
#              + 协议门面（Usb : Stream、EthercatSlave<Rx,Tx>）
#   algorithm/ PID / 滤波 / 卡尔曼 / Eigen
#   modules/   IMU / 电机 / 遥控 / 功率计
#   topic/     线程间数据契约
#   init/      注册式生命周期 + 线程模板
#   shell/     自研调试台
#   cmd/       buzzer / flash / fatal / linker
#   project/   业务工程（应用入口，由用户区 project 装配，不在此 add）
#   zephyr/    module.yml + 聚合 Kconfig（不参与编译）

add_subdirectory(drivers   ${CMAKE_CURRENT_BINARY_DIR}/drivers)
add_subdirectory(algorithm ${CMAKE_CURRENT_BINARY_DIR}/algorithm)
add_subdirectory(modules   ${CMAKE_CURRENT_BINARY_DIR}/modules)
add_subdirectory(topic     ${CMAKE_CURRENT_BINARY_DIR}/topic)
add_subdirectory(cmd       ${CMAKE_CURRENT_BINARY_DIR}/cmd)
add_subdirectory(shell     ${CMAKE_CURRENT_BINARY_DIR}/shell)
add_subdirectory(init      ${CMAKE_CURRENT_BINARY_DIR}/init)
