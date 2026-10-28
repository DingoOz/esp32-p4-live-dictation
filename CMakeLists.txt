cmake_minimum_required(VERSION 3.22)
# the engine and its service from esp32-pie-asr, used as they are (not its screen or demos)
set(PIEASR ${CMAKE_CURRENT_LIST_DIR}/third_party/esp32-pie-asr)
set(EXTRA_COMPONENT_DIRS ${PIEASR}/pieasr ${PIEASR}/components/asr_service ${PIEASR}/components/p4mic
                         ${PIEASR}/components/p4sdcard)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(dictation)
