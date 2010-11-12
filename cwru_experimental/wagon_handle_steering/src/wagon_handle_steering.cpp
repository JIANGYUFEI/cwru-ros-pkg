#include <math.h>
#include <wagon_handle_steering/wagon_handle_steering.h>
#include <pluginlib/class_list_macros.h>

PLUGINLIB_DECLARE_CLASS(wagon_handle_steering, WagonHandleSteering, wagon_handle_steering::WagonHandleSteering, nav_core::BaseLocalPlanner)

namespace wagon_handle_steering {
  WagonHandleSteering::WagonHandleSteering(): tf_(NULL), costmap_ros_(NULL) {}

  void WagonHandleSteering::initialize(std::string name, tf::TransformListener* tf, costmap_2d::Costmap2DROS* costmap_ros){
    tf_ = tf;
    costmap_ros_ = costmap_ros;
    current_waypoint_ = 0;
    started_reorienting_ = false;
    goal_reached_time_ = ros::Time::now();
    ros::NodeHandle node_private("~/" + name);

    collision_planner_.initialize(name, tf_, costmap_ros_);

    node_private.param("handle_length", handle_length_, 1.0);
    node_private.param("reorient_dist", reorient_dist_, .25);
    node_private.param("rotate_in_place_heading", rotate_in_place_head_, 0.2);
    node_private.param("rotate_in_place_distance", rotate_in_place_dist_, 0.1);
    node_private.param("desired_speed", desired_speed_, 0.5);

    node_private.param("tolerance_trans", tolerance_trans_, 0.02);
    node_private.param("tolerance_rot", tolerance_rot_, 0.04);
    node_private.param("tolerance_timeout", tolerance_timeout_, 0.5);

    node_private.param("max_vel_lin", max_vel_lin_, 0.9);
    node_private.param("max_vel_th", max_vel_th_, 1.4);

    node_private.param("min_vel_lin", min_vel_lin_, 0.1);
    node_private.param("min_vel_th", min_vel_th_, 0.0);
    node_private.param("min_in_place_vel_th", min_in_place_vel_th_, 0.0);
    node_private.param("in_place_trans_vel", in_place_trans_vel_, 0.0);

    node_private.param("trans_stopped_velocity", trans_stopped_velocity_, 1e-4);
    node_private.param("rot_stopped_velocity", rot_stopped_velocity_, 1e-4);

    ros::NodeHandle node;
    odom_sub_ = node.subscribe<nav_msgs::Odometry>("odom", 1, boost::bind(&WagonHandleSteering::odomCallback, this, _1));
    vel_pub_ = node.advertise<geometry_msgs::Twist>("cmd_vel", 10);

    ROS_DEBUG("Initialized");
  }

  void WagonHandleSteering::odomCallback(const nav_msgs::Odometry::ConstPtr& msg){
    //we assume that the odometry is published in the frame of the base
    boost::mutex::scoped_lock lock(odom_lock_);
    base_odom_.twist.twist.linear.x = msg->twist.twist.linear.x;
    base_odom_.twist.twist.linear.y = msg->twist.twist.linear.y;
    base_odom_.twist.twist.angular.z = msg->twist.twist.angular.z;
    ROS_DEBUG("In the odometry callback with velocity values: (%.2f, %.2f, %.2f)",
        base_odom_.twist.twist.linear.x, base_odom_.twist.twist.linear.y, base_odom_.twist.twist.angular.z);
  }

  double WagonHandleSteering::headingDiff(double x, double y, double pt_x, double pt_y, double heading)
  {
    double v1_x = x - pt_x;
    double v1_y = y - pt_y;
    double v2_x = cos(heading);
    double v2_y = sin(heading);

    double perp_dot = v1_x * v2_y - v1_y * v2_x;
    double dot = v1_x * v2_x + v1_y * v2_y;

    //get the signed angle
    double vector_angle = atan2(perp_dot, dot);

    return -1.0 * vector_angle;
  }

  bool WagonHandleSteering::stopped(){
    //copy over the odometry information
    nav_msgs::Odometry base_odom;
    {
      boost::mutex::scoped_lock lock(odom_lock_);
      base_odom = base_odom_;
    }

    return fabs(base_odom.twist.twist.angular.z) <= rot_stopped_velocity_
      && fabs(base_odom.twist.twist.linear.x) <= trans_stopped_velocity_
      && fabs(base_odom.twist.twist.linear.y) <= trans_stopped_velocity_;
  }

  double WagonHandleSteering::distance2D(const tf::Pose& p1, const tf::Pose& p2) {
  	double temp1 = p2.getOrigin().x() - p1.getOrigin().x();
	double temp2 = p2.getOrigin().y() - p1.getOrigin().y();

	double sq_result = temp1*temp1 + temp2*temp2;
	double result = sqrt(sq_result);

	return result;
  }

