/*
 * Copyright 2018 Bonn-Rhein-Sieg University
 *
 * Author: Mohammad Wasil, Santosh Thoduka
 *
 */
#include <mcr_scene_segmentation/scene_segmentation.h>
#include <mcr_scene_segmentation/scene_segmentation_node.h>
#include <mcr_perception_msgs/BoundingBox.h>
#include <mcr_perception_msgs/BoundingBoxList.h>
#include <mcr_perception_msgs/ObjectList.h>
#include <mcr_perception_msgs/RecognizeObject.h>
#include <mcr_scene_segmentation/impl/helpers.hpp>
#include <mas_perception_libs/bounding_box.h>
#include <mas_perception_libs/point_cloud_utils.h>
#include <mas_perception_libs/sac_plane_segmenter.h>

#include <pcl_conversions/pcl_conversions.h>
#include <pcl/point_types.h>
#include <pcl/PCLPointCloud2.h>
#include <pcl/common/centroid.h>
#include <pcl_ros/transforms.h>
#include <pcl/io/pcd_io.h>
#include <pcl_ros/point_cloud.h>

#include <Eigen/Dense>
#include <std_msgs/Float64.h>
#include <vector>
#include <string>
#include <iostream>
#include <fstream>

using mas_perception_libs::CloudFilterParams;
using mas_perception_libs::SacPlaneSegmenterParams;

SceneSegmentationNode::SceneSegmentationNode(): nh_("~"),
    bounding_box_visualizer_("bounding_boxes", Color(Color::SEA_GREEN)),
    cluster_visualizer_("tabletop_clusters"),
    label_visualizer_("labels", Color(Color::TEAL)),
    add_to_octree_(false), object_id_(0), debug_mode_(false), dataset_collection_(false)
{
    pub_debug_ = nh_.advertise<sensor_msgs::PointCloud2>("output", 1);
    pub_object_list_ = nh_.advertise<mcr_perception_msgs::ObjectList>("object_list", 1);
    sub_event_in_ = nh_.subscribe("event_in", 1, &SceneSegmentationNode::eventCallback, this);
    pub_event_out_ = nh_.advertise<std_msgs::String>("event_out", 1);
    pub_workspace_height_ = nh_.advertise<std_msgs::Float64>("workspace_height", 1);

    dynamic_reconfigure::Server<mcr_scene_segmentation::SceneSegmentationConfig>::CallbackType f =
                            boost::bind(&SceneSegmentationNode::configCallback, this, _1, _2);
    server_.setCallback(f);

    nh_.param<std::string>("object_recognizer_service_name", object_recognizer_service_name_, 
                            "/mcr_perception/object_recognizer/recognize_object");
    
    recognize_service = nh_.serviceClient<mcr_perception_msgs::RecognizeObject>(object_recognizer_service_name_);
    recognize_service.waitForExistence(ros::Duration(5));
    if (recognize_service.exists())
    {
        ROS_INFO_STREAM("Using object recognizer" << object_recognizer_service_name_);
    }
    else
    {
        ROS_WARN("Object recognition service is not available. Will return 'unknown' for all objects");
    }
    nh_.param("octree_resolution", octree_resolution_, 0.05);
    cloud_accumulation_ = CloudAccumulation::UPtr(new CloudAccumulation(octree_resolution_));

    nh_.param<bool>("debug_mode", debug_mode_, "false");
    nh_.param<bool>("dataset_collection", dataset_collection_, "false");
    nh_.param<std::string>("logdir", logdir_, "/tmp/");
}

SceneSegmentationNode::~SceneSegmentationNode() = default;

