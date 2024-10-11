#include <simple_loop_closure/simple_loop_closure_node.hpp>

SimpleLoopClosureNode::SimpleLoopClosureNode() : Node("simple_loop_closure")
{
  initialize();
  // spin();
}

void SimpleLoopClosureNode::initialize()
{
  // nh_ = ros::NodeHandle();
  // nh_private_ = ros::NodeHandle("~");
  pub_map_cloud_ = this->create_publisher<sensor_msgs::msg::PointCloud2>("/pgo_map_cloud", 1);
  pub_vis_pose_graph_ = this->create_publisher<visualization_msgs::msg::MarkerArray>("/vis_pose_graph", 1);
  pub_pgo_odometry_ = this->create_publisher<nav_msgs::msg::Odometry>("/pgo_odom", 1);

  this->declare_parameter<bool>("mapped_cloud", true);
  this->declare_parameter<double>("time_stamp_tolerance", 0.01);
  this->declare_parameter<double>("keyframe_dist_th", 0.3);
  this->declare_parameter<double>("keyframe_angular_dist_th", 0.2);
  this->declare_parameter<double>("loop_search_time_diff_th", 30.0);
  this->declare_parameter<double>("loop_search_dist_diff_th", 30.0);
  this->declare_parameter<double>("loop_search_angular_dist_th", 3.14);
  this->declare_parameter<int>("loop_search_frame_interval", 1);
  this->declare_parameter<double>("search_radius", 15.0);
  this->declare_parameter<int>("target_frame_num", 50);
  this->declare_parameter<double>("target_voxel_leaf_size", 0.4);
  this->declare_parameter<double>("source_voxel_leaf_size", 0.4);
  this->declare_parameter<double>("vis_map_voxel_leaf_size", 0.8);
  this->declare_parameter<double>("fitness_score_th", 0.3);
  this->declare_parameter<int>("vis_map_cloud_frame_interval", 3);

  this->get_parameter("mapped_cloud", mapped_cloud_);
  this->get_parameter("time_stamp_tolerance", time_stamp_tolerance_);
  this->get_parameter("keyframe_dist_th", keyframe_dist_th_);
  this->get_parameter("keyframe_angular_dist_th", keyframe_angular_dist_th_);
  this->get_parameter("loop_search_time_diff_th", loop_search_time_diff_th_);
  this->get_parameter("loop_search_dist_diff_th", loop_search_dist_diff_th_);
  this->get_parameter("loop_search_angular_dist_th", loop_search_angular_dist_th_);
  this->get_parameter("loop_search_frame_interval", loop_search_frame_interval_);
  this->get_parameter("search_radius", search_radius_);
  this->get_parameter("target_frame_num", target_frame_num_);
  this->get_parameter("target_voxel_leaf_size", target_voxel_leaf_size_);
  this->get_parameter("source_voxel_leaf_size", source_voxel_leaf_size_);
  this->get_parameter("vis_map_voxel_leaf_size", vis_map_voxel_leaf_size_);
  this->get_parameter("fitness_score_th", fitness_score_th_);
  this->get_parameter("vis_map_cloud_frame_interval", vis_map_cloud_frame_interval_);

  search_radius_ *= search_radius_;

  sub_cloud_.subscribe(this, "/cloud");
  sub_odom_.subscribe(this, "/odometry");

  // synchronizer_.reset(new Sync(SyncPolicy(50), sub_cloud_, sub_odom_));
  // synchronizer_.reset(new message_filters::Synchronizer<SyncPolicy>(SyncPolicy(50), sub_cloud_, sub_odom_));
  // synchronizer_->setMaxIntervalDuration(rclcpp::Duration(time_stamp_tolerance_, 0));

  synchronizer_ = std::make_shared<Sync>(SyncPolicy(50), sub_cloud_, sub_odom_);
  synchronizer_->setMaxIntervalDuration(rclcpp::Duration::from_seconds(time_stamp_tolerance_));
  synchronizer_->registerCallback(std::bind(&SimpleLoopClosureNode::pointCloudAndOdometryCallback, this,
                                            std::placeholders::_1, std::placeholders::_2));

  // sub_save_req_  = this->create_subscription<std_msgs::msg::String>("/save_req", 1,
  // &SimpleLoopClosureNode::saveRequestCallback, this);
  sub_save_req_ = this->create_subscription<std_msgs::msg::String>(
      "/save_req", 10, std::bind(&SimpleLoopClosureNode::saveRequestCallback, this, std::placeholders::_1));

  icp_.setMaximumIterations(50);
  icp_.setMaxCorrespondenceDistance(search_radius_ * 2.0);
  icp_.setTransformationEpsilon(0.0001);
  icp_.setEuclideanFitnessEpsilon(0.0001);
  icp_.setRANSACIterations(0);

  vg_target_.setLeafSize(target_voxel_leaf_size_, target_voxel_leaf_size_, target_voxel_leaf_size_);
  vg_source_.setLeafSize(source_voxel_leaf_size_, source_voxel_leaf_size_, source_voxel_leaf_size_);
  vg_map_.setLeafSize(vis_map_voxel_leaf_size_, vis_map_voxel_leaf_size_, vis_map_voxel_leaf_size_);

  added_odom_id_ = 0;
  searched_loop_id_ = 0;
  keyframes_cloud_.clear();
  keyframes_odom_.clear();

  gtsam::ISAM2Params parameters;
  parameters.relinearizeThreshold = 0.01;
  parameters.relinearizeSkip = 1;
  isam2_ = gtsam::ISAM2(parameters);

  Eigen::VectorXd prior_noise_vector(6);
  prior_noise_vector << 1e-12, 1e-12, 1e-12, 1e-12, 1e-12, 1e-12;
  prior_noise_ = gtsam::noiseModel::Diagonal::Variances(prior_noise_vector);
  Eigen::VectorXd odom_noise_vector(6);
  odom_noise_vector << 1e-6, 1e-6, 1e-6, 1e-4, 1e-4, 1e-4;
  odom_noise_ = gtsam::noiseModel::Diagonal::Variances(odom_noise_vector);

  odom_edge_color_.r = 0.0;
  odom_edge_color_.g = 0.75;
  odom_edge_color_.b = 1.0;
  odom_edge_color_.a = 1.0;

  loop_edge_color_.r = 1.0;
  loop_edge_color_.g = 0.75;
  loop_edge_color_.b = 0.0;
  loop_edge_color_.a = 1.0;

  node_color_.r = 0.5;
  node_color_.g = 1.0;
  node_color_.b = 0.0;
  node_color_.a = 1.0;

  edge_scale_ = 0.1;
  node_scale_ = 0.15;

  saving_ = false;
}

