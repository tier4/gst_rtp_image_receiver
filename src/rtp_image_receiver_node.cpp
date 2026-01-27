// Copyright 2025 Tier IV, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <rclcpp/rclcpp.hpp>
#include <rclcpp_components/register_node_macro.hpp>
#include <sensor_msgs/msg/compressed_image.hpp>
#include <sensor_msgs/msg/image.hpp>
#include <sensor_msgs/msg/camera_info.hpp>
#include <std_msgs/msg/header.hpp>
#include <image_transport/image_transport.hpp>
#include <camera_info_manager/camera_info_manager.hpp>
#include "image_receiver.h"
#include <opencv2/opencv.hpp>
#include <chrono>

#if __has_include(<cv_bridge/cv_bridge.hpp>)
  #include <cv_bridge/cv_bridge.hpp>
#else
  #include <cv_bridge/cv_bridge.h>
#endif
  


namespace rtp_image_receiver {

class RtpImageReceiverNode : public rclcpp::Node {
public:
    explicit RtpImageReceiverNode(const rclcpp::NodeOptions& options) 
        // get options
        : Node("rtp_image_receiver_node", options) {
        this->declare_parameter("udp_port", 5008);
        this->declare_parameter("jpeg_quality", 90);
        this->declare_parameter("buffer_size", 8388608);
        this->declare_parameter("max_buffers", 3);
        this->declare_parameter("width", 1920);
        this->declare_parameter("height", 1280);
        this->declare_parameter("verbose", false);
        this->declare_parameter("publish_raw", false);
        this->declare_parameter("publish_compressed", true);
        this->declare_parameter("frame_id", "camera");
        this->declare_parameter("camera_info_url", "");
        
        int udp_port = this->get_parameter("udp_port").as_int();
        int jpeg_quality = this->get_parameter("jpeg_quality").as_int();
        int buffer_size = this->get_parameter("buffer_size").as_int();
        int max_buffers = this->get_parameter("max_buffers").as_int();
        int width = this->get_parameter("width").as_int();
        int height = this->get_parameter("height").as_int();
        bool verbose = this->get_parameter("verbose").as_bool();
        publish_raw_ = this->get_parameter("publish_raw").as_bool();
        publish_compressed_ = this->get_parameter("publish_compressed").as_bool();
        frame_id_ = this->get_parameter("frame_id").as_string();
        std::string camera_info_url = this->get_parameter("camera_info_url").as_string();
        
        // create config
        ReceiverConfig config;
        config.udp_port = udp_port;
        config.jpeg_quality = jpeg_quality;
        config.buffer_size = buffer_size;
        config.max_buffers = max_buffers;
        config.width = width;
        config.height = height;
        config.verbose = verbose;

        // sink mode selection
        if (publish_raw_ && publish_compressed_){
            config.mode = SinkMode::RAW_AND_JPEG;
        }
        else if(publish_raw_){
            config.mode = SinkMode::RAW_ONLY;
        }
        else if(publish_compressed_){
            config.mode = SinkMode::JPEG_ONLY;
        }
        else{
            RCLCPP_ERROR(this->get_logger(), "Both publish_raw and publish_compressed are false. No data will be received. Defaulting to RAW_ONLY.");
            config.mode = SinkMode::RAW_ONLY;
        }
        
        // create gstreamer receiver
        receiver_ = std::make_unique<ImageReceiver>(config);
                
        // create publishers
        if (publish_raw_) {
            RCLCPP_INFO(this->get_logger(), "Create  raw image publisher");
            raw_pub_ = this->create_publisher<sensor_msgs::msg::Image>("~/image_raw", 10);
        }
        if (publish_compressed_) {
            RCLCPP_INFO(this->get_logger(), "Create compressed image publisher");
            compressed_pub_ = this->create_publisher<sensor_msgs::msg::CompressedImage>(
                "~/image_raw/compressed", 10);
        }
        camera_info_pub_ = this->create_publisher<sensor_msgs::msg::CameraInfo>(
            "camera_info", 10);
        
        // Initialize camera info manager
        camera_info_manager_ = std::make_shared<camera_info_manager::CameraInfoManager>(this, frame_id_);
        if (!camera_info_url.empty()) {
            camera_info_manager_->loadCameraInfo(camera_info_url);
            RCLCPP_INFO(this->get_logger(), "Loaded camera info from: %s", camera_info_url.c_str());
        } else {
            RCLCPP_WARN(this->get_logger(), "No camera_info_url provided, using default camera info");
        }
        
        // create statistics publisher
        stats_timer_ = this->create_wall_timer(
            std::chrono::seconds(5),
            std::bind(&RtpImageReceiverNode::publishStatistics, this));
        
        // setup receiver node callback
        if (config.mode == SinkMode::RAW_AND_JPEG) {
            RCLCPP_INFO(this->get_logger(), "Set RAW_AND_JPEG Callback");
            receiver_->setCombinedFrameCallback(
                std::bind(&RtpImageReceiverNode::combinedCallback, this,
                         std::placeholders::_1, std::placeholders::_2, 
                         std::placeholders::_3, std::placeholders::_4,
                         std::placeholders::_5));
        } else if (config.mode == SinkMode::RAW_ONLY) {
            RCLCPP_INFO(this->get_logger(), "Set RAW Callback");
            receiver_->setRawFrameCallback(
                std::bind(&RtpImageReceiverNode::rawCallback, this,
                         std::placeholders::_1, std::placeholders::_2, std::placeholders::_3));
        } else { // JPEG_ONLY
            RCLCPP_INFO(this->get_logger(), "Set JPEG Callback");
            receiver_->setJpegFrameCallback(
                std::bind(&RtpImageReceiverNode::jpegCallback, this,
                         std::placeholders::_1, std::placeholders::_2, std::placeholders::_3));
        }

        // launch gstreamer callback
        if (!receiver_->start()) {
            RCLCPP_ERROR(this->get_logger(), "Failed to start image receiver");
            rclcpp::shutdown();
            return;
        }
        
        // display Node information
        RCLCPP_INFO(this->get_logger(), "RTP Image Receiver Node started on UDP port %d", udp_port);
        RCLCPP_INFO(this->get_logger(), "Resolution: %dx%d", width, height);
        RCLCPP_INFO(this->get_logger(), "Log Verbose: %s", verbose ? "True" : "False");
        RCLCPP_INFO(this->get_logger(), "Publishing: raw=%s, compressed=%s", 
                    publish_raw_ ? "true" : "false",
                    publish_compressed_ ? "true" : "false");
    }
    