  bool WagonHandleSteering::computeVelocityCommands(geometry_msgs::Twist& cmd_vel){
    //get the current pose of the robot in the fixed frame
    double heading = 0.0;
    double speed = 0.0;
    double x,y;
    tf::Stamped<tf::Pose> robot_pose;
    if(!costmap_ros_->getRobotPose(robot_pose)){
      ROS_ERROR("Can't get robot pose");
      geometry_msgs::Twist empty_twist;
      cmd_vel = empty_twist;
      return false;
    }

    //we want to compute a velocity command based on our current waypoint
    tf::Stamped<tf::Pose> target_pose;
    tf::poseStampedMsgToTF(global_plan_[current_waypoint_], target_pose);

    ROS_DEBUG("WagonHandleSteering: current robot pose %f %f ==> %f", robot_pose.getOrigin().x(), robot_pose.getOrigin().y(), tf::getYaw(robot_pose.getRotation()));
    ROS_DEBUG("WagonHandleSteering: target robot pose %f %f ==> %f", target_pose.getOrigin().x(), target_pose.getOrigin().y(), tf::getYaw(target_pose.getRotation()));

    //get the distance between the two poses
    double distance = distance2D(target_pose, robot_pose);

    if (distance < handle_length_) {
    	if ((distance < reorient_dist_) || started_reorienting_) {
		started_reorienting_ = true;
		heading = tf::getYaw(target_pose.getRotation());
		ROS_DEBUG("WagonHandleSteering: Reorienting to the desired heading");
		speed = 0.0;
	} else if (!started_reorienting_) {
		heading = -1.0 * atan2(target_pose.getOrigin().x(), target_pose.getOrigin().y());
		ROS_DEBUG("WagonHandleSteering: Heading directly towards the goal"); 
		speed = exp(-1.0 * (handle_length_ / distance)) * desired_speed_;
	}
	ROS_DEBUG("WagonHandleSteering: Goal is within the handle length. Heading directly towards the goal on a heading of %f", heading);
    } else {
    
    }
    geometry_msgs::Twist limit_vel = limitTwist(diff);

    geometry_msgs::Twist test_vel = limit_vel;
    bool legal_traj = collision_planner_.checkTrajectory(test_vel.linear.x, test_vel.linear.y, test_vel.angular.z, true);

    double scaling_factor = 1.0;
    double ds = scaling_factor / samples_;

    //let's make sure that the velocity command is legal... and if not, scale down
    if(!legal_traj){
      for(int i = 0; i < samples_; ++i){
        test_vel.linear.x = limit_vel.linear.x * scaling_factor;
        test_vel.linear.y = limit_vel.linear.y * scaling_factor;
        test_vel.angular.z = limit_vel.angular.z * scaling_factor;
        test_vel = limitTwist(test_vel);
        if(collision_planner_.checkTrajectory(test_vel.linear.x, test_vel.linear.y, test_vel.angular.z, false)){
          legal_traj = true;
          break;
        }
        scaling_factor -= ds;
      }
    }

    if(!legal_traj){
      ROS_ERROR("Not legal (%.2f, %.2f, %.2f)", limit_vel.linear.x, limit_vel.linear.y, limit_vel.angular.z);
      geometry_msgs::Twist empty_twist;
      cmd_vel = empty_twist;
      return false;
    }

    //if it is legal... we'll pass it on
    cmd_vel = test_vel;

    //if we haven't reached our goal... we'll update time
    if (fabs(diff.linear.x) > tolerance_trans_ || fabs(diff.linear.y) > tolerance_trans_ || fabs(diff.angular.z) > tolerance_rot_)
      goal_reached_time_ = ros::Time::now();
    else if(current_waypoint_ < (global_plan_.size() - 1)){
      //if we're not on the last waypoint... but we've reached the next waypoint in the plan... we'll update our current waypoint
      goal_reached_time_ = ros::Time::now();
      current_waypoint_++;
    }
    //check if we've reached our goal for long enough to succeed
    else if(goal_reached_time_ + ros::Duration(tolerance_timeout_) < ros::Time::now()){
      geometry_msgs::Twist empty_twist;
      cmd_vel = empty_twist;
    }

    return true;
  }

  bool WagonHandleSteering::setPlan(const std::vector<geometry_msgs::PoseStamped>& global_plan){
    current_waypoint_ = 0;
    goal_reached_time_ = ros::Time::now();
    if(!transformGlobalPlan(*tf_, global_plan, *costmap_ros_, costmap_ros_->getGlobalFrameID(), global_plan_)){
      ROS_ERROR("Could not transform the global plan to the frame of the controller");
      return false;
    }
    return true;
  }

  bool WagonHandleSteering::isGoalReached(){
    if(goal_reached_time_ + ros::Duration(tolerance_timeout_) < ros::Time::now() && stopped()){
      return true;
    }
    return false;
  }