bool SimpleLoopClosureNode::saveEachFrames()
{
  std::string frames_directory = save_directory_ + "frames/";
  if (!std::filesystem::create_directory(frames_directory))
  {
    return false;
  }

  int optimization_result_size = 0;
  {
    MtxLockGuard guard(mtx_res_);
    optimization_result_size = optimization_result_.size();
  }

  if (optimization_result_size <= 0)
    return false;

  int digits = std::to_string(optimization_result_size).length();
  if (digits < 6)
    digits = 6;

  std::string poses_csv_filename = save_directory_ + "poses.csv";
  std::ofstream poses_csv_file(poses_csv_filename);
  if (!poses_csv_file)
  {
    return false;
  }
  poses_csv_file << "index, timestamp, x, y, z, qx, qy, qz, qw" << std::endl;
  for (int i = 0; i < optimization_result_size; i++)
  {
    Eigen::Affine3d pose;
    {
      MtxLockGuard guard(mtx_res_);
      pose = Eigen::Affine3d(optimization_result_.at<gtsam::Pose3>(i).matrix());
    }

    PointCloudType copied_cloud;
    {
      MtxLockGuard guard(mtx_buf_);
      copied_cloud = *keyframes_cloud_[i];
    }

    std::stringstream frame_filename_str;
    frame_filename_str << std::setfill('0') << std::right << std::setw(digits) << i << ".pcd";
    Eigen::Quaterniond quat(pose.rotation());
    Eigen::Vector3d trans = pose.translation();
    pcl::io::savePCDFileBinary(frames_directory + frame_filename_str.str(), copied_cloud);
    poses_csv_file << std::fixed << i << ", " << pcl_conversions::fromPCL(copied_cloud.header.stamp).seconds() << ", "
                   << trans.x() << ", " << trans.y() << ", " << trans.z() << ", " << quat.x() << ", " << quat.y()
                   << ", " << quat.z() << ", " << quat.w() << std::endl;
  }

  poses_csv_file.close();

  return true;
}

