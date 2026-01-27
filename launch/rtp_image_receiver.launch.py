from launch import LaunchDescription
from launch.actions import DeclareLaunchArgument
from launch.substitutions import LaunchConfiguration, PathJoinSubstitution
from launch_ros.actions import Node
from launch_ros.substitutions import FindPackageShare

def generate_launch_description():
    return LaunchDescription([
        DeclareLaunchArgument(
            'udp_port',
            default_value='5008',
            description='UDP port for RTP stream'
        ),
        DeclareLaunchArgument(
            'jpeg_quality',
            default_value='90',
            description='JPEG compression quality (0-100)'
        ),
        DeclareLaunchArgument(
            'buffer_size',
            default_value='8388608',
            description='Buffer size in bytes'
        ),
        DeclareLaunchArgument(
            'max_buffers',
            default_value='3',
            description='Maximum number of buffers'
        ),
        DeclareLaunchArgument(
            'publish_raw',
            default_value='false',
            description='Publish raw images'
        ),
        DeclareLaunchArgument(
            'publish_compressed',
            default_value='true',
            description='Publish compressed images'
        ),
        DeclareLaunchArgument(
            'frame_id',
            default_value='camera',
            description='Frame ID for published images'
        ),
        DeclareLaunchArgument(
            'namespace',
            default_value='rtp_receiver',
            description='Namespace for the node'
        ),
        DeclareLaunchArgument(
            'width',
            default_value='1920',
            description='Image width'
        ),
        DeclareLaunchArgument(
            'height',
            default_value='1280',
            description='Image height'
        ),
        DeclareLaunchArgument(
            'camera_info_url',
            default_value=['package://gst_rtp_image_receiver/config/camera_info.yaml'],
            description='URL for camera calibration file (file:// or package://)'
        ),
        
        Node(
            package='gst_rtp_image_receiver',
            executable='rtp_image_receiver_node',
            name='rtp_image_receiver',
            namespace=LaunchConfiguration('namespace'),
            parameters=[{
                'udp_port': LaunchConfiguration('udp_port'),
                'jpeg_quality': LaunchConfiguration('jpeg_quality'),
                'buffer_size': LaunchConfiguration('buffer_size'),
                'max_buffers': LaunchConfiguration('max_buffers'),
                'publish_raw': LaunchConfiguration('publish_raw'),
                'publish_compressed': LaunchConfiguration('publish_compressed'),
                'frame_id': LaunchConfiguration('frame_id'),
                'width': LaunchConfiguration('width'),
                'height': LaunchConfiguration('height'),
                'verbose': LaunchConfiguration('verbose'),
                'camera_info_url': LaunchConfiguration('camera_info_url'),
            }],
            output='screen',
            emulate_tty=True,
        )
    ])