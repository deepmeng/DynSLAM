
#ifndef DYNSLAM_EVALUATION_H
#define DYNSLAM_EVALUATION_H

#include "../DynSlam.h"
#include "../Input.h"
#include "Velodyne.h"
#include "ILidarEvalCallback.h"
#include "Tracklets.h"

namespace dynslam {
class DynSlam;
}

namespace dynslam {
namespace eval {

/// \brief Interface for poor man's serialization.
class ICsvSerializable {
 public:
  virtual ~ICsvSerializable() = default;

  /// \brief Should return the field names in the same order as GetData, without a newline.
  virtual std::string GetHeader() const = 0;
  virtual std::string GetData() const = 0;
};

struct DepthResult : public ICsvSerializable {
  const long measurement_count;
  const long error_count;
  const long missing_count;
  const long correct_count;

  DepthResult(const long measurement_count,
              const long error_count,
              const long missing_count,
              const long correct_count)
      : measurement_count(measurement_count),
        error_count(error_count),
        missing_count(missing_count),
        correct_count(correct_count) {
    assert(measurement_count == (error_count + missing_count + correct_count));
  }

  /// \brief Returns the ratio of correct pixels in this depth evaluation result.
  /// \param include_missing Whether to count pixels with no depth data in the evaluated depth map
  ///                        as incorrect.
  double GetCorrectPixelRatio(bool include_missing) const {
    if (include_missing) {
      return static_cast<double>(correct_count) / measurement_count;
    } else {
      return static_cast<double>(correct_count) / (measurement_count - missing_count);
    }
  }

  string GetHeader() const override {
    return "measurements_count,error_count,missing_count,correct_count";
  }

  string GetData() const override {
    return utils::Format("%d,%d,%d,%d",
                         measurement_count,
                         error_count,
                         missing_count,
                         correct_count);
  }
};

struct DepthEvaluationMeta {
  const int frame_idx;
  const std::string dataset_id;

  DepthEvaluationMeta(const int frame_idx, const string &dataset_id)
      : frame_idx(frame_idx), dataset_id(dataset_id) {}
};

/// \brief Stores the result of comparing a computed depth with a LIDAR ground truth.
struct DepthEvaluation : public ICsvSerializable {
  const int delta_max;

  /// \brief Results for the depth map synthesized from engine.
  const DepthResult fused_result;

  /// \brief Results for the depth map received as input.
  const DepthResult input_result;

  DepthEvaluation(const int delta_max,
                  DepthResult &&fused_result,
                  DepthResult &&input_result)
      : delta_max(delta_max),
        fused_result(fused_result),
        input_result(input_result) {}

  string GetHeader() const override {
    return utils::Format("fusion-total-%d,fusion-error-%d,fusion-missing-%d,fusion-correct-%d,"
                         "input-total-%d,input-error-%d,input-missing-%d,input-correct-%d",
                         delta_max, delta_max, delta_max, delta_max, delta_max, delta_max,
                         delta_max, delta_max);
  }

  string GetData() const override {
    return utils::Format("%s,%s", fused_result.GetData().c_str(), input_result.GetData().c_str());
  }
};

/// \brief Contains a frame's depth evaluation results for multiple values of $\delta_max$.
struct DepthFrameEvaluation : public ICsvSerializable {
  const DepthEvaluationMeta meta;
  const float max_depth_meters;
  const std::vector<DepthEvaluation> evaluations;

  DepthFrameEvaluation(DepthEvaluationMeta &&meta,
                       float max_depth_meters,
                       vector<DepthEvaluation> &&evaluations)
      : meta(meta), max_depth_meters(max_depth_meters), evaluations(evaluations) {}

  string GetHeader() const override {
    std::stringstream ss;
    ss << "frame";
    for (auto &eval : evaluations) {
      ss << "," << eval.GetHeader();
    }
    return ss.str();
  }

  string GetData() const override {
    std::stringstream ss;
    ss << meta.frame_idx;
    for (auto &eval :evaluations) {
      ss << "," << eval.GetData();
    }
    return ss.str();
  }
};

/// \brief Main class handling the quantitative evaluation of the DynSLAM system.
class Evaluation {
 public:
  static std::string GetCsvName(const std::string &dataset_root,
                                const Input *input,
                                float voxel_size_meters) {
    return utils::Format("%s-offset-%d-depth-%s-voxelsize-%.4f-max-depth-m-%.2f-results.csv",
                         input->GetDatasetIdentifier().c_str(),
                         input->GetCurrentFrame(),
                         input->GetDepthProvider()->GetName().c_str(),
                         voxel_size_meters,
                         input->GetDepthProvider()->GetMaxDepthMeters());
  }