void SimpleLoopClosureNode::saveThread()
{
  PointCloudType::Ptr map_cloud = constructPointCloudMap();

  if (map_cloud == NULL)
  {
    RCLCPP_WARN_STREAM(this->get_logger(), "Point cloud map is empty.");
  }
  else if (map_cloud->empty())
  {
    RCLCPP_WARN_STREAM(this->get_logger(), "Point cloud map is empty.");
  }
  else
  {
    try
    {
      if (!saveEachFrames())
      {
        throw std::runtime_error("Save failed");
      }
      pcl::io::savePCDFileBinary(save_directory_ + "map.pcd", *map_cloud);
      RCLCPP_INFO_STREAM(this->get_logger(), "Save completed.");
    }
    catch (...)
    {
      RCLCPP_WARN_STREAM(this->get_logger(), "Save failed.");
    }
  }

  saving_ = false;
}

void SimpleLoopClosureNode::saveRequestCallback(const std_msgs::msg::String::ConstSharedPtr& directory)
{
  if (saving_ == true)
  {
    RCLCPP_WARN_STREAM(this->get_logger(), "Already in process. Request is denied.");
    return;
  }

  save_directory_ = directory->data;
  if (save_directory_.back() != '/')
    save_directory_ += '/';

  RCLCPP_INFO_STREAM(this->get_logger(), "Start Saving.");
  if (save_thread_.joinable())
    save_thread_.join();

  saving_ = true;
  save_thread_ = std::thread(&SimpleLoopClosureNode::saveThread, this);
}

void SimpleLoopClosureNode::publishPoseGraphOptimizedOdometry(const Eigen::Affine3d& affine_curr,
                                                              const nav_msgs::msg::Odometry& odom_msg)
{
  Eigen::Affine3d pgo_affine;

  Eigen::Affine3d optimized_pose_last;
  int optimized_pose_id_last;
  getLastOptimizedPose(optimized_pose_last, optimized_pose_id_last);

  if (optimized_pose_id_last != 0)
  {
    MtxLockGuard guard(mtx_buf_);
    pgo_affine = optimized_pose_last * (keyframes_odom_[optimized_pose_id_last]->inverse() * affine_curr);
  }
  else
  {
    pgo_affine = affine_curr;
  }

  nav_msgs::msg::Odometry pgo_odom_msg = odom_msg;
  // tf2::toMsg(pgo_affine, pgo_odom_msg.pose.pose);
  pgo_odom_msg.pose.pose = tf2::toMsg(pgo_affine);
  pub_pgo_odometry_->publish(pgo_odom_msg);
}

void SimpleLoopClosureNode::pointCloudAndOdometryCallback(
    const sensor_msgs::msg::PointCloud2::ConstSharedPtr& cloud_msg,
    const nav_msgs::msg::Odometry::ConstSharedPtr& odom_msg)
{
  PointCloudType::Ptr cloud_curr(new PointCloudType);
  pcl::fromROSMsg(*cloud_msg, *cloud_curr);

  Affine3dPtr affine_curr(new Eigen::Affine3d);
  tf2::fromMsg(odom_msg->pose.pose, *affine_curr);

  publishPoseGraphOptimizedOdometry(*affine_curr, *odom_msg);

  if (!keyframes_odom_.empty())
  {
    MtxLockGuard guard(mtx_buf_);
    double distance = (keyframes_odom_.back()->translation() - affine_curr->translation()).norm();
    Eigen::Quaterniond quat_prev(keyframes_odom_.back()->rotation());
    Eigen::Quaterniond quat_curr(affine_curr->rotation());
    double angle = quat_prev.angularDistance(quat_curr);

    if (distance < keyframe_dist_th_ && angle < keyframe_angular_dist_th_)
      return;
  }
  else
  {
    // first call
    odom_frame_id_ = odom_msg->header.frame_id;
  }

  if (mapped_cloud_)
  {
    PointCloudType::Ptr cloud_base(new PointCloudType);
    Eigen::Affine3d affine_inv = affine_curr->inverse();
    pcl::transformPointCloud(*cloud_curr, *cloud_base, affine_inv);
    cloud_curr = cloud_base;
  }

  {
    MtxLockGuard guard(mtx_buf_);
    if (trajectory_dist_.empty())
      trajectory_dist_.push_back(0);
    else
      trajectory_dist_.push_back(trajectory_dist_.back() +
                                 (keyframes_odom_.back()->translation() - affine_curr->translation()).norm());

    keyframes_cloud_.push_back(cloud_curr);
    keyframes_odom_.push_back(affine_curr);
  }
}

