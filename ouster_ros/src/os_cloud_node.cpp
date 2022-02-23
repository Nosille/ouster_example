/**
 * @file
 * @brief Example node to publish point clouds and imu topics
 */

#include <ros/console.h>
#include <ros/ros.h>
#include <ros/service.h>
#include <sensor_msgs/Imu.h>
#include <sensor_msgs/PointCloud2.h>
#include <tf2_ros/static_transform_broadcaster.h>

#include <algorithm>
#include <chrono>
#include <memory>

#include "ouster/lidar_scan.h"
#include "ouster/types.h"
#include "ouster_ros/OSConfigSrv.h"
#include "ouster_ros/PacketMsg.h"
#include "ouster_ros/ros.h"

using PacketMsg = ouster_ros::PacketMsg;
using Cloud = ouster_ros::Cloud;
using Point = ouster_ros::Point;
namespace sensor = ouster::sensor;

int main(int argc, char** argv) {
    ros::init(argc, argv, "os_cloud_node");
    ros::NodeHandle nh("~");

    auto tf_prefix = nh.param("tf_prefix", std::string{});
    if (!tf_prefix.empty() && tf_prefix.back() != '/') tf_prefix.append("/");
    auto sensor_frame = tf_prefix + "os_sensor";
    auto imu_frame = tf_prefix + "os_imu";
    auto lidar_frame = tf_prefix + "os_lidar";

    ouster_ros::Filter filter;
    int ts_offset;
    bool use_ros_time;
    nh.param<float>("filter_min_x", filter.minX, -0.0); ROS_INFO_STREAM("filter_min_x=" << filter.minX);
    nh.param<float>("filter_max_x", filter.maxX, +0.0); ROS_INFO_STREAM("filter_max_x=" << filter.maxX);
    nh.param<float>("filter_min_y", filter.minY, -0.0); ROS_INFO_STREAM("filter_min_y=" << filter.minY);
    nh.param<float>("filter_max_y", filter.maxY, +0.0); ROS_INFO_STREAM("filter_max_y=" << filter.maxY);
    nh.param<float>("filter_min_z", filter.minZ, -0.0); ROS_INFO_STREAM("filter_min_z=" << filter.minZ);
    nh.param<float>("filter_max_z", filter.maxZ, +0.0); ROS_INFO_STREAM("filter_max_z=" << filter.maxZ);

    nh.param<bool>("use_ros_time", use_ros_time, false); ROS_INFO_STREAM("use_ros_time=" << use_ros_time);
    nh.param<int>("timestamp_offset", ts_offset, 0); 

    ouster_ros::OSConfigSrv cfg{};
    auto client = nh.serviceClient<ouster_ros::OSConfigSrv>("os_config");
    client.waitForExistence();
    if (!client.call(cfg)) {
        ROS_ERROR("Calling config service failed");
        return EXIT_FAILURE;
    }

    auto info = sensor::parse_metadata(cfg.response.metadata);
    uint32_t H = info.format.pixels_per_column;
    uint32_t W = info.format.columns_per_frame;

    auto pf = sensor::get_format(info);

    auto lidar_pub = nh.advertise<sensor_msgs::PointCloud2>("points", 10);
    auto imu_pub = nh.advertise<sensor_msgs::Imu>("imu", 100);

    auto xyz_lut = ouster::make_xyz_lut(info);

    Cloud cloud{W, H};
    ouster::LidarScan ls{W, H};

    ouster::ScanBatcher batch(W, pf);

    auto lidar_handler = [&](const PacketMsg& pm) mutable {
        if (batch(pm.buf.data(), ls)) {
            auto h = std::find_if(
                ls.headers.begin(), ls.headers.end(), [](const auto& h) {
                    return h.timestamp != std::chrono::nanoseconds{0};
                });
            if (h != ls.headers.end()) {
                scan_to_cloud(xyz_lut, h->timestamp, ls, cloud, filter);
                //add offset to timestamp
                std::chrono::nanoseconds timestamp, offset;
                timestamp = h->timestamp;
                if(use_ros_time)  timestamp = std::chrono::nanoseconds(ros::Time::now().toNSec());            
                offset = std::chrono::milliseconds{ts_offset}; 
                timestamp += offset;

                lidar_pub.publish(ouster_ros::cloud_to_cloud_msg(
                    cloud, timestamp, sensor_frame));
            }
        }
    };

    auto imu_handler = [&](const PacketMsg& p) {
        imu_pub.publish(ouster_ros::packet_to_imu_msg(p, imu_frame, pf));
    };

    auto lidar_packet_sub = nh.subscribe<PacketMsg, const PacketMsg&>(
        "lidar_packets", 2048, lidar_handler);
    auto imu_packet_sub = nh.subscribe<PacketMsg, const PacketMsg&>(
        "imu_packets", 100, imu_handler);

    // publish transforms
    tf2_ros::StaticTransformBroadcaster tf_bcast{};

    tf_bcast.sendTransform(ouster_ros::transform_to_tf_msg(
        info.imu_to_sensor_transform, sensor_frame, imu_frame));

    tf_bcast.sendTransform(ouster_ros::transform_to_tf_msg(
        info.lidar_to_sensor_transform, sensor_frame, lidar_frame));

    ros::spin();

    return EXIT_SUCCESS;
}
