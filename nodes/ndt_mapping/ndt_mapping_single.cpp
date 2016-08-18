/*
 *  Copyright (c) 2015, Nagoya University
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions are met:
 *
 *  * Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 *
 *  * Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 *  * Neither the name of Autoware nor the names of its
 *    contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 * 
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 *  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 *  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 *  DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 *  FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 *  DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 *  SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 *  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 *  OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *  OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

/*
 Localization and mapping program using Normal Distributions Transform

 Yuki KITSUKAWA
 */

#define OUTPUT // If you want to output "position_log.txt", "#define OUTPUT".

#include <iostream>
#include <sstream>
#include <fstream>
#include <string>

#include <ros/ros.h>
#include <std_msgs/Bool.h>
#include <std_msgs/Float32.h>
#include <sensor_msgs/PointCloud2.h>
#include <velodyne_pointcloud/point_types.h>
#include <velodyne_pointcloud/rawdata.h>

#include <tf/transform_broadcaster.h>
#include <tf/transform_datatypes.h>
#include <tf/transform_listener.h>
#include "pcl_ros/impl/transforms.hpp"

#include <pcl/io/io.h>
#include <pcl/io/pcd_io.h>
#include <pcl/point_types.h>
#include <pcl_conversions/pcl_conversions.h>
#include <pcl/registration/ndt.h>
#include <pcl/filters/approximate_voxel_grid.h>
#include <pcl/filters/voxel_grid.h>

#include <runtime_manager/ConfigNdtMapping.h>
#include <runtime_manager/ConfigNdtMappingOutput.h>

#include <pointshape_processor.h>

struct Position {
    double x;
    double y;
    double z;
    double roll;
    double pitch;
    double yaw;
};

// global variables
static Position previous_pos, guess_pos, current_pos, added_pos;

static double offset_x, offset_y, offset_z, offset_yaw; // current_pos - previous_pos

static pcl::PointCloud<pcl::PointXYZI> map_global;




/////////////////////// Xi  ///////////////////////////////////
std::vector<pcl::PointCloud<pcl::PointXYZI>> map_local;       // for ndt registration
std::vector<pcl::PointCloud<pcl::PointXYZRGB>> map_terrain;   // for publish

Pointshape_Processor ps_processor(360*4);

int map_local_index = 0;
int map_local_length = 5;

int map_terrain_index = 0;
int map_terrain_length = 50;
float shift_terrain = 0.05;

tf::TransformListener* tfListener = NULL;
//////////////////////////////////////////////////////////////////////



static pcl::NormalDistributionsTransform<pcl::PointXYZI, pcl::PointXYZI> ndt;
// Default values
static int iter = 30; // Maximum iterations
static float ndt_res = 1.0; // Resolution
static double step_size = 0.1; // Step size
static double trans_eps = 0.01; // Transformation epsilon

// Leaf size of VoxelGrid filter.
static double voxel_leaf_size = 2.0;

static ros::Time callback_start, callback_end, t1_start, t1_end, t2_start, t2_end, t3_start, t3_end, t4_start, t4_end, t5_start, t5_end;
static ros::Duration d_callback, d1, d2, d3, d4, d5;

static ros::Publisher ndt_map_pub;
static ros::Publisher current_pose_pub;
static geometry_msgs::PoseStamped current_pose_msg;

static ros::Publisher ndt_stat_pub, pub_velodyne_base;
static std_msgs::Bool ndt_stat_msg;

static int initial_scan_loaded = 0;

static Eigen::Matrix4f gnss_transform = Eigen::Matrix4f::Identity();

static double RANGE = 0.0;
static double SHIFT = 0.0;

tf::Transform g_transform;

/////////////////////// Xi  ///////////////////////////////////
pcl::PointCloud<pcl::PointXYZI> get_local_map(std::vector<pcl::PointCloud<pcl::PointXYZI>> map_cloud)
{
    pcl::PointCloud<pcl::PointXYZI> map_points;

    for(int i = 0; i< map_cloud.size(); i++)
    {
        map_points += map_cloud[i];
    }

    map_points.header = map_global.header;

    if(map_points.points.size() == 0)
    std::cout << "empty !" << std::endl;
    else
    std::cout << map_cloud.size() << "  " << map_points.points.size() << std::endl;

    return map_points;
}

pcl::PointCloud<pcl::PointXYZRGB> get_local_map(std::vector<pcl::PointCloud<pcl::PointXYZRGB>> map_cloud)
{
    pcl::PointCloud<pcl::PointXYZRGB> map_points;

    for(int i = 0; i< map_cloud.size(); i++)
    {
        map_points += map_cloud[i];
    }

    map_points.header = map_global.header;

    if(map_points.points.size() == 0)
    std::cout << "empty !" << std::endl;
    else
    std::cout << map_cloud.size() << "  " << map_points.points.size() << std::endl;

    return map_points;
}