pcl::PointCloud<pcl::PointXYZI>::Ptr SimpleLoopClosureNode::constructPointCloudMap(const int interval)
{
  int optimization_result_size = 0;
  int first_cloud_size = 0;
  {
    MtxLockGuard guard(mtx_res_);
    optimization_result_size = optimization_result_.size();
    if (!optimization_result_.empty())
      first_cloud_size = keyframes_cloud_.front()->size();
  }

  if (optimization_result_size <= 0)
    return NULL;

  // PointCloudType::Ptr map_cloud(new PointCloudType);
  pcl::PointCloud<pcl::PointXYZI>::Ptr map_cloud(new PointCloudType);
  map_cloud->reserve((size_t)((double)(first_cloud_size * optimization_result_size) * 1.5));
  for (int i = 0; i < optimization_result_size; i += (interval + 1))
  {
    Eigen::Affine3d pose;
    {
      MtxLockGuard guard(mtx_res_);
      pose = Eigen::Affine3d(optimization_result_.at<gtsam::Pose3>(i).matrix());
    }

    PointCloudType transformed_cloud;
    {
      MtxLockGuard guard(mtx_buf_);
      pcl::transformPointCloud(*keyframes_cloud_[i], transformed_cloud, pose);
    }

    *map_cloud += transformed_cloud;
  }

  return map_cloud;
}

void SimpleLoopClosureNode::publishMapCloud(const PointCloudType::Ptr& map_cloud)
{
  if (map_cloud == NULL)
    return;

  PointCloudType map_cloud_ds_;

  vg_map_.setInputCloud(map_cloud);
  vg_map_.filter(map_cloud_ds_);

  sensor_msgs::msg::PointCloud2 map_cloud_msg;
  pcl::toROSMsg(map_cloud_ds_, map_cloud_msg);
  map_cloud_msg.header.stamp = this->now();
  map_cloud_msg.header.frame_id = odom_frame_id_;

  pub_map_cloud_->publish(map_cloud_msg);
}

void SimpleLoopClosureNode::constructVisualizationOdometryEdges(const int& res_size,
                                                                const std_msgs::msg::Header& header,
                                                                visualization_msgs::msg::Marker& marker_msg)
{
  marker_msg.header = header;
  marker_msg.ns = "odom_edges";
  marker_msg.action = visualization_msgs::msg::Marker::ADD;
  marker_msg.pose.orientation.w = 1.0;
  marker_msg.id = 0;
  marker_msg.type = visualization_msgs::msg::Marker::LINE_LIST;
  marker_msg.scale.x = edge_scale_;
  marker_msg.color = odom_edge_color_;

  marker_msg.points.clear();
  marker_msg.points.reserve(res_size * 2);
  for (int i = 0; i < res_size - 1; i++)
  {
    MtxLockGuard guard(mtx_res_);
    gtsam::Pose3 pose1 = optimization_result_.at<gtsam::Pose3>(i);
    gtsam::Pose3 pose2 = optimization_result_.at<gtsam::Pose3>(i + 1);

    geometry_msgs::msg::Point p1, p2;
    p1.x = pose1.x();
    p1.y = pose1.y();
    p1.z = pose1.z();
    p2.x = pose2.x();
    p2.y = pose2.y();
    p2.z = pose2.z();
    marker_msg.points.emplace_back(p1);
    marker_msg.points.emplace_back(p2);
  }
}

