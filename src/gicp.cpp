#include "gicp.h"

GicpLocalizer::GicpLocalizer(ros::NodeHandle &nh, ros::NodeHandle &private_nh)
:nh_(nh)
,private_nh_(private_nh)
,tf2_listener_(tf2_buffer_)
{
    key_value_stdmap_["state"] = "Initializing";
    init_params();

    sensor_aligned_pose_pub_ = nh_.advertise<sensor_msgs::PointCloud2>("points_aligned", 10);
    gicp_pose_pub_ = nh_.advertise<geometry_msgs::PoseStamped>("gicp_pose", 10);
    exe_time_pub_ = nh_.advertise<std_msgs::Float32>("exe_time_ms", 10);
    diagnostics_pub_ = nh_.advertise<diagnostic_msgs::DiagnosticArray>("diagnostics", 10);

    // 订阅初始化位姿
    initial_pose_sub_ = nh_.subscribe("initialpose", 100, &GicpLocalizer::callback_init_pose, this);
    // 订阅点云地图
    map_points_sub_ = nh_.subscribe("points_map", 1, &GicpLocalizer::callback_pointsmap, this);
    // 订阅lidar点云
    sensor_points_sub_ = nh_.subscribe("filtered_points", 1, &GicpLocalizer::callback_pointcloud, this);

    diagnostic_thread_ = std::thread(&GicpLocalizer::timer_diagnostic, this);
    diagnostic_thread_.detach();
}

GicpLocalizer::~GicpLocalizer() = default;

void GicpLocalizer::timer_diagnostic(){
    ros::Rate rate(100);
    while(ros::ok()){
        // 定义一个diag消息
        diagnostic_msgs::DiagnosticStatus diag_status_msg;
        diag_status_msg.name = "gicp_scan_matcher";
        diag_status_msg.hardware_id = "";

        // 将所有的诊断消息推入状态集合中
        for (const auto & key_value : key_value_stdmap_) {
            diagnostic_msgs::KeyValue key_value_msg;
            key_value_msg.key = key_value.first;
            key_value_msg.value = key_value.second;
            diag_status_msg.values.push_back(key_value_msg);
        }

        // 初始化时状态为OK
        diag_status_msg.level = diagnostic_msgs::DiagnosticStatus::OK;
        diag_status_msg.message = "";
        if (key_value_stdmap_.count("state") && key_value_stdmap_["state"] == "Initializing") {
            // 如果处在初始化阶段的话为警告（WARN），同时记录消息初始化状态
            diag_status_msg.level = diagnostic_msgs::DiagnosticStatus::WARN;
            diag_status_msg.message += "Initializing State. ";
        }
        if (key_value_stdmap_.count("skipping_publish_num") &&
            std::stoi(key_value_stdmap_["skipping_publish_num"]) > 1) {
            // 如果有跳过发布大于1时，则记为警告
            diag_status_msg.level = diagnostic_msgs::DiagnosticStatus::WARN;
            diag_status_msg.message += "skipping_publish_num > 1. ";
        }
        if (key_value_stdmap_.count("skipping_publish_num") &&
            std::stoi(key_value_stdmap_["skipping_publish_num"]) >= 5) {
            // 如果跳过的条数大于5（阈值）, 则报错
            diag_status_msg.level = diagnostic_msgs::DiagnosticStatus::ERROR;
            diag_status_msg.message += "skipping_publish_num exceed limit. ";
        }

        diagnostic_msgs::DiagnosticArray diag_msg;
        diag_msg.header.stamp = ros::Time::now();
        diag_msg.status.push_back(diag_status_msg);
        // 发布
        diagnostics_pub_.publish(diag_msg);
        rate.sleep();
    }
}

void GicpLocalizer::callback_init_pose(
   const geometry_msgs::PoseWithCovarianceStamped::ConstPtr & initial_pose_msg_ptr){
    if (initial_pose_msg_ptr->header.frame_id == map_frame_) {
        initial_pose_cov_msg_ = *initial_pose_msg_ptr;
    }else{
        // get TF from pose_frame to map_frame
        geometry_msgs::TransformStamped::Ptr TF_pose_to_map_ptr(new geometry_msgs::TransformStamped);
        get_transform(map_frame_, initial_pose_msg_ptr->header.frame_id, TF_pose_to_map_ptr);

        // transform pose_frame to map_frame
        geometry_msgs::PoseWithCovarianceStamped::Ptr mapTF_initial_pose_msg_ptr(
                new geometry_msgs::PoseWithCovarianceStamped);
        tf2::doTransform(*initial_pose_msg_ptr, *mapTF_initial_pose_msg_ptr, *TF_pose_to_map_ptr);
        // mapTF_initial_pose_msg_ptr->header.stamp = initial_pose_msg_ptr->header.stamp;
        initial_pose_cov_msg_ = *mapTF_initial_pose_msg_ptr;
    }

    init_pose = false;
}

