cmake_minimum_required(VERSION 3.20)
project(elrs_protocol_sitl LANGUAGES CXX C)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
option(SITL_SANITIZE "Enable address and undefined behavior sanitizers" OFF)
set(ELRS ${CMAKE_CURRENT_SOURCE_DIR}/../src)
set(LIB_DIRS)
foreach(lib CrsfProtocol FEC CRC OTA Handset MSP TelemetryProtocol OPTIONS FIFO FHSS logging SX1280Driver SX12xxDriverCommon DEVICE Telemetry)
  list(APPEND LIB_DIRS ${ELRS}/lib/${lib})
endforeach()
execute_process(COMMAND git rev-parse --short=12 HEAD WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
  OUTPUT_VARIABLE REV OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
foreach(role tx rx)
  add_executable(elrs_sitl_${role} main.cpp ipc.cpp firmware.cpp
    ${ELRS}/lib/OTA/OTA.cpp ${ELRS}/lib/CRC/crc.cpp
    ${ELRS}/lib/FHSS/FHSS.cpp ${ELRS}/lib/FHSS/random.cpp
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
  if(SITL_SANITIZE)
    target_compile_options(elrs_sitl_${role} PRIVATE -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
    target_link_options(elrs_sitl_${role} PRIVATE -fsanitize=address,undefined)
  endif()
endforeach()
enable_testing()
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME protocol_sitl COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_sitl.py
  --bin-dir ${CMAKE_CURRENT_BINARY_DIR})
set_tests_properties(protocol_sitl PROPERTIES TIMEOUT 45 ENVIRONMENT PYTHONDONTWRITEBYTECODE=1)

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