void SceneSegmentationNode::pointcloudCallback(const sensor_msgs::PointCloud2::Ptr &msg)
{
    if (add_to_octree_)
    {
        std::string target_frame_id;
        nh_.param<std::string>("target_frame_id", target_frame_id, "base_link");
        sensor_msgs::PointCloud2 msg_transformed;
        msg_transformed.header.frame_id = target_frame_id;
        try
        {
            ros::Time common_time;
            transform_listener_.getLatestCommonTime(target_frame_id, msg->header.frame_id, common_time, NULL);
            msg->header.stamp = common_time;
            transform_listener_.waitForTransform(target_frame_id, msg->header.frame_id,
                                                 ros::Time::now(), ros::Duration(1.0));
            pcl_ros::transformPointCloud(target_frame_id, *msg, msg_transformed, transform_listener_);
        }
        catch (tf::TransformException &ex)
        {
            ROS_WARN("PCL transform error: %s", ex.what());
            ros::Duration(1.0).sleep();
            return;
        }
        PointCloud::Ptr cloud = boost::make_shared<PointCloud>();
        pcl::PCLPointCloud2 pc2;
        pcl_conversions::toPCL(msg_transformed, pc2);
        pcl::fromPCLPointCloud2(pc2, *cloud);

        cloud_accumulation_->addCloud(cloud);

        frame_id_ = msg_transformed.header.frame_id;
        
        ros::Time end_time = ros::Time::now();

        if (dataset_collection_)
        {
            segment();
            cloud_accumulation_->reset();
        }
        std_msgs::String event_out;
        add_to_octree_ = false;
        event_out.data = "e_add_cloud_stopped";
        pub_event_out_.publish(event_out);
    }
}

void SceneSegmentationNode::segment()
{
    PointCloud::Ptr cloud = boost::make_shared<PointCloud>();
    cloud->header.frame_id = frame_id_;
    cloud_accumulation_->getAccumulatedCloud(*cloud);

    std::vector<PointCloud::Ptr> clusters;
    std::vector<BoundingBox> boxes;
    double workspace_height;
    PointCloud::Ptr debug = scene_segmentation_.segment_scene(cloud, clusters, boxes, workspace_height);
    debug->header.frame_id = cloud->header.frame_id;
    std_msgs::Float64 workspace_height_msg;
    workspace_height_msg.data = workspace_height;
    pub_workspace_height_.publish(workspace_height_msg);
    pub_debug_.publish(*debug);

    mcr_perception_msgs::BoundingBoxList bounding_boxes;
    mcr_perception_msgs::ObjectList object_list;
    geometry_msgs::PoseArray poses;

    bounding_boxes.bounding_boxes.resize(boxes.size());
    object_list.objects.resize(boxes.size());

    std::vector<std::string> labels;

    ros::Time now = ros::Time::now();
    for (int i = 0; i < boxes.size(); i++)
    {
        convertBoundingBox(boxes[i], bounding_boxes.bounding_boxes[i]);

        sensor_msgs::PointCloud2 ros_cloud;
        pcl::PCLPointCloud2 pc2;
        pcl::toPCLPointCloud2(*clusters[i], pc2);
        pcl_conversions::fromPCL(pc2, ros_cloud);

        if (recognize_service.exists())
        {
            mcr_perception_msgs::RecognizeObject srv;
            srv.request.cloud = ros_cloud;
            srv.request.dimensions = bounding_boxes.bounding_boxes[i].dimensions;
            if (recognize_service.call(srv))
            {
                object_list.objects[i].name = srv.response.name;
                object_list.objects[i].probability = srv.response.probability;
            }
            else
            {
                ROS_WARN("Object recognition service call failed");
                object_list.objects[i].name = "unknown";
                object_list.objects[i].probability = 0.0;
            }
        }
        else
        {
            object_list.objects[i].name = "unknown";
            object_list.objects[i].probability = 0.0;
        }
        labels.push_back(object_list.objects[i].name);

        geometry_msgs::PoseStamped pose = getPose(boxes[i]);
        pose.header.stamp = now;
        pose.header.frame_id = frame_id_;

        std::string target_frame_id;
        if (nh_.hasParam("target_frame_id"))
        {
            nh_.param("target_frame_id", target_frame_id, frame_id_);
            if (target_frame_id != frame_id_)
            {
                try
                {
                    ros::Time common_time;
                    transform_listener_.getLatestCommonTime(frame_id_, target_frame_id, common_time, NULL);
                    pose.header.stamp = common_time;
                    transform_listener_.waitForTransform(target_frame_id, frame_id_, common_time, ros::Duration(0.1));
                    geometry_msgs::PoseStamped pose_transformed;
                    transform_listener_.transformPose(target_frame_id, pose, pose_transformed);
                    object_list.objects[i].pose = pose_transformed;
                }
                catch(tf::LookupException& ex)
                {
                    ROS_WARN("Failed to transform pose: (%s)", ex.what());
                    pose.header.stamp = now;
                    object_list.objects[i].pose = pose;
                }
            }
            else
            {
                object_list.objects[i].pose = pose;
            }
        }
        else
        {
            object_list.objects[i].pose = pose;
        }
        // publish cluster, will be used for object_list_merger
        object_list.objects[i].pointcloud = ros_cloud;

        poses.poses.push_back(object_list.objects[i].pose.pose);
        poses.header = object_list.objects[i].pose.header;

        object_list.objects[i].database_id = object_id_;
        object_id_++;

        if (dataset_collection_ || debug_mode_)
        {
            PointCloud::Ptr pointcloud = boost::make_shared<PointCloud>();
            pcl::fromROSMsg(ros_cloud, *pointcloud);
            SceneSegmentationNode::savePcd(pointcloud, object_list.objects[i].name);
        }
    }
    pub_object_list_.publish(object_list);
    bounding_box_visualizer_.publish(bounding_boxes.bounding_boxes, frame_id_);
    cluster_visualizer_.publish<PointT>(clusters, frame_id_);
    label_visualizer_.publish(labels, poses);

    ros::Time end_time = ros::Time::now();
}

