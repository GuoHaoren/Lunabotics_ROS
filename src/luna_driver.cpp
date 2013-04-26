#include "ros/ros.h"
#include "nav_msgs/Path.h"
#include "lunabotics/Emergency.h"
#include "lunabotics/Control.h"
#include "lunabotics/ControlParams.h"
#include "lunabotics/ControlMode.h"
#include "lunabotics/State.h"
#include "lunabotics/Goal.h"
#include "lunabotics/PID.h"
#include "lunabotics/ICRControl.h"
#include "lunabotics/AllWheelCommon.h"
#include "nav_msgs/GetMap.h"
#include "nav_msgs/Path.h"
#include "geometry_msgs/Point.h"
#include "lunabotics/JointPositions.h"
#include "lunabotics/AllWheelStateROS.h"
#include "geometry_msgs/PoseStamped.h"
#include <geometry_msgs/Twist.h>
#include "planning/a_star_graph.h"
#include "planning/bezier_smooth.h"
#include "geometry/allwheel.h"
#include "control/PIDController.h"
#include "tf/tf.h"
#include <tf/transform_listener.h>
#include "message_filters/subscriber.h"
#include "../protos_gen/AllWheelControl.pb.h"
#include "../protos_gen/Telemetry.pb.h"
#include "types.h"
#include "geometry/geometry.h"
#include <numeric>

#define ROBOT_DIFF_DRIVE	0


using namespace std;

float angleAccuracy = 0.4;
float distanceAccuracy = 0.2;

//Locally exponentially stable when Kp > 0; Kb < 0; Ka-Kb > 0
#define Kp 	0.15
#define Ka	0.7
#define Kb	-0.05

inline int sign(double value) {
	if (value > angleAccuracy) return 1;
	if (value < -angleAccuracy) return -1;
	return 0;
}

int seq = 0;
int bezierSegments = 20;
lunabotics::SteeringModeType controlMode = lunabotics::ACKERMANN;
lunabotics::Telemetry::PointTurnState skidState = lunabotics::Telemetry::STOPPED;
nav_msgs::GetMap mapService;
ros::ServiceClient mapClient;
pose_t currentPose;
ros::Publisher controlPublisher;
ros::Publisher controlParamsPublisher;
ros::Publisher pathPublisher;
ros::Publisher allWheelPublisher;
ros::Publisher allWheelCommonPublisher;
ros::Publisher jointPublisher;
geometry::PID pidGeometry;
ros::Publisher ICRPublisher;
geometry_msgs::Twist velocities;
bool autonomyEnabled = false;
pose_arr waypoints;
pose_arr::iterator wayIterator;
float linear_speed_limit = 1;
lunabotics::control::PIDControllerPtr pidController;
geometry::AllWheelGeometryPtr allWheelGeometry;
bool jointStatesAcquired = false;

void stop() {
	autonomyEnabled = false;
	lunabotics::Control controlMsg;
	controlMsg.motion.linear.x = 0;
	controlMsg.motion.linear.y = 0;
	controlMsg.motion.linear.z = 0;
	controlMsg.motion.angular.x = 0;
	controlMsg.motion.angular.y = 0;
	controlMsg.motion.angular.z = 0;
	controlPublisher.publish(controlMsg);
	lunabotics::ControlParams controlParamsMsg;
	controlParamsMsg.driving = autonomyEnabled;
	controlParamsMsg.next_waypoint_idx = wayIterator < waypoints.end() ? wayIterator-waypoints.begin()+1 : 0;
	controlParamsPublisher.publish(controlParamsMsg);
	waypoints.clear();
}

void finish_route() {
	ROS_INFO("Route completed");
	stop();
	
	//Send empty path to clear map in GUI
	nav_msgs::Path pathMsg;
	ros::Time now = ros::Time::now();
	pathMsg.header.stamp = now;
	pathMsg.header.seq = seq;
	pathMsg.header.frame_id = "1";
	seq++;
	pathPublisher.publish(pathMsg);
}

planning::path *getPath(pose_t startPose, pose_t goalPose, float &res)
{
	if (mapClient.call(mapService)) {
		stop();
		
		unsigned int map_size = mapService.response.map.data.size();
		if (map_size > 0) {
			ROS_INFO("Got map from service (%d cells)", (int)map_size);
			ROS_INFO("------------------------------------");
			for (unsigned int i = 0; i < mapService.response.map.info.height; i++) {
			    stringstream sstr;
				for (unsigned int j = 0; j < mapService.response.map.info.width; j++) {
					int8_t probability = mapService.response.map.data.at(i*mapService.response.map.info.width+j);
					sstr << setw(3) << static_cast<int>(probability) << " ";
				}
				ROS_INFO("%s", sstr.str().c_str());
			}
			ROS_INFO("\n");
			
			ROS_INFO("Looking for a path...");
			
			//Generate a path
			float resolution = mapService.response.map.info.resolution;
			res = resolution;
			
			int start_x = round(startPose.position.x/resolution);
			int start_y = round(startPose.position.y/resolution);
			int goal_x = round(goalPose.position.x/resolution);
			int goal_y = round(goalPose.position.y/resolution);
			
			planning::path *graph = new planning::path(mapService.response.map.data, 
									mapService.response.map.info.width, 
									mapService.response.map.info.height,
									start_x, start_y, goal_x, goal_y);
									
			
			if (graph->allNodes().size() == 0) {
				ROS_INFO("Path is not found");
				planning::path *empty = new planning::path();
				return empty;
			}
			else {
				if (graph->cornerNodes().size() == 1) {
					ROS_INFO("Robot is at the goal");
				}
				return graph;
			}
		}
		else {
			ROS_ERROR("Failed to get a proper map from the service");
		}	
	}
	else {
		ROS_ERROR("Failed to call service luna_map");
	}
	planning::path *empty = new planning::path();
	return empty;
}

