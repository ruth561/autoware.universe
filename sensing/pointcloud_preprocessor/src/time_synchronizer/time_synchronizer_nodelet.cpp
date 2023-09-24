// Copyright 2023 TIER IV, Inc.
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

/*
 * @brief PointCloudDataSynchronizerComponent class
 *
 * subscribe: pointclouds, twists
 * publish: timestamp "synchronized" pointclouds
 *
 * @author Yoshi Ri
 */

#include "pointcloud_preprocessor/time_synchronizer/time_synchronizer_nodelet.hpp"

#include <pcl_ros/transforms.hpp>

#include <pcl_conversions/pcl_conversions.h>

#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>

// postfix for output topics
#define POSTFIX_NAME "_synchronized"
//////////////////////////////////////////////////////////////////////////////////////////////

namespace pointcloud_preprocessor
{
PointCloudDataSynchronizerComponent::PointCloudDataSynchronizerComponent(
  const rclcpp::NodeOptions & node_options)
: Node("point_cloud_time_synchronizer_component", node_options)
{
  std::cerr << "🍀 < PointCloudDataSynchronizerComponent" << std::endl;
  // initialize debug tool
  {
    using tier4_autoware_utils::DebugPublisher;
    using tier4_autoware_utils::StopWatch;
    stop_watch_ptr_ = std::make_unique<StopWatch<std::chrono::milliseconds>>();
    debug_publisher_ = std::make_unique<DebugPublisher>(this, "time_synchronizer");
    stop_watch_ptr_->tic("cyclic_time");
    stop_watch_ptr_->tic("processing_time");
  }

  // Set parameters
  {
    output_frame_ = static_cast<std::string>(declare_parameter("output_frame", ""));
    if (output_frame_.empty()) {
      RCLCPP_ERROR(get_logger(), "Need an 'output_frame' parameter to be set before continuing!");
      return;
    }
    declare_parameter("input_topics", std::vector<std::string>());
    input_topics_ = get_parameter("input_topics").as_string_array();
    if (input_topics_.empty()) {
      RCLCPP_ERROR(get_logger(), "Need a 'input_topics' parameter to be set before continuing!");
      return;
    }
    if (input_topics_.size() == 1) {
      RCLCPP_ERROR(get_logger(), "Only one topic given. Need at least two topics to continue.");
      return;
    }

    // Optional parameters
    maximum_queue_size_ = static_cast<int>(declare_parameter("max_queue_size", 5));
    timeout_sec_ = static_cast<double>(declare_parameter("timeout_sec", 0.1));

    input_offset_ = declare_parameter("input_offset", std::vector<double>{});
    if (!input_offset_.empty() && input_topics_.size() != input_offset_.size()) {
      RCLCPP_ERROR(get_logger(), "The number of topics does not match the number of offsets.");
      return;
    }
  }

  // Initialize not_subscribed_topic_names_
  {
    for (const std::string & e : input_topics_) {
      not_subscribed_topic_names_.insert(e);
    }
  }

  // Initialize offset map
  {
    for (size_t i = 0; i < input_offset_.size(); ++i) {
      offset_map_[input_topics_[i]] = input_offset_[i];
    }
  }

  // tf2 listener
  {
    tf2_buffer_ = std::make_shared<tf2_ros::Buffer>(this->get_clock());
    tf2_listener_ = std::make_shared<tf2_ros::TransformListener>(*tf2_buffer_);
  }

  // Subscribers
  {
    RCLCPP_INFO_STREAM(
      get_logger(), "Subscribing to " << input_topics_.size() << " user given topics as inputs:");
    for (auto & input_topic : input_topics_) {
      RCLCPP_INFO_STREAM(get_logger(), " - " << input_topic);
    }

    // Subscribe to the filters
    filters_.resize(input_topics_.size());

    // First input_topics_.size () filters are valid
    for (size_t d = 0; d < input_topics_.size(); ++d) {
      cloud_stdmap_.insert(std::make_pair(input_topics_[d], nullptr));
      cloud_stdmap_tmp_ = cloud_stdmap_;

      // CAN'T use auto type here.
      std::function<void(const sensor_msgs::msg::PointCloud2::ConstSharedPtr msg)> cb = std::bind(
        &PointCloudDataSynchronizerComponent::cloud_callback, this, std::placeholders::_1,
        input_topics_[d]);

      filters_[d].reset();
      filters_[d] = this->create_subscription<sensor_msgs::msg::PointCloud2>(
        input_topics_[d], rclcpp::SensorDataQoS().keep_last(maximum_queue_size_), cb);
    }

    // Subscribe to the twist
    auto twist_cb =
      std::bind(&PointCloudDataSynchronizerComponent::twist_callback, this, std::placeholders::_1);
    sub_twist_ = this->create_subscription<autoware_auto_vehicle_msgs::msg::VelocityReport>(
      "/vehicle/status/velocity_status", rclcpp::QoS{100}, twist_cb);
  }

  // Transformed Raw PointCloud2 Publisher
  {
    for (auto & topic : input_topics_) {
      std::string new_topic = topic + POSTFIX_NAME;
      auto publisher = this->create_publisher<sensor_msgs::msg::PointCloud2>(
        new_topic, rclcpp::SensorDataQoS().keep_last(maximum_queue_size_));
      transformed_raw_pc_publisher_map_.insert({topic, publisher});
    }
  }

  // Set timer
  {
    const auto period_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::duration<double>(timeout_sec_));
    timer_ = rclcpp::create_timer(
      this, get_clock(), period_ns,
      std::bind(&PointCloudDataSynchronizerComponent::timer_callback, this));
  }