void SimpleLoopClosureNode::constructVisualizationLoopEdges(const int& res_size, const std_msgs::msg::Header& header,
                                                            visualization_msgs::msg::Marker& marker_msg)
{
  int edge_num;
  {
    MtxLockGuard guard(mtx_res_);
    edge_num = loop_edges_.size();
  }

  marker_msg.header = header;
  marker_msg.ns = "loop_edges";
  marker_msg.action = visualization_msgs::msg::Marker::ADD;
  marker_msg.pose.orientation.w = 1.0;
  marker_msg.id = 0;
  marker_msg.type = visualization_msgs::msg::Marker::LINE_LIST;
  marker_msg.scale.x = edge_scale_;
  marker_msg.color = loop_edge_color_;

  marker_msg.points.clear();
  marker_msg.points.reserve(edge_num * 2);
  for (int i = 0; i < edge_num; i++)
  {
    MtxLockGuard guard(mtx_res_);

    int id1 = loop_edges_[i].first;
    int id2 = loop_edges_[i].second;

    if (id1 >= res_size || id2 >= res_size)
      continue;

    gtsam::Pose3 pose1 = optimization_result_.at<gtsam::Pose3>(id1);
    gtsam::Pose3 pose2 = optimization_result_.at<gtsam::Pose3>(id2);

    geometry_msgs::msg::Point p1, p2;
    p1.x = pose1.x();
    p1.y = pose1.y();
    p1.z = pose1.z();
    p2.x = pose2.x();
    p2.y = pose2.y();
    p2.z = pose2.z();
    marker_msg.points.emplace_back(p1);
    marker_msg.points.emplace_back(p2);
  }
}

void SimpleLoopClosureNode::constructVisualizationNodes(const int& res_size, const std_msgs::msg::Header& header,
                                                        visualization_msgs::msg::Marker& marker_msg)
{
  marker_msg.header = header;
  marker_msg.ns = "nodes";
  marker_msg.action = visualization_msgs::msg::Marker::ADD;
  marker_msg.pose.orientation.w = 1.0;
  marker_msg.id = 0;
  marker_msg.type = visualization_msgs::msg::Marker::SPHERE_LIST;
  marker_msg.scale.x = node_scale_;
  marker_msg.scale.y = node_scale_;
  marker_msg.scale.z = node_scale_;
  marker_msg.color = node_color_;

  marker_msg.points.clear();
  marker_msg.points.reserve(res_size);
  for (int i = 0; i < res_size; i++)
  {
    MtxLockGuard guard(mtx_res_);
    gtsam::Pose3 pose1 = optimization_result_.at<gtsam::Pose3>(i);

    geometry_msgs::msg::Point p1;
    p1.x = pose1.x();
    p1.y = pose1.y();
    p1.z = pose1.z();
    marker_msg.points.emplace_back(p1);
  }
}

void SimpleLoopClosureNode::publishVisualizationGraph()
{
  int optimization_result_size;
  {
    MtxLockGuard guard(mtx_res_);
    optimization_result_size = optimization_result_.size();
  }

  if (optimization_result_size <= 0)
    return;

  std_msgs::msg::Header header;
  rclcpp::Time t = this->now();
  header.stamp = t;
  header.frame_id = odom_frame_id_;

  visualization_msgs::msg::MarkerArray marker_array_msg;
  marker_array_msg.markers.resize(3);
  constructVisualizationOdometryEdges(optimization_result_size, header, marker_array_msg.markers[0]);
  constructVisualizationLoopEdges(optimization_result_size, header, marker_array_msg.markers[1]);
  constructVisualizationNodes(optimization_result_size, header, marker_array_msg.markers[2]);

  pub_vis_pose_graph_->publish(marker_array_msg);
}

void SimpleLoopClosureNode::copyKeyFrames()
{
  MtxLockGuard guard(mtx_buf_);
  for (size_t i = keyframes_cloud_copied_.size(); i < keyframes_cloud_.size(); i++)
  {
    keyframes_cloud_copied_.push_back(keyframes_cloud_[i]);
    keyframes_odom_copied_.push_back(keyframes_odom_[i]);
    trajectory_dist_copied_.push_back(trajectory_dist_[i]);
  }
}

