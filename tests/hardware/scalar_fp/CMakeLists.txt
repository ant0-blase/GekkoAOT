cmake_minimum_required(VERSION 3.20)
project(GekkoAOTScalarFP C CXX)
set(DOLRECOMP_ENABLE_LLVM ON CACHE BOOL "" FORCE)
add_subdirectory("${DOLRECOMP_SOURCE}" dolrecomp EXCLUDE_FROM_ALL)
add_executable(scalar_fp_fixture fixture.cpp)
target_link_libraries(scalar_fp_fixture PRIVATE dr_llvm)
set(probes "${CMAKE_CURRENT_BINARY_DIR}/probes.o")
add_custom_command(OUTPUT "${probes}"
  COMMAND scalar_fp_fixture "${probes}" 0
  DEPENDS scalar_fp_fixture VERBATIM)
set_source_files_properties("${probes}" "${REFERENCE_OBJECT}"
  PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)
add_executable(scalar_fp_check check.c "${probes}" "${REFERENCE_OBJECT}")
target_compile_options(scalar_fp_check PRIVATE -O2 -fno-fast-math -ffp-contract=off)
target_link_libraries(scalar_fp_check PRIVATE dr_cpu)

set(memory_probes "${CMAKE_CURRENT_BINARY_DIR}/probes-memory.o")
add_custom_command(OUTPUT "${memory_probes}"
  COMMAND scalar_fp_fixture "${memory_probes}" 1
  DEPENDS scalar_fp_fixture VERBATIM)
set_source_files_properties("${memory_probes}" PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)
add_executable(scalar_fp_check_memory check.c "${memory_probes}" "${REFERENCE_OBJECT}")
target_compile_options(scalar_fp_check_memory PRIVATE -O2 -fno-fast-math -ffp-contract=off)
target_link_libraries(scalar_fp_check_memory PRIVATE dr_cpu)
