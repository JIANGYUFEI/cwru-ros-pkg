#include <ros/ros.h>
#include <image_transport/image_transport.h>
#include <opencv/cv.h>
#include <cv_bridge/CvBridge.h>
#include <image_geometry/pinhole_camera_model.h>
#include <tf/transform_listener.h>
#include <boost/foreach.hpp>
#include <iostream>

class BirdsEye
{
	ros::NodeHandle nh_;
	image_transport::ImageTransport it_;
	image_transport::CameraSubscriber sub_;
	image_transport::Publisher pub_;
	tf::TransformListener tf_listener_;
	sensor_msgs::CvBridge bridge_;
	image_geometry::PinholeCameraModel cam_model_;
	std::vector<std::string> frame_ids_;
	CvFont font_;

	public:
	BirdsEye(const std::vector<std::string>& frame_ids)
		: it_(nh_), frame_ids_(frame_ids)
	{
		std::string image_topic = nh_.resolveName("image_raw");
		sub_ = it_.subscribeCamera(image_topic, 1, &BirdsEye::imageCb, this);
		pub_ = it_.advertise("plan_view", 1);
		cvInitFont(&font_, CV_FONT_HERSHEY_SIMPLEX, 0.5, 0.5);
	}

	void imageCb(const sensor_msgs::ImageConstPtr& image_msg,
			const sensor_msgs::CameraInfoConstPtr& info_msg)
	{
		IplImage* image = NULL;
		try {
			image = bridge_.imgMsgToCv(image_msg, "bgr8");
		}
		catch (sensor_msgs::CvBridgeException& ex) {
			ROS_ERROR("[birds_eye] Failed to convert image");
			return;
		}

		cam_model_.fromCameraInfo(info_msg);

		BOOST_FOREACH(const std::string& frame_id, frame_ids_) {
			tf::StampedTransform transform;
			try {
				ros::Time acquisition_time = info_msg->header.stamp;
				ros::Duration timeout(1.0 / 30);
				tf_listener_.waitForTransform(cam_model_.tfFrame(), frame_id, acquisition_time, timeout);
				tf_listener_.lookupTransform(cam_model_.tfFrame(), frame_id, acquisition_time, transform);
			}
			catch (tf::TransformException& ex) {
				ROS_WARN("[birds_eye] TF exception:\n%s", ex.what());
				return;
			}

			tf::Point pt = transform.getOrigin();
			std::cout << pt << std::endl;
			cv::Point3d pt_cv(pt.x(), pt.y(), pt.z());
			cv::Point2d uv;
			cam_model_.project3dToPixel(pt_cv, uv);

			static const int RADIUS = 3;
			cvCircle(image, uv, RADIUS, CV_RGB(255,0,0), -1);
			CvSize text_size;
			int baseline;
			cvGetTextSize(frame_id.c_str(), &font_, &text_size, &baseline);
			CvPoint origin = cvPoint(uv.x - text_size.width / 2,
					uv.y - RADIUS - baseline - 3);
			cvPutText(image, frame_id.c_str(), origin, &font_, CV_RGB(255,0,0));
		}

		pub_.publish(bridge_.cvToImgMsg(image, "bgr8"));
	}
};

int main(int argc, char** argv)
{
	ros::init(argc, argv, "birds_eye");
	std::vector<std::string> frame_ids(argv + 1, argv + argc);
	BirdsEye birds_eye(frame_ids);
	ros::spin();
}