  geometry_msgs::Twist WagonHandleSteering::diff2D(const tf::Pose& pose1, const tf::Pose& pose2)
  {
    geometry_msgs::Twist res;
    tf::Pose diff = pose2.inverse() * pose1;
    res.linear.x = diff.getOrigin().x();
    res.linear.y = diff.getOrigin().y();
    res.angular.z = tf::getYaw(diff.getRotation());

    if(holonomic_ || (fabs(res.linear.x) <= tolerance_trans_ && fabs(res.linear.y) <= tolerance_trans_))
      return res;

    //in the case that we're not rotating to our goal position and we have a non-holonomic robot
    //we'll need to command a rotational velocity that will help us reach our desired heading
    
    //we want to compute a goal based on the heading difference between our pose and the target
    double yaw_diff = headingDiff(pose1.getOrigin().x(), pose1.getOrigin().y(), 
        pose2.getOrigin().x(), pose2.getOrigin().y(), tf::getYaw(pose2.getRotation()));

    //we'll also check if we can move more effectively backwards
    double neg_yaw_diff = headingDiff(pose1.getOrigin().x(), pose1.getOrigin().y(), 
        pose2.getOrigin().x(), pose2.getOrigin().y(), M_PI + tf::getYaw(pose2.getRotation()));

    //check if its faster to just back up
    if(fabs(neg_yaw_diff) < fabs(yaw_diff)){
      ROS_DEBUG("Negative is better: %.2f", neg_yaw_diff);
      yaw_diff = neg_yaw_diff;
    }

    //compute the desired quaterion
    tf::Quaternion rot_diff = tf::createQuaternionFromYaw(yaw_diff);
    tf::Quaternion rot = pose2.getRotation() * rot_diff;
    tf::Pose new_pose = pose1;
    new_pose.setRotation(rot);

    diff = pose2.inverse() * new_pose;
    res.linear.x = diff.getOrigin().x();
    res.linear.y = diff.getOrigin().y();
    res.angular.z = tf::getYaw(diff.getRotation());
    return res;
  }


  geometry_msgs::Twist WagonHandleSteering::limitTwist(const geometry_msgs::Twist& twist)
  {
    geometry_msgs::Twist res = twist;
    res.linear.x *= K_trans_;
    if(!holonomic_)
      res.linear.y = 0.0;
    else    
      res.linear.y *= K_trans_;
    res.angular.z *= K_rot_;

    //make sure to bound things by our velocity limits
    double lin_overshoot = sqrt(res.linear.x * res.linear.x + res.linear.y * res.linear.y) / max_vel_lin_;
    double lin_undershoot = min_vel_lin_ / sqrt(res.linear.x * res.linear.x + res.linear.y * res.linear.y);
    if (lin_overshoot > 1.0) 
    {
      res.linear.x /= lin_overshoot;
      res.linear.y /= lin_overshoot;
    }

    if(lin_undershoot > 1.0)
    {
      res.linear.x *= lin_undershoot;
      res.linear.y *= lin_undershoot;
    }

    if (fabs(res.angular.z) > max_vel_th_) res.angular.z = max_vel_th_ * sign(res.angular.z);
    if (fabs(res.angular.z) < min_vel_th_) res.angular.z = min_vel_th_ * sign(res.angular.z);

    if(fabs(res.linear.x) < in_place_trans_vel_ && fabs(res.linear.y) < in_place_trans_vel_){
      if (fabs(res.angular.z) < min_in_place_vel_th_) res.angular.z = min_in_place_vel_th_ * sign(res.angular.z);
    }

    ROS_DEBUG("Angular command %f", res.angular.z);
    return res;
  }

  bool WagonHandleSteering::transformGlobalPlan(const tf::TransformListener& tf, const std::vector<geometry_msgs::PoseStamped>& global_plan, 
      const costmap_2d::Costmap2DROS& costmap, const std::string& global_frame,
      std::vector<geometry_msgs::PoseStamped>& transformed_plan){
    const geometry_msgs::PoseStamped& plan_pose = global_plan[0];

    transformed_plan.clear();

    try{
      if (!global_plan.size() > 0)
      {
        ROS_ERROR("Recieved plan with zero length");
        return false;
      }

      tf::StampedTransform transform;
      tf.lookupTransform(global_frame, ros::Time(), 
          plan_pose.header.frame_id, plan_pose.header.stamp, 
          plan_pose.header.frame_id, transform);

      tf::Stamped<tf::Pose> tf_pose;
      geometry_msgs::PoseStamped newer_pose;
      //now we'll transform until points are outside of our distance threshold
      for(unsigned int i = 0; i < global_plan.size(); ++i){
        const geometry_msgs::PoseStamped& pose = global_plan[i];
        poseStampedMsgToTF(pose, tf_pose);
        tf_pose.setData(transform * tf_pose);
        tf_pose.stamp_ = transform.stamp_;
        tf_pose.frame_id_ = global_frame;
        poseStampedTFToMsg(tf_pose, newer_pose);

        transformed_plan.push_back(newer_pose);
      }
    }
    catch(tf::LookupException& ex) {
      ROS_ERROR("No Transform available Error: %s\n", ex.what());
      return false;
    }
    catch(tf::ConnectivityException& ex) {
      ROS_ERROR("Connectivity Error: %s\n", ex.what());
      return false;
    }
    catch(tf::ExtrapolationException& ex) {
      ROS_ERROR("Extrapolation Error: %s\n", ex.what());
      if (global_plan.size() > 0)
        ROS_ERROR("Global Frame: %s Plan Frame size %d: %s\n", global_frame.c_str(), (unsigned int)global_plan.size(), global_plan[0].header.frame_id.c_str());

      return false;
    }

    return true;
  }
};