void SceneSegmentationNode::savePcd(const PointCloud::ConstPtr &pointcloud, std::string obj_name)
{
    std::stringstream filename; // stringstream used for the conversion
    ros::Time time_now = ros::Time::now();
    filename.str(""); //clearing the stringstream
    if (debug_mode_)
    {
        filename << logdir_ << obj_name << "_" << time_now <<".pcd";
    }
    else
    {
        filename << logdir_ <<"pcd_" << time_now <<".pcd";
    }
    ROS_INFO_STREAM("Saving pointcloud to " << logdir_);
    pcl::io::savePCDFileASCII(filename.str(), *pointcloud);
}

void SceneSegmentationNode::findPlane()
{
    PointCloud::Ptr cloud = boost::make_shared<PointCloud>();
    cloud->header.frame_id = frame_id_;
    cloud_accumulation_->getAccumulatedCloud(*cloud);

    double workspace_height = 0.0;
    PointCloud::Ptr hull;
    Eigen::Vector4f coefficients;
    PointCloud::Ptr debug = scene_segmentation_.findPlane(cloud, hull, coefficients, workspace_height);
    debug->header.frame_id = cloud->header.frame_id;
    std_msgs::Float64 workspace_height_msg;
    workspace_height_msg.data = workspace_height;
    pub_workspace_height_.publish(workspace_height_msg);
    pub_debug_.publish(*debug);
}

geometry_msgs::PoseStamped SceneSegmentationNode::getPose(const BoundingBox &box)
{
    BoundingBox::Points vertices = box.getVertices();
    Eigen::Vector3f n1;
    Eigen::Vector3f n2;
    Eigen::Vector3f n3 = (vertices[4] - vertices[0]) / (vertices[4] - vertices[0]).norm();
    if ((vertices[1] - vertices[0]).norm() > (vertices[3] - vertices[0]).norm())
    {
        n1 = (vertices[1] - vertices[0]) / (vertices[1] - vertices[0]).norm();
    }
    else
    {
        n1 = (vertices[3] - vertices[0]) / (vertices[3] - vertices[0]).norm();
    }
    n2 = n3.cross(n1);
    ROS_INFO_STREAM("got norms");
    Eigen::Matrix3f m;
    m << n1 , n2 , n3;
    Eigen::Quaternion<float> q(m);
    q.normalize();

    double workspace_height = (vertices[0](2) + vertices[1](2) + vertices[2](2) + vertices[3](2)) / 4.0;

    Eigen::Vector3f centroid = box.getCenter();
    geometry_msgs::PoseStamped pose;
    pose.pose.position.x = centroid(0);
    pose.pose.position.y = centroid(1);
    pose.pose.position.z = workspace_height + object_height_above_workspace_;
    pose.pose.orientation.x = q.x();
    pose.pose.orientation.y = q.y();
    pose.pose.orientation.z = q.z();
    pose.pose.orientation.w = q.w();
    return pose;
}

