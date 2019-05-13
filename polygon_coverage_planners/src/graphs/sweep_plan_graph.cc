#include "mav_2d_coverage_planning/graphs/sweep_plan_graph.h"

#include <glog/logging.h>

#include <mav_coverage_planning_comm/timing.h>

#include <mav_coverage_graph_solvers/gk_ma.h>
#include "mav_2d_coverage_planning/geometry/sweep.h"

namespace polygon_coverage_planning {
namespace sweep_plan_graph {

bool NodeProperty::isNonOptimal(
    const visibility_graph::VisibilityGraph& visibility_graph,
    const std::vector<NodeProperty>& node_properties,
    const PathCostFunctionType& cost_function) const {
  if (waypoints.empty()) {
    LOG(WARNING) << "Node does not have waypoints.";
    return false;
  }

  for (const NodeProperty& node_property : node_properties) {
    if (node_property.waypoints.empty()) {
      LOG(WARNING) << "Comparison node does not have waypoints.";
      continue;
    }
    if (node_property.cluster == cluster) {
      std::vector<Point_2> path_front_front, path_back_back;
      if (!visibility_graph.solve(
              waypoints.front(), visibility_polygons.front(),
              node_property.waypoints.front(),
              node_property.visibility_polygons.front(), &path_front_front) ||
          !visibility_graph.solve(node_property.waypoints.back(),
                                  node_property.visibility_polygons.back(),
                                  waypoints.back(), visibility_polygons.back(),
                                  &path_back_back)) {
        continue;
      }
      if (cost_function(path_front_front) + node_property.cost +
              cost_function(path_back_back) <
          cost) {
        return true;
      }
    }
  }
  return false;
}

bool SweepPlanGraph::create() {
  clear();
  // Compute decomposition.
  computeDecomposition();
  // Offset neighboring cells.
  // Create

  size_t num_sweep_plans = 0;
  // Create sweep plans for each cluster.
  for (size_t cluster = 0; cluster < polygon_clusters_.size(); ++cluster) {
    // Compute all cluster sweeps.
    std::vector<std::vector<Point_2>> cluster_sweeps;
    timing::Timer timer_line_sweeps("line_sweeps");
    if (sweep_single_direction_) {
      Direction_2 best_dir;
      polygon_clusters_[cluster].findMinAltitude(polygon_clusters_[cluster],
                                                 &best_dir);
      visibility_graph::VisibilityGraph vis_graph(polygon_clusters_[cluster]);
      const Polygon_2& poly =
          polygon_clusters_[cluster].getPolygon().outer_boundary();
      cluster_sweeps.resize(1);
      if (!computeSweep(poly, vis_graph, sweep_distance_, best_dir, true,
                        &cluster_sweeps.front())) {
        LOG(ERROR) << "Cannot compute single sweep for cluster: " << cluster;
        return false;
      }
    } else {
      if (!computeAllSweeps(polygon_clusters_[cluster], sweep_distance_,
                            &cluster_sweeps)) {
        LOG(ERROR) << "Cannot create all sweep plans for cluster " << cluster;
        return false;
      }
    }
    num_sweep_plans += cluster_sweeps.size();
    timer_line_sweeps.Stop();

    // Create node properties.
    timing::Timer timer_node_creation("node_creation");
    std::vector<NodeProperty> node_properties;
    node_properties.resize(cluster_sweeps.size());
    for (size_t i = 0; i < node_properties.size(); ++i) {
      NodeProperty node;
      if (!createNodeProperty(cluster, &cluster_sweeps[i], &node)) {
        return false;
      }
      node_properties[i] = node;
    }
    timer_node_creation.Stop();

    timing::Timer timer_pruning("pruning");
    // Prune nodes that are definitely not optimal.
    std::vector<NodeProperty>::iterator new_end = std::remove_if(
        node_properties.begin(), node_properties.end(),
        [node_properties, this](const NodeProperty& node_property) {
          return node_property.isNonOptimal(visibility_graph_, node_properties,
                                            cost_function_);
        });
    node_properties.erase(new_end, node_properties.end());
    timer_pruning.Stop();

    // For each remaining sweep create a node.
    timing::Timer timer_edge_creation("edge_creation");
    for (const NodeProperty& node_property : node_properties) {
      if (!addNode(node_property)) {
        return false;
      }
    }
    timer_edge_creation.Stop();
  }

  LOG(INFO) << "Created sweep plan graph with " << graph_.size()
            << " nodes and " << edge_properties_.size() << " edges.";
  LOG(INFO) << "Pruned " << num_sweep_plans - graph_.size() << " nodes.";
  is_created_ = true;
  return true;
}

bool SweepPlanGraph::computeDecomposition() {
  // Create decomposition.
  timing::Timer timer_decom("decomposition");
  switch (settings_.decomposition_type) {
    case DecompositionType::kBoustrophedeon: {
      if (!computeBestBCDFromPolygonWithHoles(settings_.polygon,
                                              &polygon_clusters_)) {
        ROS_ERROR_STREAM("Cannot compute boustrophedeon decomposition.");
        return false;
      } else {
        ROS_INFO_STREAM(
            "Successfully created boustrophedeon decomposition with "
            << polygon_clusters_.size() << " polygon(s).");
      }
      break;
    }
    case DecompositionType::kTrapezoidal: {
      if (!computeBestTrapezoidalDecompositionFromPolygonWithHoles(
              settings_.polygon, &polygon_clusters_)) {
        ROS_ERROR_STREAM("Cannot compute trapezoidal decomposition.");
        return false;
      } else {
        ROS_INFO_STREAM("Successfully created trapezoidal decomposition with "
                        << polygon_clusters_.size() << " polygon(s).");
      }
      break;
    }
    default: {
      ROS_ERROR_STREAM("No valid decomposition type set.");
      return false;
      break;
    }
  }
  timer_decom.Stop();

  // Compute adjacency.
  timing::Timer timer_poly_adj("polygon_adjacency");
  if (settings_.offset_polygons && !updateDecompositionAdjacency()) {
    ROS_ERROR_STREAM("Decomposition not fully connected.");
    return false;
  }
  timer_poly_adj.Stop();

  timing::Timer timer_poly_offset("poly_offset");
  if (!offsetDecomposition()) {
    LOG(ERROR) << "Failed to offset rectangular decomposition.";
    is_initialized_ = false;
  }
  timer_poly_offset.Stop();


}

bool SweepPlanGraph::updateDecompositionAdjacency() {
  for (size_t i = 0; i < polygon_clusters_.size() - 1; ++i) {
    for (size_t j = i + 1; j < polygon_clusters_.size(); ++j) {
      PolygonWithHoles joined;
      if (CGAL::join(polygon_clusters_[i].getPolygon(),
                     polygon_clusters_[j].getPolygon(), joined)) {
        decomposition_adjacency_[i].emplace(j);
        decomposition_adjacency_[j].emplace(i);
      }
    }
  }

  // Check connectivity.
  if (polygon_clusters_.size() == 1) return decomposition_adjacency_.count(0) == 0;
  for (size_t i = 0; i < polygon_clusters_.size(); ++i) {
    if (decomposition_adjacency_.find(i) == decomposition_adjacency_.end()) {
      return false;
    }
  }
  return true;
}


bool SweepPlanGraph::offsetDecomposition() {
  // Find overlapping edges.
  std::vector<Polygon_2> offsetted_decomposition = polygon_clusters_;
  std::vector<Segment_2> offsetted_segments;
  for (size_t i = 0; i < polygon_clusters_.size(); ++i) {
    for (std::set<size_t>::iterator it = decomposition_adjacency_[i].begin();
         it != decomposition_adjacency_[i].end(); it++) {
      const Polygon_2& cell = polygon_clusters_[i].getPolygon().outer_boundary();
      const size_t num_edges_cell = cell.size();
      // If they do not touch anymore, skip.
      PolygonWithHoles joined;
      if (!CGAL::join(polygon_clusters_[i].getPolygon(),
                      polygon_clusters_[*it].getPolygon(), joined))
        continue;

      const Polygon_2& neighbor =
          polygon_clusters_[*it].getPolygon().outer_boundary();
      const size_t num_edges_neighbor = neighbor.size();
      for (size_t cell_e = 0; cell_e < num_edges_cell; ++cell_e) {
        if (std::find(offsetted_segments.begin(), offsetted_segments.end(),
                      cell.edge(cell_e)) != offsetted_segments.end())
          continue;  // Already offsetted this segment.
        for (size_t neighbor_e = 0; neighbor_e < num_edges_neighbor;
             ++neighbor_e) {
          if (std::find(offsetted_segments.begin(), offsetted_segments.end(),
                        neighbor.edge(neighbor_e)) != offsetted_segments.end())
            continue;  // Already offsetted this segment.
          // If segments intersect, offset polygon.
          CGAL::cpp11::result_of<Intersect_2(Segment_2, Segment_2)>::type
              result = CGAL::intersection(cell.edge(cell_e),
                                          neighbor.edge(neighbor_e));
          if (result) {
            if (const Segment_2* s = boost::get<Segment_2>(&*result)) {
              if (*s == cell.edge(cell_e) ||
                  s->opposite() == cell.edge(cell_e)) {
                Polygon offset_cell;
                polygon_clusters_[i].offsetEdgeWithRadialOffset(
                    cell_e, settings_.sensor_model->getSweepDistance(),
                    &offset_cell);
                Polygon intersected_offset;
                if (!offsetted_decomposition[i].intersect(offset_cell,
                                                          &intersected_offset))
                  return false;
                offsetted_decomposition[i] = intersected_offset;
                offsetted_segments.push_back(*s);
                offsetted_segments.push_back(s->opposite());
              } else if (*s == neighbor.edge(neighbor_e) ||
                         s->opposite() == neighbor.edge(neighbor_e)) {
                Polygon offset_neighbor;
                polygon_clusters_[*it].offsetEdgeWithRadialOffset(
                    neighbor_e, settings_.sensor_model->getSweepDistance(),
                    &offset_neighbor);
                Polygon intersected_offset;
                if (!offsetted_decomposition[*it].intersect(
                        offset_neighbor, &intersected_offset))
                  return false;
                offsetted_decomposition[*it] = intersected_offset;
                offsetted_segments.push_back(*s);
                offsetted_segments.push_back(s->opposite());
              } else {
                DLOG(INFO) << "Segment intersection but not identical.";
              }
            } else {
              DLOG(INFO) << "Only point intersection... "
                         << *boost::get<Point_2>(&*result);
            }
          }
        }
      }
    }
  }
  polygon_clusters_ = offsetted_decomposition;

  return true;
}

bool SweepPlanGraph::getClusters(
    std::vector<std::vector<int>>* clusters) const {
  CHECK_NOTNULL(clusters);
  clusters->clear();

  std::set<size_t> cluster_set;
  for (size_t i = 0; i < graph_.size(); ++i) {
    const NodeProperty* node = getNodeProperty(i);
    if (node == nullptr) {
      return false;  // Node property does not exist.
    }
    cluster_set.insert(node->cluster);
  }

  std::set<size_t> expected_clusters;
  for (size_t i = 0; i < cluster_set.size(); ++i) {
    expected_clusters.insert(i);
  }
  if (cluster_set != expected_clusters) {
    return false;  // Clusters not enumerated [0 .. n-1].
  }                // HERE

  clusters->resize(cluster_set.size());
  for (size_t i = 0; i < clusters->size(); ++i) {
    for (size_t j = 0; j < graph_.size(); ++j) {
      const NodeProperty* node = getNodeProperty(j);
      if (node == nullptr) {
        return false;  // Node property does not exist.
      }
      if (node->cluster == i) {
        (*clusters)[i].push_back(static_cast<int>(j));
      }
    }
  }

  return true;
}

bool SweepPlanGraph::createNodeProperty(size_t cluster,
                                        std::vector<Point_2>* waypoints,
                                        NodeProperty* node) const {
  CHECK_NOTNULL(waypoints);
  CHECK_NOTNULL(node);

  std::vector<Polygon> visibility_polygons;
  if (!computeStartAndGoalVisibility(visibility_graph_.getPolygon(), waypoints,
                                     &visibility_polygons)) {
    LOG(ERROR) << "Cannot compute start and goal visibility graph.";
    return false;
  }

  *node =
      NodeProperty(*waypoints, cost_function_, cluster, visibility_polygons);

  return true;
}

bool SweepPlanGraph::addEdges() {
  if (graph_.empty()) {
    LOG(ERROR) << "Cannot add edges to an empty graph.";
    return false;
  }

  const size_t new_id = graph_.size() - 1;
  for (size_t adj_id = 0; adj_id < new_id; ++adj_id) {
    EdgeId forwards_edge_id(new_id, adj_id);
    EdgeProperty edge_property;
    if (isConnected(forwards_edge_id) &&
        computeEdge(forwards_edge_id, &edge_property)) {
      double cost = -1.0;
      if (!computeCost(forwards_edge_id, edge_property, &cost) ||
          !addEdge(forwards_edge_id, edge_property, cost)) {
        return false;
      }
    }
    EdgeId backwards_edge_id(adj_id, new_id);
    if (isConnected(backwards_edge_id) &&
        computeEdge(backwards_edge_id, &edge_property)) {
      double cost = -1.0;
      if (!computeCost(backwards_edge_id, edge_property, &cost) ||
          !addEdge(backwards_edge_id, edge_property, cost)) {
        return false;
      }
    }
  }

  return true;
}

bool SweepPlanGraph::computeEdge(const EdgeId& edge_id,
                                 EdgeProperty* edge_property) const {
  CHECK_NOTNULL(edge_property);

  // Access node properties:
  const NodeProperty* from_node_property = getNodeProperty(edge_id.first);
  const NodeProperty* to_node_property = getNodeProperty(edge_id.second);
  if (from_node_property == nullptr || to_node_property == nullptr) {
    return false;
  }

  // Calculate shortest path.
  if (from_node_property->waypoints.empty() ||
      to_node_property->waypoints.empty()) {
    LOG(ERROR) << "Waypoints in node property are empty.";
    return false;
  }

  std::vector<Point_2> shortest_path;
  if (!visibility_graph_.solve(from_node_property->waypoints.back(),
                               from_node_property->visibility_polygons.back(),
                               to_node_property->waypoints.front(),
                               to_node_property->visibility_polygons.front(),
                               &shortest_path)) {
    LOG(ERROR) << "Cannot compute shortest path from "
               << from_node_property->waypoints.back() << " to "
               << to_node_property->waypoints.front();
    return false;
  }

  *edge_property = EdgeProperty(shortest_path, cost_function_);

  return true;
}

bool SweepPlanGraph::computeCost(const EdgeId& edge_id,
                                 const EdgeProperty& edge_property,
                                 double* cost) const {
  // cost = from_sweep_cost + cost(from_end, to_start)
  const NodeProperty* from_node_property = getNodeProperty(edge_id.first);
  if (from_node_property == nullptr) {
    return false;
  }
  *cost = from_node_property->cost + edge_property.cost;
  return true;
}

bool SweepPlanGraph::isConnected(const EdgeId& edge_id) const {
  // Access node properties:
  const NodeProperty* from_node_property = getNodeProperty(edge_id.first);
  const NodeProperty* to_node_property = getNodeProperty(edge_id.second);
  if (from_node_property == nullptr || to_node_property == nullptr) {
    return false;
  }

  return from_node_property->cluster !=
             to_node_property->cluster    // Different clusters.
         && edge_id.first != goal_idx_    // No connection from goal.
         && edge_id.second != start_idx_  // No connection to start.
         && !(edge_id.first == start_idx_ &&
              edge_id.second ==
                  goal_idx_);  // No direct connection between start and goal.
}

bool SweepPlanGraph::solve(const Point_2& start, const Point_2& goal,
                           std::vector<Point_2>* waypoints) const {
  CHECK_NOTNULL(waypoints);
  waypoints->clear();

  if (!is_created_) {
    LOG(ERROR) << "Graph not created.";
    return false;
  }

  // Create temporary copies to add start and goal.
  SweepPlanGraph temp_gtsp_graph = *this;

  NodeProperty start_node, goal_node;
  if (!createNodeProperty(polygon_clusters_.size(), start, &start_node) ||
      !createNodeProperty(polygon_clusters_.size() + 1, goal, &goal_node)) {
    return false;
  }

  if (!temp_gtsp_graph.addStartNode(start_node) ||
      !temp_gtsp_graph.addGoalNode(goal_node)) {
    LOG(ERROR) << "Cannot add start and goal.";
    return false;
  }
  const size_t goal_idx = temp_gtsp_graph.size() - 1;
  const size_t start_idx = temp_gtsp_graph.size() - 2;

  // Solve using GK MA.
  std::vector<std::vector<int>> m = temp_gtsp_graph.getAdjacencyMatrix();
  std::vector<std::vector<int>> clusters;
  if (!temp_gtsp_graph.getClusters(&clusters)) {
    LOG(ERROR) << "Cannot get clusters.";
    return false;
  }
  gk_ma::Task task(m, clusters);
  gk_ma::GkMa& solver = gk_ma::GkMa::getInstance();
  solver.setSolver(task);

  LOG(INFO) << "Start solving GTSP";
  if (!solver.solve()) {
    LOG(ERROR) << "GkMa solution failed.";
    return false;
  }
  LOG(INFO) << "Finished solving GTSP";
  std::vector<int> solution_int = solver.getSolution();
  Solution solution(solution_int.size());
  std::copy(solution_int.begin(), solution_int.end(), solution.begin());

  // Sort solution such that start node is at begin.
  Solution::iterator start_it =
      std::find(solution.begin(), solution.end(), start_idx);
  if (start_it == solution.end()) {
    LOG(ERROR) << "Cannot find start node in solution.";
    return false;
  }
  std::rotate(solution.begin(), start_it, solution.end());
  if (solution.back() != goal_idx) {
    LOG(ERROR) << "Goal node is not at back of solution.";
    return false;
  }

  if (!temp_gtsp_graph.getWaypoints(solution, waypoints)) {
    LOG(ERROR) << "Cannot recover waypoints.";
    return false;
  }

  return true;
}

bool SweepPlanGraph::getWaypoints(const Solution& solution,
                                  std::vector<Point_2>* waypoints) const {
  CHECK_NOTNULL(waypoints);
  waypoints->clear();

  for (size_t i = 0; i < solution.size() - 1; ++i) {
    const EdgeId edge_id(solution[i], solution[i + 1]);

    // Add sweep plan / start / goal waypoints.
    const NodeProperty* node_property = getNodeProperty(edge_id.first);
    if (node_property == nullptr) {
      return false;
    }
    waypoints->insert(waypoints->end(), node_property->waypoints.begin(),
                      node_property->waypoints.end());

    // Add shortest path.
    const EdgeProperty* edge_property = getEdgeProperty(edge_id);
    if (edge_property == nullptr) {
      return false;
    }
    // Crop first and last waypoint as these are included in sweep plan.
    waypoints->insert(waypoints->end(), edge_property->waypoints.begin() + 1,
                      edge_property->waypoints.end() - 1);
    // Add last waypoint.
    if (i == solution.size() - 2) {
      waypoints->insert(waypoints->end(), edge_property->waypoints.end() - 1,
                        edge_property->waypoints.end());
    }
  }
  return true;
}

bool SweepPlanGraph::computeStartAndGoalVisibility(
    const Polygon& polygon, std::vector<Point_2>* sweep,
    std::vector<Polygon>* visibility_polygons) const {
  CHECK_NOTNULL(sweep);
  CHECK_NOTNULL(visibility_polygons);
  visibility_polygons->resize(2);

  if (sweep->front() == sweep->back()) {
    visibility_polygons->resize(1);
    if (!computeVisibility(polygon, &sweep->front(),
                           &visibility_polygons->front())) {
      return false;
    } else {
      sweep->back() = sweep->front();
      return true;
    }
  } else {
    visibility_polygons->resize(2);
    return computeVisibility(polygon, &sweep->front(),
                             &visibility_polygons->front()) &&
           computeVisibility(polygon, &sweep->back(),
                             &visibility_polygons->back());
  }
}

bool SweepPlanGraph::computeVisibility(const Polygon& polygon, Point_2* vertex,
                                       Polygon* visibility_polygon) const {
  CHECK_NOTNULL(vertex);
  CHECK_NOTNULL(visibility_polygon);

  *vertex = polygon.pointInPolygon(*vertex)
                ? *vertex
                : polygon.projectPointOnHull(*vertex);
  return polygon.computeVisibilityPolygon(*vertex, visibility_polygon);
}

}  // namespace sweep_plan_graph
}  // namespace polygon_coverage_planning
