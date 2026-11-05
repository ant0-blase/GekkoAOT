cmake_minimum_required(VERSION 3.20)
project(GekkoAOTByteReverse C CXX)
set(DOLRECOMP_ENABLE_LLVM ON CACHE BOOL "" FORCE)
add_subdirectory("${DOLRECOMP_SOURCE}" dolrecomp EXCLUDE_FROM_ALL)
add_executable(native_boundary_fixture boundary_fixture.cpp)
target_link_libraries(native_boundary_fixture PRIVATE dr_llvm)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/boundary-manifest.txt"
     "0x80018400 OSGetTime 0xfffc0000\n")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/boundary-glue")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/boundary-glue/generated.h"
     "#include \"cpu/cpu.h\"\nint dolrecomp_call(CPUState*,u32);\n#define DOLRECOMP_ENTRY_POINT 0x80018000u\n#define MODULE_GAME_ID \"TST000\"\n")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/boundary-glue/module_tables.inc"
     "static const GekkoAOTRange s_code_ranges[]={{0x80018000,0x80018408}};\n#define GEKKOAOT_CODE_RANGE_COUNT 1\n#define s_smc_ranges NULL\n#define GEKKOAOT_SMC_RANGE_COUNT 0\n#define s_chunk_ranges NULL\n#define s_chunk_hashes NULL\n#define GEKKOAOT_CHUNK_RANGE_COUNT 0\n")
foreach(mode IN ITEMS 0 1 2 3 4 5 6 7)
  set(probes "${CMAKE_CURRENT_BINARY_DIR}/boundary-${mode}.o")
  add_custom_command(OUTPUT "${probes}"
    COMMAND ${CMAKE_COMMAND} -E env
      "DOLRECOMP_STATIC_INTERCEPTS=${CMAKE_CURRENT_BINARY_DIR}/boundary-manifest.txt"
      "DOLRECOMP_NATIVE_SDK_CLOCK_LOWERING=0"
      $<TARGET_FILE:native_boundary_fixture> "${probes}" ${mode}
    DEPENDS native_boundary_fixture "${CMAKE_CURRENT_BINARY_DIR}/boundary-manifest.txt" VERBATIM)
  set_source_files_properties("${probes}" PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)
  add_executable(native_boundary_check_${mode} boundary_check.c boundary_dispatch.c
                 ../../../runtime/module/module_export.c "${probes}")
  target_include_directories(native_boundary_check_${mode} PRIVATE
      "${CMAKE_CURRENT_BINARY_DIR}/boundary-glue" ../../../runtime)
  target_link_libraries(native_boundary_check_${mode} PRIVATE dr_cpu)
endforeach()
add_executable(byte_reverse_fixture fixture.cpp)
add_executable(flat_vmem_fixture flat_vmem_fixture.cpp)
target_link_libraries(flat_vmem_fixture PRIVATE dr_llvm)
foreach(mode IN ITEMS 0 1 2 3)
  foreach(lower IN ITEMS 0 1)
    set(flat_object "${CMAKE_CURRENT_BINARY_DIR}/flat-${mode}-${lower}.o")
    add_custom_command(OUTPUT "${flat_object}"
      COMMAND ${CMAKE_COMMAND} -E env "DOLRECOMP_NATIVE_ADDRESS_LOWERING=1" "DOLRECOMP_NATIVE_LOCKED_CACHE=0"
        "DOLRECOMP_NATIVE_FLAT_VMEM=${lower}"
        $<TARGET_FILE:flat_vmem_fixture> "${flat_object}" ${mode} ${lower}
      DEPENDS flat_vmem_fixture VERBATIM)
    set_source_files_properties("${flat_object}" PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)
    add_executable(flat_vmem_check_${mode}_${lower} flat_vmem_check.cpp "${flat_object}")
    target_compile_features(flat_vmem_check_${mode}_${lower} PRIVATE cxx_std_20)
    target_compile_definitions(flat_vmem_check_${mode}_${lower} PRIVATE FLAT_LOWERED=${lower})
    target_include_directories(flat_vmem_check_${mode}_${lower} PRIVATE ../../../runtime)
    target_link_libraries(flat_vmem_check_${mode}_${lower} PRIVATE dr_cpu)
  endforeach()
endforeach()
target_link_libraries(byte_reverse_fixture PRIVATE dr_llvm)
foreach(mode IN ITEMS 0 1)
  set(probes "${CMAKE_CURRENT_BINARY_DIR}/probes-${mode}.o")
  add_custom_command(OUTPUT "${probes}"
    COMMAND byte_reverse_fixture "${probes}" ${mode}
    DEPENDS byte_reverse_fixture VERBATIM)
  set_source_files_properties("${probes}" PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)
  add_executable(byte_reverse_check_${mode} check.c "${probes}")
  target_link_libraries(byte_reverse_check_${mode} PRIVATE dr_cpu)
