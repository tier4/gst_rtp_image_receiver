from launch import LaunchDescription
from launch_ros.actions import Node

def generate_launch_description():
    nodes = []

    for i in range(0, 8):
        process_name = f'proc_gst_recv_{i}'        
        node = Node(
            package='gst_rtp_image_receiver',
            executable='rtp_image_receiver_node', 
            namespace=f'camera{i}',
            name='rtp_image_receiver',
            prefix=f'bash -c "exec -a {process_name} \\"$@\\"" --',
            parameters=[{
                'udp_port': 5004 + i,
                'frame_id': f'camera{i}',
                'width': 1920,
                'height': 1280,
                'jpeg_quality': 90,
                'verbose': True,
                'publish_raw': True,
                'publish_compressed': False
            }]
        )
        nodes.append(node)

    return LaunchDescription(nodes)