  // Diagnostic Updater
  {
    updater_.setHardwareID("synchronize_data_checker");
    updater_.add("concat_status", this, &PointCloudDataSynchronizerComponent::checkSyncStatus);
  }
}

//////////////////////////////////////////////////////////////////////////////////////////////
// overloaded functions
void PointCloudDataSynchronizerComponent::transformPointCloud(
  const PointCloud2::ConstSharedPtr & in, PointCloud2::SharedPtr & out)
{
  transformPointCloud(in, out, output_frame_);
}

// 入力データを TF2 へ変換する関数（らしい）
// ここではほとんど時間がかからない、とあるが、それは本当か？（検証あるのみ）
void PointCloudDataSynchronizerComponent::transformPointCloud(
  const PointCloud2::ConstSharedPtr & in, PointCloud2::SharedPtr & out,
  const std::string & target_frame)
{
  // Transform the point clouds into the specified output frame
  if (target_frame != in->header.frame_id) {
    std::cerr << "🧱 [ transformPointCloud ] target_frame != in->header.frame_id" << std::endl;
    // TODO(YamatoAndo): use TF2
    if (!pcl_ros::transformPointCloud(target_frame, *in, *out, *tf2_buffer_)) {
      RCLCPP_ERROR(
        this->get_logger(),
        "[transformPointCloud] Error converting first input dataset from %s to %s.",
        in->header.frame_id.c_str(), target_frame.c_str());
      return;
    }
  } else {
    std::cerr << "🧱 [ transformPointCloud ] make_shared が呼ばれそう" << std::endl;
    out = std::make_shared<PointCloud2>(*in);
  }
}

// 新しい時刻のデータを古い時刻のデータへ変換するための行列を作成する関数。
// velocity_report なんかを使ってデータの整形を行っている。
/**
 * @brief compute transform to adjust for old timestamp
 *
 * @param old_stamp
 * @param new_stamp
 * @return Eigen::Matrix4f: transformation matrix from new_stamp to old_stamp
 */
