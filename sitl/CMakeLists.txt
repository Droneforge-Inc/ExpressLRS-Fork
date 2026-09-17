cmake_minimum_required(VERSION 3.20)
project(elrs_protocol_sitl LANGUAGES CXX C)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
option(SITL_SANITIZE "Enable address and undefined behavior sanitizers" OFF)
# Apply instrumentation to every native target, including standalone tests.
if(SITL_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()
set(ELRS ${CMAKE_CURRENT_SOURCE_DIR}/../src)
set(LIB_DIRS)
foreach(lib CrsfProtocol FEC CRC OTA Handset Df3 MSP TelemetryProtocol OPTIONS FIFO FHSS logging SX1280Driver SX12xxDriverCommon DEVICE Telemetry StubbornSender StubbornReceiver)
  list(APPEND LIB_DIRS ${ELRS}/lib/${lib})
endforeach()
execute_process(COMMAND git rev-parse --short=12 HEAD WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
  OUTPUT_VARIABLE REV OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
foreach(role tx rx)
  add_executable(elrs_sitl_${role} main.cpp ipc.cpp firmware.cpp
    ${ELRS}/lib/OTA/OTA.cpp ${ELRS}/lib/CRC/crc.cpp
    ${ELRS}/lib/FHSS/FHSS.cpp ${ELRS}/lib/FHSS/random.cpp
    ${ELRS}/lib/StubbornSender/stubborn_sender.cpp
    ${ELRS}/lib/StubbornReceiver/stubborn_receiver.cpp
    ${ELRS}/lib/Handset/CRSF.cpp ${ELRS}/src/common.cpp)
  target_include_directories(elrs_sitl_${role} PRIVATE include ${ELRS}/include ${ELRS}/src ${LIB_DIRS})
  string(TOUPPER ${role} ROLE)
  target_compile_definitions(elrs_sitl_${role} PRIVATE TARGET_NATIVE=1 TARGET_SITL=1 TARGET_${ROLE}=1
    Regulatory_Domain_ISM_2400=1 PROGMEM= SITL_REVISION="${REV}")
  target_compile_options(elrs_sitl_${role} PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-unused-variable
    -ffunction-sections -fdata-sections)
  target_link_options(elrs_sitl_${role} PRIVATE -Wl,--gc-sections)
  if(role STREQUAL rx)
    target_sources(elrs_sitl_${role} PRIVATE ${ELRS}/src/rx-serial/SerialCRSF.cpp
      ${ELRS}/src/rx-serial/SerialIO.cpp ${ELRS}/lib/Telemetry/telemetry.cpp)
  endif()
endforeach()
enable_testing()
foreach(test reference_session reference_stream link_snapshot)
  add_executable(${test}_test tests/test_${test}.cpp)
  target_include_directories(${test}_test PRIVATE ${ELRS}/lib/Df3 ${ELRS}/lib/Handset)
  target_compile_options(${test}_test PRIVATE -Wall -Wextra -Werror)
  # Shared transport headers must also compile with embedded C++11 toolchains.
  set_target_properties(${test}_test PROPERTIES CXX_STANDARD 11)
  add_test(NAME ${test} COMMAND ${test}_test)
endforeach()
add_executable(telemetry_queue_test tests/test_telemetry_queue.cpp
  ${ELRS}/lib/Telemetry/telemetry.cpp ${ELRS}/lib/CRC/crc.cpp)
target_include_directories(telemetry_queue_test PRIVATE ${ELRS}/include ${ELRS}/src ${LIB_DIRS})
target_compile_definitions(telemetry_queue_test PRIVATE TARGET_NATIVE=1 TARGET_SITL=1 TARGET_RX=1
  Regulatory_Domain_ISM_2400=1 PROGMEM=)
target_compile_options(telemetry_queue_test PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-unused-variable)
add_test(NAME telemetry_queue COMMAND telemetry_queue_test)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME reference_link COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_reference_link.py
  --bin-dir ${CMAKE_CURRENT_BINARY_DIR})
set_tests_properties(reference_link PROPERTIES TIMEOUT 45 ENVIRONMENT PYTHONDONTWRITEBYTECODE=1)
add_test(NAME protocol_sitl COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_sitl.py
  --bin-dir ${CMAKE_CURRENT_BINARY_DIR})
set_tests_properties(protocol_sitl PROPERTIES TIMEOUT 45 ENVIRONMENT PYTHONDONTWRITEBYTECODE=1)
add_test(NAME uplink_sitl COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_uplink.py
  --bin-dir ${CMAKE_CURRENT_BINARY_DIR})
set_tests_properties(uplink_sitl PROPERTIES TIMEOUT 45 ENVIRONMENT PYTHONDONTWRITEBYTECODE=1)

# Optional read-only compatibility probe. Compiles Betaflight's actual parser,
# not a copied decoder. The standalone ELRS build does not require this checkout.
set(BETAFLIGHT_SOURCE "" CACHE PATH "Betaflight checkout for the parser compatibility probe")
if(BETAFLIGHT_SOURCE)
  add_executable(betaflight_crsf_probe tests/betaflight_probe.c
    ${BETAFLIGHT_SOURCE}/src/main/rx/crsf.c ${BETAFLIGHT_SOURCE}/src/main/common/crc.c)
  target_include_directories(betaflight_crsf_probe PRIVATE ${BETAFLIGHT_SOURCE}/src/test/unit ${BETAFLIGHT_SOURCE}/src/main)
  target_compile_definitions(betaflight_crsf_probe PRIVATE UNIT_TEST)
  target_compile_options(betaflight_crsf_probe PRIVATE -ffunction-sections -fdata-sections)
  target_link_options(betaflight_crsf_probe PRIVATE -Wl,--gc-sections)
endif()