void SimpleLoopClosureNode::getLastOptimizedPose(Eigen::Affine3d& pose, int& id)
{
  MtxLockGuard guard(mtx_res_);
  if (!optimization_result_.empty())
  {
    pose = optimization_result_.at<gtsam::Pose3>(optimization_result_.size() - 1).matrix();
    id = optimization_result_.size() - 1;
  }
  else
  {
    MtxLockGuard guard(mtx_buf_);
    if (!keyframes_odom_.empty())
    {
      pose = *keyframes_odom_[0];
      id = 0;
    }
    else
    {
      pose = Eigen::Affine3d();
      id = 0;
    }
  }
}

bool SimpleLoopClosureNode::constructOdometryGraph(gtsam::NonlinearFactorGraph& graph, gtsam::Values& init_estimate)
{
  if (added_odom_id_ >= keyframes_odom_copied_.size())
    return false;

  if (added_odom_id_ == 0)
  {
    gtsam::Pose3 pose = gtsam::Pose3(keyframes_odom_copied_[0]->matrix());
    graph.add(gtsam::PriorFactor<gtsam::Pose3>(0, pose, prior_noise_));
    init_estimate.insert(0, pose);
    added_odom_id_ = 1;
  }

  Eigen::Affine3d optimized_pose_last;
  int optimized_pose_id_last;
  getLastOptimizedPose(optimized_pose_last, optimized_pose_id_last);
  for (size_t i = added_odom_id_; i < keyframes_odom_copied_.size(); i++)
  {
    Eigen::Affine3d pose_diff = keyframes_odom_copied_[i - 1]->inverse() * (*keyframes_odom_copied_[i]);
    graph.add(gtsam::BetweenFactor<gtsam::Pose3>(i - 1, i, gtsam::Pose3(pose_diff.matrix()), odom_noise_));

    Eigen::Affine3d pose_init = optimized_pose_last * (keyframes_odom_copied_[optimized_pose_id_last]->inverse() *
                                                       (*keyframes_odom_copied_[i]));
    init_estimate.insert(i, gtsam::Pose3(pose_init.matrix()));

    added_odom_id_++;
  }

  return true;
}

bool SimpleLoopClosureNode::constructKDTreeMatrix(KDTreeMatrix& kdtree_mat)
{
  MtxLockGuard guard(mtx_res_);
  if (optimization_result_.empty())
  {
    return false;
  }

  kdtree_mat.resize(optimization_result_.size(), 3);
  for (size_t i = 0; i < optimization_result_.size(); i++)
  {
    kdtree_mat.row(i) = optimization_result_.at<gtsam::Pose3>(i).translation();
  }

  return true;
}

int SimpleLoopClosureNode::searchTarget(const KDTree& kdtree, const int& id_query, const Eigen::Affine3d& pose_query,
                                        const rclcpp::Time& stamp_query)
{
  NanoFlannSearchResult search_result;
  search_result.reserve(1000);
  nanoflann::SearchParameters search_params(0.0, true);
  kdtree.index_->radiusSearch(pose_query.translation().data(), search_radius_, search_result, search_params);

  Eigen::Quaterniond quat_query(pose_query.rotation());

  int target_id = -1;
  for (size_t i = 0; i < search_result.size(); i++)
  {
    int tmp_id = search_result[i].first;
    rclcpp::Time tmp_stamp = pcl_conversions::fromPCL(keyframes_cloud_copied_[tmp_id]->header.stamp);
    double time_diff = std::fabs(stamp_query.seconds() - tmp_stamp.seconds());

    Eigen::Affine3d affine_target;
    {
      MtxLockGuard guard(mtx_res_);
      affine_target = Eigen::Affine3d(optimization_result_.at<gtsam::Pose3>(tmp_id).matrix());
    }

    Eigen::Quaterniond quat_target(affine_target.rotation());
    double angle_diff = quat_query.angularDistance(quat_target);
    double dist_diff = std::fabs(trajectory_dist_copied_[id_query] - trajectory_dist_copied_[tmp_id]);
    if (time_diff > loop_search_time_diff_th_ && angle_diff < loop_search_angular_dist_th_ &&
        dist_diff > loop_search_dist_diff_th_)
    {
      target_id = tmp_id;
      break;
    }
  }

  return target_id;
}

