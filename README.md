# ROS 2 RTP Image Receiver Node via Gstreamer

ROS 2 node for processing real-time video streams from UDP/RTP sources. This node is designed to handle YCbCr 4:2:2 format video, convert it to BGR for display, and then compress it into JPEG format. It leverages hardware acceleration (VA-API, NVIDIA, Jetson) for optimal performance.

## 🚀 Features

Hardware-accelerated video processing for high-speed performance.
Publishes both raw and compressed image topics.
All settings are configurable via ROS 2 parameters.
Real-time performance metrics and debugging information.

## ⚡️ Topics

### Published Topics

- `~/image_compressed` (`sensor_msgs/msg/CompressedImage`): The processed and JPEG-compressed images.
- `~/image_raw` (`sensor_msgs/msg/Image`): The raw BGR images. This topic is optional and can be enabled via parameters.

## ⚙️ Parameters

| Parameter | Type | Default | Description |
|---|---|---|---|
| `udp_port`            | `int`   | `5008`      | UDP port for the RTP stream. |
| `jpeg_quality`        | `int`   | `90`        | JPEG compression quality (`0`-`100`). |
| `buffer_size`         | `int`   | `8388608`   | Buffer size for the UDP socket in bytes. |
| `max_buffers`         | `int`   | `3`         | Maximum number of buffers to use. |
| `publish_raw`         | `bool`  | `false`     | If `true`, publishes the `~/image_raw` topic. |
| `publish_compressed`  | `bool`  | `true`      | If `true`, publishes the `~/image_compressed` topic. |
| `frame_id`            | `string`| `"camera"`  | Frame ID for published images. |

## 🛠️ Build Instructions

### Prerequisites

You'll need to install the necessary ROS 2 and GStreamer dependencies.

```
# Install ROS 2 dependencies
sudo apt install ros-humble-cv-bridge ros-humble-image-transport ros-humble-compressed-image-transport

# Install GStreamer dependencies
sudo apt-get install \
    libgstreamer1.0-dev \
    libgstreamer-plugins-base1.0-dev \
    gstreamer1.0-plugins-bad \
    gstreamer1.0-vaapi \
    gstreamer1.0-tools

# Install OpenCV
sudo apt install libopencv-dev
```

### Build

Navigate to your workspace and build the package.

```
# Source ROS 2
source /opt/ros/humble/setup.bash

#Build the package
cd /home/comlops/rtp_example
colcon build --packages-select gst_rtp_image_receiver

# Source the workspace to use the built packages
source install/setup.bash
```


## 🚀 Usage

### Run the node

You can run the node directly from the command line, with or without parameters.

```
# Basic usage
ros2 run gst_rtp_image_receiver rtp_image_receiver_node

# With custom parameters
ros2 run gst_rtp_image_receiver rtp_image_receiver_node --ros-args \
    -p udp_port:=5008 \
    -p jpeg_quality:=95 \
    -p publish_raw:=true
```

### Using launch files

The node can also be launched using ROS 2 launch files for easier configuration.

- Example with a Python launch file

    ```
    ros2 launch gst_rtp_image_receiver rtp_image_receiver.launch.py
    ```

- With launch file arguments

    ```
    ros2 launch gst_rtp_image_receiver rtp_image_receiver.launch.py \
        udp_port:=5008 \
        publish_raw:=true \
        namespace:=my_camera
    ```

## 🔬 Testing

### Send a test RTP stream

Use GStreamer to generate a test pattern and stream it via RTP.

```
gst-launch-1.0 videotestsrc ! \
    video/x-raw,width=2880,height=1860,format=UYVY,framerate=30/1 ! \
    rtpvrawpay ! udpsink host=127.0.0.1 port=5008
```

### View the output

Use `rqt_image_view` to subscribe to the published topics and display the images.

- View compressed images

    ```
    ros2 run rqt_image_view rqt_image_view
    ```

### Monitor topics

You can use standard ROS 2 CLI tools to monitor the published topics.

```
# List all active topics
ros2 topic list

# Echo compressed image info (useful for checking header and metadata)
ros2 topic echo /rtp_receiver/gst_rtp_image_receiver/image_compressed --no-arr

# Check the publishing rate
ros2 topic hz /rtp_receiver/gst_rtp_image_receiver/image_compressed
```

## 🏎️ Performance Tuning

### CPU Governor (for x86 systems)

Set the CPU governor to performance for maximum throughput.

```
echo performance | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor
```

### Jetson Optimization (for NVIDIA Jetson platforms)

Use the built-in Jetson tools to maximize performance.

```
sudo jetson_clocks
sudo nvpmodel -m 0
```

## 🐛 Debugging

### Enable GStreamer debug output

Set the GST_DEBUG environment variable to get verbose GStreamer output.

```
export GST_DEBUG=3
ros2 run gst_rtp_image_receiver rtp_image_receiver_node
```

### Check node info

Get detailed information about the running node, including subscribed and published topics.

```
ros2 node info /rtp_receiver/gst_rtp_image_receiver
```