    ~RtpImageReceiverNode() {
        if (receiver_) {
            receiver_->stop();
        }
    }
    
private:
    std_msgs::msg::Header createHeader(const ImageReceiver::RTPTimestamp& timestamp) {
        std_msgs::msg::Header header;
        header.frame_id = frame_id_;
        // Use the extracted RTP timestamp if available
        if (timestamp.seconds > 0) {
            header.stamp.sec = timestamp.seconds;
            header.stamp.nanosec = timestamp.nanoseconds;
        } else {
            // Otherwise, use the current ROS time
            header.stamp = this->now();
        }
        return header;
    }

    // Raw image publishing helper
    void publishRaw(const uint8_t* data, size_t size, const std_msgs::msg::Header& header) {
        if (config_.verbose || (publish_raw_ && raw_pub_->get_subscription_count() > 0)) { 
            cv::Mat bgr_image(config_.height, config_.width, CV_8UC3, (void*)data);
            sensor_msgs::msg::Image::SharedPtr image_msg = 
                cv_bridge::CvImage(header, "bgr8", bgr_image).toImageMsg();
            raw_pub_->publish(*image_msg);
            RCLCPP_INFO(this->get_logger(), "Publish Image...Timestamp: %09u.%09u", header.stamp.sec, header.stamp.nanosec);
        }
    }

