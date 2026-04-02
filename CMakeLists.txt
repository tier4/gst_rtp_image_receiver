cmake_minimum_required(VERSION 3.10)
project(gst_rtp_image_receiver)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Platform detection
if(EXISTS "/etc/nv_tegra_release")
    set(JETSON_PLATFORM ON)
    add_definitions(-DJETSON_PLATFORM)
endif()

# Find ROS 2 packages
find_package(ament_cmake REQUIRED)
find_package(rclcpp REQUIRED)
find_package(rclcpp_components REQUIRED)
find_package(sensor_msgs REQUIRED)
find_package(std_msgs REQUIRED)
find_package(cv_bridge REQUIRED)
find_package(image_transport REQUIRED)
find_package(camera_info_manager REQUIRED)
find_package(OpenCV REQUIRED)

# Dependencies
find_package(PkgConfig REQUIRED)
pkg_check_modules(GST REQUIRED 
    gstreamer-1.0>=1.14
    gstreamer-app-1.0>=1.14
    gstreamer-rtp-1.0>=1.14
)

# Source files
set(SOURCES
    src/image_receiver.cpp
)

# Library creation
add_library(imagereceiver SHARED ${SOURCES})

target_include_directories(imagereceiver 
    PUBLIC 
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${GST_INCLUDE_DIRS}
)

target_link_libraries(imagereceiver
    PRIVATE
        ${GST_LIBRARIES}
)

target_compile_options(imagereceiver PRIVATE ${GST_CFLAGS_OTHER})

# ROS 2 Component library
add_library(rtp_image_receiver_component SHARED
    src/rtp_image_receiver_node.cpp
)

target_include_directories(rtp_image_receiver_component
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${GST_INCLUDE_DIRS}
        ${OpenCV_INCLUDE_DIRS}
)

ament_target_dependencies(rtp_image_receiver_component
    rclcpp
    rclcpp_components
    sensor_msgs
    std_msgs
    cv_bridge
    image_transport
    camera_info_manager
)

target_link_libraries(rtp_image_receiver_component
    imagereceiver
    ${GST_LIBRARIES}
    ${OpenCV_LIBS}
)

rclcpp_components_register_node(
    rtp_image_receiver_component
    PLUGIN "rtp_image_receiver::RtpImageReceiverNode"
    EXECUTABLE rtp_image_receiver_node
)

# # Original demo executable
# add_executable(image_receiver_demo examples/main.cpp)
# target_link_libraries(image_receiver_demo imagereceiver)

# Installation
install(TARGETS imagereceiver
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
)

install(DIRECTORY include/
    DESTINATION include
)

install(TARGETS
    rtp_image_receiver_component
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
)

install(TARGETS
    rtp_image_receiver_node
    # image_receiver_demo
    DESTINATION lib/${PROJECT_NAME}
)

# Install launch files
install(DIRECTORY launch
    DESTINATION share/${PROJECT_NAME}
)

# Install config files
install(DIRECTORY config
    DESTINATION share/${PROJECT_NAME}
)

# Tests
if(BUILD_TESTING)
    find_package(ament_lint_auto REQUIRED)
    ament_lint_auto_find_test_dependencies()
endif()

ament_package()