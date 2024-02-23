
#define EIGEN_DEFAULT_IO_FORMAT Eigen::IOFormat(6, 0, " ", "\n", "", "")
#define _GLIBCXX_ASSERTIONS

#include <absl/container/flat_hash_map.h>
#include <ceres/ceres.h>
#include <glog/logging.h>
#include <pcl/io/ply_io.h>
#include <pcl_conversions/pcl_conversions.h>
#include <tf/transform_broadcaster.h>

#include "common/histogram.h"
#include "common/utils.h"
#include "feature_map.h"
#include "knn_surfel_matcher.h"
#include "odometry/cost_functor.h"
#include "odometry/lidar_odometry.h"
#include "odometry/spline_interpolation.h"
#include "surfel_extraction.h"

namespace {

void PrintSurfelResiduals(const std::vector<ceres::ResidualBlockId> &residual_ids, ceres::Problem &problem) {
  std::vector<double>             residuals;
  double                          cost;
  ceres::Problem::EvaluateOptions options;
  options.apply_loss_function = true;
  options.residual_blocks     = residual_ids;
  problem.Evaluate(options, &cost, &residuals, nullptr, nullptr);
  Histogram hist;
  for (auto &e : residuals) {
    hist.Add(e);
  }
  LOG(INFO) << "Surfel residuals, cost: " << cost << ", dist: " << hist.ToString(10);
}

void PrintImuResiduals(const std::vector<ceres::ResidualBlockId> &residual_ids, ceres::Problem &problem) {
  if (residual_ids.empty()) {
    return;
  }
  std::vector<double>             residuals;
  double                          cost;
  ceres::Problem::EvaluateOptions options;
  options.apply_loss_function = true;
  options.residual_blocks     = residual_ids;
  problem.Evaluate(options, &cost, &residuals, nullptr, nullptr);
  Histogram                hist[4];
  std::vector<std::string> residual_types = {"gyro", "acc", "gyro_bias", "acc_bias"};
  for (int i = 0; i < residuals.size(); i += 12) {
    for (int j = 0; j < 4; ++j) {
      auto residuals_part = Vector3d{residuals[i + j * 3], residuals[i + j * 3 + 1], residuals[i + j * 3 + 2]};
      hist[j].Add(residuals_part.norm());
    }
  }
  for (int j = 0; j < 4; ++j) {
    LOG(INFO) << "Imu residuals with type " << residual_types[j] << ", cost: " << cost << ", dist: " << hist[j].ToString(10);
  }
}

void PrintSampleStates(const std::deque<SampleState::Ptr> &states) {
  for (auto &e : states) {
    LOG(INFO) << "\np:  " << e->pos.transpose() << "\nDp: " << e->pos_cor.transpose() << "\nq:  " << e->rot.coeffs().transpose() << "\nbg: " << e->bg.transpose() << "\nba: " << e->ba.transpose();
  }
}

void UndistortSweep(const std::vector<hilti_ros::Point> &sweep_in,
                    const std::deque<ImuState>          &imu_states,
                    std::vector<hilti_ros::Point>       &sweep_out) {
  sweep_out.clear();
  for (auto &pt : sweep_in) {
    auto it  = std::lower_bound(imu_states.begin(), imu_states.end(), pt.time, [](const ImuState &a, auto b) { return a.timestamp < b; });
    auto idx = it - imu_states.begin();
    CHECK(idx >= 1 && idx < imu_states.size()) << idx;
    double      factor      = (pt.time - imu_states[idx - 1].timestamp) / (imu_states[idx].timestamp - imu_states[idx - 1].timestamp);
    Vector3d    pos         = imu_states[idx - 1].pos * (1 - factor) + imu_states[idx].pos * factor;
    Quaterniond rot         = imu_states[idx - 1].rot.slerp(factor, imu_states[idx].rot);
    auto        new_pt      = pt;
    new_pt.getVector3fMap() = (rot * new_pt.getVector3fMap().cast<double>() + pos).cast<float>();
    sweep_out.push_back(new_pt);
  }
}

void UpdateSurfelPoses(const std::deque<ImuState> &imu_states, std::deque<Surfel::Ptr> &surfels) {
  for (auto &surfel : surfels) {
    auto it  = std::lower_bound(imu_states.begin(), imu_states.end(), surfel->timestamp, [](const ImuState &a, auto b) { return a.timestamp < b; });
    auto idx = it - imu_states.begin();
    CHECK(idx != 0 && idx != imu_states.size()) << idx;
    double      factor = (surfel->timestamp - imu_states[idx - 1].timestamp) / (imu_states[idx].timestamp - imu_states[idx - 1].timestamp);
    Vector3d    pos    = imu_states[idx - 1].pos * (1 - factor) + imu_states[idx].pos * factor;
    Quaterniond rot    = imu_states[idx - 1].rot.slerp(factor, imu_states[idx].rot);
    surfel->UpdatePose(pos, rot);
  }
}

/**
 * @brief Build a sweep from points_buff
 *
 * Timestamp order: l_0 < l_1 < ... < l_{n-1} < lidar_end_time
 *
 * @param points_buff
 * @param sweep_endtime
 * @param sweep
 */
void BuildSweep(std::deque<hilti_ros::Point> &points_buff, double sweep_endtime, std::vector<hilti_ros::Point> &sweep) {
  double start_time = points_buff.front().time;
  sweep.clear();
  while (!points_buff.empty() && points_buff.front().time < sweep_endtime) {
    sweep.push_back(points_buff.front());
    points_buff.pop_front();
  }
}

void UpdateSamplePoses(std::deque<SampleState::Ptr> &sample_states) {
  for (auto &sample_state : sample_states) {
    sample_state->rot = Exp(sample_state->rot_cor) * sample_state->rot;
    sample_state->pos = sample_state->pos_cor + sample_state->pos;
    sample_state->rot_cor.setZero();
    sample_state->pos_cor.setZero();
  }
}

class CubicBSplineSampleCorrector {
 public:
  CubicBSplineSampleCorrector(const std::deque<SampleState::Ptr> &sample_states) {
    std::vector<double>   sample_timestamps;
    std::vector<Vector3d> sample_rot;
    std::vector<Vector3d> sample_pos;
    for (auto &sample_state : sample_states) {
      sample_timestamps.push_back(sample_state->timestamp);
      sample_rot.push_back(sample_state->rot_cor);
      sample_pos.push_back(sample_state->pos_cor);
    }
    rot_interp_.reset(new CubicBSplineInterpolator(sample_timestamps, sample_rot));
    pos_interp_.reset(new CubicBSplineInterpolator(sample_timestamps, sample_pos));
  }