Eigen::Matrix4f PointCloudDataSynchronizerComponent::computeTransformToAdjustForOldTimestamp(
  const rclcpp::Time & old_stamp, const rclcpp::Time & new_stamp)
{
  // return identity if no twist is available or old_stamp is newer than new_stamp
  if (twist_ptr_queue_.empty() || old_stamp > new_stamp) {
    return Eigen::Matrix4f::Identity();
  }

  auto old_twist_ptr_it = std::lower_bound(
    std::begin(twist_ptr_queue_), std::end(twist_ptr_queue_), old_stamp,
    [](const geometry_msgs::msg::TwistStamped::ConstSharedPtr & x_ptr, const rclcpp::Time & t) {
      return rclcpp::Time(x_ptr->header.stamp) < t;
    });
  old_twist_ptr_it =
    old_twist_ptr_it == twist_ptr_queue_.end() ? (twist_ptr_queue_.end() - 1) : old_twist_ptr_it;

  auto new_twist_ptr_it = std::lower_bound(
    std::begin(twist_ptr_queue_), std::end(twist_ptr_queue_), new_stamp,
    [](const geometry_msgs::msg::TwistStamped::ConstSharedPtr & x_ptr, const rclcpp::Time & t) {
      return rclcpp::Time(x_ptr->header.stamp) < t;
    });
  new_twist_ptr_it =
    new_twist_ptr_it == twist_ptr_queue_.end() ? (twist_ptr_queue_.end() - 1) : new_twist_ptr_it;

  auto prev_time = old_stamp;
  double x = 0.0;
  double y = 0.0;
  double yaw = 0.0;
  for (auto twist_ptr_it = old_twist_ptr_it; twist_ptr_it != new_twist_ptr_it + 1; ++twist_ptr_it) {
    const double dt =
      (twist_ptr_it != new_twist_ptr_it)
        ? (rclcpp::Time((*twist_ptr_it)->header.stamp) - rclcpp::Time(prev_time)).seconds()
        : (rclcpp::Time(new_stamp) - rclcpp::Time(prev_time)).seconds();

    if (std::fabs(dt) > 0.1) {
      RCLCPP_WARN_STREAM_THROTTLE(
        get_logger(), *get_clock(), std::chrono::milliseconds(10000).count(),
        "Time difference is too large. Cloud not interpolate. Please confirm twist topic and "
        "timestamp");
      break;
    }

    const double dis = (*twist_ptr_it)->twist.linear.x * dt;
    yaw += (*twist_ptr_it)->twist.angular.z * dt;
    x += dis * std::cos(yaw);
    y += dis * std::sin(yaw);
    prev_time = (*twist_ptr_it)->header.stamp;
  }
  Eigen::AngleAxisf rotation_x(0, Eigen::Vector3f::UnitX());
  Eigen::AngleAxisf rotation_y(0, Eigen::Vector3f::UnitY());
  Eigen::AngleAxisf rotation_z(yaw, Eigen::Vector3f::UnitZ());
  Eigen::Translation3f translation(x, y, 0);
  Eigen::Matrix4f rotation_matrix = (translation * rotation_z * rotation_y * rotation_x).matrix();
  return rotation_matrix;
}