void emergencyCallback(const lunabotics::Emergency& msg)
{
	//Use msg to stop driving if applicable
}

void stateCallback(const lunabotics::State& msg)
{    
	//ROS_INFO("Pose updated");
	currentPose.position = msg.odometry.pose.pose.position;
	currentPose.orientation = msg.odometry.pose.pose.orientation;
	velocities = msg.odometry.twist.twist;
	pidGeometry.setLinearVelocity(velocities.linear.x);
	pidGeometry.setCurrentPose(currentPose);
}

void controlModeCallback(const lunabotics::ControlMode& msg)
{
	controlMode = (lunabotics::SteeringModeType)msg.mode;
	if (controlMode == lunabotics::ACKERMANN) {
		linear_speed_limit = msg.linear_speed_limit;
		bezierSegments = (int)msg.smth_else;
	}
	
	ROS_INFO("Switching control mode to %s", controlModeToString(controlMode).c_str());
					
}

void autonomyCallback(const std_msgs::Bool& msg)
{
	//Use msg to toggle autonomy
	if (msg.data) {
			
	}
	else {
		stop();
	}
}

void pidCallback(const lunabotics::PID& msg) 
{
	pidController->setP(msg.p);
	pidController->setI(msg.i);
	pidController->setD(msg.d);
	#if ROBOT_DIFF_DRIVE
	pidGeometry.setVelocityMultiplier(msg.velocity_multiplier);
	pidGeometry.setVelocityOffset(msg.velocity_offset);
	#endif
}

void ICRCallback(const lunabotics::ICRControl& msg) 
{		
	point_t ICRMsg = msg.ICR;
	ICRPublisher.publish(ICRMsg);
	
	float angle_front_left;
	float angle_front_right;
	float angle_rear_left;
	float angle_rear_right;
	if (allWheelGeometry->calculateAngles(msg.ICR, angle_front_left, angle_front_right, angle_rear_left, angle_rear_right)) {
		lunabotics::AllWheelStateROS controlMsg;
		controlMsg.steering.left_front = angle_front_left;
		controlMsg.steering.right_front = angle_front_right;
		controlMsg.steering.left_rear = angle_rear_left;
		controlMsg.steering.right_rear = angle_rear_right;
		float vel_front_left;
		float vel_front_right;
		float vel_rear_left;
		float vel_rear_right;
		if (fabs(msg.ICR.x-currentPose.position.x) < 0.001 && fabs(msg.ICR.y-currentPose.position.y) < 0.001) {
			//Point turn
			vel_front_left = vel_rear_right = msg.velocity;
			vel_front_right = vel_rear_left = -msg.velocity;
		}
		else if (!allWheelGeometry->calculateVelocities(msg.ICR, msg.velocity, vel_front_left, vel_front_right, vel_rear_left, vel_rear_right)) {
			vel_front_left = vel_front_right = vel_rear_left = vel_rear_right = 0;			
		}
		controlMsg.driving.left_front = vel_front_left;
		controlMsg.driving.right_front = vel_front_right;
		controlMsg.driving.left_rear = vel_rear_left;
		controlMsg.driving.right_rear = vel_rear_right;
		
		
		ROS_INFO("VELOCITY %.2f | %.2f | %.2f | %.2f", vel_front_left, vel_front_right, vel_rear_left, vel_rear_right);
		
		allWheelPublisher.publish(controlMsg);
	}
}

