#include "rclcpp/rclcpp.hpp"
#include "nav_msgs/msg/path.hpp"
#include "geometry_msgs/msg/pose_stamped.hpp"
#include "geometry_msgs/msg/pose.hpp"
#include "geometry_msgs/msg/point.hpp"
#include "geometry_msgs/msg/twist.hpp"
#include "geometry_msgs/msg/quaternion.hpp"
#include "sensor_msgs/msg/nav_sat_fix.hpp"
#include "nav_msgs/msg/odometry.hpp"
#include "message_filters/subscriber.h"
#include "message_filters/synchronizer.h"
#include "message_filters/sync_policies/approximate_time.h"

#include "farmbot_interfaces/action/nav.hpp"
#include "rclcpp/rclcpp.hpp"
#include "rclcpp_action/rclcpp_action.hpp"
#include "std_msgs/msg/empty.hpp"

#include "std_srvs/srv/trigger.hpp"
 
#include <iostream>
#include <thread>
#include <mutex>
#include <string>

enum class RobotState {
    Idle,
    Running,
    Paused,
    Stopped
};

std::string stateToString(RobotState state) {
    switch (state) {
        case RobotState::Idle: return "Idle";
        case RobotState::Running: return "Running";
        case RobotState::Paused: return "Paused";
        case RobotState::Stopped: return "Stopped";
        default: return "Unknown";
    }
}

float deg2rad(float deg) {
    return deg * M_PI / 180;
}

float rad2deg(float rad) {
    return rad * 180 / M_PI;
}

class Navigator : public rclcpp::Node {
    private:
        bool inited_waypoints = false;
        message_filters::Subscriber<sensor_msgs::msg::NavSatFix> fix_sub_;
        message_filters::Subscriber<nav_msgs::msg::Odometry> odom_sub_;
        std::shared_ptr<message_filters::Synchronizer<message_filters::sync_policies::ApproximateTime<sensor_msgs::msg::NavSatFix, nav_msgs::msg::Odometry>>> sync_;
        
        nav_msgs::msg::Path path_nav;
        int16_t index_wp_reached;
        RobotState state;
        rclcpp::TimerBase::SharedPtr path_timer;
        rclcpp::Publisher<nav_msgs::msg::Path>::SharedPtr path_pub;

        float max_linear_speed;
        float max_angular_speed;
        std::string name;
        std::string topic_prefix_param;
        bool autostart;

        geometry_msgs::msg::Pose current_pose_;
        sensor_msgs::msg::NavSatFix current_gps_;
        geometry_msgs::msg::Twist current_twist_;
        geometry_msgs::msg::Point target_pose_;
        rclcpp::Publisher<geometry_msgs::msg::Twist>::SharedPtr cmd_vel;
        //action_server
        using TheAction = farmbot_interfaces::action::Nav;
        using GoalHandle = rclcpp_action::ServerGoalHandle<TheAction>;
        std::shared_ptr<GoalHandle> handeler_;
        rclcpp_action::Server<TheAction>::SharedPtr action_server_;
        //services
        rclcpp::Service<std_srvs::srv::Trigger>::SharedPtr start_srv;
        rclcpp::Service<std_srvs::srv::Trigger>::SharedPtr pause_srv;
        rclcpp::Service<std_srvs::srv::Trigger>::SharedPtr stop_srv;