    // Compressed image publishing helper
    void publishCompressed(const uint8_t* data, size_t size, const std_msgs::msg::Header& header) {
        if (config_.verbose || (publish_compressed_ && compressed_pub_->get_subscription_count() > 0)) {
            auto compressed_msg = std::make_unique<sensor_msgs::msg::CompressedImage>();
            compressed_msg->header = header;
            compressed_msg->format = "jpeg";
            compressed_msg->data.assign(data, data + size);
            compressed_pub_->publish(std::move(compressed_msg));
            RCLCPP_INFO(this->get_logger(), "Publish CompressedImage...Timestamp: %09u.%09u", header.stamp.sec, header.stamp.nanosec);
        }
    }

    // CameraInfo publishing helper
    void publishCameraInfo(const std_msgs::msg::Header& header) {
        if (camera_info_pub_->get_subscription_count() > 0) {
            auto camera_info_msg = camera_info_manager_->getCameraInfo();
            camera_info_msg.header = header;
            camera_info_pub_->publish(camera_info_msg);
        }
    }

    // Callback for JPEG_ONLY mode
    void jpegCallback(const uint8_t* data, size_t size, const ImageReceiver::RTPTimestamp& timestamp) {
        std_msgs::msg::Header header = createHeader(timestamp);
        
        publishCompressed(data, size, header);
        publishCameraInfo(header);
        
        frame_count_++;
    }

    // Callback for RAW_ONLY mode
    void rawCallback(const uint8_t* data, size_t size, const ImageReceiver::RTPTimestamp& timestamp) {
        std_msgs::msg::Header header = createHeader(timestamp);

        publishRaw(data, size, header);
        publishCameraInfo(header);
        
        frame_count_++;
    }

    // Callback for RAW_AND_JPEG mode
    void combinedCallback(const uint8_t* raw_data, size_t raw_size, 
                          const uint8_t* jpeg_data, size_t jpeg_size, 
                          const ImageReceiver::RTPTimestamp& timestamp) {
        std_msgs::msg::Header header = createHeader(timestamp);
        
        // (Publish both data streams, which are synchronized by PTS within ImageReceiver::Impl)
        publishRaw(raw_data, raw_size, header);
        publishCompressed(jpeg_data, jpeg_size, header);
        publishCameraInfo(header);

        frame_count_++;
    }
    
    // gstreamer statistics
    void publishStatistics() {
        auto stats = receiver_->getStatistics();
        RCLCPP_INFO(this->get_logger(), 
            "Stats : Frames(Raw)=%lu, Dropped(Raw)=%lu, Frames(Jpeg)=%lu, Dropped(Jpeg)=%lu, FPS=%.2f",
            stats.frames_processed_raw, stats.frames_dropped_raw,
            stats.frames_processed_jpeg, stats.frames_dropped_jpeg,
            stats.average_fps_raw);
    }
    
    // member arguments
    std::unique_ptr<ImageReceiver> receiver_;
    rclcpp::Publisher<sensor_msgs::msg::CompressedImage>::SharedPtr compressed_pub_;
    rclcpp::Publisher<sensor_msgs::msg::CameraInfo>::SharedPtr camera_info_pub_;
    rclcpp::Publisher<sensor_msgs::msg::Image>::SharedPtr raw_pub_;
    rclcpp::TimerBase::SharedPtr stats_timer_;
    std::shared_ptr<camera_info_manager::CameraInfoManager> camera_info_manager_;
    
    ReceiverConfig config_;
    bool publish_raw_;
    bool publish_compressed_;
    std::string frame_id_;
    uint64_t frame_count_ = 0;
};

}  // namespace rtp_image_receiver

RCLCPP_COMPONENTS_REGISTER_NODE(rtp_image_receiver::RtpImageReceiverNode)