void goalCallback(const lunabotics::Goal& msg) 
{
	waypoints.clear();
	
	angleAccuracy = msg.angleAccuracy;
	distanceAccuracy = msg.distanceAccuracy;
	
	//Specify params
	pose_t goal;
	goal.position = msg.point;
	goal.orientation = tf::createQuaternionMsgFromYaw(0);
		ROS_INFO("Requesting path between (%.1f,%.1f) and (%.1f,%.1f)",
			  currentPose.position.x, currentPose.position.y,
			  goal.position.x, goal.position.y);
	float resolution;
	planning::path *path = getPath(currentPose, goal, resolution);
	
	if (path->is_initialized()) {
		stringstream sstr;
		
		nav_msgs::Path pathMsg;
		ros::Time now = ros::Time::now();
		pathMsg.header.stamp = now;
		pathMsg.header.seq = seq;
		pathMsg.header.frame_id = "map";
		seq++;
		
		
		point_arr corner_points = path->cornerPoints(resolution);
		point_arr pts;
		
		if (controlMode == lunabotics::ACKERMANN) {
			unsigned int size = corner_points.size();
			if (size > 2) {
				point_t startPoint = corner_points.at(0);
				point_t endPoint = corner_points.at(size-1);
				point_indexed_arr closest_obstacles = path->closestObstaclePoints(resolution);
				unsigned int obst_size = closest_obstacles.size();
				
				pts.push_back(startPoint);
			
				//Get bezier quadratic curves for each point-turn
				for (unsigned int i = 1; i < size-1; i++) {
					point_t q0, q2;
					point_t prev = corner_points.at(i-1);
					point_t curr = corner_points.at(i);
					point_t next = corner_points.at(i+1);
					
					bool hasObstacle = false;
					point_t obstaclePoint;
					
					//Since obstacle is the center of occupied cell, we want p to be at its edge
					if (obst_size > 0) {
						int start = std::min(obst_size-1, i-1);
						for (int j = start; j >= 0; j--) {
							point_indexed indexedObstacle = closest_obstacles.at(j);
							if (indexedObstacle.index == (int)i) {
								hasObstacle = true;
								obstaclePoint = indexedObstacle.point;
								break;
							}
						}
					}
					
					
					if (i == 1) {
						q0 = prev;
					}
					else {
						q0 = geometry::midPoint(prev, curr);
					}
					if (i == size-2) {
						q2 = next;
					}
					else {
						q2 = geometry::midPoint(next, curr);
					}
					
					point_arr curve;
					if (hasObstacle) {
						point_t p = geometry::midPoint(obstaclePoint, curr);
						curve = planning::trajectory_bezier(q0, curr, q2, p, bezierSegments);
						ROS_INFO("Curve from tetragonal q0=(%f,%f) q1=(%f,%f), q2=(%f,%f), p=(%f,%f)", q0.x, q0.y, curr.x, curr.y, q2.x, q2.y, p.x, p.y);
					}
					else {
						curve = planning::quadratic_bezier(q0, curr, q2, bezierSegments);
						ROS_INFO("Curve without tetragonal q0=(%f,%f) q1=(%f,%f), q2=(%f,%f)", q0.x, q0.y, curr.x, curr.y, q2.x, q2.y);
					}
					
					//Append curve points
					pts.insert(pts.end(), curve.begin(), curve.end());
				}	
				pts.push_back(endPoint);	
			}
			else {
				pts = corner_points;
			}
		}
		else {
			pts = corner_points;
		}
		
		int poseSeq = 0;
		for (point_arr::iterator it = pts.begin(); it != pts.end(); it++) {
			point_t pt = *it;
			
			//float x_m = node.x*resolution;
			//float y_m = node.y*resolution;
			float x_m = pt.x;
			float y_m = pt.y;
			
			pose_t waypoint;
			waypoint.position = pt;
			waypoint.orientation = tf::createQuaternionMsgFromYaw(0);
			waypoints.push_back(waypoint);
	//		sstr << "->(" << x_m << "," << y_m << ")";
			
			
			geometry_msgs::PoseStamped pose;
			pose.header.seq = poseSeq++;
			pose.header.stamp = now;
			pose.header.frame_id = "map";
			pose.pose = waypoint;
			pathMsg.poses.push_back(pose);
		}
		pidGeometry.setTrajectory(pts);
		
		wayIterator = controlMode == lunabotics::ACKERMANN ? waypoints.begin() : waypoints.begin()+1;		
	//	ROS_INFO("Returned path: %s", sstr.str().c_str());
		pose_t waypoint = waypoints.at(0);
		ROS_INFO("Heading towards (%.1f,%.1f)", (*wayIterator).position.x, (*wayIterator).position.y);
		
		pathPublisher.publish(pathMsg);
		
		autonomyEnabled = true;
		lunabotics::ControlParams controlParamsMsg;
		controlParamsMsg.driving = autonomyEnabled;
		controlParamsMsg.next_waypoint_idx = wayIterator < waypoints.end() ? wayIterator-waypoints.begin()+1 : 0;
		controlParamsPublisher.publish(controlParamsMsg);
	}
	else {
		ROS_INFO("Path is empty");
	}
	
	delete path;
}