    public:
        Navigator(): Node("path_server",
            rclcpp::NodeOptions()
            .allow_undeclared_parameters(true)
            .automatically_declare_parameters_from_overrides(true)
        ) {
            this->action_server_ = rclcpp_action::create_server<TheAction>(this, "/navigation",
                std::bind(&Navigator::handle_goal, this, std::placeholders::_1, std::placeholders::_2),
                std::bind(&Navigator::handle_cancel, this, std::placeholders::_1),
                std::bind(&Navigator::handle_accepted, this, std::placeholders::_1)
            );

            name = "path_server";
            topic_prefix_param = "/fb";
            max_linear_speed = 0.5;
            max_angular_speed = 0.5;
            autostart = false;

            try {
                name = this->get_parameter("name").as_string(); 
                topic_prefix_param = this->get_parameter("topic_prefix").as_string();
            } catch (...) {
                RCLCPP_WARN(this->get_logger(), "No parameters %s found, using default values", name.c_str());
            }

            try {
                max_linear_speed = this->get_parameter("max_linear_speed").as_double();
                max_angular_speed = this->get_parameter("max_angular_speed").as_double();
                RCLCPP_INFO(this->get_logger(), "Max linear speed: %f, Max angular speed: %f", max_linear_speed, max_angular_speed);
            } catch (...) {
                RCLCPP_WARN(this->get_logger(), "Angluar or linear speed parameters not found, using default values of 0.5");
            }

            try {
                autostart = this->get_parameter("autostart").as_bool();
            } catch (...) {
                RCLCPP_WARN(this->get_logger(), "Autostart parameter not found, using default value of false");
            }

            index_wp_reached = 0;
            state = RobotState::Idle;
            path_timer = this->create_wall_timer(std::chrono::milliseconds(1000), std::bind(&Navigator::path_timer_callback, this));
            fix_sub_.subscribe(this, topic_prefix_param + "/loc/fix");
            odom_sub_.subscribe(this, topic_prefix_param + "/loc/odom");
            sync_ = std::make_shared<message_filters::Synchronizer<message_filters::sync_policies::ApproximateTime<sensor_msgs::msg::NavSatFix, nav_msgs::msg::Odometry>>>(10);
            sync_->connectInput(fix_sub_, odom_sub_);
            sync_->registerCallback(std::bind(&Navigator::sync_callback, this, std::placeholders::_1, std::placeholders::_2));

            path_pub = this->create_publisher<nav_msgs::msg::Path>(topic_prefix_param + "/nav/path", 10);
            cmd_vel = this->create_publisher<geometry_msgs::msg::Twist>("/cmd_vel", 10);

            //services
            start_srv = this->create_service<std_srvs::srv::Trigger>(topic_prefix_param + "/Start_Srv", std::bind(&Navigator::start_callback, this, std::placeholders::_1, std::placeholders::_2));
            pause_srv = this->create_service<std_srvs::srv::Trigger>(topic_prefix_param + "/Pause_Srv", std::bind(&Navigator::pause_callback, this, std::placeholders::_1, std::placeholders::_2));
            stop_srv = this->create_service<std_srvs::srv::Trigger>(topic_prefix_param + "/Stop_Srv", std::bind(&Navigator::stop_callback, this, std::placeholders::_1, std::placeholders::_2));
        }
    
    private:
        void sync_callback(const sensor_msgs::msg::NavSatFix::ConstSharedPtr& fix, const nav_msgs::msg::Odometry::ConstSharedPtr& odom) {
            // RCLCPP_INFO(this->get_logger(), "Sync callback");
            current_pose_ = odom->pose.pose;
            current_gps_ = *fix;
        }

        void start_callback(const std::shared_ptr<std_srvs::srv::Trigger::Request> request, std::shared_ptr<std_srvs::srv::Trigger::Response> response){
            (void)request;
            state = RobotState::Running;
            response->success = true;
            response->message = "Started";
        }

        void pause_callback(const std::shared_ptr<std_srvs::srv::Trigger::Request> request, std::shared_ptr<std_srvs::srv::Trigger::Response> response){
            (void)request;
            state = RobotState::Paused;
            response->success = true;
            response->message = "Paused";
        }

        void stop_callback(const std::shared_ptr<std_srvs::srv::Trigger::Request> request, std::shared_ptr<std_srvs::srv::Trigger::Response> response){
            (void)request;
            state = RobotState::Stopped;
            response->success = true;
            response->message = "Stopped";
        }

        rclcpp_action::GoalResponse handle_goal(const rclcpp_action::GoalUUID & uuid, std::shared_ptr<const TheAction::Goal> goal){
            goal->mission.poses;
            RCLCPP_INFO(this->get_logger(), "Received goal request");
            (void)uuid;
            return rclcpp_action::GoalResponse::ACCEPT_AND_EXECUTE;
        }