//////////////////////////////////////////////////////////////////////


static void param_callback(const runtime_manager::ConfigNdtMapping::ConstPtr& input)
{
  ndt_res = input->resolution;
  step_size = input->step_size;
  trans_eps = input->trans_eps;
  voxel_leaf_size = input->leaf_size;

  std::cout << "param_callback" << std::endl;
  std::cout << "ndt_res: " << ndt_res << std::endl;
  std::cout << "step_size: " << step_size << std::endl;
  std::cout << "trans_eps: " << trans_eps << std::endl;
  std::cout << "voxel_leaf_size: " << voxel_leaf_size << std::endl;
}

static void output_callback(const runtime_manager::ConfigNdtMappingOutput::ConstPtr& input)
{
  double filter_res = input->filter_res;
  std::string filename = input->filename;
  std::cout << "output_callback" << std::endl;
  std::cout << "filter_res: " << filter_res << std::endl;
  std::cout << "filename: " << filename << std::endl;

  pcl::PointCloud<pcl::PointXYZI>::Ptr map_ptr(new pcl::PointCloud<pcl::PointXYZI>(map_global));
  pcl::PointCloud<pcl::PointXYZI>::Ptr map_filtered(new pcl::PointCloud<pcl::PointXYZI>());
  map_ptr->header.frame_id = "map";
  map_filtered->header.frame_id = "map";
  sensor_msgs::PointCloud2::Ptr map_msg_ptr(new sensor_msgs::PointCloud2);

  // Apply voxelgrid filter
  if(filter_res == 0.0){
    std::cout << "Original: " << map_ptr->points.size() << " points." << std::endl;
    pcl::toROSMsg(*map_ptr, *map_msg_ptr);
  }else{
    pcl::VoxelGrid<pcl::PointXYZI> voxel_grid_filter;
    voxel_grid_filter.setLeafSize(filter_res, filter_res, filter_res);
    voxel_grid_filter.setInputCloud(map_ptr);
    voxel_grid_filter.filter(*map_filtered);
    std::cout << "Original: " << map_ptr->points.size() << " points." << std::endl;
    std::cout << "Filtered: " << map_filtered->points.size() << " points." << std::endl;
    pcl::toROSMsg(*map_filtered, *map_msg_ptr);
  }

  std::cout << "publishing map" << std::endl;
  ndt_map_pub.publish(*map_msg_ptr);

  // Writing Point Cloud data to PCD file
  if(voxel_leaf_size == 0.0){
    pcl::io::savePCDFileASCII(filename, *map_ptr);
    std::cout << "Saved " << map_ptr->points.size() << " data points to " << filename << "." << std::endl;
  }else{
    pcl::io::savePCDFileASCII(filename, *map_filtered);
    std::cout << "Saved " << map_filtered->points.size() << " data points to " << filename << "." << std::endl;
  }
}

