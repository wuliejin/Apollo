add_executable(test_dualsense_usbip_protocol
        dualsense_usbip_protocol_test.cpp
        ${CMAKE_SOURCE_DIR}/src/platform/windows/dualsense_usbip_protocol.cpp)
target_include_directories(test_dualsense_usbip_protocol PRIVATE
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/third-party/libvirtualgamepad/include)
set_target_properties(test_dualsense_usbip_protocol PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON)
add_test(NAME test_dualsense_usbip_protocol COMMAND test_dualsense_usbip_protocol)