pcl::PointCloud<pcl::PointXYZI>::Ptr SimpleLoopClosureNode::constructTargetCloud(const int target_id)
{
  pcl::PointCloud<pcl::PointXYZI>::Ptr target_cloud(new PointCloudType);
  pcl::PointCloud<pcl::PointXYZI>::Ptr target_cloud_ds(new PointCloudType);

  for (size_t i = target_id - target_frame_num_; i <= target_id + target_frame_num_; ++i)
  {
    Eigen::Affine3d tmp_affine;
    {
      MtxLockGuard guard(mtx_res_);
      if (i < 0 || i >= optimization_result_.size())
        continue;

      tmp_affine = Eigen::Affine3d(optimization_result_.at<gtsam::Pose3>(i).matrix());
    }

    PointCloudType tmp_cloud;
    pcl::transformPointCloud(*keyframes_cloud_copied_[i], tmp_cloud, tmp_affine);

    *target_cloud += tmp_cloud;
  }

  if (target_voxel_leaf_size_ <= 0.0)
  {
    target_cloud_ds = target_cloud;
  }
  else
  {
    vg_target_.setInputCloud(target_cloud);
    vg_target_.filter(*target_cloud_ds);
  }

  return target_cloud_ds;
}

pcl::PointCloud<pcl::PointXYZI>::Ptr SimpleLoopClosureNode::constructSourceCloud(const int source_id)
{
  pcl::PointCloud<pcl::PointXYZI>::Ptr source_cloud_ds(new PointCloudType);

  if (source_voxel_leaf_size_ <= 0.0)
  {
    *source_cloud_ds = *keyframes_cloud_copied_[source_id];
  }
  else
  {
    vg_source_.setInputCloud(keyframes_cloud_copied_[source_id]);
    vg_source_.filter(*source_cloud_ds);
  }

  return source_cloud_ds;
}

bool SimpleLoopClosureNode::tryRegistration(const Eigen::Affine3d& init_pose, const PointCloudType::Ptr& source_cloud,
                                            const PointCloudType::Ptr& target_cloud, Eigen::Affine3d& result,
                                            double& score)
{
  PointCloudType::Ptr unused_cloud(new PointCloudType);
  icp_.setInputSource(source_cloud);
  icp_.setInputTarget(target_cloud);
  icp_.align(*unused_cloud, init_pose.matrix().cast<float>());

  Eigen::Affine3f result_f(icp_.getFinalTransformation());
  result = result_f.cast<double>();
  score = icp_.getFitnessScore();

  if (icp_.hasConverged() && score < fitness_score_th_)
    return true;

  return false;
}

bool SimpleLoopClosureNode::constructLoopEdge(gtsam::NonlinearFactorGraph& graph)
{
  if (searched_loop_id_ >= keyframes_cloud_copied_.size())
    return false;

  KDTreeMatrix kdtree_mat;
  if (!constructKDTreeMatrix(kdtree_mat))
    return false;

  KDTree kdtree(3, std::cref(kdtree_mat), 10);
  kdtree.index_->buildIndex();

  Eigen::Affine3d optimized_pose_last;
  int optimized_pose_id_last;
  getLastOptimizedPose(optimized_pose_last, optimized_pose_id_last);

  for (size_t i = searched_loop_id_; i < keyframes_cloud_copied_.size(); i += (loop_search_frame_interval_ + 1))
  {
    Eigen::Affine3d pose_query = optimized_pose_last * (keyframes_odom_copied_[optimized_pose_id_last]->inverse() *
                                                        (*keyframes_odom_copied_[i]));
    rclcpp::Time stamp_query = pcl_conversions::fromPCL(keyframes_cloud_copied_[i]->header.stamp);

    int target_id = searchTarget(kdtree, i, pose_query, stamp_query);

    if (target_id == -1)
      continue;

    PointCloudType::Ptr target_cloud = constructTargetCloud(target_id);
    PointCloudType::Ptr source_cloud = constructSourceCloud(i);
    Eigen::Affine3d registration_result;
    double fitness_score;
    if (!tryRegistration(pose_query, source_cloud, target_cloud, registration_result, fitness_score))
      continue;

    Eigen::VectorXd noise_vector(6);
    noise_vector << fitness_score, fitness_score, fitness_score, fitness_score, fitness_score, fitness_score;
    gtsam::SharedNoiseModel constraint_noise = gtsam::noiseModel::Robust::Create(
        gtsam::noiseModel::mEstimator::Cauchy::Create(1), gtsam::noiseModel::Diagonal::Variances(noise_vector));

    Eigen::Affine3d target_pose;
    {
      MtxLockGuard guard(mtx_res_);
      target_pose = Eigen::Affine3d(optimization_result_.at<gtsam::Pose3>(target_id).matrix());
    }

    Eigen::Affine3d pose_diff = registration_result.inverse() * (target_pose);
    graph.add(gtsam::BetweenFactor<gtsam::Pose3>(i, target_id, gtsam::Pose3(pose_diff.matrix()), constraint_noise));
    RCLCPP_INFO_STREAM(this->get_logger(), "Loop Detected:" << i << " -> " << target_id);

    {
      MtxLockGuard guard(mtx_res_);
      loop_edges_.push_back(LoopEdgeID(i, target_id));
    }
  }
  searched_loop_id_ = keyframes_cloud_copied_.size();

  return true;
}