endforeach()
add_executable(sdk_clock_fixture clock_fixture.cpp)
target_link_libraries(sdk_clock_fixture PRIVATE dr_llvm)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/clock-manifest.txt"
     "0x80014100 OSGetTime 0xfffc0000\n0x80014200 OSGetTick 0xfffc0001\n")
foreach(mode IN ITEMS 0 1 2 3)
  foreach(lower IN ITEMS 0 1)
    set(probes "${CMAKE_CURRENT_BINARY_DIR}/clock-${mode}-${lower}.o")
    add_custom_command(OUTPUT "${probes}"
      COMMAND ${CMAKE_COMMAND} -E env
        "DOLRECOMP_STATIC_INTERCEPTS=${CMAKE_CURRENT_BINARY_DIR}/clock-manifest.txt"
        "DOLRECOMP_NATIVE_SDK_CLOCK_LOWERING=${lower}"
        $<TARGET_FILE:sdk_clock_fixture> "${probes}" ${mode} ${lower}
      DEPENDS sdk_clock_fixture "${CMAKE_CURRENT_BINARY_DIR}/clock-manifest.txt" VERBATIM)
    set_source_files_properties("${probes}" PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)
    add_executable(sdk_clock_check_${mode}_${lower} clock_check.c "${probes}")
    target_compile_definitions(sdk_clock_check_${mode}_${lower} PRIVATE CLOCK_LOWERED=${lower} CLOCK_MODE=${mode})
    target_link_libraries(sdk_clock_check_${mode}_${lower} PRIVATE dr_cpu)
  endforeach()
endforeach()
add_executable(store_restart_fixture restart_fixture.cpp)
target_link_libraries(store_restart_fixture PRIVATE dr_llvm)
foreach(mode IN ITEMS 0 1 2 3)
  set(probes "${CMAKE_CURRENT_BINARY_DIR}/restart-${mode}.o")
  add_custom_command(OUTPUT "${probes}"
    COMMAND store_restart_fixture "${probes}" ${mode}
    DEPENDS store_restart_fixture VERBATIM)
  set_source_files_properties("${probes}" PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)
  add_executable(store_restart_check_${mode} restart_check.c "${probes}")
  target_link_libraries(store_restart_check_${mode} PRIVATE dr_cpu)