  bool GetCorr(double timestamp, Vector3d &rot_cor, Vector3d &pos_cor) {
    CHECK(rot_interp_ && pos_interp_) << "Interpolator not initialized";
    auto rot_cor_ptr = rot_interp_->Interp(timestamp);
    auto pos_cor_ptr = pos_interp_->Interp(timestamp);
    CHECK((rot_cor_ptr && pos_cor_ptr) || (!rot_cor_ptr && !pos_cor_ptr)) << "Interpolation failed";
    if (rot_cor_ptr) {
      rot_cor = *rot_cor_ptr;
      pos_cor = *pos_cor_ptr;
      return true;
    } else {
      return false;
    }
  }

 private:
  std::shared_ptr<CubicBSplineInterpolator> rot_interp_;
  std::shared_ptr<CubicBSplineInterpolator> pos_interp_;
};

/**
 * @brief Update imu poses by sample state corrections
 *
 * @param sample_states
 * @param imu_states
 */
void UpdateImuPoses(const std::deque<SampleState::Ptr> &sample_states,
                    std::deque<ImuState>               &imu_states) {
  std::deque<ImuState>        imu_states_new      = imu_states;
  int                         corrected_first_idx = -1, corrected_last_idx = -1;
  CubicBSplineSampleCorrector corrector(sample_states);
  // update imu poses by sample state corrections
  for (int i = 0; i < imu_states_new.size(); ++i) {
    auto &imu_state = imu_states_new[i];
    // todo kk use cubic bspline interpolation
    Vector3d rot_cor, pos_cor;
    bool     interp_ok = corrector.GetCorr(imu_state.timestamp, rot_cor, pos_cor);
    if (interp_ok) {
      imu_state.rot = Exp(rot_cor) * imu_state.rot;
      imu_state.pos = pos_cor + imu_state.pos;

      if (corrected_first_idx == -1) corrected_first_idx = i;
      corrected_last_idx = i;
    }
  }
  // update heading and tailing imu poses
  if (corrected_first_idx != -1) {
    LOG(INFO) << "corrected extra imu poses in [0, " << corrected_first_idx << ") and (" << corrected_last_idx << "," << imu_states_new.size();
    for (int i = corrected_first_idx - 1; i >= 0; --i) {  // update [0, corrected_first_idx)
      Rigid3d pose_i_new = Rigid3d{imu_states[i].pos, imu_states[i].rot} *
                           Rigid3d{imu_states[i + 1].pos, imu_states[i + 1].rot}.inverse() *
                           Rigid3d{imu_states_new[i + 1].pos, imu_states_new[i + 1].rot};
      imu_states_new[i].rot = pose_i_new.rotation();
      imu_states_new[i].pos = pose_i_new.translation();
    }
    for (int i = corrected_last_idx + 1; i < imu_states_new.size(); ++i) {  // update (corrected_last_idx, N)
      Rigid3d pose_i_new = Rigid3d{imu_states[i].pos, imu_states[i].rot} *
                           Rigid3d{imu_states[i - 1].pos, imu_states[i - 1].rot}.inverse() *
                           Rigid3d{imu_states_new[i - 1].pos, imu_states_new[i - 1].rot};
      imu_states_new[i].rot = pose_i_new.rotation();
      imu_states_new[i].pos = pose_i_new.translation();
    }
  }
  imu_states = imu_states_new;
}

/**
 * @brief Trim to sliding window
 *
 * Timestamp order: sample_0 <= imu_0 <= surfel_0
 *
 * @param sample_states
 * @param imu_states
 * @param surfels
 * @param window_duration
 */
void ShrinkToFit(std::deque<SampleState::Ptr> &sample_states,
                 std::deque<ImuState>         &imu_states,
                 std::deque<Surfel::Ptr>      &surfels,
                 double                        window_duration) {
  if (sample_states.empty() || sample_states.back()->timestamp - sample_states.front()->timestamp <= window_duration) {
    return;
  }
  while (sample_states.back()->timestamp - sample_states.front()->timestamp > window_duration) {
    sample_states.pop_front();
  }
  while (imu_states.front().timestamp < sample_states.front()->timestamp) {
    imu_states.pop_front();
  }
  while (surfels.front()->timestamp < imu_states.front().timestamp) {
    surfels.pop_front();
  }
}

}  // namespace

