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


#ifndef RMF_TRAFFIC__GEOMETRY__SHAPE_HPP
#define RMF_TRAFFIC__GEOMETRY__SHAPE_HPP

#include <rmf_utils/impl_ptr.hpp>

#include <memory>

namespace rmf_traffic {
namespace geometry {

class FinalShape;

//==============================================================================
/// \brief This is the base class of different shape classes that can be used
/// by the rmf_traffic library. This cannot (currently) be extended
/// by downstream libraries; instead, users must choose one of the pre-defined
/// shape types belonging to this library.
///
/// \sa Box, Circle, Polygon
class Shape
{
public:

  virtual FinalShape finalize() const = 0;

  // Abstract shape references must not be moved, because we cannot ensure that
  // they get moved into the same derived type.
  Shape(Shape&&) = delete;
  Shape& operator=(Shape&&) = delete;

  /// \internal
  class Internal;
  virtual ~Shape();

protected:

  /// \internal
  Shape(std::unique_ptr<Internal> internal);

private:

  std::unique_ptr<Internal> _internal;

};

using ShapePtr = std::shared_ptr<Shape>;
using ConstShapePtr = std::shared_ptr<const Shape>;

//==============================================================================
/// This is a finalized shape whose parameters can no longer be mutated.
class FinalShape
{
public:

  /// Look at the source of this FinalShape to inspect its parameters.
  const Shape& source() const;

  class Implementation;
private:
  rmf_utils::impl_ptr<Implementation> _pimpl;
};

using FinalShapePtr = std::shared_ptr<Shape>;
using ConstFinalShapePtr = std::shared_ptr<const FinalShape>;

} // namespace geometry
} // namespace rmf_traffic

#endif // RMF_TRAFFIC__GEOMETRY__SHAPE_HPP