void controlSkidAllWheel() 
{
	if (wayIterator >= waypoints.end()) {
		ROS_INFO("Finishing route");
		finish_route();
		return;
	}
	double dx = (*wayIterator).position.x-currentPose.position.x;
	double dy = (*wayIterator).position.y-currentPose.position.y;
	double angle = geometry::normalizedAngle(atan2(dy, dx)-tf::getYaw(currentPose.orientation));
	
	lunabotics::AllWheelCommon msg;

	lunabotics::ControlParams controlParamsMsg;
	controlParamsMsg.driving = autonomyEnabled;
	
	switch (skidState) {
		case lunabotics::Telemetry::STOPPED: {
			ROS_INFO("SKID: stopped dx: %.5f dy: %.5f angle: %.5f", dx, dy, angle);
			
			if (fabs(dx) < distanceAccuracy && fabs(dy) < distanceAccuracy) {
				wayIterator++;
				if (wayIterator >= waypoints.end()) {
					finish_route();
				}
				else {
					controlParamsMsg.has_trajectory_data = true;
					controlParamsMsg.next_waypoint_idx = wayIterator < waypoints.end() ? wayIterator-waypoints.begin()+1 : 0;	
					pose_t nextWaypointPose = *wayIterator;
					ROS_INFO("Waypoint reached. Now heading towards (%.1f,%.1f)", nextWaypointPose.position.x, nextWaypointPose.position.y);
				}
			}
			else if (fabs(angle) > angleAccuracy) {
				skidState = lunabotics::Telemetry::TURNING;
			}
			else if (fabs(dx) > distanceAccuracy || fabs(dy) > distanceAccuracy) {
				skidState = lunabotics::Telemetry::DRIVING;
			}				
		}	
		break;
		case lunabotics::Telemetry::DRIVING: {	
			ROS_INFO("SKID: driving        dx: %.5f dy: %.5f angle: %.5f", dx, dy, angle);
			if (fabs(dx) < distanceAccuracy && fabs(dy) < distanceAccuracy) {
				skidState = lunabotics::Telemetry::STOPPED;
				msg.predefined_cmd = lunabotics::AllWheelControl::STOP;
			}
			else if (fabs(angle) > angleAccuracy) {
				skidState = lunabotics::Telemetry::TURNING;
			}	
			else {
				msg.predefined_cmd = lunabotics::AllWheelControl::DRIVE_FORWARD;
			}
		}
		break;
		case lunabotics::Telemetry::TURNING: {
			int direction = sign(angle);
			ROS_INFO("SKID: turning  %d (%.2f-%.2f)      dx: %.5f dy: %.5f angle: %.5f", direction, angle, angleAccuracy, dx, dy, angle);
		
			if (direction == 0) {
				skidState = lunabotics::Telemetry::STOPPED;
				msg.predefined_cmd = lunabotics::AllWheelControl::STOP;
			}
			else {
				ROS_INFO("SKID: %s", direction == -1 ? "Right" : "Left");
				if (direction == -1) {
					msg.predefined_cmd = lunabotics::AllWheelControl::TURN_CW;
				}
				else {
					msg.predefined_cmd = lunabotics::AllWheelControl::TURN_CCW;
				}
			}	
		}
		break;
		default: break;
	}
	
	controlParamsMsg.point_turn_state = skidState;
	controlParamsMsg.has_point_turn_state = true;
	controlParamsPublisher.publish(controlParamsMsg);
	allWheelCommonPublisher.publish(msg);
}

void controlSkidDiffDrive() 
{
	if (wayIterator >= waypoints.end()) {
		finish_route();
		return;
	}
	double dx = (*wayIterator).position.x-currentPose.position.x;
	double dy = (*wayIterator).position.y-currentPose.position.y;
	double angle = geometry::normalizedAngle(atan2(dy, dx)-tf::getYaw(currentPose.orientation));
	
	
	lunabotics::ControlParams controlParamsMsg;
	controlParamsMsg.driving = autonomyEnabled;
	controlParamsMsg.has_point_turn_state = true;
	lunabotics::Control controlMsg;
	controlMsg.motion.linear.x = 0;
	controlMsg.motion.linear.y = 0;
	controlMsg.motion.linear.z = 0;
	controlMsg.motion.angular.x = 0;
	controlMsg.motion.angular.y = 0;
	controlMsg.motion.angular.z = 0;
					
	switch (skidState) {
		case lunabotics::Telemetry::STOPPED: {
			ROS_INFO("SKID: stopped        dx: %.5f dy: %.5f angle: %.5f", dx, dy, angle);
			
			if (fabs(dx) < distanceAccuracy && fabs(dy) < distanceAccuracy) {
				wayIterator++;
				if (wayIterator >= waypoints.end()) {
					finish_route();
				}
				else {
					controlParamsMsg.has_trajectory_data = true;
					controlParamsMsg.next_waypoint_idx = wayIterator < waypoints.end() ? wayIterator-waypoints.begin()+1 : 0;
					pose_t nextWaypointPose = *wayIterator;
					ROS_INFO("Waypoint reached. Now heading towards (%.1f,%.1f)", nextWaypointPose.position.x, nextWaypointPose.position.y);
				}
			}
			else if (fabs(angle) > angleAccuracy) {
				skidState = lunabotics::Telemetry::TURNING;
			}
			else if (fabs(dx) > distanceAccuracy || fabs(dy) > distanceAccuracy) {
				skidState = lunabotics::Telemetry::DRIVING;
			}				
		}	
		break;
		case lunabotics::Telemetry::DRIVING: {	
		//	ROS_INFO("SKID: driving        dx: %.5f dy: %.5f angle: %.5f", dx, dy, angle);	
			if (fabs(dx) < distanceAccuracy && fabs(dy) < distanceAccuracy) {
				skidState = lunabotics::Telemetry::STOPPED;
				controlMsg.motion.linear.x = 0;
				controlMsg.motion.angular.z = 0;
			}
			else if (fabs(angle) > angleAccuracy) {
				skidState = lunabotics::Telemetry::TURNING;
			}	
			else {
				controlMsg.motion.linear.x = 1;
				controlMsg.motion.angular.z = 0;
			}
		}
		break;
		case lunabotics::Telemetry::TURNING: {
			ROS_INFO("SKID: turning        dx: %.5f dy: %.5f angle: %.5f", dx, dy, angle);
			int direction = sign(angle);
		
			if (direction == 0) {
				skidState = lunabotics::Telemetry::STOPPED;
				controlMsg.motion.linear.x = 0;
				controlMsg.motion.angular.z = 0;
			}
			else {
				ROS_INFO("SKID: %s", direction == -1 ? "Right" : "Left");
				controlMsg.motion.linear.x = 0;
				controlMsg.motion.angular.z = 1*direction;
			}	
		}
		break;
		default: break;
	}
	
	controlParamsMsg.point_turn_state = skidState;
	controlParamsPublisher.publish(controlParamsMsg);
	controlPublisher.publish(controlMsg);
}