// cloud_stdmap_ に入っている現在の点群データたちを最も古いタイムスタンプに合わせて変形し、
// 変形後のデータを map として返す関数。
// データの時刻を揃える関数。
// パブリッシュする直前に呼ばれる。
std::map<std::string, sensor_msgs::msg::PointCloud2::SharedPtr>
PointCloudDataSynchronizerComponent::synchronizeClouds()
{
  // map for storing the transformed point clouds
  std::map<std::string, sensor_msgs::msg::PointCloud2::SharedPtr> transformed_clouds;

  // cloud_stdmap_ に同期したい点群データが入っている
  // まず、その中から時刻（タイムスタンプ）を取り出し、ソートする。
  // Step1. gather stamps and sort it
  std::vector<rclcpp::Time> pc_stamps;
  for (const auto & e : cloud_stdmap_) {
    transformed_clouds[e.first] = nullptr;
    if (e.second != nullptr) {
      pc_stamps.push_back(rclcpp::Time(e.second->header.stamp));
    }
  }
  if (pc_stamps.empty()) {
    return transformed_clouds;
  }
  // sort stamps and get oldest stamp
  std::sort(pc_stamps.begin(), pc_stamps.end());
  std::reverse(pc_stamps.begin(), pc_stamps.end());
  // 点群データのタイムスタンプの中で最も古い時刻のものを見つけ、oldest_stamp とする。
  const auto oldest_stamp = pc_stamps.back();

  // Step2. Calculate compensation transform and concatenate with the oldest stamp
  for (const auto & e : cloud_stdmap_) {
    if (e.second != nullptr) {
      // 元の点群データ e から変形後のデータ transformed_cloud_ptr を作成する。
      // ここでやってる変形って何？TF2 への変換？
      sensor_msgs::msg::PointCloud2::SharedPtr transformed_cloud_ptr(
        new sensor_msgs::msg::PointCloud2());
      transformPointCloud(e.second, transformed_cloud_ptr);

      // 新しい時刻から古い時刻への変換行列を作成する
      // calculate transforms to oldest stamp
      Eigen::Matrix4f adjust_to_old_data_transform = Eigen::Matrix4f::Identity();
      rclcpp::Time transformed_stamp = rclcpp::Time(e.second->header.stamp);
      for (const auto & stamp : pc_stamps) {
        const auto new_to_old_transform =
          computeTransformToAdjustForOldTimestamp(stamp, transformed_stamp);
        adjust_to_old_data_transform = new_to_old_transform * adjust_to_old_data_transform;
        transformed_stamp = std::min(transformed_stamp, stamp);
      }
      // 作成した変換行列をもとに、pcl_ros::transformPointCloud 関数で点群データの変換を行う。
      sensor_msgs::msg::PointCloud2::SharedPtr transformed_delay_compensated_cloud_ptr(
        new sensor_msgs::msg::PointCloud2());
      pcl_ros::transformPointCloud(
        adjust_to_old_data_transform, *transformed_cloud_ptr,
        *transformed_delay_compensated_cloud_ptr);
      // gather transformed clouds
      transformed_delay_compensated_cloud_ptr->header.stamp = oldest_stamp;
      transformed_delay_compensated_cloud_ptr->header.frame_id = output_frame_;
      // 変換後の点群データを戻り値ようの map に設定する。
      transformed_clouds[e.first] = transformed_delay_compensated_cloud_ptr;
    } else {
      not_subscribed_topic_names_.insert(e.first);
    }
  }
  return transformed_clouds;
}