bool SimpleLoopClosureNode::updateISAM2(const gtsam::NonlinearFactorGraph& graph, const gtsam::Values& init_estimate)
{
  if (graph.empty())
    return false;

  if (init_estimate.empty())
    isam2_.update(graph);
  else
    isam2_.update(graph, init_estimate);
  isam2_.update();

  {
    MtxLockGuard guard(mtx_res_);
    optimization_result_ = isam2_.calculateEstimate();
  }

  return true;
}

void SimpleLoopClosureNode::visualizeThread()
{
  // rclcpp::Rate rate(1);
  const std::chrono::seconds rate_duration(1);
  while (rclcpp::ok() && !stop_visualize_thread_)
  {
    PointCloudType::Ptr map_cloud = constructPointCloudMap(vis_map_cloud_frame_interval_);
    publishMapCloud(map_cloud);
    publishVisualizationGraph();
    // rate.sleep();
    std::this_thread::sleep_for(rate_duration);
  }
}

void SimpleLoopClosureNode::loopCloseThread()
{
  // rclcpp::Rate rate(1);
  const std::chrono::seconds rate_duration(1);
  while (rclcpp::ok() && !stop_loop_closure_thread_)
  {
    gtsam::NonlinearFactorGraph graph;
    gtsam::Values init_estimate;

    copyKeyFrames();
    constructOdometryGraph(graph, init_estimate);
    constructLoopEdge(graph);
    updateISAM2(graph, init_estimate);

    // rate.sleep();
    std::this_thread::sleep_for(rate_duration);
  }
}

void SimpleLoopClosureNode::spin()
{
  stop_loop_closure_thread_ = false;
  stop_visualize_thread_ = false;

  std::thread loop_close_thread(&SimpleLoopClosureNode::loopCloseThread, this);
  std::thread visualize_thread(&SimpleLoopClosureNode::visualizeThread, this);

  RCLCPP_INFO(this->get_logger(), "Loop Closure Started");
  // rclcpp::spin(Node);
  // rclcpp::spin(this->Node);
  // rclcpp::spin(shared_from_this());

  stop_loop_closure_thread_ = true;
  stop_visualize_thread_ = true;

  loop_close_thread.join();
  visualize_thread.join();
  // if (loop_close_thread.joinable())
    // loop_close_thread.join();
  // if (visualize_thread.joinable())
    // visualize_thread.join();
  if (save_thread_.joinable())
    save_thread_.join();
}

int main(int argc, char** argv)
{
  rclcpp::init(argc, argv);
  // SimpleLoopClosureNode loop_closure_node;
  SimpleLoopClosureNode::Ptr loop_closure_node = std::make_shared<SimpleLoopClosureNode>();
  // auto loop_closure_node = std::make_shared<SharedSimpleLoopClosureNode>();
  loop_closure_node->spin();
  rclcpp::spin(loop_closure_node);  // Call spin method of the node
  rclcpp::shutdown();
  return 0;
}
