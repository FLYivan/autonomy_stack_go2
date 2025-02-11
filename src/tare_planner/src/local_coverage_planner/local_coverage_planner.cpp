/**
 * @file local_coverage_planner.cpp
 * @author Chao Cao (ccao1@andrew.cmu.edu)
 * @brief Class that ensures coverage in the surroundings of the robot
 * @version 0.1
 * @date 2021-05-31
 *
 * @copyright Copyright (c) 2021
 *
 */

#include "local_coverage_planner/local_coverage_planner.h"

namespace local_coverage_planner_ns {
const std::string LocalCoveragePlanner::kRuntimeUnit = "us";

bool LocalCoveragePlannerParameter::ReadParameters(rclcpp::Node::SharedPtr nh) {
  nh->get_parameter("kMinAddPointNumSmall", kMinAddPointNum);
  nh->get_parameter("kMinAddFrontierPointNum", kMinAddFrontierPointNum);
  nh->get_parameter("kGreedyViewPointSampleRange", kGreedyViewPointSampleRange);
  nh->get_parameter("kLocalPathOptimizationItrMax",
                    kLocalPathOptimizationItrMax);

  return true;
}
LocalCoveragePlanner::LocalCoveragePlanner(rclcpp::Node::SharedPtr nh)
    : lookahead_point_update_(false), use_frontier_(true),
      local_coverage_complete_(false) {
  parameters_.ReadParameters(nh);
}

int LocalCoveragePlanner::GetBoundaryViewpointIndex(
    const exploration_path_ns::ExplorationPath &global_path) {
  int boundary_viewpoint_index = robot_viewpoint_ind_;
  if (!global_path.nodes_.empty()) {
    if (viewpoint_manager_->InLocalPlanningHorizon(
            global_path.nodes_.front().position_)) {
      for (int i = 0; i < global_path.nodes_.size(); i++) {
        if (global_path.nodes_[i].type_ ==
                exploration_path_ns::NodeType::GLOBAL_VIEWPOINT ||
            global_path.nodes_[i].type_ ==
                exploration_path_ns::NodeType::HOME ||
            !viewpoint_manager_->InLocalPlanningHorizon(
                global_path.nodes_[i].position_)) {
          break;
        }
        boundary_viewpoint_index =
            viewpoint_manager_->GetNearestCandidateViewPointInd(
                global_path.nodes_[i].position_);
      }
    }
  }
  return boundary_viewpoint_index;
}

void LocalCoveragePlanner::GetBoundaryViewpointIndices(
    exploration_path_ns::ExplorationPath global_path) {
  start_viewpoint_ind_ = GetBoundaryViewpointIndex(global_path);
  global_path.Reverse();
  end_viewpoint_ind_ = GetBoundaryViewpointIndex(global_path);
}

void LocalCoveragePlanner::GetNavigationViewPointIndices(
    exploration_path_ns::ExplorationPath global_path,
    std::vector<int> &navigation_viewpoint_indices) {
  // Get start and end point
  robot_viewpoint_ind_ =
      viewpoint_manager_->GetNearestCandidateViewPointInd(robot_position_);
  lookahead_viewpoint_ind_ =
      viewpoint_manager_->GetNearestCandidateViewPointInd(lookahead_point_);
  if (!lookahead_point_update_ ||
      !viewpoint_manager_->InRange(lookahead_viewpoint_ind_)) {
    lookahead_viewpoint_ind_ = robot_viewpoint_ind_;
  }
  // Get connecting viewpoints to the global path
  GetBoundaryViewpointIndices(global_path);

  // Update the coverage with viewpoints that must visit
  navigation_viewpoint_indices.push_back(start_viewpoint_ind_);
  navigation_viewpoint_indices.push_back(end_viewpoint_ind_);
  navigation_viewpoint_indices.push_back(robot_viewpoint_ind_);
  navigation_viewpoint_indices.push_back(lookahead_viewpoint_ind_);
}

void LocalCoveragePlanner::UpdateViewPointCoveredPoint(
    std::vector<bool> &point_list, int viewpoint_index, bool use_array_ind) {
  for (const auto &point_ind : viewpoint_manager_->GetViewPointCoveredPointList(
           viewpoint_index, use_array_ind)) {
    MY_ASSERT(misc_utils_ns::InRange<bool>(point_list, point_ind));
    point_list[point_ind] = true;
  }
}
void LocalCoveragePlanner::UpdateViewPointCoveredFrontierPoint(
    std::vector<bool> &frontier_point_list, int viewpoint_index,
    bool use_array_ind) {
  for (const auto &point_ind :
       viewpoint_manager_->GetViewPointCoveredFrontierPointList(
           viewpoint_index, use_array_ind)) {
    MY_ASSERT(misc_utils_ns::InRange<bool>(frontier_point_list, point_ind));
    frontier_point_list[point_ind] = true;
  }
}

void LocalCoveragePlanner::EnqueueViewpointCandidates(
    std::vector<std::pair<int, int>> &cover_point_queue,
    std::vector<std::pair<int, int>> &frontier_queue,
    const std::vector<bool> &covered_point_list,
    const std::vector<bool> &covered_frontier_point_list,
    const std::vector<int> &selected_viewpoint_array_indices) {
  for (const auto &viewpoint_index :
       viewpoint_manager_->GetViewPointCandidateIndices()) {
    if (viewpoint_manager_->ViewPointVisited(viewpoint_index) ||
        !viewpoint_manager_->ViewPointInExploringCell(viewpoint_index)) {
      continue;
    }
    int viewpoint_array_index =
        viewpoint_manager_->GetViewPointArrayInd(viewpoint_index);
    if (std::find(selected_viewpoint_array_indices.begin(),
                  selected_viewpoint_array_indices.end(),
                  viewpoint_array_index) !=
        selected_viewpoint_array_indices.end()) {
      continue;
    }
    int covered_point_num = viewpoint_manager_->GetViewPointCoveredPointNum(
        covered_point_list, viewpoint_array_index, true);
    if (covered_point_num >= parameters_.kMinAddPointNum) {
      cover_point_queue.emplace_back(covered_point_num, viewpoint_index);
    } else if (use_frontier_) {
      int covered_frontier_point_num =
          viewpoint_manager_->GetViewPointCoveredFrontierPointNum(
              covered_frontier_point_list, viewpoint_array_index, true);
      if (covered_frontier_point_num >= parameters_.kMinAddFrontierPointNum) {
        frontier_queue.emplace_back(covered_frontier_point_num,
                                    viewpoint_index);
      }
    }
  }

  // Sort the queue
  std::sort(cover_point_queue.begin(), cover_point_queue.end(), SortPairInRev);
  if (use_frontier_) {
    std::sort(frontier_queue.begin(), frontier_queue.end(), SortPairInRev);
  }
}

void LocalCoveragePlanner::SelectViewPoint(
    const std::vector<std::pair<int, int>> &queue,  // 视点队列
    const std::vector<bool> &covered,  // 已覆盖点的标记
    std::vector<int> &selected_viewpoint_indices, bool use_frontier) {  // 已选择的视点索引和是否使用边界标记
  if (use_frontier) {  // 如果使用边界
    if (queue.empty() || queue[0].first < parameters_.kMinAddFrontierPointNum) {  // 如果队列为空或第一个视点覆盖的边界点数小于阈值
      return;  // 返回
    }
  } else {  // 如果不使用边界
    if (queue.empty() || queue[0].first < parameters_.kMinAddPointNum) {  // 如果队列为空或第一个视点覆盖的点数小于阈值
      return;  // 返回
    }
  }

  std::vector<bool> covered_copy;  // 创建已覆盖点标记的副本
  for (int i = 0; i < covered.size(); i++) {  // 遍历已覆盖点标记
    covered_copy.push_back(covered[i]);  // 复制标记
  }
  std::vector<std::pair<int, int>> queue_copy;  // 创建队列的副本
  for (int i = 0; i < queue.size(); i++) {  // 遍历队列
    queue_copy.push_back(queue[i]);  // 复制队列元素
  }

  int sample_range = 0;  // 采样范围
  for (int i = 0; i < queue_copy.size(); i++) {  // 遍历队列副本
    if (use_frontier) {  // 如果使用边界
      if (queue_copy[i].first >= parameters_.kMinAddFrontierPointNum) {  // 如果视点覆盖的边界点数大于等于阈值
        sample_range++;  // 增加采样范围
      }
    } else {  // 如果不使用边界
      if (queue_copy[i].first >= parameters_.kMinAddPointNum) {  // 如果视点覆盖的点数大于等于阈值
        sample_range++;  // 增加采样范围
      }
    }
  }

  sample_range =  // 限制采样范围
      std::min(parameters_.kGreedyViewPointSampleRange, sample_range);  // 取贪心采样范围和计算得到的范围的较小值
  std::random_device rd;  // 随机数生成器
  std::mt19937 gen(rd());  // Mersenne Twister伪随机数生成器
  std::uniform_int_distribution<int> gen_next_queue_idx(0, sample_range - 1);  // 均匀分布的整数随机数生成器
  int queue_idx = gen_next_queue_idx(gen);  // 生成随机队列索引
  int cur_ind = queue_copy[queue_idx].second;  // 获取当前视点索引

  while (true) {  // 循环选择视点
    int cur_array_ind = viewpoint_manager_->GetViewPointArrayInd(cur_ind);  // 获取当前视点的数组索引
    if (use_frontier) {  // 如果使用边界
      for (const auto &point_ind :  // 遍历当前视点覆盖的边界点
           viewpoint_manager_->GetViewPointCoveredFrontierPointList(
               cur_array_ind, true))  // 获取视点覆盖的边界点列表

      {
        MY_ASSERT(misc_utils_ns::InRange<bool>(covered_copy, point_ind));  // 断言点索引在范围内
        if (!covered_copy[point_ind]) {  // 如果点未被覆盖
          covered_copy[point_ind] = true;  // 标记为已覆盖
        }
      }
    } else {  // 如果不使用边界
      for (const auto &point_ind :  // 遍历当前视点覆盖的点
           viewpoint_manager_->GetViewPointCoveredPointList(cur_array_ind,
                                                            true)) {  // 获取视点覆盖的点列表
        MY_ASSERT(misc_utils_ns::InRange<bool>(covered_copy, point_ind));  // 断言点索引在范围内
        if (!covered_copy[point_ind]) {  // 如果点未被覆盖
          covered_copy[point_ind] = true;  // 标记为已覆盖
        }
      }
    }
    selected_viewpoint_indices.push_back(cur_ind);  // 将当前视点添加到已选择列表
    queue_copy.erase(queue_copy.begin() + queue_idx);  // 从队列中删除当前视点

    // Update the queue  // 更新队列
    for (int i = 0; i < queue_copy.size(); i++) {  // 遍历队列
      int add_point_num = 0;  // 新增覆盖点数
      int ind = queue_copy[i].second;  // 获取视点索引
      int array_ind = viewpoint_manager_->GetViewPointArrayInd(ind);  // 获取视点数组索引
      if (use_frontier) {  // 如果使用边界
        for (const auto &point_ind :  // 遍历视点覆盖的边界点
             viewpoint_manager_->GetViewPointCoveredFrontierPointList(array_ind,
                                                                      true)) {  // 获取视点覆盖的边界点列表
          MY_ASSERT(misc_utils_ns::InRange<bool>(covered_copy, point_ind));  // 断言点索引在范围内
          if (!covered_copy[point_ind]) {  // 如果点未被覆盖
            add_point_num++;  // 增加新增覆盖点数
          }
        }
      } else {  // 如果不使用边界
        for (const auto &point_ind :  // 遍历视点覆盖的点
             viewpoint_manager_->GetViewPointCoveredPointList(array_ind,
                                                              true)) {  // 获取视点覆盖的点列表
          MY_ASSERT(misc_utils_ns::InRange<bool>(covered_copy, point_ind));  // 断言点索引在范围内
          if (!covered_copy[point_ind]) {  // 如果点未被覆盖
            add_point_num++;  // 增加新增覆盖点数
          }
        }
      }

      queue_copy[i].first = add_point_num;  // 更新队列中视点的覆盖点数
    }

    std::sort(queue_copy.begin(), queue_copy.end(), SortPairInRev);  // 对队列按覆盖点数降序排序

    if (queue_copy.empty() ||  // 如果队列为空
        queue_copy[0].first < parameters_.kMinAddPointNum) {  // 或第一个视点覆盖的点数小于阈值
      break;  // 退出循环
    }
    if (use_frontier) {  // 如果使用边界
      if (queue_copy.empty() ||  // 如果队列为空
          queue_copy[0].first < parameters_.kMinAddFrontierPointNum) {  // 或第一个视点覆盖的边界点数小于阈值
        break;  // 退出循环
      }
    }

    // Randomly select the next point  // 随机选择下一个点
    int sample_range = 0;  // 采样范围
    for (int i = 0; i < queue.size(); i++) {  // 遍历队列
      if (use_frontier) {  // 如果使用边界
        if (queue[i].first >= parameters_.kMinAddFrontierPointNum) {  // 如果视点覆盖的边界点数大于等于阈值
          sample_range++;  // 增加采样范围
        }
      } else {  // 如果不使用边界
        if (queue[i].first >= parameters_.kMinAddPointNum) {  // 如果视点覆盖的点数大于等于阈值
          sample_range++;  // 增加采样范围
        }
      }
    }
    sample_range =  // 限制采样范围
        std::min(parameters_.kGreedyViewPointSampleRange, sample_range);  // 取贪心采样范围和计算得到的范围的较小值
    std::uniform_int_distribution<int> gen_next_queue_idx(0, sample_range - 1);  // 均匀分布的整数随机数生成器
    queue_idx = gen_next_queue_idx(gen);  // 生成随机队列索引
    cur_ind = queue_copy[queue_idx].second;  // 获取下一个视点索引
  }
}

void LocalCoveragePlanner::SelectViewPointFromFrontierQueue(
    std::vector<std::pair<int, int>> &frontier_queue,  // 边界队列
    std::vector<bool> &frontier_covered,  // 边界点是否被覆盖的标记
    std::vector<int> &selected_viewpoint_indices) {  // 已选择的视点索引
  if (use_frontier_ && !frontier_queue.empty() &&
      frontier_queue[0].first > parameters_.kMinAddFrontierPointNum) {  // 如果使用边界且队列非空且第一个视点覆盖的边界点数超过阈值
    // Update the frontier queue
    for (const auto &ind : selected_viewpoint_indices) {  // 遍历已选择的视点
      UpdateViewPointCoveredFrontierPoint(frontier_covered, ind);  // 更新每个视点覆盖的边界点
    }
    for (int i = 0; i < frontier_queue.size(); i++) {  // 遍历边界队列
      int ind = frontier_queue[i].second;  // 获取视点索引
      int covered_frontier_point_num =
          viewpoint_manager_->GetViewPointCoveredFrontierPointNum(
              frontier_covered, ind);  // 获取视点覆盖的边界点数量
      frontier_queue[i].first = covered_frontier_point_num;  // 更新队列中的覆盖点数
    }
    std::sort(frontier_queue.begin(), frontier_queue.end(), SortPairInRev);  // 按覆盖点数降序排序
    SelectViewPoint(frontier_queue, frontier_covered,
                    selected_viewpoint_indices, true);  // 从边界队列中选择视点
  }
}

exploration_path_ns::ExplorationPath LocalCoveragePlanner::SolveTSP(
    const std::vector<int> &selected_viewpoint_indices,
    std::vector<int> &ordered_viewpoint_indices)

{
  // nav_msgs::msg::Path tsp_path;
  exploration_path_ns::ExplorationPath tsp_path;

  if (selected_viewpoint_indices.empty()) {
    return tsp_path;
  }

  // Get start and end index
  int start_ind = selected_viewpoint_indices.size() - 1;
  int end_ind = selected_viewpoint_indices.size() - 1;
  int robot_ind = 0;
  int lookahead_ind = 0;

  for (int i = 0; i < selected_viewpoint_indices.size(); i++) {
    if (selected_viewpoint_indices[i] == start_viewpoint_ind_) {
      start_ind = i;
    }
    if (selected_viewpoint_indices[i] == end_viewpoint_ind_) {
      end_ind = i;
    }
    if (selected_viewpoint_indices[i] == robot_viewpoint_ind_) {
      robot_ind = i;
    }
    if (selected_viewpoint_indices[i] == lookahead_viewpoint_ind_) {
      lookahead_ind = i;
    }
  }

  bool has_start_end_dummy = start_ind != end_ind;
  bool has_robot_lookahead_dummy = robot_ind != lookahead_ind;

  // Get distance matrix
  int node_size;
  if (has_start_end_dummy && has_robot_lookahead_dummy) {
    node_size = selected_viewpoint_indices.size() + 2;
  } else if (has_start_end_dummy || has_robot_lookahead_dummy) {
    node_size = selected_viewpoint_indices.size() + 1;
  } else {
    node_size = selected_viewpoint_indices.size();
  }
  misc_utils_ns::Timer find_path_timer("find path");
  find_path_timer.Start();
  std::vector<std::vector<int>> distance_matrix(node_size,
                                                std::vector<int>(node_size, 0));
  std::vector<int> tmp;
  for (int i = 0; i < selected_viewpoint_indices.size(); i++) {
    int from_ind = selected_viewpoint_indices[i];
    // int from_graph_idx = graph_index_map_[from_ind];
    for (int j = 0; j < i; j++) {
      int to_ind = selected_viewpoint_indices[j];
      nav_msgs::msg::Path path =
          viewpoint_manager_->GetViewPointShortestPath(from_ind, to_ind);
      double path_length = misc_utils_ns::GetPathLength(path);
      //   int to_graph_idx = graph_index_map_[to_ind];
      //   double path_length =
      //       misc_utils_ns::AStarSearch(candidate_viewpoint_graph_,
      //       candidate_viewpoint_dist_,
      //                                  candidate_viewpoint_position_,
      //                                  from_graph_idx, to_graph_idx, false,
      //                                  tmp);
      distance_matrix[i][j] = static_cast<int>(10 * path_length);
    }
  }

  for (int i = 0; i < selected_viewpoint_indices.size(); i++) {
    for (int j = i + 1; j < selected_viewpoint_indices.size(); j++) {
      distance_matrix[i][j] = distance_matrix[j][i];
    }
  }

  // Add a dummy node to connect the start and end nodes
  if (has_start_end_dummy && has_robot_lookahead_dummy) {
    int start_end_dummy_node_ind = node_size - 1;
    int robot_lookahead_dummy_node_ind = node_size - 2;
    for (int i = 0; i < selected_viewpoint_indices.size(); i++) {
      if (i == start_ind || i == end_ind) {
        distance_matrix[i][start_end_dummy_node_ind] = 0;
        distance_matrix[start_end_dummy_node_ind][i] = 0;
      } else {
        distance_matrix[i][start_end_dummy_node_ind] = 9999;
        distance_matrix[start_end_dummy_node_ind][i] = 9999;
      }
      if (i == robot_ind || i == lookahead_ind) {
        distance_matrix[i][robot_lookahead_dummy_node_ind] = 0;
        distance_matrix[robot_lookahead_dummy_node_ind][i] = 0;
      } else {
        distance_matrix[i][robot_lookahead_dummy_node_ind] = 9999;
        distance_matrix[robot_lookahead_dummy_node_ind][i] = 9999;
      }
    }

    distance_matrix[start_end_dummy_node_ind][robot_lookahead_dummy_node_ind] =
        9999;
    distance_matrix[robot_lookahead_dummy_node_ind][start_end_dummy_node_ind] =
        9999;
  } else if (has_start_end_dummy) {
    int end_node_ind = node_size - 1;
    for (int i = 0; i < selected_viewpoint_indices.size(); i++) {
      if (i == start_ind || i == end_ind) {
        distance_matrix[i][end_node_ind] = 0;
        distance_matrix[end_node_ind][i] = 0;
      } else {
        distance_matrix[i][end_node_ind] = 9999;
        distance_matrix[end_node_ind][i] = 9999;
      }
    }
  } else if (has_robot_lookahead_dummy) {
    int end_node_ind = node_size - 1;
    for (int i = 0; i < selected_viewpoint_indices.size(); i++) {
      if (i == robot_ind || i == lookahead_ind) {
        distance_matrix[i][end_node_ind] = 0;
        distance_matrix[end_node_ind][i] = 0;
      } else {
        distance_matrix[i][end_node_ind] = 9999;
        distance_matrix[end_node_ind][i] = 9999;
      }
    }
  }

  find_path_timer.Stop(false);
  find_path_runtime_ += find_path_timer.GetDuration(kRuntimeUnit);

  misc_utils_ns::Timer tsp_timer("tsp");
  tsp_timer.Start();

  tsp_solver_ns::DataModel data;
  data.distance_matrix = distance_matrix;
  data.depot = start_ind;

  tsp_solver_ns::TSPSolver tsp_solver(data);
  tsp_solver.Solve();

  std::vector<int> path_index;
  if (has_start_end_dummy) {
    tsp_solver.getSolutionNodeIndex(path_index, true);
  } else {
    tsp_solver.getSolutionNodeIndex(path_index, false);
  }

  // Get rid of the dummy node connecting the robot and lookahead point
  for (int i = 0; i < path_index.size(); i++) {
    if (path_index[i] >= selected_viewpoint_indices.size() ||
        path_index[i] < 0) {
      path_index.erase(path_index.begin() + i);
      i--;
    }
  }

  ordered_viewpoint_indices.clear();
  for (int i = 0; i < path_index.size(); i++) {
    ordered_viewpoint_indices.push_back(
        selected_viewpoint_indices[path_index[i]]);
  }

  // Add the end node index
  if (start_ind == end_ind && !path_index.empty()) {
    path_index.push_back(path_index[0]);
  }

  tsp_timer.Stop(false);
  tsp_runtime_ += tsp_timer.GetDuration(kRuntimeUnit);

  if (path_index.size() > 1) {
    int cur_ind;
    int next_ind;
    int from_graph_idx;
    int to_graph_idx;

    for (int i = 0; i < path_index.size() - 1; i++) {
      cur_ind = selected_viewpoint_indices[path_index[i]];
      next_ind = selected_viewpoint_indices[path_index[i + 1]];

      //   from_graph_idx = graph_index_map_[cur_ind];
      //   to_graph_idx = graph_index_map_[next_ind];

      // Add viewpoint node
      // int cur_array_ind = grid_->GetArrayInd(cur_ind);
      // geometry_msgs::msg::Point cur_node_position =
      // viewpoints_[cur_array_ind].GetPosition();
      geometry_msgs::msg::Point cur_node_position =
          viewpoint_manager_->GetViewPointPosition(cur_ind);
      exploration_path_ns::Node cur_node(
          cur_node_position, exploration_path_ns::NodeType::LOCAL_VIEWPOINT);
      cur_node.local_viewpoint_ind_ = cur_ind;
      if (cur_ind == robot_viewpoint_ind_) {
        cur_node.type_ = exploration_path_ns::NodeType::ROBOT;
      } else if (cur_ind == lookahead_viewpoint_ind_) {
        int covered_point_num =
            viewpoint_manager_->GetViewPointCoveredPointNum(cur_ind);
        int covered_frontier_num =
            viewpoint_manager_->GetViewPointCoveredFrontierPointNum(cur_ind);
        if (covered_point_num > parameters_.kMinAddPointNum ||
            covered_frontier_num > parameters_.kMinAddFrontierPointNum) {
          cur_node.type_ = exploration_path_ns::NodeType::LOCAL_VIEWPOINT;
        } else {
          cur_node.type_ = exploration_path_ns::NodeType::LOOKAHEAD_POINT;
        }
      } else if (cur_ind == start_viewpoint_ind_) {
        cur_node.type_ = exploration_path_ns::NodeType::LOCAL_PATH_START;
      } else if (cur_ind == end_viewpoint_ind_) {
        cur_node.type_ = exploration_path_ns::NodeType::LOCAL_PATH_END;
      }
      tsp_path.Append(cur_node);

      nav_msgs::msg::Path path_between_viewpoints =
          viewpoint_manager_->GetViewPointShortestPath(cur_ind, next_ind);

      //   std::vector<int> path_graph_indices;
      //   misc_utils_ns::AStarSearch(candidate_viewpoint_graph_,
      //   candidate_viewpoint_dist_, candidate_viewpoint_position_,
      //                              from_graph_idx, to_graph_idx, true,
      //                              path_graph_indices);
      // Add viapoint nodes;
      //   if (path_graph_indices.size() > 2)
      if (path_between_viewpoints.poses.size() > 2) {
        // for (int j = 1; j < path_graph_indices.size() - 1; j++)
        for (int j = 1; j < path_between_viewpoints.poses.size() - 1; j++) {
          //   int graph_idx = path_graph_indices[j];
          //   int ind = candidate_indices_[graph_idx];
          exploration_path_ns::Node node;
          node.type_ = exploration_path_ns::NodeType::LOCAL_VIA_POINT;
          node.local_viewpoint_ind_ = -1;
          //   geometry_msgs::msg::Point node_position =
          //   viewpoint_manager_->GetViewPointPosition(ind); node.position_.x()
          //   = node_position.x; node.position_.y() = node_position.y;
          //   node.position_.z() = node_position.z;
          node.position_.x() = path_between_viewpoints.poses[j].pose.position.x;
          node.position_.y() = path_between_viewpoints.poses[j].pose.position.y;
          node.position_.z() = path_between_viewpoints.poses[j].pose.position.z;
          tsp_path.Append(node);
        }
      }

      geometry_msgs::msg::Point next_node_position =
          viewpoint_manager_->GetViewPointPosition(next_ind);
      exploration_path_ns::Node next_node(
          next_node_position, exploration_path_ns::NodeType::LOCAL_VIEWPOINT);
      next_node.local_viewpoint_ind_ = next_ind;
      if (next_ind == robot_viewpoint_ind_) {
        next_node.type_ = exploration_path_ns::NodeType::ROBOT;
      } else if (next_ind == lookahead_viewpoint_ind_) {
        next_node.type_ = exploration_path_ns::NodeType::LOOKAHEAD_POINT;
      } else if (next_ind == start_viewpoint_ind_) {
        next_node.type_ = exploration_path_ns::NodeType::LOCAL_PATH_START;
      } else if (next_ind == end_viewpoint_ind_) {
        next_node.type_ = exploration_path_ns::NodeType::LOCAL_PATH_END;
      }
      tsp_path.Append(next_node);
    }
  }

  return tsp_path;
}

exploration_path_ns::ExplorationPath
LocalCoveragePlanner::SolveLocalCoverageProblem(
    const exploration_path_ns::ExplorationPath &global_path,
    int uncovered_point_num, int uncovered_frontier_point_num) {
  exploration_path_ns::ExplorationPath local_path;

  find_path_runtime_ = 0;
  viewpoint_sampling_runtime_ = 0;
  tsp_runtime_ = 0;

  local_coverage_complete_ = false;

  misc_utils_ns::Timer find_path_timer("find path");
  find_path_timer.Start();

  std::vector<int> navigation_viewpoint_indices;
  GetNavigationViewPointIndices(global_path, navigation_viewpoint_indices);

  find_path_timer.Stop(false);
  find_path_runtime_ += find_path_timer.GetDuration(kRuntimeUnit);

  // Sampling viewpoints
  misc_utils_ns::Timer viewpoint_sampling_timer("viewpoint sampling");
  viewpoint_sampling_timer.Start();

  std::vector<bool> covered(uncovered_point_num, false);
  std::vector<bool> frontier_covered(uncovered_frontier_point_num, false);

  std::vector<int> pre_selected_viewpoint_array_indices;
  std::vector<int> reused_viewpoint_indices;
  for (auto &viewpoint_array_ind : last_selected_viewpoint_array_indices_) {
    if (viewpoint_manager_->ViewPointVisited(viewpoint_array_ind, true) ||
        !viewpoint_manager_->IsViewPointCandidate(viewpoint_array_ind, true)) {
      continue;
    }
    int covered_point_num = viewpoint_manager_->GetViewPointCoveredPointNum(
        covered, viewpoint_array_ind, true);
    if (covered_point_num >= parameters_.kMinAddPointNum) {
      reused_viewpoint_indices.push_back(
          viewpoint_manager_->GetViewPointInd(viewpoint_array_ind));
    } else if (use_frontier_) {
      int covered_frontier_point_num =
          viewpoint_manager_->GetViewPointCoveredFrontierPointNum(
              frontier_covered, viewpoint_array_ind, true);
      if (covered_frontier_point_num >= parameters_.kMinAddFrontierPointNum) {
        reused_viewpoint_indices.push_back(
            viewpoint_manager_->GetViewPointInd(viewpoint_array_ind));
      }
    }
  }

  for (const auto &ind : reused_viewpoint_indices) {
    int viewpoint_array_ind = viewpoint_manager_->GetViewPointArrayInd(ind);
    pre_selected_viewpoint_array_indices.push_back(viewpoint_array_ind);
  }
  for (const auto &ind : navigation_viewpoint_indices) {
    int array_ind = viewpoint_manager_->GetViewPointArrayInd(ind);
    pre_selected_viewpoint_array_indices.push_back(array_ind);
  }

  // Update coverage
  for (auto &viewpoint_array_ind : pre_selected_viewpoint_array_indices) {
    // Update covered points and frontiers
    UpdateViewPointCoveredPoint(covered, viewpoint_array_ind, true);
    if (use_frontier_) {
      UpdateViewPointCoveredFrontierPoint(frontier_covered, viewpoint_array_ind,
                                          true);
    }
  }

  // Enqueue candidate viewpoints
  std::vector<std::pair<int, int>> queue;
  std::vector<std::pair<int, int>> frontier_queue;
  EnqueueViewpointCandidates(queue, frontier_queue, covered, frontier_covered,
                             pre_selected_viewpoint_array_indices);

  viewpoint_sampling_timer.Stop(false, kRuntimeUnit);
  viewpoint_sampling_runtime_ +=
      viewpoint_sampling_timer.GetDuration(kRuntimeUnit);
  std::vector<int> ordered_viewpoint_indices;  // 存储有序的视点索引
  if (!queue.empty() && queue[0].first > parameters_.kMinAddPointNum) {  // 如果队列不为空且第一个视点覆盖的点数超过最小阈值
    double min_path_length = DBL_MAX;  // 初始化最小路径长度为最大值
    for (int itr = 0; itr < parameters_.kLocalPathOptimizationItrMax; itr++) {  // 循环优化局部路径
      std::vector<int> selected_viewpoint_indices_itr;  // 存储当前迭代选择的视点索引

      // Select from the queue
      misc_utils_ns::Timer select_viewpoint_timer("select viewpoints");  // 创建选择视点的计时器
      select_viewpoint_timer.Start();  // 开始计时
      SelectViewPoint(queue, covered, selected_viewpoint_indices_itr, false);  // 从队列中选择视点
      SelectViewPointFromFrontierQueue(frontier_queue, frontier_covered,  // 从边界队列中选择视点
                                       selected_viewpoint_indices_itr);

      // Add viewpoints from last planning cycle
      for (const auto &ind : reused_viewpoint_indices) {  // 添加上一个规划周期的视点
        selected_viewpoint_indices_itr.push_back(ind);  // 将视点添加到当前选择列表
      }



      // Add viewpoints for navigation
      for (const auto &ind : navigation_viewpoint_indices) {  // 添加导航用的视点
        selected_viewpoint_indices_itr.push_back(ind);  // 将导航视点添加到选择列表
      }

      misc_utils_ns::UniquifyIntVector(selected_viewpoint_indices_itr);  // 去除重复的视点索引

      select_viewpoint_timer.Stop(false, kRuntimeUnit);  // 停止计时
      viewpoint_sampling_runtime_ +=  // 累加视点采样运行时间
          select_viewpoint_timer.GetDuration(kRuntimeUnit);

      // Solve the TSP problem
      exploration_path_ns::ExplorationPath local_path_itr;  // 创建局部路径对象
      local_path_itr =  // 求解TSP问题获取路径
          SolveTSP(selected_viewpoint_indices_itr, ordered_viewpoint_indices);

      double path_length = local_path_itr.GetLength();  // 获取路径长度
      if (!local_path_itr.nodes_.empty() && path_length < min_path_length)  // 如果找到更短的有效路径

      {
        min_path_length = path_length;  // 更新最小路径长度
        local_path = local_path_itr;  // 更新局部路径
        last_selected_viewpoint_indices_ = ordered_viewpoint_indices;  // 更新最后选择的视点索引
      }
    }
  } else {  // 如果队列为空或第一个视点覆盖点数不足

    std::cout << "Entering else branch - queue empty or insufficient coverage" << std::endl;
    if (queue.empty()) {
        std::cout << "Queue为空" << std::endl;
    } else {
        std::cout << "第一个视点（最优的视点）的覆盖点数为: " << queue[0].first 
                  << " (required: " << parameters_.kMinAddPointNum << ")" << std::endl;
    }


    misc_utils_ns::Timer select_viewpoint_timer("viewpoint sampling");  // 创建视点采样计时器
    select_viewpoint_timer.Start();  // 开始计时

    // std::cout << "entering tsp routine" << std::endl;
    std::vector<int> selected_viewpoint_indices_itr;  // 存储选择的视点索引

    // Add viewpoints from last planning cycle
    for (const auto &ind : reused_viewpoint_indices) {  // 添加上一个规划周期的视点
      selected_viewpoint_indices_itr.push_back(ind);  // 将视点添加到选择列表
    }

    std::cout << "添加上一个规划周期的视点后，视点列表大小为: " 
      << selected_viewpoint_indices_itr.size() << " viewpoints" << std::endl;   // 调试代码

    SelectViewPointFromFrontierQueue(frontier_queue, frontier_covered,  // 从边界队列中选择视点
                                     selected_viewpoint_indices_itr);

    std::cout << "从边界队列中选择视点后，视点列表大小为: " 
              << selected_viewpoint_indices_itr.size() << " viewpoints" << std::endl  // 调试代码


    if (selected_viewpoint_indices_itr.empty()) {  // 如果没有选择任何视点
      local_coverage_complete_ = true;  // 标记局部覆盖完成
    }

    // Add viewpoints for navigation
    for (const auto &ind : navigation_viewpoint_indices) {  // 添加导航用的视点
      selected_viewpoint_indices_itr.push_back(ind);  // 将导航视点添加到选择列表
    }

    misc_utils_ns::UniquifyIntVector(selected_viewpoint_indices_itr);  // 去除重复的视点索引

    select_viewpoint_timer.Stop(false, kRuntimeUnit);  // 停止视点采样计时
    viewpoint_sampling_runtime_ +=  // 累加视点采样运行时间
        select_viewpoint_timer.GetDuration(kRuntimeUnit);

    local_path =  // 求解TSP问题获取路径
        SolveTSP(selected_viewpoint_indices_itr, ordered_viewpoint_indices);

    last_selected_viewpoint_indices_ = ordered_viewpoint_indices;  // 更新最后选择的视点索引
  }

  last_selected_viewpoint_array_indices_.clear();
  for (const auto &ind : last_selected_viewpoint_indices_) {
    int array_ind = viewpoint_manager_->GetViewPointArrayInd(ind);
    last_selected_viewpoint_array_indices_.push_back(array_ind);
  }

  int viewpoint_num = viewpoint_manager_->GetViewPointNum();
  for (int i = 0; i < viewpoint_num; i++) {
    viewpoint_manager_->SetViewPointSelected(i, false, true);
  }
  for (const auto &viewpoint_index : last_selected_viewpoint_indices_) {
    if (viewpoint_index != robot_viewpoint_ind_ &&
        viewpoint_index != start_viewpoint_ind_ &&
        viewpoint_index != end_viewpoint_ind_ &&
        viewpoint_index != lookahead_viewpoint_ind_) {
      viewpoint_manager_->SetViewPointSelected(viewpoint_index, true);
    }
  }
  return local_path;
}

void LocalCoveragePlanner::GetSelectedViewPointVisCloud(
    pcl::PointCloud<pcl::PointXYZI>::Ptr &cloud) {
  cloud->clear();
  for (const auto &viewpoint_index : last_selected_viewpoint_indices_) {
    geometry_msgs::msg::Point position =
        viewpoint_manager_->GetViewPointPosition(viewpoint_index);
    pcl::PointXYZI point;
    point.x = position.x;
    point.y = position.y;
    point.z = position.z;
    if (viewpoint_index == robot_viewpoint_ind_) {
      point.intensity = 0.0;
    } else if (viewpoint_index == start_viewpoint_ind_) {
      point.intensity = 1.0;
    } else if (viewpoint_index == end_viewpoint_ind_) {
      point.intensity = 2.0;
    } else {
      point.intensity = 3.0;
    }
    cloud->points.push_back(point);
  }
}

} // namespace local_coverage_planner_ns