// 溜まった点群データをパブリッシュする関数
// @@@publish
void PointCloudDataSynchronizerComponent::publish()
{
  stop_watch_ptr_->toc("processing_time", true);
  not_subscribed_topic_names_.clear();

  const auto & transformed_raw_points = PointCloudDataSynchronizerComponent::synchronizeClouds();

  // 変形後の点群データたちを（*_synchronized というトピックに）パブリッシュする
  // publish transformed raw pointclouds
  for (const auto & e : transformed_raw_points) {
    if (e.second) {
      auto output = std::make_unique<sensor_msgs::msg::PointCloud2>(*e.second);
      transformed_raw_pc_publisher_map_[e.first]->publish(std::move(output));
    } else {
      RCLCPP_WARN(
        this->get_logger(), "transformed_raw_points[%s] is nullptr, skipping pointcloud publish.",
        e.first.c_str());
    }
  }

  updater_.force_update();

  cloud_stdmap_ = cloud_stdmap_tmp_;
  std::for_each(std::begin(cloud_stdmap_tmp_), std::end(cloud_stdmap_tmp_), [](auto & e) {
    e.second = nullptr;
  });
  // add processing time for debug
  if (debug_publisher_) {
    const double cyclic_time_ms = stop_watch_ptr_->toc("cyclic_time", true);
    const double processing_time_ms = stop_watch_ptr_->toc("processing_time", true);
    debug_publisher_->publish<tier4_debug_msgs::msg::Float64Stamped>(
      "debug/cyclic_time_ms", cyclic_time_ms);
    debug_publisher_->publish<tier4_debug_msgs::msg::Float64Stamped>(
      "debug/processing_time_ms", processing_time_ms);
  }
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void PointCloudDataSynchronizerComponent::convertToXYZICloud(
  const sensor_msgs::msg::PointCloud2::SharedPtr & input_ptr,
  sensor_msgs::msg::PointCloud2::SharedPtr & output_ptr)
{
  output_ptr->header = input_ptr->header;
  PointCloud2Modifier<PointXYZI> output_modifier{*output_ptr, input_ptr->header.frame_id};
  output_modifier.reserve(input_ptr->width);

  bool has_intensity = std::any_of(
    input_ptr->fields.begin(), input_ptr->fields.end(),
    [](auto & field) { return field.name == "intensity"; });

  sensor_msgs::PointCloud2Iterator<float> it_x(*input_ptr, "x");
  sensor_msgs::PointCloud2Iterator<float> it_y(*input_ptr, "y");
  sensor_msgs::PointCloud2Iterator<float> it_z(*input_ptr, "z");

  if (has_intensity) {
    sensor_msgs::PointCloud2Iterator<float> it_i(*input_ptr, "intensity");
    for (; it_x != it_x.end(); ++it_x, ++it_y, ++it_z, ++it_i) {
      PointXYZI point;
      point.x = *it_x;
      point.y = *it_y;
      point.z = *it_z;
      point.intensity = *it_i;
      output_modifier.push_back(std::move(point));
    }
  } else {
    for (; it_x != it_x.end(); ++it_x, ++it_y, ++it_z) {
      PointXYZI point;
      point.x = *it_x;
      point.y = *it_y;
      point.z = *it_z;
      point.intensity = 0.0f;
      output_modifier.push_back(std::move(point));
    }
  }
}

void PointCloudDataSynchronizerComponent::setPeriod(const int64_t new_period)
{
  if (!timer_) {
    return;
  }
  int64_t old_period = 0;
  rcl_ret_t ret = rcl_timer_get_period(timer_->get_timer_handle().get(), &old_period);
  if (ret != RCL_RET_OK) {
    rclcpp::exceptions::throw_from_rcl_error(ret, "Couldn't get old period");
  }
  ret = rcl_timer_exchange_period(timer_->get_timer_handle().get(), new_period, &old_period);
  if (ret != RCL_RET_OK) {
    rclcpp::exceptions::throw_from_rcl_error(ret, "Couldn't exchange_period");
  }
}

// @@@cloud_callback
// 各トピック（top, left, right）からサブスクライブしたときに呼ばれるコールバック関数
void PointCloudDataSynchronizerComponent::cloud_callback(
  const sensor_msgs::msg::PointCloud2::ConstSharedPtr & input_ptr, const std::string & topic_name)
{
  std::lock_guard<std::mutex> lock(mutex_);
  // ここの make_shared いらない
  auto input = std::make_shared<sensor_msgs::msg::PointCloud2>(*input_ptr);
  sensor_msgs::msg::PointCloud2::SharedPtr xyzi_input_ptr(new sensor_msgs::msg::PointCloud2());
  // サブスクライブした点群データを XYZI 形式に変換
  // （現在は XYZI 形式のデータが流れてくるため、変換する必要がないが、
  // 　いずれ XYZIRC 形式のデータが流れてくることになるらしい。）
  // （単純に ConstSharedPtr から SharedPtr へ変換していると考えれば良さそう。）
  convertToXYZICloud(input, xyzi_input_ptr);

  const bool is_already_subscribed_this = (cloud_stdmap_[topic_name] != nullptr);
  const bool is_already_subscribed_tmp = std::any_of(
    std::begin(cloud_stdmap_tmp_), std::end(cloud_stdmap_tmp_),
    [](const auto & e) { return e.second != nullptr; });

  if (is_already_subscribed_this) {
    // もし他のデータが全て揃う前に2度目のデータをサブスクライブしたら
    // tmp のほうにデータを格納しておく。
    cloud_stdmap_tmp_[topic_name] = xyzi_input_ptr;

    if (!is_already_subscribed_tmp) {
      // すでに他のトピックから2度のデータが流れてきているようなら
      // タイマーをセットし、時間制限を設ける。
      auto period = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::duration<double>(timeout_sec_));
      try {
        setPeriod(period.count());
      } catch (rclcpp::exceptions::RCLError & ex) {
        RCLCPP_WARN_THROTTLE(get_logger(), *get_clock(), 5000, "%s", ex.what());
      }
      timer_->reset();
    }
  } else {
    // 以前パブリッシュしてから最初のサブスクライブの場合、
    // 通常の stdmap にデータを格納しておく。
    cloud_stdmap_[topic_name] = xyzi_input_ptr;

    const bool is_subscribed_all = std::all_of(
      std::begin(cloud_stdmap_), std::end(cloud_stdmap_),
      [](const auto & e) { return e.second != nullptr; });

    if (is_subscribed_all) {
      // 各トピックからの点群データが揃っていればパブリッシュする。
      // その際、各店群データは最新のものを使用したいため、stdmap_tmp から stdmap へデータを移す
      for (const auto & e : cloud_stdmap_tmp_) {
        if (e.second != nullptr) {
          cloud_stdmap_[e.first] = e.second;
        }
      }
      std::for_each(std::begin(cloud_stdmap_tmp_), std::end(cloud_stdmap_tmp_), [](auto & e) {
        e.second = nullptr;
      });

      timer_->cancel();
      publish();
    } else if (offset_map_.size() > 0) {
      // まだ全ての点群データが揃っていない場合、
      timer_->cancel();
      auto period = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::duration<double>(timeout_sec_ - offset_map_[topic_name]));
      try {
        setPeriod(period.count());
      } catch (rclcpp::exceptions::RCLError & ex) {
        RCLCPP_WARN_THROTTLE(get_logger(), *get_clock(), 5000, "%s", ex.what());
      }
      timer_->reset();
    }
  }
}

