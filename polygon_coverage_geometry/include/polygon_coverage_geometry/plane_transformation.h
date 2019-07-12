#ifndef POLYGON_COVERAGE_GEOMETRY_PLANE_TRANSFORMATION_H_
#define POLYGON_COVERAGE_GEOMETRY_PLANE_TRANSFORMATION_H_

#include <vector>

namespace polygon_coverage_planning {
// Given a plane in 3D, transform points on it to 2D and back preserving
// distances.
// WARNING: Inexact operation!
// https://stackoverflow.com/questions/8780646/mapping-coordinates-from-plane-given-by-normal-vector-to-xy-plane
template <class Kernel>
class PlaneTransformation {
  typedef typename Kernel::Point_2 Point_2;
  typedef typename Kernel::Point_3 Point_3;
  typedef typename Kernel::Plane_3 Plane_3;
  typedef typename Kernel::Vector_3 Vector_3;
  typedef typename Kernel::FT FT;

 public:
  PlaneTransformation(const Plane_3& plane);
  PlaneTransformation() : PlaneTransformation(Plane_3(0.0, 0.0, 1.0, 0.0)){};
  Point_2 to2d(const Point_3& p_3) const;
  std::vector<Point_2> to2d(const std::vector<Point_3>& p_3) const;
  Point_3 to3d(const Point_2& p_2) const;
  std::vector<Point_3> to3d(const std::vector<Point_2>& p_2) const;
  Point_3 to3dOnPlane(const Point_2& p_2) const;
  inline Plane_3 getPlane() const { return plane_; }

 private:
  enum ProjectionType { kXY, kYZ, kZX };
  Vector_3 normalize(const Vector_3& v) const;
  Vector_3 b_1_;
  Vector_3 b_2_;
  Vector_3 p_0_;
  Plane_3 plane_;
  ProjectionType projection_type_;
  bool is_edge_case_;
};
}  // namespace polygon_coverage_planning

#include "polygon_coverage_geometry/impl/plane_transformation_impl.h"

#endif  // POLYGON_COVERAGE_GEOMETRY_PLANE_TRANSFORMATION_H_