        rclcpp_action::CancelResponse handle_cancel(const std::shared_ptr<GoalHandle> goal_handle){
            RCLCPP_INFO(this->get_logger(), "Received request to cancel goal");
            (void)goal_handle;
            return rclcpp_action::CancelResponse::ACCEPT;
        }

        void handle_accepted(const std::shared_ptr<GoalHandle> goal_handle){
            state = RobotState::Idle;
            index_wp_reached = 0;
            if (handeler_ && handeler_->is_active()) {
                RCLCPP_INFO(this->get_logger(), "ABORTING PREVIOUS GOAL...");
                path_nav.poses.clear();
                stop_moving();
                handeler_->abort(std::make_shared<TheAction::Result>());
            }
            handeler_ = goal_handle;
            // this needs to return quickly to avoid blocking the executor, so spin up a new thread
            std::thread{std::bind(&Navigator::execute, this, std::placeholders::_1), goal_handle}.detach();
        }

        void execute(const std::shared_ptr<GoalHandle> goal_handle){
            // RCLCPP_INFO(this->get_logger(), "Executing goal");
            const auto goal = goal_handle->get_goal();
            auto feedback = std::make_shared<TheAction::Feedback>();
            auto result = std::make_shared<TheAction::Result>();

            rclcpp::Rate wait_rate(1);
            while(state == RobotState::Idle){
                if (autostart) {
                    state = RobotState::Running;
                    break;
                }
                fill_feedback(feedback);
                goal_handle->publish_feedback(feedback);
                wait_rate.sleep();
                RCLCPP_INFO(this->get_logger(), "Waiting for start signal");
            }

            rclcpp::Rate loop_rate(10);
            bool stop = false;
            for (auto a_pose: path_setter(goal->mission.poses)) {
                target_pose_ = a_pose.pose.position;
                RCLCPP_INFO(this->get_logger(), "Going to: %f, %f, currently at: %f, %f", target_pose_.x, target_pose_.y, current_pose_.position.x, current_pose_.position.y);
                if (stop) {
                    // path_nav.poses.clear();
                    stop_moving();
                    // goal_handle->publish_feedback(feedback);
                    // goal_handle->abort(result);
                    // state = RobotState::Idle;
                    // index_wp_reached = 0;
                    break;
                } 
                while (rclcpp::ok()){
                    if (goal_handle->is_canceling()) {
                        fill_feedback(feedback);
                        path_nav.poses.clear();
                        stop_moving();
                        goal_handle->canceled(result);
                        return;
                    } else if (!goal_handle->is_active()){
                        fill_feedback(feedback);
                        stop_moving();
                        return;
                    }
                    std::array<double, 3> nav_params = get_nav_params(max_angular_speed, max_linear_speed);
                    geometry_msgs::msg::Twist twist;
                    twist.linear.x = nav_params[0];
                    twist.angular.z = nav_params[1];
                    current_twist_ = twist;
                    cmd_vel->publish(twist);
                    fill_feedback(feedback);
                    RCLCPP_INFO(this->get_logger(), "Going to: %f, %f, currently at: %f, %f", target_pose_.x, target_pose_.y, current_pose_.position.x, current_pose_.position.y);
                    goal_handle->publish_feedback(feedback);
                    loop_rate.sleep();
                    if (nav_params[2] < 0.1) {
                        break;
                    }
                    if (state == RobotState::Paused) {
                        stop_moving();
                        while (state == RobotState::Paused) {
                            fill_feedback(feedback);
                            goal_handle->publish_feedback(feedback);
                            wait_rate.sleep();
                            RCLCPP_INFO(this->get_logger(), "Robot is paused");
                        }
                    } else if (state == RobotState::Stopped) {
                        // stop_moving();
                        RCLCPP_INFO(this->get_logger(), "Robot received stop signal");
                        stop = true;
                    }
                }
                index_wp_reached++; 
            }
            // Goal is done, send success message
            if (rclcpp::ok()) {
                fill_feedback(feedback);
                goal_handle->publish_feedback(feedback); 
                fill_result(result);
                goal_handle->succeed(result);
                RCLCPP_INFO(this->get_logger(), "Goal succeeded");
                RCLCPP_INFO(this->get_logger(), "Last index: %d", index_wp_reached);
                state = RobotState::Idle;
                index_wp_reached = 0;
            }
        }