endforeach()
add_executable(native_hardware_fixture hardware_fixture.cpp)
target_link_libraries(native_hardware_fixture PRIVATE dr_llvm)
foreach(mode IN ITEMS 0 1 2 3)
  foreach(lower IN ITEMS 0 1 2)
    if(lower EQUAL 0)
      set(hardware 0)
    else()
      set(hardware 1)
    endif()
    if(lower EQUAL 2)
      set(wgtry 1)
    else()
      set(wgtry 0)
    endif()
    set(probes "${CMAKE_CURRENT_BINARY_DIR}/hardware-${mode}-${lower}.o")
    add_custom_command(OUTPUT "${probes}"
      COMMAND ${CMAKE_COMMAND} -E env "DOLRECOMP_NATIVE_HARDWARE_LOWERING=${hardware}"
        "DOLRECOMP_NATIVE_WGPIPE_TRY_LOWERING=${wgtry}"
        $<TARGET_FILE:native_hardware_fixture> "${probes}" ${mode} ${lower}
      DEPENDS native_hardware_fixture VERBATIM)
    set_source_files_properties("${probes}" PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)
    add_executable(native_hardware_check_${mode}_${lower} hardware_check.c "${probes}")
    target_compile_definitions(native_hardware_check_${mode}_${lower} PRIVATE HARDWARE_LOWERED=${lower} HARDWARE_MODE=${mode})
    target_include_directories(native_hardware_check_${mode}_${lower} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../../runtime")
    target_link_libraries(native_hardware_check_${mode}_${lower} PRIVATE dr_cpu)
  endforeach()
endforeach()
add_executable(wgpipe_partial_fixture partial_fixture.cpp)
target_link_libraries(wgpipe_partial_fixture PRIVATE dr_llvm)
foreach(mode IN ITEMS 0 1 2 3)
  foreach(lower IN ITEMS 0 1)
    set(probes "${CMAKE_CURRENT_BINARY_DIR}/partial-${mode}-${lower}.o")
    add_custom_command(OUTPUT "${probes}"
      COMMAND ${CMAKE_COMMAND} -E env "DOLRECOMP_NATIVE_HARDWARE_LOWERING=${lower}"
        "DOLRECOMP_NATIVE_WGPIPE_TRY_LOWERING=${lower}"
        $<TARGET_FILE:wgpipe_partial_fixture> "${probes}" ${mode}
      DEPENDS wgpipe_partial_fixture VERBATIM)
    set_source_files_properties("${probes}" PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)
    add_executable(wgpipe_partial_check_${mode}_${lower} partial_check.c "${probes}")
    target_compile_definitions(wgpipe_partial_check_${mode}_${lower} PRIVATE PARTIAL_LOWERED=${lower})
    target_include_directories(wgpipe_partial_check_${mode}_${lower} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../../runtime")
    target_link_libraries(wgpipe_partial_check_${mode}_${lower} PRIVATE dr_cpu)
  endforeach()
endforeach()

add_executable(stable_mmio_fixture stable_fixture.cpp)
target_link_libraries(stable_mmio_fixture PRIVATE dr_llvm)
foreach(mode IN ITEMS 0 1 2 3)
  foreach(lower IN ITEMS 0 1)
    set(probes "${CMAKE_CURRENT_BINARY_DIR}/stable-${mode}-${lower}.o")
    add_custom_command(OUTPUT "${probes}"
      COMMAND ${CMAKE_COMMAND} -E env "DOLRECOMP_NATIVE_HARDWARE_LOWERING=${lower}"
        $<TARGET_FILE:stable_mmio_fixture> "${probes}" ${mode}
      DEPENDS stable_mmio_fixture VERBATIM)
    set_source_files_properties("${probes}" PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)
    add_executable(stable_mmio_check_${mode}_${lower} stable_check.c "${probes}")
    target_compile_definitions(stable_mmio_check_${mode}_${lower} PRIVATE STABLE_MODE=${mode} STABLE_LOWERED=${lower})
    target_include_directories(stable_mmio_check_${mode}_${lower} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../../runtime")
    target_link_libraries(stable_mmio_check_${mode}_${lower} PRIVATE dr_cpu)
  endforeach()
endforeach()

add_executable(rfi_pending_check rfi_check.c)
target_link_libraries(rfi_pending_check PRIVATE dr_cpu)
foreach(mode IN ITEMS 0 1)
  foreach(lower IN ITEMS 0 1)
    set(address_object "${CMAKE_CURRENT_BINARY_DIR}/address-${mode}-${lower}.o")
    add_custom_command(OUTPUT "${address_object}"
      COMMAND ${CMAKE_COMMAND} -E env "DOLRECOMP_NATIVE_ADDRESS_LOWERING=${lower}" "DOLRECOMP_NATIVE_LOCKED_CACHE=0"
        $<TARGET_FILE:byte_reverse_fixture> "${address_object}" ${mode}
      DEPENDS byte_reverse_fixture VERBATIM)
    set_source_files_properties("${address_object}" PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)
    add_executable(native_address_check_${mode}_${lower} address_check.cpp
      ../../../runtime/native/guest_address_window.cpp "${address_object}")
    target_compile_features(native_address_check_${mode}_${lower} PRIVATE cxx_std_20)
    target_include_directories(native_address_check_${mode}_${lower} PRIVATE ../../../runtime)
    target_link_libraries(native_address_check_${mode}_${lower} PRIVATE dr_cpu)
  endforeach()
endforeach()

add_executable(locked_cache_fixture locked_cache_fixture.cpp)
target_link_libraries(locked_cache_fixture PRIVATE dr_llvm)
foreach(mode IN ITEMS 0 1 2 3)
  foreach(lower IN ITEMS 0 1)
    set(lc_object "${CMAKE_CURRENT_BINARY_DIR}/lc-${mode}-${lower}.o")
    add_custom_command(OUTPUT "${lc_object}"
      COMMAND ${CMAKE_COMMAND} -E env "DOLRECOMP_NATIVE_ADDRESS_LOWERING=1"
        "DOLRECOMP_NATIVE_FLAT_VMEM=0" "DOLRECOMP_NATIVE_LOCKED_CACHE=${lower}"
        $<TARGET_FILE:locked_cache_fixture> "${lc_object}" ${mode} ${lower}
      DEPENDS locked_cache_fixture VERBATIM)
    set_source_files_properties("${lc_object}" PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)
    add_executable(locked_cache_check_${mode}_${lower} locked_cache_check.cpp "${lc_object}")
    target_compile_features(locked_cache_check_${mode}_${lower} PRIVATE cxx_std_20)
    target_compile_definitions(locked_cache_check_${mode}_${lower} PRIVATE LC_LOWERED=${lower})
    target_include_directories(locked_cache_check_${mode}_${lower} PRIVATE ../../../runtime)
    target_link_libraries(locked_cache_check_${mode}_${lower} PRIVATE dr_cpu)
  endforeach()
endforeach()
add_executable(locked_cache_binding_check locked_cache_binding.c)
target_include_directories(locked_cache_binding_check PRIVATE ../../../runtime)
target_link_libraries(locked_cache_binding_check PRIVATE dr_cpu)