void LidarOdometry::BuildLidarResiduals(const std::vector<SurfelCorrespondence> &surfel_corrs, ceres::Problem &problem, std::vector<ceres::ResidualBlockId> &residual_ids) {
  for (const auto &surfel_corr : surfel_corrs) {
    CHECK_LT(surfel_corr.s1->timestamp, surfel_corr.s2->timestamp) << std::fixed << std::setprecision(6) << surfel_corr.s1->timestamp << " " << surfel_corr.s2->timestamp;  // bug: disorder happens

    auto sp1r_it = std::upper_bound(sample_states_sld_win_.begin(), sample_states_sld_win_.end(), surfel_corr.s1->timestamp, [](double lhs, const SampleState::Ptr &rhs) { return lhs < rhs->timestamp; });
    CHECK(sp1r_it != sample_states_sld_win_.begin());
    CHECK(sp1r_it != sample_states_sld_win_.end());

    auto sp1l    = *(sp1r_it - 1);
    auto sp1r    = *(sp1r_it);
    auto sp2r_it = std::upper_bound(sample_states_sld_win_.begin(), sample_states_sld_win_.end(), surfel_corr.s2->timestamp, [](double lhs, const SampleState::Ptr &rhs) { return lhs < rhs->timestamp; });
    CHECK(sp2r_it != sample_states_sld_win_.begin());
    CHECK(sp2r_it != sample_states_sld_win_.end());
    auto sp2l = *(sp2r_it - 1);
    auto sp2r = *(sp2r_it);

    if (sp1r_it == sample_states_sld_win_.begin() || sp1r_it == sample_states_sld_win_.end() || sp2r_it == sample_states_sld_win_.begin() || sp2r_it == sample_states_sld_win_.end()) {
      // todo kk
      continue;
    }

    auto loss_function = new ceres::CauchyLoss(0.4);  // todo kk set cauchy loss param
    if (sp1r->timestamp < sp2l->timestamp) {
      auto residual_id = problem.AddResidualBlock(
          new SurfelMatchBinaryFactor<0>(surfel_corr.s1, sp1l, sp1r, surfel_corr.s2, sp2l, sp2r),
          loss_function,
          sp1l->data_cor,
          sp1r->data_cor,
          sp2l->data_cor,
          sp2r->data_cor);
      residual_ids.push_back(residual_id);
    } else if (sp1r->timestamp == sp2l->timestamp) {
      auto residual_id = problem.AddResidualBlock(
          new SurfelMatchBinaryFactor<1>(surfel_corr.s1, sp1l, sp1r, surfel_corr.s2, sp2l, sp2r),
          loss_function,
          sp1l->data_cor,
          sp1r->data_cor,
          sp2r->data_cor);
      residual_ids.push_back(residual_id);
    } else {
      auto residual_id = problem.AddResidualBlock(
          new SurfelMatchBinaryFactor<2>(surfel_corr.s1, sp1l, sp1r, surfel_corr.s2, sp2l, sp2r),
          loss_function,
          sp1l->data_cor,
          sp1r->data_cor);
      residual_ids.push_back(residual_id);
    }
  }
}

