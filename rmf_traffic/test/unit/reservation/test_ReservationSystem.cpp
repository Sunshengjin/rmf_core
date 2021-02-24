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

#include <rmf_traffic/reservations/reservations.hpp>

#include <rmf_utils/catch.hpp>

SCENARIO("Verify that reservations work")
{
  const std::string test_map_name = "test_map";
  using namespace std::literals;
  using namespace rmf_traffic::reservations;

  std::string wp0 = "waypoint_0";
  std::string wp1 = "waypoint_1";

  WHEN("Given an empty sequence")
  {
    ReservationSystem reservation_system;
    THEN("Able to reserve a slot with infinite waiting")
    {
      auto time = std::chrono::steady_clock::now();
      std::vector<std::string> waypoints 
        {wp0, wp1};
      auto res = reservation_system.reserve(time, waypoints);
      CHECK(res.has_value());
    }

    THEN("Able to reserve a slot with finite waiting")
    {
      auto time = std::chrono::steady_clock::now();
       
      std::vector<std::string> waypoints 
        {wp0, wp1};
      
      auto res = reservation_system.reserve(time, waypoints, {1h});
      CHECK(res.has_value());
    }
  }

  WHEN("Given a system with an infinite reservation and one free waypoint")
  {
    ReservationSystem reservation_system;
    auto curr_time = std::chrono::steady_clock::now();
    auto reservation_time = curr_time + 10h;

    std::vector<std::string> waypoints 
        {wp0};
    reservation_system.reserve(reservation_time, waypoints);

    THEN("Cannot make an infinite reservation before it.")
    {
      auto res = reservation_system.reserve(reservation_time-5h, waypoints);
      CHECK(!res.has_value());
    }

    THEN("Cannot make a fixed time reservation after it.")
    {
      auto res = reservation_system.reserve(reservation_time+5h, waypoints, {2h});
      CHECK(!res.has_value());
    }

    THEN("Can make a reservation before it if it doesn't overlap.")
    {
      auto res = reservation_system.reserve(reservation_time-5h, waypoints, {2h});
      CHECK(res.has_value());
    }

    THEN("Cannot make a reservation before it if it overlaps.")
    {
      auto res = reservation_system.reserve(reservation_time-5h, waypoints, {7h});
      CHECK(!res.has_value());
    }

    THEN("Cannot make a reservation after it.")
    {
      auto res = reservation_system.reserve(reservation_time-5h, waypoints, {7h});
      CHECK(!res.has_value());
    }
  }

  WHEN("Given a system with one finite reservation and one free waypoint")
  {
    ReservationSystem reservation_system;
    auto curr_time = std::chrono::steady_clock::now();
    auto reservation_time = curr_time + 10h;

    std::vector<std::string> waypoints 
        {wp0};
    reservation_system.reserve(reservation_time, waypoints, {2h});

    THEN("Can make infinite reservation exactly afterwards")
    {
      auto res = reservation_system.reserve(reservation_time+2h, waypoints);
      CHECK(res.has_value());
    }

    THEN("Cannot make infinite reservation before")
    {
      auto res = reservation_system.reserve(reservation_time-2h, waypoints);
      CHECK(!res.has_value());
    }

    THEN("Cannot make overlapping reservation before")
    {
      auto res = reservation_system.reserve(reservation_time-2h, waypoints, {3h});
      CHECK(!res.has_value());
    }

    THEN("Cannot make fully overlapping reservation before")
    {
      auto res = reservation_system.reserve(reservation_time-2h, waypoints, {7h});
      CHECK(!res.has_value());
    }

    THEN("Cannot make same reservation")
    {
      auto res = reservation_system.reserve(reservation_time, waypoints, {2h});
      CHECK(!res.has_value());
    }

    THEN("Can make fixed time reservation before")
    {
      auto res = reservation_system.reserve(reservation_time-2h, waypoints, {2h});
      CHECK(res.has_value());
    }
  }

  WHEN("Given a system with two waypoints")
  {
    ReservationSystem reservation_system;
    auto curr_time = std::chrono::steady_clock::now();
    auto reservation_time = curr_time + 10h;

    std::vector<std::string> waypoints 
        {wp0, wp1};
    reservation_system.reserve(reservation_time, waypoints, {2h});

    THEN("Can request at most twice the same time period")
    {
      auto res = reservation_system.reserve(reservation_time, waypoints, {2h});
      CHECK(res.has_value());

      auto res2 = reservation_system.reserve(reservation_time, waypoints, {2h});
      CHECK(!res2.has_value());
    }

    THEN("Allocates waypoints in order")
    {
      auto res = reservation_system.reserve(reservation_time, waypoints, {2h});
      CHECK(res.has_value());
      CHECK(res->waypoint() == wp1);
    }
  }
}

SCENARIO("Verify that cancelation works")
{
  const std::string test_map_name = "test_map";
  using namespace std::literals;
  using namespace rmf_traffic::reservations;

  std::string wp0 = "waypoint_0";

  GIVEN("A system with two points and a reservation")
  {
    ReservationSystem reservation_system;
    auto curr_time = std::chrono::steady_clock::now();
    auto reservation_time = curr_time + 10h;

    std::vector<std::string> waypoints 
        {wp0};
    auto res = reservation_system.reserve(reservation_time, waypoints, {2h});
    WHEN("We cancel a reservation")
    {
      auto res1 = reservation_system.reserve(reservation_time, waypoints, {2h});
      CHECK(!res1.has_value());
      
      reservation_system.cancel_reservation(res->reservation_id());

      THEN("We can make another reservation")
      {
        auto res2 = reservation_system.reserve(reservation_time, waypoints, {2h});
        CHECK(res2.has_value());
      }

    }
    WHEN("We cancel a non-existant reservation")
    { 
      THEN("The system throws an exception")
      {
        CHECK_THROWS(
          reservation_system.cancel_reservation(res->reservation_id()+100)
        );
      }
    }
  }
   
}