        void fill_feedback(TheAction::Feedback::SharedPtr feedback){
            feedback->current_position = current_gps_;
            feedback->current_vel = current_twist_;
            feedback->index_wp_reached = index_wp_reached;
        }

        void fill_result(TheAction::Result::SharedPtr result){
            result->current_position = current_gps_;
            result->current_vel = current_twist_;
            result->index_wp_reached = index_wp_reached;
        }
        
        std::array<double, 3> get_nav_params(double angle_max=1.0, double velocity_max=1.0) {
            double distance = std::sqrt(
                std::pow(target_pose_.x - current_pose_.position.x, 2) + 
                std::pow(target_pose_.y - current_pose_.position.y, 2));
            double velocity = 0.2 * distance;
            double preheading = std::atan2(
                target_pose_.y - current_pose_.position.y, 
                target_pose_.x - current_pose_.position.x);
            double orientation = std::atan2(2 * (current_pose_.orientation.w * current_pose_.orientation.z + 
                                                current_pose_.orientation.x * current_pose_.orientation.y), 
                                            1 - 2 * (std::pow(current_pose_.orientation.y, 2) + std::pow(current_pose_.orientation.z, 2)));
            RCLCPP_INFO(this->get_logger(), "Desired Heading: %f, Current Heading: %f", rad2deg(preheading), rad2deg(orientation));
            double heading = preheading - orientation;
            double heading_corrected = std::atan2(std::sin(heading), std::cos(heading));
            //print desired heading and current heading
            // RCLCPP_INFO(this->get_logger(), "Desired heading: %f, Current heading: %f", heading, orientation);
            double angular = std::max(-angle_max, std::min(heading_corrected, angle_max));
            velocity = std::max(-velocity_max, std::min(velocity, velocity_max));
            return {velocity, angular, distance};
        }

        void stop_moving() {
            geometry_msgs::msg::Twist twist;
            twist.linear.x = 0.0;
            twist.angular.z = 0.0;
            cmd_vel->publish(twist);
        }
        std::vector<geometry_msgs::msg::PoseStamped> path_setter(const std::vector<geometry_msgs::msg::PoseStamped>& poses){
            path_nav.poses.clear();
            for (const geometry_msgs::msg::PoseStamped& element : poses) {
                geometry_msgs::msg::PoseStamped a_pose = element;
                a_pose.header.stamp = this->now();
                a_pose.header.frame_id = "map";
                path_nav.poses.push_back(a_pose);
            }
            inited_waypoints = true;
            return path_nav.poses;
        }
        void path_timer_callback(){
            path_nav.header.stamp = this->now();
            path_nav.header.frame_id = "map";
            if (inited_waypoints){
                path_pub->publish(path_nav);
            }
        }
    public:
        void processKeyboardInput(char key) {
            RCLCPP_INFO(this->get_logger(), "Key Pressed: %c", key);
            // Add your logic to handle the key press here
        }
};


class KeyHandler : public rclcpp::Node {
    public:
        KeyHandler(): Node("key_handler") {
            auto callback = [this](char key) {
                this->processKeyboardInput(key);
            };
            RCLCPP_INFO(this->get_logger(), "Press a key to continue...");
            std::thread([callback]() {
                char key;
                while (std::cin >> key) {
                    callback(key);
                }
            }).detach();
        }

        void processKeyboardInput(char key) {
            RCLCPP_INFO(this->get_logger(), "Key Pressed: %c", key);
            // Add your logic to handle the key press here
        }
};


int main(int argc, char *argv[]) {
    rclcpp::init(argc, argv);
    rclcpp::executors::MultiThreadedExecutor executor;
    auto node = std::make_shared<Navigator>();
    // auto key_handler = std::make_shared<KeyHandler>();
    try {
        executor.add_node(node);
        // executor.add_node(key_handler);
        executor.spin();
    } catch (const std::exception &e) {
        RCLCPP_ERROR(node->get_logger(), e.what());
    }

    rclcpp::shutdown();
    return 0;
}