void LidarOdometry::BuildImuResiduals(const std::deque<ImuState> &imu_states, ceres::Problem &problem, std::vector<ceres::ResidualBlockId> &residual_ids) {
  for (int i = 0; i < imu_states.size() - 2; ++i) {  // todo kk
    auto &i1 = imu_states[i];
    auto &i2 = imu_states[i + 1];
    auto &i3 = imu_states[i + 2];
    if (i1.timestamp < sample_states_sld_win_.front()->timestamp) {
      continue;
    }
    if (i3.timestamp > sample_states_sld_win_.back()->timestamp) {
      break;
    }
    auto sp2_it = std::upper_bound(sample_states_sld_win_.begin(), sample_states_sld_win_.end(), i1.timestamp, [](double lhs, const SampleState::Ptr &rhs) { return lhs < rhs->timestamp; });
    auto sp1    = *(sp2_it - 1);
    auto sp2    = *(sp2_it);
    if (sp2_it == sample_states_sld_win_.end() - 1) {
      auto residual_id = problem.AddResidualBlock(
          new ImuFactor<1>(i1, i2, i3,
                           sp1->timestamp, sp2->timestamp, DBL_MAX,
                           config_.gyroscope_noise_density_cost_weight,
                           config_.accelerometer_noise_density_cost_weight,
                           config_.gyroscope_random_walk_cost_weight,
                           config_.accelerometer_random_walk_cost_weight,
                           1 / config_.imu_rate, sample_states_sld_win_.back()->grav),
          new ceres::TrivialLoss(),  // todo kk use loss function
          sp1->data_cor,
          sp2->data_cor);
      residual_ids.push_back(residual_id);
    } else {
      auto sp3         = *(sp2_it + 1);
      auto residual_id = problem.AddResidualBlock(
          new ImuFactor<0>(i1, i2, i3,
                           sp1->timestamp, sp2->timestamp, sp3->timestamp,
                           config_.gyroscope_noise_density_cost_weight,
                           config_.accelerometer_noise_density_cost_weight,
                           config_.gyroscope_random_walk_cost_weight,
                           config_.accelerometer_random_walk_cost_weight,
                           1 / config_.imu_rate, sample_states_sld_win_.back()->grav),
          new ceres::TrivialLoss(),
          sp1->data_cor,
          sp2->data_cor,
          sp3->data_cor);
      residual_ids.push_back(residual_id);
    }
  }
}

