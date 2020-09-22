/*
 * Copyright (C) 2020 Open Source Robotics Foundation
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

#include <rmf_tasks/agv/State.hpp>

namespace rmf_tasks {
namespace agv {

//==============================================================================
class State::Implementation
{
public:

  Implementation(
    std::size_t waypoint, 
    std::size_t charging_waypoint,
    rmf_traffic::Time finish_time,
    double battery_soc,
    double threshold_soc)
  : _waypoint(waypoint),
    _charging_waypoint(charging_waypoint),
    _finish_time(finish_time),
    _battery_soc(battery_soc),
    _threshold_soc(threshold_soc)
  {}

  std::size_t _waypoint;
  std::size_t _charging_waypoint;
  rmf_traffic::Time _finish_time;
  double _battery_soc;
  double _threshold_soc;
};

//==============================================================================
State::State(
  std::size_t waypoint, 
  std::size_t charging_waypoint,
  rmf_traffic::Time finish_time,
  double battery_soc,
  double threshold_soc)
: _pimpl(rmf_utils::make_impl<Implementation>(
    Implementation(
      waypoint, charging_waypoint, finish_time, battery_soc, threshold_soc)))
{}

//==============================================================================
State::State()
: _pimpl(rmf_utils::make_impl<Implementation>(
    Implementation(0, 0, std::chrono::steady_clock::now(), 0.0, 0.0)))
{}

//==============================================================================
std::size_t State::waypoint() const
{
  return _pimpl->_waypoint;
}

//==============================================================================
State& State::waypoint(std::size_t new_waypoint)
{
  _pimpl->_waypoint = new_waypoint;
  return *this;
}

//==============================================================================
std::size_t State::charging_waypoint() const
{
  return _pimpl->_charging_waypoint;
}

//==============================================================================
State& State::charging_waypoint(std::size_t new_charging_waypoint)
{
  _pimpl->_charging_waypoint = new_charging_waypoint;
  return *this;
}

//==============================================================================
rmf_traffic::Time State::finish_time() const
{
  return _pimpl->_finish_time;
}

//==============================================================================
State& State::finish_time(rmf_traffic::Time new_finish_time)
{
  _pimpl->_finish_time = new_finish_time;
  return *this;
}

//==============================================================================
double State::battery_soc() const
{
  return _pimpl->_battery_soc;
}

//==============================================================================
State& State::battery_soc(double new_battery_soc)
{
  _pimpl->_battery_soc = new_battery_soc;
  return *this;
}

//==============================================================================
double State::threshold_soc() const
{
  return _pimpl->_threshold_soc;
}

//==============================================================================
State& State::threshold_soc(double new_threshold_soc)
{
  _pimpl->_threshold_soc = new_threshold_soc;
  return *this;
}

//==============================================================================
} // namespace agv
} // namespace rmf_tasks
