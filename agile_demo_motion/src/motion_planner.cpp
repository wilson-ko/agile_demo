#include "motion_planner.hpp"
#include "util.hpp"

#include <dr_eigen/eigen.hpp>
#include <dr_param/param.hpp>
#include <eigen_conversions/eigen_kdl.h>
#include <controller_manager_msgs/SwitchController.h>

namespace agile_demo_motion {

MotionPlanner::MotionPlanner(ros::NodeHandle & node) :
	joint_action_client{node, "/pos_based_pos_traj_controller/follow_joint_trajectory", true},
	tracik_solver{
		dr::getParam<std::string>(node, "chain_start", "base_link"),
		dr::getParam<std::string>(node, "chain_end", "ee_link"),
		dr::getParam<std::string>(node, "urdf_param", "/robot_description"),
		dr::getParam<double>(node, "timeout", 2.0),
		dr::getParam<double>(node, "eps", 1e-5),
		TRAC_IK::SolveType::Distance
	}
{
	if (!tracik_solver.getKDLChain(kdl_chain)) {
		throw std::runtime_error("There was no valid KDL chain found.");
	}

	joint_names       = dr::getParam<std::vector<std::string>>(node, "/pos_based_pos_traj_controller/joints", joint_names);
	controller_client = node.serviceClient<controller_manager_msgs::SwitchController>("/controller_manager/switch_controller", true);
	joint_state_sub   = node.subscribe("/joint_states", 1, &MotionPlanner::jointStateCallback, this);

	joint_action_client.waitForServer();

	if (!controller_client.exists()) throw std::runtime_error("Controller manager is not running.");

	controller_manager_msgs::SwitchController srv;
	srv.request.start_controllers.push_back("pos_based_pos_traj_controller");
	srv.request.stop_controllers.push_back("vel_based_pos_traj_controller");
	srv.request.strictness = 0;

	if(!controller_client.call(srv)) throw std::runtime_error("Failed to switch controllers.");

	ROS_INFO_STREAM("Motion planner is initialised!");
}

sensor_msgs::JointState MotionPlanner::currentJointState() {
	return *current_joint_state;
}

bool MotionPlanner::moveToGoal(control_msgs::FollowJointTrajectoryGoal const & goal, double timeout) {
	joint_action_client.sendGoal(goal);
	joint_action_client.waitForResult(ros::Duration(timeout));
	if (joint_action_client.getState() == actionlib::SimpleClientGoalState::SUCCEEDED) {
		ROS_INFO_STREAM("Successfully executed trajectory.");
		return true;
	} else {
		ROS_ERROR_STREAM("Failed to execute trajectory, status is " << joint_action_client.getState().toString());
		return false;
	}
}

bool MotionPlanner::moveToGoal(Eigen::Isometry3d const & goal) {
	KDL::Frame kdl_goal;
	tf::transformEigenToKDL(goal, kdl_goal);

	while (!current_joint_state) {
		ROS_WARN_THROTTLE(5, "Still waiting for joint state message.");
		ros::spinOnce();
	}

	KDL::JntArray result(kdl_chain.getNrOfJoints());
	KDL::JntArray kdl_current_joint_state = toKDLJoints(current_joint_state->position);
	int status = tracik_solver.CartToJnt(kdl_current_joint_state, kdl_goal, result);
	if (status < 0) {
		ROS_ERROR_STREAM("Inverse kinematics failed, status is " << status);
		return false;
	}

	control_msgs::FollowJointTrajectoryGoal action_goal;
	try {
		std::vector<double> joint_pos = fromKDLJoints(result);
		action_goal = constructGoal(
			joint_pos,
			{{1.0, 1.0, 1.0, 1.0, 1.0, 1.0}},
			joint_names,
			distance(joint_pos, current_joint_state->position)
		);
	} catch (std::runtime_error const & e) {
		ROS_ERROR_STREAM(e.what());
		return false;
	}

	return moveToGoal(action_goal);
}

void MotionPlanner::jointStateCallback(sensor_msgs::JointState const & joint_state) {
	current_joint_state = joint_state;
}

}

int main(int argc, char** argv) {
	ros::init(argc, argv, "motion_planner");
	ros::NodeHandle node_handle{"~"};

	Eigen::Isometry3d pose = Eigen::Translation3d{0.162, 0.334, 0.128} * Eigen::Quaterniond(0.487, -0.500, 0.512, 0.500);
	agile_demo_motion::MotionPlanner motion_planner{node_handle};
	motion_planner.moveToGoal(pose);

	ros::spin();
	return 0;

}
