# Invoke from any cwd with VISION_BUILD_DIR and optional VISION_HLS_MODE env.
set root [file dirname [file dirname [file normalize [info script]]]]
if {![info exists ::env(VISION_BUILD_DIR)]} { error "Set VISION_BUILD_DIR to a dedicated build directory" }
set build_dir $::env(VISION_BUILD_DIR)
set mode synth
if {[info exists ::env(VISION_HLS_MODE)]} { set mode $::env(VISION_HLS_MODE) }
if {$mode ni {verify synth cosim}} { error "mode must be verify, synth or cosim" }
file mkdir [file dirname $build_dir]
cd [file dirname $build_dir]
open_project -reset [file tail $build_dir]
set_top quant_engine
add_files "$root/hls/quant_engine.cpp" -cflags "-std=c++11"
if {$mode eq "cosim"} {
    add_files -tb "$root/tests/test_quant_cosim.cpp" -cflags "-std=c++11"
} else {
    add_files -tb "$root/tests/test_quant_engine.cpp" -cflags "-std=c++11"
}
open_solution -reset solution1 -flow_target vivado
set_part {xczu9eg-ffvb1156-2-e}
create_clock -period 10 -name default
config_interface -m_axi_addr64
csim_design -clean
if {$mode ne "verify"} { csynth_design }
if {$mode eq "cosim"} { cosim_design -rtl verilog }
exit