static void points_callback(const sensor_msgs::PointCloud2::ConstPtr& input)
{
    pcl::PointCloud<pcl::PointXYZRGB> filtered_single_scan;
    filtered_single_scan = ps_processor.process_velodyne(input, tfListener);
    filtered_single_scan.header.frame_id = "base_link";

    double r;
    pcl::PointXYZI p;
    pcl::PointCloud<pcl::PointXYZI> tmp, scan;
    pcl::PointCloud<pcl::PointXYZRGB> tmp_rgb, scan_selected;
    pcl::PointCloud<pcl::PointXYZI>::Ptr filtered_scan_ptr (new pcl::PointCloud<pcl::PointXYZI>());
    pcl::PointCloud<pcl::PointXYZI>::Ptr transformed_scan_ptr (new pcl::PointCloud<pcl::PointXYZI>());
    pcl::PointCloud<pcl::PointXYZRGB>::Ptr transformed_rgb_ptr (new pcl::PointCloud<pcl::PointXYZRGB>());
    tf::Quaternion q;
    Eigen::Matrix4f t(Eigen::Matrix4f::Identity());

    tf::Transform transform;

    ros::Time scan_time = input->header.stamp;

    tf::StampedTransform velodyne_to_map;
    tfListener->waitForTransform("/base_link", input->header.frame_id,ros::Time::now(), ros::Duration(1.0));
    tfListener->lookupTransform("/base_link", input->header.frame_id, ros::Time(0), velodyne_to_map);

    sensor_msgs::PointCloud2 cloud_map;
    Eigen::Matrix4f eigen_transform;
    pcl_ros::transformAsMatrix (velodyne_to_map, eigen_transform);
    pcl_ros::transformPointCloud (eigen_transform, *input, cloud_map);

    // cloud_map.header.frame_id = "base_link";
   // pub_velodyne_base.publish(cloud_map);

    cloud_map.header.frame_id = "map";

////////////////////////////////// xi /////////////////////////////////////////
    pcl::fromROSMsg(cloud_map, tmp);
    pcl::fromROSMsg(cloud_map, tmp_rgb);
 //   pcl::fromROSMsg(*input, tmp);
///////////////////////////////////////////////////////////////////////////////

    int cloud_index = 0;
    for (pcl::PointCloud<pcl::PointXYZI>::const_iterator item = tmp.begin(); item != tmp.end(); item++)
    {
        p.x = (double) item->x;
        p.y = (double) item->y;
        p.z = (double) item->z;
        p.intensity = (double) item->intensity;

        r = sqrt(pow(p.x, 2.0) + pow(p.y, 2.0));
        if(r > RANGE)
        {
            scan.push_back(p);
        }

        // select points for other process
        if(r < 10)
        {
            scan_selected.push_back(tmp_rgb.points[cloud_index]);	
        }
        cloud_index++;
    }

    pcl::PointCloud<pcl::PointXYZI>::Ptr scan_ptr(new pcl::PointCloud<pcl::PointXYZI>(scan));

    // Add initial point cloud to velodyne_map
    if(initial_scan_loaded == 0){
      map_global += *scan_ptr;
      initial_scan_loaded = 1;
/////////////////////////////////////////////
      map_local.resize(map_local_length);  // local point cloud buffer for NDT registration 
      map_local[0] = *scan_ptr;

      map_terrain.resize(map_terrain_length);  // local sekected points buffer with color for publish to other processers
    //   map_terrain[0] = scan_selected;
      map_terrain[0] =  filtered_single_scan;
    }

    // Apply voxelgrid filter
    pcl::VoxelGrid<pcl::PointXYZI> voxel_grid_filter;
    voxel_grid_filter.setLeafSize(voxel_leaf_size, voxel_leaf_size, voxel_leaf_size);
    voxel_grid_filter.setInputCloud(scan_ptr);
    voxel_grid_filter.filter(*filtered_scan_ptr);

  //  pcl::PointCloud<pcl::PointXYZI>::Ptr map_ptr(new pcl::PointCloud<pcl::PointXYZI>(map_global));
    pcl::PointCloud<pcl::PointXYZI>::Ptr map_ptr(new pcl::PointCloud<pcl::PointXYZI>(get_local_map(map_local)));

    ndt.setTransformationEpsilon(trans_eps);
    ndt.setStepSize(step_size);
    ndt.setResolution(ndt_res);
    ndt.setMaximumIterations(iter);
    ndt.setInputSource(filtered_scan_ptr);
    ndt.setInputTarget(map_ptr);

    guess_pos.x = previous_pos.x + offset_x;
    guess_pos.y = previous_pos.y + offset_y;
    guess_pos.z = previous_pos.z + offset_z;
    guess_pos.roll = previous_pos.roll;
    guess_pos.pitch = previous_pos.pitch;
    guess_pos.yaw = previous_pos.yaw + offset_yaw;

    Eigen::AngleAxisf init_rotation_x(guess_pos.roll, Eigen::Vector3f::UnitX());
    Eigen::AngleAxisf init_rotation_y(guess_pos.pitch, Eigen::Vector3f::UnitY());
    Eigen::AngleAxisf init_rotation_z(guess_pos.yaw, Eigen::Vector3f::UnitZ());

    Eigen::Translation3f init_translation(guess_pos.x, guess_pos.y, guess_pos.z);

    Eigen::Matrix4f init_guess = (init_translation * init_rotation_z * init_rotation_y * init_rotation_x).matrix();

    t3_end = ros::Time::now();
    d3 = t3_end - t3_start;

    t4_start = ros::Time::now();

    pcl::PointCloud<pcl::PointXYZI>::Ptr output_cloud(new pcl::PointCloud<pcl::PointXYZI>);
    ndt.align(*output_cloud, init_guess);

    t = ndt.getFinalTransformation();

////////////////////////////////// xi /////////////////////////////////////////
    pcl::transformPointCloud(*scan_ptr, *transformed_scan_ptr, t);
    // pcl::transformPointCloud(scan_selected, *transformed_rgb_ptr, t);
    pcl::transformPointCloud(filtered_single_scan, *transformed_rgb_ptr, t);
    // cout << transformed_rgb_ptr->points.size() << " " << ps_processor.cloud_reformed_height.points.size();
    for(int i = 0; i < filtered_single_scan.size(); i++ )
    {
        transformed_rgb_ptr->points[i].z = ps_processor.cloud_reformed_height.points[i].z;
    }

///////////////////////////////////////////////////////////////////////////////
    tf::Matrix3x3 tf3d;

    tf3d.setValue(static_cast<double>(t(0, 0)), static_cast<double>(t(0, 1)), static_cast<double>(t(0, 2)),
          static_cast<double>(t(1, 0)), static_cast<double>(t(1, 1)), static_cast<double>(t(1, 2)),
          static_cast<double>(t(2, 0)), static_cast<double>(t(2, 1)), static_cast<double>(t(2, 2)));

    // Update current_pos.
    current_pos.x = t(0, 3);
    current_pos.y = t(1, 3);
    current_pos.z = t(2, 3);
    tf3d.getRPY(current_pos.roll, current_pos.pitch, current_pos.yaw, 1);

    transform.setOrigin(tf::Vector3(current_pos.x, current_pos.y, current_pos.z));
    q.setRPY(current_pos.roll, current_pos.pitch, current_pos.yaw);
    transform.setRotation(q);

    g_transform = transform;

    //br.sendTransform(tf::StampedTransform(transform, scan_time, "map", "base_link"));
    //br.sendTransform(tf::StampedTransform(transform, ros::Time::now(), "base_link", "map"));

    // Calculate the offset (curren_pos - previous_pos)
    offset_x = current_pos.x - previous_pos.x;
    offset_y = current_pos.y - previous_pos.y;
    offset_z = current_pos.z - previous_pos.z;
    offset_yaw = current_pos.yaw - previous_pos.yaw;

    // Update position and posture. current_pos -> previous_pos
    previous_pos.x = current_pos.x;
    previous_pos.y = current_pos.y;
    previous_pos.z = current_pos.z;
    previous_pos.roll = current_pos.roll;
    previous_pos.pitch = current_pos.pitch;
    previous_pos.yaw = current_pos.yaw;

    // Calculate the shift between added_pos and current_pos
    double shift = sqrt(pow(current_pos.x-added_pos.x, 2.0) + pow(current_pos.y-added_pos.y, 2.0));
    if(shift >= SHIFT){
      //map_global += *transformed_scan_ptr;
      added_pos.x = current_pos.x;
      added_pos.y = current_pos.y;
      added_pos.z = current_pos.z;
      added_pos.roll = current_pos.roll;
      added_pos.pitch = current_pos.pitch;
      added_pos.yaw = current_pos.yaw;



///////////////////////// Xi /////////////////////////
      map_local_index ++;
      if(map_local_index == map_local_length)
    map_local_index = 0;
      std::cout << map_local_index << std::endl;
      map_local[map_local_index] = *transformed_scan_ptr;
/////////////////////////////////////////////////////
    }

    if(shift > shift_terrain)
    {
    map_terrain_index ++;
    if(map_terrain_index == map_terrain_length)
        map_terrain_index = 0;

    map_terrain[map_terrain_index] = *transformed_rgb_ptr;
    std::cout << map_terrain_index << " " << map_terrain_length << std::endl;
    }

    pcl::PointCloud<pcl::PointXYZRGB> terrain_cloud = get_local_map(map_terrain);

  //  map_ptr = pcl::PointCloud<pcl::PointXYZI>::Ptr(new pcl::PointCloud<pcl::PointXYZI>(map_global));
  //  map_ptr = pcl::PointCloud<pcl::PointXYZI>::Ptr(new pcl::PointCloud<pcl::PointXYZI>(get_local_map(map_local)));

    sensor_msgs::PointCloud2::Ptr map_msg_ptr(new sensor_msgs::PointCloud2);
   // pcl::toROSMsg(*map_ptr, *map_msg_ptr);
    pcl::toROSMsg(terrain_cloud, *map_msg_ptr);

    map_msg_ptr->header.frame_id = "map";
    ndt_map_pub.publish(*map_msg_ptr);

    q.setRPY(current_pos.roll, current_pos.pitch, current_pos.yaw);
    current_pose_msg.header.frame_id = "map";
    current_pose_msg.header.stamp = scan_time;
    current_pose_msg.pose.position.x = current_pos.x;
    current_pose_msg.pose.position.y = current_pos.y;
    current_pose_msg.pose.position.z = current_pos.z;
    current_pose_msg.pose.orientation.x = q.x();
    current_pose_msg.pose.orientation.y = q.y();
    current_pose_msg.pose.orientation.z = q.z();
    current_pose_msg.pose.orientation.w = q.w();

    current_pose_pub.publish(current_pose_msg);

    std::cout << "-----------------------------------------------------------------" << std::endl;
    std::cout << "Sequence number: " << input->header.seq << std::endl;
    std::cout << "Number of scan points: " << scan_ptr->size() << " points." << std::endl;
    std::cout << "Number of filtered scan points: " << filtered_scan_ptr->size() << " points." << std::endl;
    std::cout << "transformed_scan_ptr: " << transformed_scan_ptr->points.size() << " points." << std::endl;
    std::cout << "map: " << map_global.points.size() << " points." << std::endl;
    std::cout << "NDT has converged: " << ndt.hasConverged() << std::endl;
    std::cout << "Fitness score: " << ndt.getFitnessScore() << std::endl;
    std::cout << "Number of iteration: " << ndt.getFinalNumIteration() << std::endl;
    std::cout << "(x,y,z,roll,pitch,yaw):" << std::endl;
    std::cout << "(" << current_pos.x << ", " << current_pos.y << ", " << current_pos.z << ", " << current_pos.roll << ", " << current_pos.pitch << ", " << current_pos.yaw << ")" << std::endl;
    std::cout << "Transformation Matrix:" << std::endl;
    std::cout << t << std::endl;
    std::cout << "shift: " << shift << std::endl;
    std::cout << "-----------------------------------------------------------------" << std::endl;

}