void LidarOdometry::PredictImuStatesAndSampleStates(double end_time) {
  // 1. try to initialize imu states and sample states
  CHECK_GE(imu_buff_.size(), 2);
  auto        dt           = 1 / config_.imu_rate;
  static bool init_sld_win = false;
  if (!init_sld_win) {
    for (int i = 0; i < 2; ++i) {
      auto imu_msg = imu_buff_.front();
      imu_buff_.pop_front();

      ImuState imu_state;
      imu_state.timestamp = imu_msg.timestamp;
      imu_state.acc       = imu_msg.linear_acceleration;
      imu_state.gyr       = imu_msg.angular_velocity;
      imu_state.pos       = Vector3d::Zero();
      if (i == 0) {
        imu_state.rot = Quaterniond::Identity();
      } else {
        imu_state.rot = Exp((imu_states_sld_win_.back().gyr + imu_state.gyr) / 2 * dt);
      }
      imu_states_sld_win_.push_back(imu_state);
    }

    SampleState::Ptr ss(new SampleState);
    ss->timestamp = imu_states_sld_win_.front().timestamp;
    ss->ba.setZero();  // todo kk estimate bias when platform is stationary
    ss->bg.setZero();
    ss->grav = -config_.gravity_norm * imu_states_sld_win_.front().acc.normalized();
    ss->rot  = imu_states_sld_win_.front().rot;
    ss->pos  = imu_states_sld_win_.front().pos;
    sample_states_sld_win_.push_back(ss);

    init_sld_win = true;
  }

  // 2. predict imu states
  auto ba   = sample_states_sld_win_.back()->ba;
  auto bg   = sample_states_sld_win_.back()->bg;
  auto grav = sample_states_sld_win_.back()->grav;
  while (!imu_buff_.empty()) {
    int size = imu_states_sld_win_.size();

    auto imu_msg = imu_buff_.front();
    imu_buff_.pop_front();

    ImuState imu_state;
    imu_state.timestamp = imu_msg.timestamp;
    imu_state.acc       = imu_msg.linear_acceleration;
    imu_state.gyr       = imu_msg.angular_velocity;
    imu_state.rot       = imu_states_sld_win_[size - 1].rot * Exp(((imu_states_sld_win_[size - 1].gyr + imu_state.gyr) / 2 - bg) * dt);
    imu_state.pos       = (imu_states_sld_win_[size - 2].rot * (imu_states_sld_win_[size - 2].acc - ba) + grav) * dt * dt + 2 * imu_states_sld_win_[size - 1].pos - imu_states_sld_win_[size - 2].pos;
    imu_states_sld_win_.push_back(imu_state);

    if (imu_state.timestamp >= end_time) {
      // ensure that we have enough imu states
      break;
    }
  }

  // 3. add more sample states
  double sample_states_oldtime = sample_states_sld_win_.back()->timestamp;
  auto   sample_states_oldsize = sample_states_sld_win_.size();
  for (double timestamp = sample_states_oldtime + config_.sample_dt; timestamp < end_time; timestamp += config_.sample_dt) {
    SampleState::Ptr ss(new SampleState);
    ss->timestamp = timestamp;
    ss->ba        = ba;
    ss->bg        = bg;
    ss->grav      = grav;

    auto it  = std::lower_bound(imu_states_sld_win_.begin(), imu_states_sld_win_.end(), timestamp, [&](const ImuState &a, double b) {
      return a.timestamp < b;
    });
    auto idx = it - imu_states_sld_win_.begin();

    CHECK_NE(idx, 0);
    CHECK_NE(idx, imu_states_sld_win_.size());

    double factor = (timestamp - imu_states_sld_win_[idx - 1].timestamp) / (imu_states_sld_win_[idx].timestamp - imu_states_sld_win_[idx - 1].timestamp);
    ss->rot       = imu_states_sld_win_[idx - 1].rot.slerp(factor, imu_states_sld_win_[idx].rot);
    ss->pos       = (1 - factor) * imu_states_sld_win_[idx - 1].pos + factor * imu_states_sld_win_[idx].pos;
    CHECK_GE(factor, 0);
    CHECK_LE(factor, 1);
    sample_states_sld_win_.push_back(ss);
  }
  LOG(INFO) << std::fixed << std::setprecision(6) << "Adding sample states_" << sample_states_sld_win_.size() - sample_states_oldsize << "(" << sample_states_oldtime << "," << sample_states_sld_win_.back()->timestamp << "]";
}