void GicpLocalizer::callback_pointsmap(
   const sensor_msgs::PointCloud2::ConstPtr & map_points_msg_ptr){
    fast_gicp::FastVGICP<pcl::PointXYZ, pcl::PointXYZ> vgicp_new;

    vgicp_new.setResolution(resolution_);
    vgicp_new.setNumThreads(numThreads_);

    pcl::PointCloud<pcl::PointXYZ>::Ptr map_points_ptr(new pcl::PointCloud<pcl::PointXYZ>);
    pcl::fromROSMsg(*map_points_msg_ptr, *map_points_ptr);
//    vgicp_new.setInputTarget(map_points_ptr);

    pcl::PointCloud<pcl::PointXYZ>::Ptr filtered(new pcl::PointCloud<pcl::PointXYZ>());
    voxelgrid_.setInputCloud(map_points_ptr);
    voxelgrid_.filter(*filtered);
    map_points_ptr = filtered;

    vgicp_.setInputTarget(map_points_ptr);
    pcl::PointCloud<pcl::PointXYZ>::Ptr output_cloud(new pcl::PointCloud<pcl::PointXYZ>);
//    vgicp_new.align(*output_cloud, Eigen::Matrix4f::Identity());
    vgicp_.align(*output_cloud, Eigen::Matrix4f::Identity());

}

