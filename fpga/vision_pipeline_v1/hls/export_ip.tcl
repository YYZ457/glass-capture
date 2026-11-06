# Export an already checked synthesis project; never rebuild or reset it here.
if {![info exists ::env(VISION_BUILD_DIR)]} { error "Set VISION_BUILD_DIR" }
set project $::env(VISION_BUILD_DIR)
if {![file exists "$project/checked-interface.json"]} { error "Run build-hls verification first" }
cd [file dirname $project]
open_project [file tail $project]
open_solution solution1
export_design -rtl verilog -format ip_catalog -output "$project/quant_engine_ip.zip"
exit