void controlSkid() {
	//	ROS_INFO("Control mode point-turn");
		#if ROBOT_DIFF_DRIVE
			controlSkidDiffDrive();
		#else
			controlSkidAllWheel();
		#endif
}

void controlAckermannAllWheel()
{
	//////////////////////////////// ARC-TURN WITH ALL-WHEEL STEERING TEST /////////////////////
	
	if (wayIterator >= waypoints.end()) {
		finish_route();
		return;
	}
	
	//If suddenly skipped a waypoint, proceed with the next ones, don't get stuck with current
	double dist = geometry::distanceBetweenPoints((*wayIterator).position, currentPose.position);
	for (pose_arr::iterator it = wayIterator+1; it < waypoints.end(); it++) {
		double newDist = geometry::distanceBetweenPoints((*it).position, currentPose.position);
		if (newDist < dist) {
			wayIterator = it;
			dist = newDist;
		}
	}
	
	if (dist < distanceAccuracy) {
		wayIterator++;
	}
	if (wayIterator >= waypoints.end()) {
		finish_route();
		return;
	}
	
	
	double dx = (*wayIterator).position.x-currentPose.position.x;
	double dy = (*wayIterator).position.y-currentPose.position.y;
	
	if (waypoints.size() >= 2) {
		
		//In the beginning turn in place towards the second waypoint (first waypoint is at the robot's position). It helps to solve problems with pid
		if (wayIterator < waypoints.begin()+2) {
			wayIterator = waypoints.begin()+1;
			double angle = geometry::normalizedAngle(atan2(dy, dx)-tf::getYaw(currentPose.orientation));
			if (fabs(angle) > angleAccuracy) {
				ROS_WARN("Facing away from the trajectory. Turning in place");
				controlSkid();
				return;
			}
		}
		
		
		double y_err = pidGeometry.getReferenceDistance();
		lunabotics::ControlParams controlParamsMsg;
		controlParamsMsg.trajectory_point = pidGeometry.getClosestTrajectoryPoint();
		controlParamsMsg.velocity_point = pidGeometry.getReferencePoint();
		controlParamsMsg.y_err = y_err;
		controlParamsMsg.driving = autonomyEnabled;
		controlParamsMsg.t_trajectory_point = pidGeometry.getClosestTrajectoryPointInLocalFrame();
		controlParamsMsg.t_velocity_point = pidGeometry.getReferencePointInLocalFrame();
		controlParamsMsg.next_waypoint_idx = wayIterator < waypoints.end() ? wayIterator-waypoints.begin()+1 : 0;
		controlParamsPublisher.publish(controlParamsMsg);
		
		//Control law
		
		double signal;
		if (pidController->control(y_err, signal)) {
			ROS_WARN("DW %.2f", signal);
			
			double gamma1 = -signal/2;
			
			if (fabs(gamma1) < 0.00001) {
				gamma1 = 0.01;
			}
			
			point_t ICR;
			double alpha = M_PI_2-gamma1;
			double offset_y = allWheelGeometry->left_front().x;
			double offset_x = tan(alpha)*offset_y;
			ICR.x = offset_x;
			ICR.y = 0;
			ICRPublisher.publish(ICR);
			
			ROS_INFO("Alpha %f offset %f,%f", alpha, offset_x, offset_y);
			
			float velocity = 0.1;
			
			float angle_front_left;
			float angle_front_right;
			float angle_rear_left;
			float angle_rear_right;
			if (allWheelGeometry->calculateAngles(ICR, angle_front_left, angle_front_right, angle_rear_left, angle_rear_right)) {
				lunabotics::AllWheelStateROS controlMsg;
				controlMsg.steering.left_front = angle_front_left;
				controlMsg.steering.right_front = angle_front_right;
				controlMsg.steering.left_rear = angle_rear_left;
				controlMsg.steering.right_rear = angle_rear_right;
				float vel_front_left;
				float vel_front_right;
				float vel_rear_left;
				float vel_rear_right;
				if (fabs(ICR.x-currentPose.position.x) < 0.001 && fabs(ICR.y-currentPose.position.y) < 0.001) {
					//Point turn
					vel_front_left = vel_rear_right = velocity;
					vel_front_right = vel_rear_left = -velocity;
				}
				else if (!allWheelGeometry->calculateVelocities(ICR, velocity, vel_front_left, vel_front_right, vel_rear_left, vel_rear_right)) {
					vel_front_left = vel_front_right = vel_rear_left = vel_rear_right = 0;			
				}
				controlMsg.driving.left_front = vel_front_left;
				controlMsg.driving.right_front = vel_front_right;
				controlMsg.driving.left_rear = vel_rear_left;
				controlMsg.driving.right_rear = vel_rear_right;
				
				allWheelPublisher.publish(controlMsg);
			}
		}
	}
	else {
		//No need for curvature, just straight driving
		controlSkid();
	}
	return;
	
	///////////////////////////////////////////////////////////////////
	
}