bool LidarOdometry::SyncHeadingMsgs() {
  static bool sync_done = false;
  if (sync_done) {
    return true;
  }

  if (imu_buff_.empty() || points_buff_.empty()) {
    return false;
  }

  if (imu_buff_.back().timestamp < points_buff_.front().time) {
    LOG(INFO) << "waiting for imu message...";
    return false;
  }

  while (imu_buff_.front().timestamp < points_buff_.front().time) {
    imu_buff_.pop_front();
    CHECK(!imu_buff_.empty());
  }

  while (points_buff_.front().time < imu_buff_.front().timestamp) {
    points_buff_.pop_front();
    CHECK(!points_buff_.empty());
  }

  sync_done = true;

  return true;
}

void LidarOdometry::AddLidarScan(const pcl::PointCloud<hilti_ros::Point>::Ptr &msg) {
  // transform points from lidar frame to imu frame
  for (auto pt : *msg) {
    pt.getVector3fMap() = (this->config_.ext_lidar2imu * pt.getVector3fMap().cast<double>()).cast<float>();
    CHECK(points_buff_.empty() || pt.time >= points_buff_.back().time);
    if (pt.getVector3fMap().norm() < config_.min_range || pt.getVector3fMap().norm() > config_.max_range || config_.blind_bounding_box.contains(pt.getVector3fMap().cast<double>())) {
      continue;
    }
    points_buff_.push_back(pt);
  }

  if (!SyncHeadingMsgs()) {
    return;
  }

  // 1. collect scan to sweep
  std::vector<hilti_ros::Point> sweep;
  auto                          sweep_endtime = points_buff_.front().time + config_.sweep_duration;
  if (points_buff_.back().time < sweep_endtime || imu_buff_.empty() ||
      imu_buff_.back().timestamp < sweep_endtime) {
    // LOG(INFO) << "Waiting to construct a sweep: " << points_buff_.back().time - points_buff_.front().time;
    return;
  }

  // 2. integrate IMU poses in windows
  PredictImuStatesAndSampleStates(sweep_endtime);
  sweep_endtime = sample_states_sld_win_.back()->timestamp;  // todo kk tmp

  BuildSweep(points_buff_, sweep_endtime, sweep);
  LOG(INFO) << std::fixed << std::setprecision(6) << "Build sweep " << sweep_id_ << " with points_" << sweep.size() << "[" << sweep.front().time << "," << sweep.back().time << "] by sweep_endtime " << sweep_endtime;

  // 3. undistort sweep by IMU poses
  std::vector<hilti_ros::Point> sweep_undistorted;
  UndistortSweep(sweep, imu_states_sld_win_, sweep_undistorted);

  // 4. extract surfels and add to windows, the first time surfels will be add to global map
  std::deque<Surfel::Ptr> surfels_sweep;
  GlobalMap               map;
  BuildSurfels(sweep_undistorted, surfels_sweep, map);
  surfels_sld_win_.insert(surfels_sld_win_.end(), surfels_sweep.begin(), surfels_sweep.end());
  UpdateSurfelPoses(imu_states_sld_win_, surfels_sld_win_);

  for (int iter_num = 0; iter_num < config_.outer_iter_num_max; ++iter_num) {
    std::vector<SurfelCorrespondence> surfel_corrs;

    if (0) {
      FeatureMap::Create(surfels_sld_win_, 3, 0.8, surfel_corrs);
    } else {
      KnnSurfelMatcher window_surfel_matcher;
      window_surfel_matcher.BuildIndex(surfels_sld_win_);
      window_surfel_matcher.Match(surfels_sld_win_, surfel_corrs);
    }

    // 5. sovle poses in windows
    ceres::Problem                      problem;
    std::vector<ceres::ResidualBlockId> surfel_residual_ids, imu_residual_ids;
    BuildLidarResiduals(surfel_corrs, problem, surfel_residual_ids);
    BuildImuResiduals(imu_states_sld_win_, problem, imu_residual_ids);  // todo kk flag

    PrintSurfelResiduals(surfel_residual_ids, problem);
    PrintImuResiduals(imu_residual_ids, problem);

    ceres::Solver::Options option;
    option.minimizer_progress_to_stdout = true;
    option.linear_solver_type           = ceres::SPARSE_NORMAL_CHOLESKY;
    option.max_num_iterations           = config_.inner_iter_num_max;
    ceres::Solver::Summary summary;
    problem.SetParameterization(sample_states_sld_win_[0]->data_cor, new ceres::SubsetParameterization(12, {3, 4, 5}));  // todo kk fix pos of first sample state
    ceres::Solve(option, &problem, &summary);
    LOG(INFO) << summary.BriefReport();

    UpdateImuPoses(sample_states_sld_win_, imu_states_sld_win_);
    UpdateSurfelPoses(imu_states_sld_win_, surfels_sld_win_);
    UpdateSamplePoses(sample_states_sld_win_);

    PrintSurfelResiduals(surfel_residual_ids, problem);
    PrintImuResiduals(imu_residual_ids, problem);
    PrintSampleStates(sample_states_sld_win_);
  }

  ShrinkToFit(sample_states_sld_win_, imu_states_sld_win_, surfels_sld_win_, config_.sliding_window_duration);

  static int i = 0;
  if (++i == 30) {
    exit(EXIT_FAILURE);
  }

  PubSurfels(surfels_sld_win_, pub_plane_map_);
  {
    sensor_msgs::PointCloud2          msg;
    pcl::PointCloud<hilti_ros::Point> cloud;
    for (auto &e : points_buff_) {
      cloud.push_back(e);
    }
    pcl::toROSMsg(cloud, msg);
    msg.header.stamp.fromSec(cloud.points[0].time);
    msg.header.frame_id = "imu_link";
    pub_scan_in_imu_frame_.publish(msg);
  }
  {
    static tf::TransformBroadcaster br;
    tf::Transform                   transform;
    transform.setOrigin(tf::Vector3(sample_states_sld_win_.back()->pos[0], sample_states_sld_win_.back()->pos[1], sample_states_sld_win_.back()->pos[2]));
    transform.setRotation(tf::Quaternion(sample_states_sld_win_.back()->rot.x(), sample_states_sld_win_.back()->rot.y(), sample_states_sld_win_.back()->rot.z(), sample_states_sld_win_.back()->rot.w()));
    br.sendTransform(tf::StampedTransform(transform, ros::Time().fromSec(sample_states_sld_win_.back()->timestamp), "world", "imu_link"));
  }

  ++sweep_id_;
}

void LidarOdometry::AddImuData(const ImuData &msg) {
  auto msg_new = msg;
  // msg_new.angular_velocity += Vector3d::Constant(0.02);
  this->imu_buff_.push_back(msg_new);
}

LidarOdometry::LidarOdometry() {
  pub_plane_map_         = nh_.advertise<visualization_msgs::MarkerArray>("/current_planes", 10);
  pub_scan_in_imu_frame_ = nh_.advertise<sensor_msgs::PointCloud2>("/scan_in_imu_frame", 10);
}