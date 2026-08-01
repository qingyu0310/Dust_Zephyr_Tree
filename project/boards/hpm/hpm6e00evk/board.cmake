macro(app_set_runner_args)
  board_runner_args(openocd "--cmd-pre-init=adapter speed 4000")
endmacro()