void controlAckermannDiffDrive()
{
	//////////////////////////////// ARC-TURN WITH DIFFERENTIAL DRIVE TEST /////////////////////
	
	if (wayIterator >= waypoints.end()) {
		finish_route();
		return;
	}
	
	//If suddenly skipped a waypoint, proceed with the next ones, don't get stuck with current
	double dist = geometry::distanceBetweenPoints((*wayIterator).position, currentPose.position);
	for (pose_arr::iterator it = wayIterator+1; it < waypoints.end(); it++) {
		double newDist = geometry::distanceBetweenPoints((*it).position, currentPose.position);
		if (newDist < dist) {
			wayIterator = it;
			dist = newDist;
		}
	}
	
	if (dist < distanceAccuracy) {
		wayIterator++;
	}
	if (wayIterator >= waypoints.end()) {
		finish_route();
		return;
	}
	
	
	double dx = (*wayIterator).position.x-currentPose.position.x;
	double dy = (*wayIterator).position.y-currentPose.position.y;
	
	if (waypoints.size() >= 2) {
		
		//In the beginning turn in place towards the second waypoint (first waypoint is at the robot's position). It helps to solve problems with pid
		if (wayIterator < waypoints.begin()+2) {
			wayIterator = waypoints.begin()+1;
			double angle = geometry::normalizedAngle(atan2(dy, dx)-tf::getYaw(currentPose.orientation));
			if (fabs(angle) > angleAccuracy) {
				ROS_WARN("Facing away from the trajectory. Turning in place");
				controlSkid();
				return;
			}
		}
		
		
		
		double y_err = pidGeometry.getReferenceDistance();
		lunabotics::ControlParams controlParamsMsg;
		controlParamsMsg.trajectory_point = pidGeometry.getClosestTrajectoryPoint();
		controlParamsMsg.velocity_point = pidGeometry.getReferencePoint();
		controlParamsMsg.y_err = y_err;
		controlParamsMsg.driving = autonomyEnabled;
		controlParamsMsg.t_trajectory_point = pidGeometry.getClosestTrajectoryPointInLocalFrame();
		controlParamsMsg.t_velocity_point = pidGeometry.getReferencePointInLocalFrame();
		controlParamsMsg.next_waypoint_idx = wayIterator < waypoints.end() ? wayIterator-waypoints.begin()+1 : 0;
		controlParamsPublisher.publish(controlParamsMsg);
		
		//Control law
		
		double dw;
		if (pidController->control(y_err, dw)) {
			dw *= -3;
			ROS_WARN("DW %.2f", dw);
			
			//The higher angular speed, the lower linear speed is
			#pragma message("This top w is for diff drive only")
			double top_w = 1.57;
			double v = linear_speed_limit * std::max(0.0, (top_w-fabs(dw)))/top_w;
			v = std::max(0.01, v);
				
			lunabotics::Control controlMsg;
			controlMsg.motion.linear.x = v;
			controlMsg.motion.linear.y = 0;
			controlMsg.motion.linear.z = 0;
			controlMsg.motion.angular.x = 0;
			controlMsg.motion.angular.y = 0;
			controlMsg.motion.angular.z = dw;
			controlPublisher.publish(controlMsg);
		}
	}
	else {
		//No need for curvature, just straight driving
		controlSkid();
	}
	return;
	
	///////////////////////////////////////////////////////////////////
	
	
	
	
	
	
	double theta = tf::getYaw((*wayIterator).orientation) - tf::getYaw(currentPose.orientation);
	
	//Reparametrization
	double rho = sqrt(pow(dx, 2)+pow(dy, 2));
	double beta = -atan2(dy, dx);
	double alpha = -(beta+theta);
	
	
	if (fabs(rho) < 0.05 && fabs(beta) < 0.1 && fabs(alpha) < 0.1) {
		wayIterator++;
		if (wayIterator >= waypoints.end()) {
			finish_route();
		}
		else {
			pose_t nextWaypointPose = *wayIterator;
			ROS_INFO("Waypoint reached. Now heading towards (%.1f,%.1f)", nextWaypointPose.position.x, nextWaypointPose.position.y);
		}
	}
	else {
	
		//Control law
		double v = Kp*rho;
		double w = Ka*alpha+Kb*beta;
		ROS_INFO("dx:%f dy:%f theta:%f rho:%f alpha:%f beta:%f v:%f w:%f", dx, dy, theta, rho, alpha, beta, v, w);
		
		lunabotics::Control controlMsg;
		controlMsg.motion.linear.x = v;
		controlMsg.motion.linear.y = 0;
		controlMsg.motion.linear.z = 0;
		controlMsg.motion.angular.x = 0;
		controlMsg.motion.angular.y = 0;
		controlMsg.motion.angular.z = w;
		controlPublisher.publish(controlMsg);
	}
}

