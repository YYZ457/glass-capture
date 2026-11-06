if {![info exists ::env(LINESCAN_BUILD)]} {error "Set LINESCAN_BUILD"}
set project $::env(LINESCAN_BUILD)
if {![file exists "$project/checked-interface.json"]} {error "Check synthesis first"}
cd [file dirname $project]
open_project [file tail $project]
open_solution solution1
export_design -rtl verilog -format ip_catalog -output "$project/preprocess_tile_ip.zip"
exit