void GicpLocalizer::callback_pointcloud(
    const sensor_msgs::PointCloud2::ConstPtr & sensor_points_sensorTF_msg_ptr)
{
    const auto exe_start_time = std::chrono::system_clock::now();
    // add map mutex
    std::lock_guard<std::mutex> lock(gicp_map_mtx_);

    // 记录该frame的id和timestamp
    const std::string sensor_frame = sensor_points_sensorTF_msg_ptr->header.frame_id;
    const auto sensor_ros_time = sensor_points_sensorTF_msg_ptr->header.stamp;

    boost::shared_ptr<pcl::PointCloud<pcl::PointXYZ>> sensor_points_sensorTF_ptr(
            new pcl::PointCloud<pcl::PointXYZ>);

    pcl::fromROSMsg(*sensor_points_sensorTF_msg_ptr, *sensor_points_sensorTF_ptr);
    // get TF base to sensor
    geometry_msgs::TransformStamped::Ptr TF_base_to_sensor_ptr(new geometry_msgs::TransformStamped);
    get_transform(base_frame_, sensor_frame, TF_base_to_sensor_ptr);

    const Eigen::Affine3d base_to_sensor_affine = tf2::transformToEigen(*TF_base_to_sensor_ptr);
    const Eigen::Matrix4f base_to_sensor_matrix = base_to_sensor_affine.matrix().cast<float>();
    // 将得到的该帧点云转换到当前的tf关系下
    boost::shared_ptr<pcl::PointCloud<pcl::PointXYZ>> sensor_points_baselinkTF_ptr(
            new pcl::PointCloud<pcl::PointXYZ>);
    pcl::transformPointCloud(
            *sensor_points_sensorTF_ptr, *sensor_points_baselinkTF_ptr, base_to_sensor_matrix);

    vgicp_.setInputSource(sensor_points_baselinkTF_ptr);

    if(vgicp_.getInputTarget() == nullptr){
        ROS_WARN_STREAM_THROTTLE(1, "No MAP!");
        return;
    }
    // align
    Eigen::Matrix4f initial_pose_matrix;
    if (!init_pose){
        Eigen::Affine3d initial_pose_affine;
        tf2::fromMsg(initial_pose_cov_msg_.pose.pose, initial_pose_affine);
        initial_pose_matrix = initial_pose_affine.matrix().cast<float>();
        // for the first time, we don't know the pre_trans, so just use the init_trans,
        // which means, the delta trans for the second time is 0
        pre_trans = initial_pose_matrix;
        init_pose = true;
    }else
    {
        // use predicted pose as init guess (currently we only impl linear model)
        initial_pose_matrix = pre_trans * delta_trans;
    }

    pcl::PointCloud<pcl::PointXYZ>::Ptr output_cloud(new pcl::PointCloud<pcl::PointXYZ>);
    const auto align_start_time = std::chrono::system_clock::now();
    key_value_stdmap_["state"] = "Aligning";
    vgicp_.align(*output_cloud, initial_pose_matrix);
    key_value_stdmap_["state"] = "Sleeping";
    const auto align_end_time = std::chrono::system_clock::now();
    const float align_time = std::chrono::duration_cast<std::chrono::microseconds>(align_end_time - align_start_time).count() /1000.0;

    const Eigen::Matrix4f result_pose_matrix = vgicp_.getFinalTransformation();
    Eigen::Affine3d result_pose_affine;
    result_pose_affine.matrix() = result_pose_matrix.cast<double>();
    const geometry_msgs::Pose result_pose_msg = tf2::toMsg(result_pose_affine);

    const auto exe_end_time = std::chrono::system_clock::now();
    const auto exe_time = std::chrono::duration_cast<std::chrono::microseconds>(exe_end_time - exe_start_time).count() / 1000.0;
    // calculate the delta tf from pre_trans to current_trans
    delta_trans = pre_trans.inverse() * result_pose_matrix;

    // 显示出平移的距离
    Eigen::Vector3f delta_translation = delta_trans.block<3, 1>(0, 3);
    std::cout<<"delta x: "<<delta_translation(0) << " y: "<<delta_translation(1)<<
             " z: "<<delta_translation(2)<<std::endl;

    // 显示出旋转的角度
    Eigen::Matrix3f delta_rotation_matrix = delta_trans.block<3, 3>(0, 0);
    Eigen::Vector3f delta_euler = delta_rotation_matrix.eulerAngles(2,1,0);
    std::cout<<"delta yaw: "<<delta_euler(0) << " pitch: "<<delta_euler(1)<<
             " roll: "<<delta_euler(2)<<std::endl;

    pre_trans = result_pose_matrix;
    // publish
    geometry_msgs::PoseStamped result_pose_stamped_msg;
    result_pose_stamped_msg.header.stamp = sensor_ros_time;
    result_pose_stamped_msg.header.frame_id = map_frame_;
    result_pose_stamped_msg.pose = result_pose_msg;

    gicp_pose_pub_.publish(result_pose_stamped_msg);

    // publish tf(map frame to base frame)
    publish_tf(map_frame_, base_frame_, result_pose_stamped_msg);

    // publish aligned point cloud
    pcl::PointCloud<pcl::PointXYZ>::Ptr sensor_points_mapTF_ptr(new pcl::PointCloud<pcl::PointXYZ>);
    pcl::transformPointCloud(
            *sensor_points_baselinkTF_ptr, *sensor_points_mapTF_ptr, result_pose_matrix);
    sensor_msgs::PointCloud2 sensor_points_mapTF_msg;
    pcl::toROSMsg(*sensor_points_mapTF_ptr, sensor_points_mapTF_msg);
    sensor_points_mapTF_msg.header.stamp = sensor_ros_time;
    sensor_points_mapTF_msg.header.frame_id = map_frame_;
    sensor_aligned_pose_pub_.publish(sensor_points_mapTF_msg);

    std_msgs::Float32 exe_time_msg;
    exe_time_msg.data = exe_time;
    exe_time_pub_.publish(exe_time_msg);

    key_value_stdmap_["seq"] = std::to_string(sensor_points_sensorTF_msg_ptr->header.seq);
    std::cout << "------------------------------------------------" << std::endl;
    std::cout << "align_time: " << align_time << "ms" << std::endl;
    std::cout << "exe_time: " << exe_time << "ms" << std::endl;
}

void GicpLocalizer::init_params(){
    private_nh_.getParam("base_frame", base_frame_);
    ROS_INFO("base_frame_id: %s", base_frame_.c_str());

    leafsize_ = 0.1;
    resolution_ = 1.0;
    numThreads_ = 2;

    private_nh_.getParam("resolution", resolution_);
    private_nh_.getParam("numthreads", numThreads_);
    private_nh_.getParam("leafsize", leafsize_);

    voxelgrid_.setLeafSize(leafsize_, leafsize_, leafsize_);
    vgicp_.setResolution(resolution_);
    vgicp_.setNumThreads(numThreads_);

    map_frame_ = "map";

    ROS_INFO( "resolution: %lf, numthreads: %d", resolution_, numThreads_);
}