int main(int argc, char **argv)
{
    previous_pos.x = 0.0;
    previous_pos.y = 0.0;
    previous_pos.z = 0.0;
    previous_pos.roll = 0.0;
    previous_pos.pitch = 0.0;
    previous_pos.yaw = 0.0;

    current_pos.x = 0.0;
    current_pos.y = 0.0;
    current_pos.z = 0.0;
    current_pos.roll = 0.0;
    current_pos.pitch = 0.0;
    current_pos.yaw = 0.0;

    guess_pos.x = 0.0;
    guess_pos.y = 0.0;
    guess_pos.z = 0.0;
    guess_pos.roll = 0.0;
    guess_pos.pitch = 0.0;
    guess_pos.yaw = 0.0;

    added_pos.x = 0.0;
    added_pos.y = 0.0;
    added_pos.z = 0.0;
    added_pos.roll = 0.0;
    added_pos.pitch = 0.0;
    added_pos.yaw = 0.0;

    offset_x = 0.0;
    offset_y = 0.0;
    offset_z = 0.0;
    offset_yaw = 0.0;

    ros::init(argc, argv, "ndt_mapping");

    ros::NodeHandle nh;
    ros::NodeHandle private_nh("~");
    ros::Rate loop_rate(9);

    // setting parameters
    private_nh.getParam("range", RANGE);
    std::cout << "RANGE: " << RANGE << std::endl;
    private_nh.getParam("shift", SHIFT);
    std::cout << "SHIFT: " << SHIFT << std::endl;
///////////////////////////////////// Xi ////////////////////////
    private_nh.getParam("map_local_length", map_local_length);
    std::cout << "map_local_length: " << map_local_length << std::endl;
    private_nh.getParam("map_terrain_length", map_terrain_length);
    std::cout << "map_terrain_length: " << map_terrain_length << std::endl;
    private_nh.getParam("shift_terrain", shift_terrain);
    std::cout << "shift_terrain: " << shift_terrain << std::endl;
//////////////////////////////////////////////////////////////////////

    map_global.header.frame_id = "map";
    tfListener = new (tf::TransformListener);

    ndt_map_pub = nh.advertise<sensor_msgs::PointCloud2>("/ndt_map", 1000);
    current_pose_pub = nh.advertise<geometry_msgs::PoseStamped>("/current_pose", 1000);
    pub_velodyne_base = nh.advertise<sensor_msgs::PointCloud2>("/velodyne_points_base", 1);

    ros::Subscriber param_sub = nh.subscribe("config/ndt_mapping", 10, param_callback);
    ros::Subscriber output_sub = nh.subscribe("config/ndt_mapping_output", 10, output_callback);
    ros::Subscriber points_sub = nh.subscribe("points_raw", 1, points_callback);

    tf::TransformBroadcaster br;

    while (ros::ok())
    {
        br.sendTransform(tf::StampedTransform(g_transform.inverse(), ros::Time::now(), "base_link", "map"));
        ros::spinOnce();
        loop_rate.sleep();
    }

    return 0;
}