 public:
  Evaluation(const std::string &dataset_root,
             const Input *input,
             const Eigen::Matrix4d &velodyne_to_rgb,
             float voxel_size_meters)
      : velodyne_(new Velodyne(utils::Format("%s/%s",
                                             dataset_root.c_str(),
                                             input->GetConfig().velodyne_folder.c_str()),
                                input->GetConfig().velodyne_fname_format,
                                velodyne_to_rgb)),
        csv_dump_(new std::ofstream(GetCsvName(dataset_root, input, voxel_size_meters))),
        eval_tracklets_(! input->GetConfig().tracklet_folder.empty())
  {
    if (this->eval_tracklets_) {
      cout << "Found tracklet GT data. Enabling track evaluation!" << endl;
      std::string tracklet_fpath = utils::Format("%s/%s", dataset_root.c_str(),
                                                 input->GetConfig().tracklet_folder.c_str());
      frame_to_tracklets_ = ReadGroupedTracklets(tracklet_fpath);
    }
  }

  Evaluation(const Evaluation&) = delete;
  Evaluation(const Evaluation&&) = delete;
  Evaluation& operator=(const Evaluation&) = delete;
  Evaluation& operator=(const Evaluation&&) = delete;

  virtual ~Evaluation() {
    delete csv_dump_;
    delete velodyne_;
  }

  /// \brief Supermethod in charge of all per-frame evaluation metrics.
  void EvaluateFrame(Input *input, DynSlam *dyn_slam);

  DepthFrameEvaluation EvaluateFrame(int frame_idx, Input *input, DynSlam *dyn_slam);

  static uint depth_delta(uchar computed_depth, uchar ground_truth_depth) {
    return static_cast<uint>(abs(
        static_cast<int>(computed_depth) - static_cast<int>(ground_truth_depth)));
  }

  /// \brief Compares a fused depth map and input depth map to the corresponding LIDAR pointcloud,
  ///        which is considered to be the ground truth.
  /// \param compare_on_intersection If true, then the accuracy of both input and fused depth is
  /// computed only for ground truth LIDAR points which have both corresponding input depth, as well
  /// as fused depth. Otherwise, the input and depth accuracies are compute separately.
  ///
  /// Projects each LIDAR point into both the left and the right camera frames, in order to compute
  /// the ground truth disparity. Then, if input and/or rendered depth values are available at
  /// those coordinates, computes their corresponding disparity as well, comparing it to the ground
  /// truth disparity.
  ///
  /// A disparity is counted as accurate if the absoluted difference between it and the ground truth
  /// disparity is less than 'delta_max'. Based on the evaluation method from [0].
  ///
  /// [0]: Sengupta, S., Greveson, E., Shahrokni, A., & Torr, P. H. S. (2013). Urban 3D semantic modelling using stereo vision. Proceedings - IEEE International Conference on Robotics and Automation, 580–585. https://doi.org/10.1109/ICRA.2013.6630632
  DepthEvaluation EvaluateDepth(const Eigen::MatrixX4f &lidar_points,
                                const float *rendered_depth,
                                const cv::Mat1s &input_depth_mm,
                                const Eigen::Matrix4d &velo_to_left_gray_cam,
                                const Eigen::Matrix34d &proj_left_color,
                                const Eigen::Matrix34d &proj_right_color,
                                float baseline_m,
                                int frame_width,
                                int frame_height,
                                float min_depth_meters,
                                float max_depth_meters,
                                uint delta_max,
                                bool compare_on_intersection,
                                ILidarEvalCallback *callback) const;

  Velodyne *GetVelodyne() {
    return velodyne_;
  }

  const Velodyne *GetVelodyne() const {
    return velodyne_;
  }

  SUPPORT_EIGEN_FIELDS;

 private:
  Velodyne *velodyne_;
  ostream *csv_dump_;

  // Used in CSV data dumping.
  bool wrote_header_ = false;

  const bool eval_tracklets_;
  std::map<int, std::vector<TrackletFrame, Eigen::aligned_allocator<TrackletFrame>>> frame_to_tracklets_;
};

}
}

#endif //DYNSLAM_EVALUATION_H