void PointCloudDataSynchronizerComponent::timer_callback()
{
  using std::chrono_literals::operator""ms;
  timer_->cancel();
  if (mutex_.try_lock()) {
    publish();
    mutex_.unlock();
  } else {
    try {
      std::chrono::nanoseconds period = 10ms;
      setPeriod(period.count());
    } catch (rclcpp::exceptions::RCLError & ex) {
      RCLCPP_WARN_THROTTLE(get_logger(), *get_clock(), 5000, "%s", ex.what());
    }
    timer_->reset();
  }
}

// /vehicle/statuc/velocity_status トピックからサブスクライブしたときに呼ばれるコールバック
// @@@twist_callback
void PointCloudDataSynchronizerComponent::twist_callback(
  const autoware_auto_vehicle_msgs::msg::VelocityReport::ConstSharedPtr input)
{
  // if rosbag restart, clear buffer
  if (!twist_ptr_queue_.empty()) {
    if (rclcpp::Time(twist_ptr_queue_.front()->header.stamp) > rclcpp::Time(input->header.stamp)) {
      twist_ptr_queue_.clear();
    }
  }

  // pop old data
  while (!twist_ptr_queue_.empty()) {
    if (
      rclcpp::Time(twist_ptr_queue_.front()->header.stamp) + rclcpp::Duration::from_seconds(1.0) >
      rclcpp::Time(input->header.stamp)) {
      break;
    }
    twist_ptr_queue_.pop_front();
  }

  auto twist_ptr = std::make_shared<geometry_msgs::msg::TwistStamped>();
  twist_ptr->header.stamp = input->header.stamp;
  twist_ptr->twist.linear.x = input->longitudinal_velocity;
  twist_ptr->twist.linear.y = input->lateral_velocity;
  twist_ptr->twist.angular.z = input->heading_rate;
  twist_ptr_queue_.push_back(twist_ptr);
}

void PointCloudDataSynchronizerComponent::checkSyncStatus(
  diagnostic_updater::DiagnosticStatusWrapper & stat)
{
  for (const std::string & e : input_topics_) {
    const std::string subscribe_status = not_subscribed_topic_names_.count(e) ? "NG" : "OK";
    stat.add(e, subscribe_status);
  }

  const int8_t level = not_subscribed_topic_names_.empty()
                         ? diagnostic_msgs::msg::DiagnosticStatus::OK
                         : diagnostic_msgs::msg::DiagnosticStatus::WARN;
  const std::string message = not_subscribed_topic_names_.empty()
                                ? "Concatenate all topics"
                                : "Some topics are not concatenated";
  stat.summary(level, message);
}
}  // namespace pointcloud_preprocessor

#include <rclcpp_components/register_node_macro.hpp>
RCLCPP_COMPONENTS_REGISTER_NODE(pointcloud_preprocessor::PointCloudDataSynchronizerComponent)