void controlAckermann() 
{
//	ROS_INFO("Control mode ackermann");
	#if ROBOT_DIFF_DRIVE
		controlAckermannDiffDrive();
	#else
		controlAckermannAllWheel();
	#endif
}

int main(int argc, char **argv)
{
	ros::init(argc, argv, "luna_driver");
	ros::NodeHandle nodeHandle("lunabotics");
	
	ros::Subscriber emergencySubscriber = nodeHandle.subscribe("emergency", 256, emergencyCallback);
	ros::Subscriber autonomySubscriber = nodeHandle.subscribe("autonomy", 1, autonomyCallback);
	ros::Subscriber stateSubscriber = nodeHandle.subscribe("state", 256, stateCallback);
	ros::Subscriber goalSubscriber = nodeHandle.subscribe("goal", 256, goalCallback);
	ros::Subscriber pidSubscriber = nodeHandle.subscribe("pid", sizeof(float)*3, pidCallback);
	ros::Subscriber controlModeSubscriber = nodeHandle.subscribe("control_mode", 1, controlModeCallback);
	ros::Subscriber ICRSubscriber = nodeHandle.subscribe("icr", sizeof(float)*3, ICRCallback);
	controlPublisher = nodeHandle.advertise<lunabotics::Control>("control", 256);
	pathPublisher = nodeHandle.advertise<nav_msgs::Path>("path", 256);
	ICRPublisher = nodeHandle.advertise<geometry_msgs::Point>("icr_state", sizeof(float)*2);
	allWheelPublisher = nodeHandle.advertise<lunabotics::AllWheelStateROS>("all_wheel", sizeof(float)*8);
	controlParamsPublisher = nodeHandle.advertise<lunabotics::ControlParams>("control_params", 256);
	mapClient = nodeHandle.serviceClient<nav_msgs::GetMap>("map");
	jointPublisher = nodeHandle.advertise<lunabotics::JointPositions>("joints", sizeof(float)*2*4);
	allWheelCommonPublisher = nodeHandle.advertise<lunabotics::AllWheelCommon>("all_wheel_common", sizeof(uint32_t));
	
	point_t zeroPoint; zeroPoint.x = 0; zeroPoint.y = 0;
	allWheelGeometry = new geometry::AllWheelGeometry(zeroPoint, zeroPoint, zeroPoint, zeroPoint);
		
	pidController = new lunabotics::control::PIDController(0.05, 0.1, 0.18);
	
	tf::TransformListener listener;
	
	ROS_INFO("Driver ready"); 
	
	ros::Rate loop_rate(200);
	while (ros::ok()) {
		tf::StampedTransform transform;
		point_t point;
		if (!jointStatesAcquired) {
			try {
				listener.lookupTransform("left_front_joint", "base_link", ros::Time(0), transform);
				point.x = transform.getOrigin().x();
				point.y = transform.getOrigin().y();
				allWheelGeometry->set_left_front(point);
				listener.lookupTransform("right_front_joint", "base_link", ros::Time(0), transform);
				point.x = transform.getOrigin().x();
				point.y = transform.getOrigin().y();
				allWheelGeometry->set_right_front(point);
				listener.lookupTransform("left_rear_joint", "base_link", ros::Time(0), transform);
				point.x = transform.getOrigin().x();
				point.y = transform.getOrigin().y();
				allWheelGeometry->set_left_rear(point);
				listener.lookupTransform("right_rear_joint", "base_link", ros::Time(0), transform);
				point.x = transform.getOrigin().x();
				point.y = transform.getOrigin().y();
				allWheelGeometry->set_right_rear(point);
				jointStatesAcquired = true;
			}
			catch (tf::TransformException e) {
				ROS_ERROR("%s", e.what());
			}
		}
		else {
			lunabotics::JointPositions msg;
			msg.left_front = allWheelGeometry->left_front();
			msg.left_rear = allWheelGeometry->left_rear();
			msg.right_front = allWheelGeometry->right_front();
			msg.right_rear = allWheelGeometry->right_rear();
			jointPublisher.publish(msg);
		}
		/*
	/////////////////////////////////////////////////////////////
	if (!autonomyEnabled) {
		nav_msgs::Path pathMsg;
		geometry_msgs::PoseStamped waypoint1;
		waypoint1.pose.position.x = 3;
		waypoint1.pose.position.y = 3;
		waypoint1.pose.orientation = tf::createQuaternionMsgFromYaw(0);
		geometry_msgs::PoseStamped waypoint2;
		waypoint2.pose.position.x = 5;
		waypoint2.pose.position.y = 5;
		waypoint2.pose.orientation = tf::createQuaternionMsgFromYaw(0);
		pathMsg.poses.push_back(waypoint1);
		pathMsg.poses.push_back(waypoint2);
		pathPublisher.publish(pathMsg);
		
		
		
		pose_t closestWaypoint1;
		closestWaypoint1.position.x = 3;
		closestWaypoint1.position.y = 3;
		pose_t closestWaypoint2;
		closestWaypoint2.position.x = 5;
		closestWaypoint2.position.y = 5;
		double dist1 = point_distance(currentPose.position, closestWaypoint1.position);
		double dist2 = point_distance(currentPose.position, closestWaypoint2.position);
		if (dist2 < dist1) {
			//Swap values to keep waypoint1 always the closest one
			double tmp_dist = dist1;
			dist1 = dist2;
			dist2 = tmp_dist;
			pose_t tmp_waypoint = closestWaypoint1;
			closestWaypoint1 = closestWaypoint2;
			closestWaypoint2 = tmp_waypoint;
		}
		double length = point_distance(closestWaypoint1.position, closestWaypoint2.position);
		double angle = angle_between_line_and_curr_pos(length, dist1, dist2);
		double y_err = distance_to_line(dist1, angle);		
		point_t closestTrajectoryPoint = closest_trajectory_point(length, dist1, angle, closestWaypoint1.position, closestWaypoint2.position);
		double closestTrajectoryPointAngle = atan2(closestTrajectoryPoint.y-currentPose.position.y, closestTrajectoryPoint.x-currentPose.position.x);
		//double goalAngle = atan2((*wayIterator).position.y-currentPose.position.y, (*wayIterator).position.x-currentPose.position.x);
		double angle_diff = closestTrajectoryPointAngle - tf::getYaw(currentPose.orientation);
		angle_diff = normalize_angle(angle_diff);
		
		 //goalAngle - closestTrajectoryPointAngle;
		if (angle_diff < 0) {
			y_err *= -1;
		}
		
		ROS_INFO("angle to closest %f, heading %f, diff %f", closestTrajectoryPointAngle, tf::getYaw(currentPose.orientation), angle_diff); 
		
		//ROS_INFO("local %f,%f | closest %f,%f | one %f,%f | two %f,%f | Y_err %f", currentPose.position.x,currentPose.position.y, closestTrajectoryPoint.x,closestTrajectoryPoint.y,closestWaypoint1.position.x,closestWaypoint1.position.y,closestWaypoint2.position.x,closestWaypoint2.position.y, y_err);
		
		lunabotics::ControlParams controlParamsMsg;
		controlParamsMsg.trajectory_point = closestTrajectoryPoint;
		controlParamsMsg.y_err = y_err;
		controlParamsMsg.driving = true;
		controlParamsMsg.next_waypoint_idx = wayIterator < waypoints.end() ? wayIterator-waypoints.begin()+1 : 0;
		controlParamsPublisher.publish(controlParamsMsg);
		
		
		
		ros::spinOnce();
		loop_rate.sleep();
		
		
		continue;
	}
		
		
	/////////////////////////////////////////////////////////////
		//*/
		
		
		//Whenever needed send control message
		if (autonomyEnabled) {
			//ROS_INFO("autonomous");
			if (wayIterator < waypoints.end()) {
			
				if (isnan((*wayIterator).position.x) || isnan((*wayIterator).position.y)) {
					ROS_WARN("Waypoint position undetermined");
				}
				else if (isnan(currentPose.position.x) || isnan(currentPose.position.y)) {
					ROS_WARN("Current position undetermined");
				}
				else {
					switch (controlMode) {
						case lunabotics::ACKERMANN: controlAckermann(); break;
						case lunabotics::TURN_IN_SPOT: controlSkid(); break;
						case lunabotics::CRAB: 
						//	ROS_INFO("Control mode crab");
								break;
						default:
						ROS_INFO("Control mode UNKNOWN");
						 break;
					}
				}
			}
			else {
				ROS_ERROR("Way iterator out of bounds");
				stop();
			}
		}
		
		//ROS_INFO("beat");
		
		
		ros::spinOnce();
		loop_rate.sleep();
	}
	
	delete pidController;
	delete allWheelGeometry;
	
	return 0;
}