bool GicpLocalizer::get_transform(
   const std::string & target_frame, const std::string & source_frame,
   const geometry_msgs::TransformStamped::Ptr & transform_stamped_ptr, const ros::Time & time_stamp)
{
    if (target_frame == source_frame) {
        transform_stamped_ptr->header.stamp = time_stamp;
        transform_stamped_ptr->header.frame_id = target_frame;
        transform_stamped_ptr->child_frame_id = source_frame;
        transform_stamped_ptr->transform.translation.x = 0.0;
        transform_stamped_ptr->transform.translation.y = 0.0;
        transform_stamped_ptr->transform.translation.z = 0.0;
        transform_stamped_ptr->transform.rotation.x = 0.0;
        transform_stamped_ptr->transform.rotation.y = 0.0;
        transform_stamped_ptr->transform.rotation.z = 0.0;
        transform_stamped_ptr->transform.rotation.w = 1.0;
        return true;
    }

    try{
        *transform_stamped_ptr =
            tf2_buffer_.lookupTransform(target_frame, source_frame, time_stamp);
    } catch (tf2::TransformException & ex) {
        ROS_WARN("%s", ex.what());
        ROS_ERROR("Please publish TF %s to %s", target_frame.c_str(), source_frame.c_str());

        transform_stamped_ptr->header.stamp = time_stamp;
        transform_stamped_ptr->header.frame_id = target_frame;
        transform_stamped_ptr->child_frame_id = source_frame;
        transform_stamped_ptr->transform.translation.x = 0.0;
        transform_stamped_ptr->transform.translation.y = 0.0;
        transform_stamped_ptr->transform.translation.z = 0.0;
        transform_stamped_ptr->transform.rotation.x = 0.0;
        transform_stamped_ptr->transform.rotation.y = 0.0;
        transform_stamped_ptr->transform.rotation.z = 0.0;
        transform_stamped_ptr->transform.rotation.w = 1.0;
        return false;
    }
    return true;
}

bool GicpLocalizer::get_transform(
    const std::string & target_frame, const std::string & source_frame,
    const geometry_msgs::TransformStamped::Ptr & transform_stamped_ptr)
{
    if (target_frame == source_frame) {
        transform_stamped_ptr->header.stamp = ros::Time::now();
        transform_stamped_ptr->header.frame_id = target_frame;
        transform_stamped_ptr->child_frame_id = source_frame;
        transform_stamped_ptr->transform.translation.x = 0.0;
        transform_stamped_ptr->transform.translation.y = 0.0;
        transform_stamped_ptr->transform.translation.z = 0.0;
        transform_stamped_ptr->transform.rotation.x = 0.0;
        transform_stamped_ptr->transform.rotation.y = 0.0;
        transform_stamped_ptr->transform.rotation.z = 0.0;
        transform_stamped_ptr->transform.rotation.w = 1.0;
        return true;
    }

    try {
        *transform_stamped_ptr =
                tf2_buffer_.lookupTransform(target_frame, source_frame, ros::Time(0), ros::Duration(1.0));
    } catch (tf2::TransformException & ex) {
        ROS_WARN("%s", ex.what());
        ROS_ERROR("Please publish TF %s to %s", target_frame.c_str(), source_frame.c_str());

        transform_stamped_ptr->header.stamp = ros::Time::now();
        transform_stamped_ptr->header.frame_id = target_frame;
        transform_stamped_ptr->child_frame_id = source_frame;
        transform_stamped_ptr->transform.translation.x = 0.0;
        transform_stamped_ptr->transform.translation.y = 0.0;
        transform_stamped_ptr->transform.translation.z = 0.0;
        transform_stamped_ptr->transform.rotation.x = 0.0;
        transform_stamped_ptr->transform.rotation.y = 0.0;
        transform_stamped_ptr->transform.rotation.z = 0.0;
        transform_stamped_ptr->transform.rotation.w = 1.0;
        return false;
    }
    return true;
}

void GicpLocalizer::publish_tf(
        const std::string & frame_id, const std::string & child_frame_id,
        const geometry_msgs::PoseStamped & pose_msg)
{
    geometry_msgs::TransformStamped transform_stamped;
    transform_stamped.header.frame_id = frame_id;
    transform_stamped.child_frame_id = child_frame_id;
    transform_stamped.header.stamp = pose_msg.header.stamp;

    transform_stamped.transform.translation.x = pose_msg.pose.position.x;
    transform_stamped.transform.translation.y = pose_msg.pose.position.y;
    transform_stamped.transform.translation.z = pose_msg.pose.position.z;

    tf2::Quaternion tf_quaternion;
    tf2::fromMsg(pose_msg.pose.orientation, tf_quaternion);
    transform_stamped.transform.rotation.x = tf_quaternion.x();
    transform_stamped.transform.rotation.y = tf_quaternion.y();
    transform_stamped.transform.rotation.z = tf_quaternion.z();
    transform_stamped.transform.rotation.w = tf_quaternion.w();

    tf2_broadcaster_.sendTransform(transform_stamped);
}

int main(int argc, char **argv){
    ros::init(argc, argv, "gicp_localizer");
    ros::NodeHandle nh;
    ros::NodeHandle private_nh("~");

    GicpLocalizer gicp_localizer(nh, private_nh);
    ROS_INFO("\033[1;32m---->\033[0m Gicp Localizer Started.");
    ros::spin();

    return 0;
}