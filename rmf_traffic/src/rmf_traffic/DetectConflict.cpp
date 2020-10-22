/*
 * Copyright (C) 2019 Open Source Robotics Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
*/

#include "geometry/ShapeInternal.hpp"
#include "DetectConflictInternal.hpp"
#include "ProfileInternal.hpp"
#include "Spline.hpp"
#include "StaticMotion.hpp"

#include "DetectConflictInternal.hpp"

#include <fcl/narrowphase/continuous_collision.h>
#include <fcl/math/motion/spline_motion.h>
#include <fcl/narrowphase/collision.h>

#include <unordered_map>

namespace rmf_traffic {

//==============================================================================
class invalid_trajectory_error::Implementation
{
public:

  std::string what;

  static invalid_trajectory_error make_segment_num_error(
    std::size_t num_segments,
    std::size_t line,
    std::string function)
  {
    invalid_trajectory_error error;
    error._pimpl->what = std::string()
      + "[rmf_traffic::invalid_trajectory_error] Attempted to check a "
      + "conflict with a Trajectory that has [" + std::to_string(num_segments)
      + "] segments. This is not supported. Trajectories must have at least "
      + "2 segments to check them for conflicts. "
        + function + ":" + std::to_string(line);
    return error;
  }

  static invalid_trajectory_error make_missing_shape_error(
    const Time time)
  {
    invalid_trajectory_error error;
    error._pimpl->what = std::string()
      + "[rmf_traffic::invalid_trajectory_error] Attempting to check a "
      + "conflict with a Trajectory that has no shape specified for the "
      + "profile of its waypoint at time ["
      + std::to_string(time.time_since_epoch().count())
      + "ns]. This is not supported.";

    return error;
  }
};

//==============================================================================
const char* invalid_trajectory_error::what() const noexcept
{
  return _pimpl->what.c_str();
}

//==============================================================================
invalid_trajectory_error::invalid_trajectory_error()
: _pimpl(rmf_utils::make_impl<Implementation>())
{
  // This constructor is a no-op, but we'll keep a definition for it in case we
  // need it in the future. Allowing the default constructor to be inferred
  // could cause issues if we want to change the implementation of this
  // exception in the future, like if we want to add more information to the
  // error message output.
}

namespace {
//==============================================================================
bool have_time_overlap(
  const Trajectory& trajectory_a,
  const Trajectory& trajectory_b)
{
  const auto* t_a0 = trajectory_a.start_time();
  const auto* t_bf = trajectory_b.finish_time();

  // Neither of these can be null, because both trajectories should have at
  // least two elements.
  assert(t_a0 != nullptr);
  assert(t_bf != nullptr);

  if (*t_bf < *t_a0)
  {
    // If Trajectory `b` finishes before Trajectory `a` starts, then there
    // cannot be any conflict.
    return false;
  }

  const auto* t_b0 = trajectory_b.start_time();
  const auto* t_af = trajectory_a.finish_time();

  // Neither of these can be null, because both trajectories should have at
  // least two elements.
  assert(t_b0 != nullptr);
  assert(t_af != nullptr);

  if (*t_af < *t_b0)
  {
    // If Trajectory `a` finished before Trajectory `b` starts, then there
    // cannot be any conflict.
    return false;
  }

  return true;
}

//==============================================================================
std::tuple<Trajectory::const_iterator, Trajectory::const_iterator>
get_initial_iterators(
  const Trajectory& trajectory_a,
  const Trajectory& trajectory_b)
{
  const Time& t_a0 = *trajectory_a.start_time();
  const Time& t_b0 = *trajectory_b.start_time();

  Trajectory::const_iterator a_it;
  Trajectory::const_iterator b_it;

  if (t_a0 < t_b0)
  {
    // Trajectory `a` starts first, so we begin evaluating at the time
    // that `b` begins
    a_it = trajectory_a.find(t_b0);
    b_it = ++trajectory_b.begin();
  }
  else if (t_b0 < t_a0)
  {
    // Trajectory `b` starts first, so we begin evaluating at the time
    // that `a` begins
    a_it = ++trajectory_a.begin();
    b_it = trajectory_b.find(t_a0);
  }
  else
  {
    // The Trajectories begin at the exact same time, so both will begin
    // from their start
    a_it = ++trajectory_a.begin();
    b_it = ++trajectory_b.begin();
  }

  return {a_it, b_it};
}

//==============================================================================
struct BoundingBox
{
  Eigen::Vector2d min;
  Eigen::Vector2d max;
};

//==============================================================================
struct BoundingProfile
{
  BoundingBox footprint;
  BoundingBox vicinity;
};

//==============================================================================
double evaluate_spline(
  const Eigen::Vector4d& coeffs,
  const double t)
{
  // Assume time is parameterized [0,1]
  return coeffs[3] * t * t * t
    + coeffs[2] * t * t
    + coeffs[1] * t
    + coeffs[0];
}

//==============================================================================
std::array<double, 2> get_local_extrema(
  const Eigen::Vector4d& coeffs)
{
  std::vector<double> extrema_candidates;
  // Store boundary values as potential extrema
  extrema_candidates.emplace_back(evaluate_spline(coeffs, 0));
  extrema_candidates.emplace_back(evaluate_spline(coeffs, 1));

  // When derivate of spline motion is not quadratic
  if (std::abs(coeffs[3]) < 1e-12)
  {
    if (std::abs(coeffs[2]) > 1e-12)
    {
      double t = -coeffs[1] / (2 * coeffs[2]);
      extrema_candidates.emplace_back(evaluate_spline(coeffs, t));
    }
  }
  else
  {
    // Calculate the discriminant otherwise
    const double D = (4 * pow(coeffs[2], 2) - 12 * coeffs[3] * coeffs[1]);

    if (std::abs(D) < 1e-4)
    {
      const double t = (-2 * coeffs[2]) / (6 * coeffs[3]);
      const double extrema = evaluate_spline(coeffs, t);
      extrema_candidates.emplace_back(extrema);
    }
    else if (D < 0)
    {
      // If D is negative, then the local extrema would be imaginary. This will
      // happen for splines that have no local extrema. When that happens, the
      // endpoints of the spline are the only extrema.
    }
    else
    {
      const double t1 = ((-2 * coeffs[2]) + std::sqrt(D)) / (6 * coeffs[3]);
      const double t2 = ((-2 * coeffs[2]) - std::sqrt(D)) / (6 * coeffs[3]);

      extrema_candidates.emplace_back(evaluate_spline(coeffs, t1));
      extrema_candidates.emplace_back(evaluate_spline(coeffs, t2));
    }
  }

  std::array<double, 2> extrema;
  assert(!extrema_candidates.empty());
  extrema[0] = *std::min_element(
    extrema_candidates.begin(),
    extrema_candidates.end());
  extrema[1] = *std::max_element(
    extrema_candidates.begin(),
    extrema_candidates.end());

  return extrema;
}

//==============================================================================
BoundingBox get_bounding_box(const rmf_traffic::Spline& spline)
{
  auto params = spline.get_params();
  std::array<double, 2> extrema_x = get_local_extrema(params.coeffs[0]);
  std::array<double, 2> extrema_y = get_local_extrema(params.coeffs[1]);

  return BoundingBox{
    Eigen::Vector2d{extrema_x[0], extrema_y[0]},
    Eigen::Vector2d{extrema_x[1], extrema_y[1]}
  };
}

//==============================================================================
/// Create a bounding box which will never overlap with any other BoundingBox
BoundingBox void_box()
{
  constexpr double inf = std::numeric_limits<double>::infinity();
  return BoundingBox{
    Eigen::Vector2d{inf, inf},
    Eigen::Vector2d{-inf, -inf}
  };
}

//==============================================================================
BoundingBox adjust_bounding_box(
  const BoundingBox& input,
  const double value)
{
  BoundingBox box = input;
  box.min -= Eigen::Vector2d{value, value};
  box.max += Eigen::Vector2d{value, value};

  return box;
}

//==============================================================================
BoundingProfile get_bounding_profile(
  const rmf_traffic::Spline& spline,
  const Profile::Implementation& profile)
{
  BoundingBox base_box = get_bounding_box(spline);

  const auto& footprint = profile.footprint;
  const auto f_box = footprint ?
    adjust_bounding_box(base_box, footprint->get_characteristic_length()) :
    void_box();

  const auto& vicinity = profile.vicinity;
  const auto v_box = vicinity ?
    adjust_bounding_box(base_box, vicinity->get_characteristic_length()) :
    void_box();

  return BoundingProfile{f_box, v_box};
}

//==============================================================================
bool overlap(
  const BoundingBox& box_a,
  const BoundingBox& box_b)
{
  for (int i = 0; i < 2; ++i)
  {
    if (box_a.max[i] < box_b.min[i])
      return false;

    if (box_b.max[i] < box_a.min[i])
      return false;
  }

  return true;
}

//==============================================================================
std::shared_ptr<fcl::SplineMotion<double>> make_uninitialized_fcl_spline_motion()
{
  // This function is only necessary because SplineMotion does not provide a
  // default constructor, and we want to be able to instantiate one before
  // we have any paramters to provide to it.
  fcl::Matrix3d R;
  fcl::Vector3d T;

  // The constructor that we are using is a no-op (apparently it was declared,
  // but its definition is just `// TODO`, so we don't need to worry about
  // unintended consequences. If we update the version of FCL, this may change,
  // so I'm going to leave a FIXME tag here to keep us aware of that.
  return std::make_shared<fcl::SplineMotion<double>>(R, T, R, T);
}

//==============================================================================
fcl::ContinuousCollisionRequestd make_fcl_request()
{
  fcl::ContinuousCollisionRequestd request;
  request.ccd_solver_type = fcl::CCDC_CONSERVATIVE_ADVANCEMENT;
  request.gjk_solver_type = fcl::GST_LIBCCD;

  return request;
}

//==============================================================================
rmf_utils::optional<double> check_collision(
  const geometry::FinalConvexShape& shape_a,
  const std::shared_ptr<fcl::SplineMotion<double>>& motion_a,
  const geometry::FinalConvexShape& shape_b,
  const std::shared_ptr<fcl::SplineMotion<double>>& motion_b,
  const fcl::ContinuousCollisionRequestd& request)
{
  const auto obj_a = fcl::ContinuousCollisionObjectd(
    geometry::FinalConvexShape::Implementation::get_collision(shape_a),
    motion_a);

  const auto obj_b = fcl::ContinuousCollisionObjectd(
    geometry::FinalConvexShape::Implementation::get_collision(shape_b),
    motion_b);

  fcl::ContinuousCollisionResultd result;
  fcl::collide(&obj_a, &obj_b, request, result);

  if (result.is_collide)
    return result.time_of_contact;

  return rmf_utils::nullopt;
}


rmf_utils::optional<double> check_collision_fcl(
  const geometry::FinalConvexShape& shape_a,
  const std::shared_ptr<fcl::SplineMotion<double>>& motion_a,
  const geometry::FinalConvexShape& shape_b,
  const std::shared_ptr<fcl::SplineMotion<double>>& motion_b)
{
  return check_collision(shape_a, motion_a, shape_b, motion_b,
    make_fcl_request());
}

inline bool circle_ray_intersection(
  Eigen::Vector2d ray_dir, Eigen::Vector2d ray_origin,
  Eigen::Vector2d circle_center, double circle_radius, double& interp)
{
  Eigen::Vector2d ray_to_circle_center = circle_center - ray_origin;
  double ray_to_circle_sqdist = ray_to_circle_center.squaredNorm();
  double circle_radiussq = circle_radius * circle_radius;

  double ray_length = ray_dir.norm();
  if (ray_length <= 1e-07f)
    return ray_to_circle_sqdist <= circle_radiussq;

  Eigen::Vector2d ray_dir_normalized = ray_dir / ray_length;
  double projected_len = ray_to_circle_center.dot(ray_dir_normalized);
  
  // circle behind ray and outside of circle
  if (projected_len < 0.f && ray_to_circle_sqdist > circle_radiussq)
    return false;

  // check if circle collides with ray segment via checking minimum distance
  // from circle to ray
  double footlengthsq = ray_to_circle_sqdist - projected_len * projected_len;
  if (footlengthsq > circle_radiussq)
    return false;

  // we intersected (on a positive-infinate ray), compute the point of first peneration with circle by ray
  // reframe the problem as a line going through a circle and you're computing an inner triangle
  // with the point of intersection as a part of the triangle and circle_radiussq as the hypothenuse
  double r_sq = circle_radiussq - footlengthsq;
  double len_to_intersect = projected_len - sqrt(r_sq);

  //interPt = (ray_dir_normalized * len_to_intersect) + ray_origin;

  // time of intersection is intersected length / total length
  interp = len_to_intersect / ray_length;
  if (interp > 1.0 || interp < 0.0)
    return false;
  return true;
}

inline bool swept_circle_intersection(
      Eigen::Vector2d a_pt1, Eigen::Vector2d a_velstep,
      Eigen::Vector2d b_pt1, Eigen::Vector2d b_velstep,
      double radius_a, double radius_b,
      double& interp)
{
  //redefine the problem as ray with relative velocity - pillar with combined radius 
  Eigen::Vector2d ray_dir = a_velstep - b_velstep;
  Eigen::Vector2d ray_origin = a_pt1;

  Eigen::Vector2d pillar_center = b_pt1;
  double pillar_radius = radius_a + radius_b;

  return circle_ray_intersection(ray_dir, ray_origin, pillar_center, pillar_radius, interp);
}

rmf_utils::optional<double> check_collision_piecewise_sweep(
  double radius_a,
  const std::array<Eigen::Vector4d, 3>& motion_a,
  double radius_b,
  const std::array<Eigen::Vector4d, 3>& motion_b)
{
#if 0
  // re-parameterize the spline into arc-length, then do sweep circle tests
  double minradius_sq = (radius_a + radius_b) * (radius_a + radius_b);

  auto compute_spline_arclength = [](
    std::array<Eigen::Vector4d, 3> spline_coeff, double percent)
  {
    // Compute arc-length using Gaussian quadrature
    // see: https://medium.com/@all2one/how-to-compute-the-length-of-a-spline-e44f5f04c40
    // Compute a length of a spline segment by using 5-point Legendre-Gauss quadrature
    // https://en.wikipedia.org/wiki/Gaussian_quadrature

    struct LegendreGaussCoefficient
    {
      double abscissa;
      double weight;
    };

    static const LegendreGaussCoefficient legendre_gauss_coefficients[] =
    {
      { 0.0f, 0.5688889 },
      { -0.5384693, 0.47862867 },
      { 0.5384693, 0.47862867 },
      { -0.90617985, 0.23692688 },
      { 0.90617985, 0.23692688 }
    };

    //special case for straight line segments

    double length = 0.0f;
    for (const auto& lcoeff : legendre_gauss_coefficients)
    {
      // interval range switch from [-1, 1] to [0,1]
      double t = 0.5f * (1.f + lcoeff.abscissa);
      // do a weighted sum
      Eigen::Vector3d derivative = rmf_traffic::compute_velocity(spline_coeff, t);
      length += derivative.norm() * lcoeff.weight;
    }
    return 0.5f * length * percent;
  };

  auto arclength_param_to_spline_param = [compute_spline_arclength](
    std::array<Eigen::Vector4d, 3> spline_coeff, double arclengthparam, double arclength)
    -> double
  {
    //convert from s(arclength param) to t
    double s = arclengthparam / arclength;
    for (uint i = 0; i < 2; ++i)
    {
      Eigen::Vector3d derivative = rmf_traffic::compute_velocity(spline_coeff, s);
      double tangentmag = derivative.norm();
      if (tangentmag > 0.f)
      {
        s -= (compute_spline_arclength(spline_coeff, s) - arclengthparam) / tangentmag;
        s = s < 0.f ? 0.f : s;
        s = s > 1.f ? 1.f : s;
      }
    }
    return s;
  };

  double motion_a_length = compute_spline_arclength(motion_a, 1.0f);
  double motion_b_length = compute_spline_arclength(motion_b, 1.0f);

  uint steps = 3;

  double motion_a_len = 0.0f, motion_b_len = 0.0f;
  double motion_a_length_delta = motion_a_length / (double)steps;
  double motion_b_length_delta = motion_b_length / (double)steps;
  for (uint i = 0; i < steps; ++i)
  {
    // compute before and after step and 
    double a_t1 = arclength_param_to_spline_param(motion_a, motion_a_len, motion_a_length);
    double a_t2 = arclength_param_to_spline_param(motion_a, motion_a_len + motion_a_length_delta, motion_a_length);
    Eigen::Vector3d motion_a_seg_pt1 = rmf_traffic::compute_position(motion_a, a_t1);
    Eigen::Vector3d motion_a_seg_pt2 = rmf_traffic::compute_position(motion_a, a_t2);

    double b_t1 = arclength_param_to_spline_param(motion_b, motion_b_len, motion_b_length);
    double b_t2 = arclength_param_to_spline_param(motion_b, motion_b_len + motion_b_length_delta, motion_b_length);
    Eigen::Vector3d motion_b_seg_pt1 = rmf_traffic::compute_position(motion_b, b_t1);
    Eigen::Vector3d motion_b_seg_pt2 = rmf_traffic::compute_position(motion_b, b_t2);

    // do capsule-capsule between st lines on motion
    Eigen::Vector2d motion_a_seg_pt1_2d(motion_a_seg_pt1.x(), motion_a_seg_pt1.y());
    Eigen::Vector2d motion_b_seg_pt1_2d(motion_b_seg_pt1.x(), motion_b_seg_pt1.y());

    Eigen::Vector2d motion_a_seg_pt2_2d(motion_a_seg_pt2.x(), motion_a_seg_pt2.y());
    Eigen::Vector2d motion_b_seg_pt2_2d(motion_b_seg_pt2.x(), motion_b_seg_pt2.y());

    //LOG("a_t1: %g b_t1: %g", a_t1, b_t1);
    
    double interp = 0.0f;
    if (swept_circle_intersection(
      motion_a_seg_pt1_2d, motion_a_seg_pt2_2d - motion_a_seg_pt1_2d,
      motion_b_seg_pt1_2d, motion_b_seg_pt2_2d - motion_b_seg_pt1_2d,
      radius_a, radius_b, interp))
    {
      //printf("intersect! interp: %g\n", interp);
      double t_interp = a_t1 + interp * (a_t2 - a_t1);

      //t_interp = spline.ArcLengthParamToSplineParam(lparam, length);
      //t_interp = interp;
      return t_interp;
    }

    motion_a_len = motion_a_length_delta * ((double)i + 1.0);
    motion_b_len = motion_b_length_delta * ((double)i + 1.0);
  }
#endif 
  return rmf_utils::nullopt;
}

//==============================================================================
Profile::Implementation convert_profile(const Profile& profile)
{
  Profile::Implementation output = Profile::Implementation::get(profile);
  if (!output.vicinity)
    output.vicinity = output.footprint;

  return output;
}

//==============================================================================
Time compute_time(
  const double scaled_time,
  const Time start_time,
  const Time finish_time)
{
  const Duration delta_t{
    Duration::rep(scaled_time * (finish_time - start_time).count())
  };

  return start_time + delta_t;
}

} // anonymous namespace

//==============================================================================
rmf_utils::optional<rmf_traffic::Time> DetectConflict::between(
  const Profile& profile_a,
  const Trajectory& trajectory_a,
  const Profile& profile_b,
  const Trajectory& trajectory_b,
  Interpolate interpolation)
{
  return Implementation::between(
    profile_a, trajectory_a,
    profile_b, trajectory_b,
    interpolation);
}

namespace {

//==============================================================================
bool check_overlap(
  const Profile::Implementation& profile_a,
  const Spline& spline_a,
  const Profile::Implementation& profile_b,
  const Spline& spline_b,
  const Time time)
{
  using ConvexPair = std::array<geometry::ConstFinalConvexShapePtr, 2>;
  // TODO(MXG): If footprint and vicinity are equal, we can probably reduce this
  // to just one check.
  std::array<ConvexPair, 2> pairs = {
    ConvexPair{profile_a.footprint, profile_b.vicinity},
    ConvexPair{profile_a.vicinity, profile_b.footprint}
  };

  fcl::CollisionRequestd request;
  fcl::CollisionResultd result;
  for (const auto pair : pairs)
  {
    auto pos_a = spline_a.compute_position(time);
    auto pos_b = spline_b.compute_position(time);
    
    fcl::Matrix3d rot_a = fcl::AngleAxisd(pos_a[2], Eigen::Vector3d::UnitZ()).toRotationMatrix();
    fcl::Matrix3d rot_b = fcl::AngleAxisd(pos_b[2], Eigen::Vector3d::UnitZ()).toRotationMatrix();

    fcl::CollisionObjectd obj_a(
      geometry::FinalConvexShape::Implementation::get_collision(*pair[0]),
      rot_a,
      fcl::Vector3d(pos_a[0], pos_a[1], 0.0));

    fcl::CollisionObjectd obj_b(
      geometry::FinalConvexShape::Implementation::get_collision(*pair[1]),
      rot_b,
      fcl::Vector3d(pos_b[0], pos_b[1], 0.0));

    if (fcl::collide(&obj_a, &obj_b, request, result) > 0)
      return true;
  }
  return false;
}

//==============================================================================
bool close_start(
  const Profile::Implementation& profile_a,
  const Trajectory::const_iterator& a_it,
  const Profile::Implementation& profile_b,
  const Trajectory::const_iterator& b_it)
{
  // If two trajectories start very close to each other, then we do not consider
  // it a conflict for them to be in each other's vicinities. This gives robots
  // an opportunity to back away from each other without it being considered a
  // schedule conflict.
  Spline spline_a(a_it);
  Spline spline_b(b_it);
  const auto start_time =
    std::max(spline_a.start_time(), spline_b.start_time());

  return check_overlap(profile_a, spline_a, profile_b, spline_b, start_time);
}

//==============================================================================
rmf_utils::optional<rmf_traffic::Time> detect_invasion(
    const Profile::Implementation& profile_a,
    Trajectory::const_iterator a_it,
    const Trajectory::const_iterator& a_end,
    const Profile::Implementation& profile_b,
    Trajectory::const_iterator b_it,
    const Trajectory::const_iterator& b_end,
    std::vector<DetectConflict::Implementation::Conflict>* output_conflicts)
{
  rmf_utils::optional<Spline> spline_a;
  rmf_utils::optional<Spline> spline_b;

  std::shared_ptr<fcl::SplineMotion<double>> motion_a =
    make_uninitialized_fcl_spline_motion();
  std::shared_ptr<fcl::SplineMotion<double>> motion_b =
    make_uninitialized_fcl_spline_motion();

  const fcl::ContinuousCollisionRequest request = make_fcl_request();

  // This flag lets us know that we need to test both a's footprint in b's
  // vicinity and b's footprint in a's vicinity.
  const bool test_complement =
    (profile_a.vicinity != profile_a.footprint)
    || (profile_b.vicinity != profile_b.footprint);

  if (output_conflicts)
    output_conflicts->clear();

  while (a_it != a_end && b_it != b_end)
  {
    if (!spline_a)
      spline_a = Spline(a_it);

    if (!spline_b)
      spline_b = Spline(b_it);

    const Time start_time =
      std::max(spline_a->start_time(), spline_b->start_time());

    const Time finish_time =
      std::min(spline_a->finish_time(), spline_b->finish_time());

    *motion_a = spline_a->to_fcl(start_time, finish_time);
    *motion_b = spline_b->to_fcl(start_time, finish_time);

    const auto bound_a = get_bounding_profile(*spline_a, profile_a);
    const auto bound_b = get_bounding_profile(*spline_b, profile_b);

    if (overlap(bound_a.footprint, bound_b.vicinity))
    {
      //printf("overlap pass\n");
      
      if (const auto collision = check_collision(
          *profile_a.footprint, motion_a,
          *profile_b.vicinity, motion_b, request))
      {
        //printf("check_collision pass\n");
        const auto time = compute_time(*collision, start_time, finish_time);
        if (!output_conflicts)
          return time;

        output_conflicts->emplace_back(
          DetectConflict::Implementation::Conflict{a_it, b_it, time});
      }
      // else
      //   printf("check_collision fail\n");
    }

    if (test_complement && overlap(bound_a.vicinity, bound_b.footprint))
    {
      // printf("overlap #2 pass\n");
      if (const auto collision = check_collision(
          *profile_a.vicinity, motion_a,
          *profile_b.footprint, motion_b, request))
      {
        // printf("check_collision #2 pass\n");
        const auto time = compute_time(*collision, start_time, finish_time);
        if (!output_conflicts)
          return time;

        output_conflicts->emplace_back(
          DetectConflict::Implementation::Conflict{a_it, b_it, time});
      }
    }

    if (spline_a->finish_time() < spline_b->finish_time())
    {
      spline_a = rmf_utils::nullopt;
      ++a_it;
    }
    else if (spline_b->finish_time() < spline_a->finish_time())
    {
      spline_b = rmf_utils::nullopt;
      ++b_it;
    }
    else
    {
      spline_a = rmf_utils::nullopt;
      ++a_it;

      spline_b = rmf_utils::nullopt;
      ++b_it;
    }
  }

  if (!output_conflicts)
    return rmf_utils::nullopt;

  if (output_conflicts->empty())
    return rmf_utils::nullopt;

  return output_conflicts->front().time;
}

//==============================================================================
Trajectory slice_trajectory(
    const Time start_time,
    const Spline& spline,
    Trajectory::const_iterator it,
    const Trajectory::const_iterator& end)
{
  Trajectory output;
  output.insert(
    start_time,
    spline.compute_position(start_time),
    spline.compute_velocity(start_time));

  for (; it != end; ++it)
    output.insert(*it);

  return output;
}

//==============================================================================
rmf_utils::optional<rmf_traffic::Time> detect_approach(
    const Profile::Implementation& profile_a,
    Trajectory::const_iterator a_it,
    const Trajectory::const_iterator& a_end,
    const Profile::Implementation& profile_b,
    Trajectory::const_iterator b_it,
    const Trajectory::const_iterator& b_end,
    std::vector<DetectConflict::Implementation::Conflict>* output_conflicts)
{
  rmf_utils::optional<Spline> spline_a;
  rmf_utils::optional<Spline> spline_b;

  while (a_it != a_end && b_it != b_end)
  {
    if (!spline_a)
      spline_a = Spline(a_it);

    if (!spline_b)
      spline_b = Spline(b_it);

    const DistanceDifferential D(*spline_a, *spline_b);

    if (D.initially_approaching())
    {
      const auto time = D.start_time();
      if (!output_conflicts)
        return time;

      output_conflicts->emplace_back(
          DetectConflict::Implementation::Conflict{a_it, b_it, time});
    }

    const auto approach_times = D.approach_times();
    for (const auto t : approach_times)
    {
      if (!check_overlap(profile_a, *spline_a, profile_b, *spline_b, t))
      {
        // If neither vehicle is in the vicinity of the other, then we should
        // revert to the normal invasion detection approach to identifying
        // conflicts.

        // TODO(MXG): Consider an approach that does not require making copies
        // of the trajectories.
        const Trajectory sliced_trajectory_a =
            slice_trajectory(t, *spline_a, a_it, a_end);

        const Trajectory sliced_trajectory_b =
            slice_trajectory(t, *spline_b, b_it, b_end);

        return detect_invasion(
          profile_a, ++sliced_trajectory_a.begin(), sliced_trajectory_a.end(),
          profile_b, ++sliced_trajectory_b.begin(), sliced_trajectory_b.end(),
          output_conflicts);
      }

      // If one of the vehicles is still inside the vicinity of another during
      // this approach time, then we consider this to be a conflict.
      if (!output_conflicts)
        return t;

      output_conflicts->emplace_back(
            DetectConflict::Implementation::Conflict{a_it, b_it, t});
    }

    const bool still_close = check_overlap(
          profile_a, *spline_a, profile_b, *spline_b, D.finish_time());

    if (spline_a->finish_time() < spline_b->finish_time())
    {
      spline_a = rmf_utils::nullopt;
      ++a_it;
    }
    else if (spline_b->finish_time() < spline_a->finish_time())
    {
      spline_b = rmf_utils::nullopt;
      ++b_it;
    }
    else
    {
      spline_a = rmf_utils::nullopt;
      ++a_it;

      spline_b = rmf_utils::nullopt;
      ++b_it;
    }

    if (!still_close)
    {
      return detect_invasion(
        profile_a, a_it, a_end,
        profile_b, b_it, b_end,
        output_conflicts);
    }
  }

  if (!output_conflicts)
    return rmf_utils::nullopt;

  if (output_conflicts->empty())
    return rmf_utils::nullopt;

  return output_conflicts->front().time;
}

} // anonymous namespace

//==============================================================================
rmf_utils::optional<rmf_traffic::Time> DetectConflict::Implementation::between(
  const Profile& input_profile_a,
  const Trajectory& trajectory_a,
  const Profile& input_profile_b,
  const Trajectory& trajectory_b,
  Interpolate /*interpolation*/,
  std::vector<Conflict>* output_conflicts)
{
  if (trajectory_a.size() < 2)
  {
    throw invalid_trajectory_error::Implementation
        ::make_segment_num_error(
          trajectory_a.size(), __LINE__, __FUNCTION__);
  }

  if (trajectory_b.size() < 2)
  {
    throw invalid_trajectory_error::Implementation
        ::make_segment_num_error(
          trajectory_b.size(), __LINE__, __FUNCTION__);
  }

  const Profile::Implementation profile_a = convert_profile(input_profile_a);
  const Profile::Implementation profile_b = convert_profile(input_profile_b);

  // Return early if there is no geometry in the profiles
  // TODO(MXG): Should this produce an exception? Is this an okay scenario?
  if (!profile_a.footprint && !profile_b.footprint)
    return rmf_utils::nullopt;

  // Return early if either profile is missing both a vicinity and a footprint.
  // NOTE(MXG): Since convert_profile will promote the vicinity to have the same
  // value as the footprint when the vicinity has a nullptr value, checking if
  // a vicinity doesn't exist is the same as checking that both the vicinity and
  // footprint doesn't exist.
  if (!profile_a.vicinity || !profile_b.vicinity)
    return rmf_utils::nullopt;

  // Return early if there is no time overlap between the trajectories
  if (!have_time_overlap(trajectory_a, trajectory_b))
    return rmf_utils::nullopt;

  Trajectory::const_iterator a_it;
  Trajectory::const_iterator b_it;
  std::tie(a_it, b_it) = get_initial_iterators(trajectory_a, trajectory_b);

  if (close_start(profile_a, a_it, profile_b, b_it))
  {
    // If the vehicles are already starting in close proximity, then we consider
    // it a conflict if they get any closer while within that proximity.
    return detect_approach(
          profile_a,
          std::move(a_it),
          trajectory_a.end(),
          profile_b,
          std::move(b_it),
          trajectory_b.end(),
          output_conflicts);
  }

  // If the vehicles are starting an acceptable distance from each other, then
  // check if either one invades the vicinity of the other.
  return detect_invasion(
        profile_a,
        std::move(a_it),
        trajectory_a.end(),
        profile_b,
        std::move(b_it),
        trajectory_b.end(),
        output_conflicts);
}

namespace internal {
//==============================================================================
bool detect_conflicts(
  const Profile& profile,
  const Trajectory& trajectory,
  const Spacetime& region,
  DetectConflict::Implementation::Conflicts* output_conflicts)
{
#ifndef NDEBUG
  // This should never actually happen because this function only gets used
  // internally, and so there should be several layers of quality checks on the
  // trajectories to prevent this. But we'll put it in here just in case.
  if (trajectory.size() < 2)
  {
    std::cerr << "[rmf_traffic::internal::detect_conflicts] An invalid "
              << "trajectory was passed to detect_conflicts. This is a bug "
              << "that should never happen. Please alert the RMF developers."
              << std::endl;
    throw invalid_trajectory_error::Implementation
          ::make_segment_num_error(trajectory.size(), 0, "");
  }
#endif // NDEBUG

  const auto vicinity = profile.vicinity();
  if (!vicinity)
    return false;

  const Time trajectory_start_time = *trajectory.start_time();
  const Time trajectory_finish_time = *trajectory.finish_time();

  const Time start_time = region.lower_time_bound ?
    std::max(*region.lower_time_bound, trajectory_start_time) :
    trajectory_start_time;

  const Time finish_time = region.upper_time_bound ?
    std::min(*region.upper_time_bound, trajectory_finish_time) :
    trajectory_finish_time;

  if (finish_time < start_time)
  {
    // If the trajectory or region finishes before the other has started, that
    // means there is no overlap in time between the region and the trajectory,
    // so it is impossible for them to conflict.
    return false;
  }

  const Trajectory::const_iterator begin_it =
    trajectory_start_time < start_time ?
    trajectory.find(start_time) : ++trajectory.begin();

  const Trajectory::const_iterator end_it =
    finish_time < trajectory_finish_time ?
    ++trajectory.find(finish_time) : trajectory.end();

  std::shared_ptr<fcl::SplineMotion<double>> motion_trajectory =
    make_uninitialized_fcl_spline_motion();
  std::shared_ptr<internal::StaticMotion> motion_region =
    std::make_shared<internal::StaticMotion>(region.pose);

  const fcl::ContinuousCollisionRequestd request = make_fcl_request();

  const std::shared_ptr<fcl::CollisionGeometryd> vicinity_geom =
    geometry::FinalConvexShape::Implementation::get_collision(*vicinity);

  if (output_conflicts)
    output_conflicts->clear();

  for (auto it = begin_it; it != end_it; ++it)
  {
    Spline spline_trajectory{it};

    const Time spline_start_time =
      std::max(spline_trajectory.start_time(), start_time);
    const Time spline_finish_time =
      std::min(spline_trajectory.finish_time(), finish_time);

    *motion_trajectory = spline_trajectory.to_fcl(
      spline_start_time, spline_finish_time);

    const auto obj_trajectory = fcl::ContinuousCollisionObjectd(
      vicinity_geom, motion_trajectory);

    assert(region.shape);
    const auto& region_shapes = geometry::FinalShape::Implementation
      ::get_collisions(*region.shape);
    for (const auto& region_shape : region_shapes)
    {
      const auto obj_region = fcl::ContinuousCollisionObjectd(
        region_shape, motion_region);

      // TODO(MXG): We should do a broadphase test here before using
      // fcl::collide

      fcl::ContinuousCollisionResultd result;
      fcl::collide(&obj_trajectory, &obj_region, request, result);
      if (result.is_collide)
      {
        if (!output_conflicts)
          return true;

        output_conflicts->emplace_back(
          DetectConflict::Implementation::Conflict{
            it, it,
            compute_time(
              result.time_of_contact,
              spline_start_time,
              spline_finish_time)
          });
      }
    }
  }

  if (!output_conflicts)
    return false;

  return !output_conflicts->empty();
}
} // namespace internal

} // namespace rmf_traffic