void SceneSegmentationNode::eventCallback(const std_msgs::String::ConstPtr &msg)
{
    std_msgs::String event_out;
    if (msg->data == "e_start")
    {
        sub_cloud_ = nh_.subscribe("input", 1, &SceneSegmentationNode::pointcloudCallback, this);
        event_out.data = "e_started";
    }
    else if (msg->data == "e_add_cloud_start")
    {
        add_to_octree_ = true;
        // Not needed so that not to affect the action server
        return;
    }
    else if (msg->data == "e_add_cloud_stop")
    {
        add_to_octree_ = false;
        event_out.data = "e_add_cloud_stopped";
    }
    else if (msg->data == "e_find_plane")
    {
        findPlane();
        cloud_accumulation_->reset();
        event_out.data = "e_done";
    }
    else if (msg->data == "e_segment")
    {
        segment();
        cloud_accumulation_->reset();
        event_out.data = "e_done";
    }
    else if (msg->data == "e_reset")
    {
        cloud_accumulation_->reset();
        event_out.data = "e_reset";
    }
    else if (msg->data == "e_stop")
    {
        sub_cloud_.shutdown();
        cloud_accumulation_->reset();
        event_out.data = "e_stopped";
    }
    else
    {
        return;
    }
    pub_event_out_.publish(event_out);
}

void SceneSegmentationNode::configCallback(mcr_scene_segmentation::SceneSegmentationConfig &config, uint32_t level)
{
    CloudFilterParams cloudFilterParams;
    cloudFilterParams.mPassThroughLimitMinX = static_cast<float>(config.passthrough_limit_min_x);
    cloudFilterParams.mPassThroughLimitMaxX = static_cast<float>(config.passthrough_limit_max_x);
    cloudFilterParams.mPassThroughLimitMinY = static_cast<float>(config.passthrough_limit_min_y);
    cloudFilterParams.mPassThroughLimitMaxY = static_cast<float>(config.passthrough_limit_max_y);
    cloudFilterParams.mVoxelLimitMinZ = static_cast<float>(config.voxel_limit_min_z);
    cloudFilterParams.mVoxelLimitMaxZ = static_cast<float>(config.voxel_limit_max_z);
    cloudFilterParams.mVoxelLeafSize = static_cast<float>(config.voxel_leaf_size);
    scene_segmentation_.setCloudFilterParams(cloudFilterParams);

    SacPlaneSegmenterParams planeFitParams;
    planeFitParams.mNormalRadiusSearch = config.normal_radius_search;
    planeFitParams.mSacMaxIterations = config.sac_max_iterations;
    planeFitParams.mSacDistThreshold = config.sac_distance_threshold;
    planeFitParams.mSacOptimizeCoeffs = config.sac_optimize_coefficients;
    planeFitParams.mSacEpsAngle = config.sac_eps_angle;
    planeFitParams.mSacNormalDistWeight = config.sac_normal_distance_weight;
    scene_segmentation_.setPlaneSegmenterParams(planeFitParams);

    scene_segmentation_.setPrismParams(config.prism_min_height, config.prism_max_height);
    scene_segmentation_.setOutlierParams(config.outlier_radius_search, config.outlier_min_neighbors);
    scene_segmentation_.setClusterParams(config.cluster_tolerance, config.cluster_min_size, config.cluster_max_size,
            config.cluster_min_height, config.cluster_max_height, config.cluster_max_length,
            config.cluster_min_distance_to_polygon);
    object_height_above_workspace_ = config.object_height_above_workspace;
}

int main(int argc, char** argv)
{
    ros::init(argc, argv, "scene_segmentation_node");
    SceneSegmentationNode scene_seg;
    ros::spin();
    return 0;
}

