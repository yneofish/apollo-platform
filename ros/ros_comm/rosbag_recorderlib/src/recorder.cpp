/*********************************************************************
* Software License Agreement (BSD License)
*
*  Copyright (c) 2008, Willow Garage, Inc.
*  All rights reserved.
*
*  Redistribution and use in source and binary forms, with or without
*  modification, are permitted provided that the following conditions
*  are met:
*
*   * Redistributions of source code must retain the above copyright
*     notice, this list of conditions and the following disclaimer.
*   * Redistributions in binary form must reproduce the above
*     copyright notice, this list of conditions and the following
*     disclaimer in the documentation and/or other materials provided
*     with the distribution.
*   * Neither the name of Willow Garage, Inc. nor the names of its
*     contributors may be used to endorse or promote products derived
*     from this software without specific prior written permission.
*
*  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
*  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
*  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
*  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
*  COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
*  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
*  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
*  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
*  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
*  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
*  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
*  POSSIBILITY OF SUCH DAMAGE.
********************************************************************/

#include "rosbag/recorder.h"

#include <sys/stat.h>
#include <boost/filesystem.hpp>
// Boost filesystem v3 is default in 1.46.0 and above
// Fallback to original posix code (*nix only) if this is not true
#if BOOST_FILESYSTEM_VERSION < 3
#include <sys/statvfs.h>
#endif
#include <time.h>

#include <queue>
#include <set>
#include <sstream>
#include <string>

#include <boost/foreach.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/regex.hpp>
#include <boost/thread.hpp>
#include <boost/python.hpp>
#include <boost/thread/xtime.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/ini_parser.hpp>
#include <boost/date_time/local_time/local_time.hpp>

#include <ros/ros.h>
#include <rosbag/view.h>
#include "std_msgs/String.h"
#include <topic_tools/shape_shifter.h>

#include "ros/network.h"
#include "ros/xmlrpc_manager.h"
#include "XmlRpc.h"

#define foreach BOOST_FOREACH

using std::cout;
using std::endl;
using std::set;
using std::string;
using std::vector;
using boost::shared_ptr;
using ros::Time;

namespace rosbag {
// OutgoingMessage
OutgoingMessage::OutgoingMessage(
    string const& _topic, 
    topic_tools::ShapeShifter::ConstPtr _msg, 
    boost::shared_ptr<ros::M_string> _connection_header, 
    Time _time) :
    topic(_topic), msg(_msg), connection_header(_connection_header), time(_time)
{
}

// OutgoingQueue
OutgoingQueue::OutgoingQueue(
    string const& _filename, 
    std::queue<OutgoingMessage>* _queue, 
    Time _time) :
    filename(_filename), queue(_queue), time(_time)
{
}

// RecorderOptions
RecorderOptions::RecorderOptions() :
    trigger(false),
    record_all(false),
    regex(false),
    do_exclude(false),
    quiet(false),
    append_date(true),
    snapshot(false),
    verbose(false),
    compression(compression::LZ4),
    prefix(""),
    path(""),
    sub_path(""),
    name(""),
    exclude_regex(),
    buffer_size(1048576 * 256),
    chunk_size(1024 * 768),
    limit(0),
    split(false),
    max_size(0),
    max_duration(-1.0),
    min_space(1073741824),
    node("")
{
}

// Recorder
Recorder::Recorder(RecorderOptions const& options) :
    options_(options),
    num_subscribers_(0),
    exit_code_(0),
    queue_size_(0),
    split_count_(0),
    writing_enabled_(true)
{
}

int Recorder::run() {
    if (options_.trigger) {
        doTrigger();
        return 0;
    }

    if (options_.topics.size() == 0) {
        // Make sure limit is not specified with automatic topic subscription
        if (options_.limit > 0) {
            fprintf(stderr, "Specifing a count is not valid with automatic topic subscription.\n");
            return 1;
        }

        // Make sure topics are specified
        if (!options_.record_all && (options_.node == std::string(""))) {
            fprintf(stderr, "No topics specified.\n");
            return 1;
        }
    }

    int argc_ = 0;
    char** argv_ = NULL;
    ros::init(argc_, argv_, options_.prefix, ros::init_options::AnonymousName);
    ros::NodeHandle nh;
    if (!nh.ok()) {
        return 0;
    }

    last_buffer_warn_ = Time();
    queue_ = new std::queue<OutgoingMessage>;

    // Subscribe to each topic
    if (!options_.regex) {
    	foreach(string const& topic, options_.topics) {
            subscribe(topic);
        }
    }

    if (! ros::Time::waitForValid(ros::WallDuration(2.0))) {
        ROS_WARN("/use_sim_time set to true and no clock published.  Still waiting for valid time...");
    }

    ros::Time::waitForValid();

    start_time_ = ros::Time::now();

    // Don't bother doing anything if we never got a valid time
    if (!nh.ok()) {
        return 0;
    }

    ros::Subscriber trigger_sub;

    // Spin up a thread for writing to the file
    boost::thread record_thread;
    if (options_.snapshot) {
        record_thread = boost::thread(boost::bind(&Recorder::doRecordSnapshotter, this));

        // Subscribe to the snapshot trigger
        trigger_sub = nh.subscribe<std_msgs::Empty>(
            "snapshot_trigger", 100, boost::bind(&Recorder::snapshotTrigger, this, _1)
        );
    }
    else {
        record_thread = boost::thread(boost::bind(&Recorder::doRecord, this));
    }

    ros::Timer check_master_timer;
    if (options_.record_all || options_.regex || (options_.node != std::string(""))) {
        // check for master first
        doCheckMaster(ros::TimerEvent(), nh);
        check_master_timer = nh.createTimer(
            ros::Duration(1.0), 
            boost::bind(&Recorder::doCheckMaster, 
            this, 
            _1, 
            boost::ref(nh))
        );
    }

    ros::MultiThreadedSpinner s(10);
    ros::spin(s);

    queue_condition_.notify_all();

    record_thread.join();

    delete queue_;

    return exit_code_;
}

shared_ptr<ros::Subscriber> Recorder::subscribe(string const& topic) {
    ROS_INFO("Subscribing to %s", topic.c_str());

    ros::NodeHandle nh;
    shared_ptr<int> count(new int(options_.limit));
    shared_ptr<ros::Subscriber> sub(new ros::Subscriber);

    ros::SubscribeOptions ops;
    ops.topic = topic;
    ops.queue_size = 100;
    ops.md5sum = ros::message_traits::md5sum<topic_tools::ShapeShifter>();
    ops.datatype = ros::message_traits::datatype<topic_tools::ShapeShifter>();
    ops.helper = ros::SubscriptionCallbackHelperPtr(
        new ros::SubscriptionCallbackHelperT<const ros::MessageEvent<topic_tools::ShapeShifter const>& >(
            boost::bind(&Recorder::doQueue, this, _1, topic, sub, count)
        )
    );
    *sub = nh.subscribe(ops);

    currently_recording_.insert(topic);
    num_subscribers_++;

    return sub;
}

std::string Recorder::getWritingFilename() {
    return write_filename_;
}

bool Recorder::isSubscribed(string const& topic) const {
    return currently_recording_.find(topic) != currently_recording_.end();
}

bool Recorder::shouldSubscribeToTopic(std::string const& topic, bool from_node) {
    // ignore already known topics
    if (isSubscribed(topic)) {
        return false;
    }
    if (topic == "/eap/data_recorder/update_path") {
        return true;
    }
    if (topic == "/eap/data_recorder/cmd") {
        return true;
    }

    // subtract exclusion regex, if any
    if (options_.do_exclude && boost::regex_match(topic, options_.exclude_regex)) {
        return false;
    }

    if (options_.record_all || from_node) {
        return true;
    }
    
    if (options_.regex) {
        // Treat the topics as regular expressions
        foreach(string const& regex_str, options_.topics) {
            boost::regex e(regex_str);
            boost::smatch what;
            if (boost::regex_match(topic, what, e, boost::match_extra)) {
                return true;
            }
        }
    }
    else {
        foreach(string const& t, options_.topics) {
            if (t == topic) {
                return true;
            }
        }
    }
    
    return false;
}

template<class T>
std::string Recorder::timeToStr(T ros_t) {
    std::stringstream msg;
    const boost::posix_time::ptime now=
        boost::posix_time::second_clock::local_time();
    boost::posix_time::time_facet *const f=
        new boost::posix_time::time_facet("%Y%m%d%H%M%S");
    msg.imbue(std::locale(msg.getloc(),f));
    msg << now;
    return msg.str();
}

//! Callback to be invoked to save messages into a queue
void Recorder::doQueue(
    const ros::MessageEvent<topic_tools::ShapeShifter const>& msg_event, 
    string const& topic, 
    shared_ptr<ros::Subscriber> subscriber, 
    shared_ptr<int> count) {
    //void Recorder::doQueue(topic_tools::ShapeShifter::ConstPtr msg, string const& topic, shared_ptr<ros::Subscriber> subscriber, shared_ptr<int> count) {
    Time rectime = Time::now();
    
    if (options_.verbose)
        cout << "Received message on topic " << subscriber->getTopic() << endl;

    OutgoingMessage out(
        topic, 
        msg_event.getMessage(), 
        msg_event.getConnectionHeaderPtr(), 
        rectime
    );
    
    {
        boost::mutex::scoped_lock lock(queue_mutex_);
        queue_->push(out);
        queue_size_ += out.msg->size();
        
        // Check to see if buffer has been exceeded
        while (options_.buffer_size > 0 && queue_size_ > options_.buffer_size) {
            OutgoingMessage drop = queue_->front();
            queue_->pop();
            queue_size_ -= drop.msg->size();

            if (!options_.snapshot) {
                Time now = Time::now();
                if (now > last_buffer_warn_ + ros::Duration(5.0)) {
                    ROS_WARN("rosbag record buffer exceeded.  Dropping oldest queued message.");
                    last_buffer_warn_ = now;
                }
            }
        }
    }
  
    if (!options_.snapshot) {
        queue_condition_.notify_all();
    }

    // If we are book-keeping count, decrement and possibly shutdown
    if ((*count) > 0) {
        (*count)--;
        if ((*count) == 0) {
            subscriber->shutdown();

            num_subscribers_--;

            if (num_subscribers_ == 0) {
                ros::shutdown();
            }
        }
    }
}

void Recorder::updateFilenames() {
    vector<string> parts;

    std::string path = options_.path;
    std::string sub_path = options_.sub_path;
    std::string output_path;
    std::string prefix;
    if (path != "") {
        if (sub_path != "") {
            output_path = path + string("/") + sub_path + string("/");
        } else {
            output_path = path + string("/");
        }
        // std::cout << output_path << std::endl;
        if (! boost::filesystem::exists(output_path)) {
            boost::filesystem::create_directory(output_path);
        }
        prefix = output_path + options_.prefix;
    } else {
        prefix = options_.prefix;
    }
        
    uint32_t ind = prefix.rfind(".bag");

    if (ind != -1 && ind == prefix.size() - 4) {
        prefix.erase(ind);
    }

    if (prefix.length() > 0) {
        parts.push_back(prefix);
    }
    if (options_.split) {
        parts.push_back(boost::lexical_cast<string>(split_count_));
    }
    if (options_.append_date) {
        parts.push_back(timeToStr(ros::WallTime::now()));
    }

    target_filename_ = parts[0];
    for (unsigned int i = 1; i < parts.size(); i++) {
        target_filename_ += string("_") + parts[i];
    }
    write_filename_ = target_filename_ + string(".bag.active");
}

//! Callback to be invoked to actually do the recording
void Recorder::snapshotTrigger(std_msgs::Empty::ConstPtr trigger) {
    updateFilenames();
    
    ROS_INFO("Triggered snapshot recording with name %s.", target_filename_.c_str());
    
    {
        boost::mutex::scoped_lock lock(queue_mutex_);
        queue_queue_.push(OutgoingQueue(target_filename_, queue_, Time::now()));
        queue_      = new std::queue<OutgoingMessage>;
        queue_size_ = 0;
    }

    queue_condition_.notify_all();
}

void Recorder::startWriting() {
    bag_.setCompression(options_.compression);
    bag_.setChunkThreshold(options_.chunk_size);

    updateFilenames();
    try {
        bag_.open(write_filename_, bagmode::Write);
    }
    catch (rosbag::BagException e) {
        ROS_ERROR("Error writing: %s", e.what());
        exit_code_ = 1;
        ros::shutdown();
    }
    ROS_INFO("Recording to %s.", target_filename_.c_str());
}

void Recorder::stopWriting() {
    target_filename_ = target_filename_ + string("_") + timeToStr(ros::WallTime::now());
    std::string tmp = target_filename_;
    target_filename_ += string(".bag");
    ROS_INFO("Closing %s.", target_filename_.c_str());
    bag_.close();
    rename(write_filename_.c_str(), target_filename_.c_str());
    createMeta(tmp);
}

void Recorder::createMeta(const std::string& rosbag_filename) {
    std::string bag_filename = rosbag_filename + string(".bag");
    if (! boost::filesystem::exists(bag_filename)) {
        ROS_INFO("Creating meta for for %s, file do not exist.", bag_filename.c_str());
        return;
    }
    boost::filesystem::path const f(bag_filename);
    std::string meta_path = boost::filesystem::canonical(f.parent_path()).string() + string("/meta/");
    if (! boost::filesystem::exists(meta_path)) {
        boost::filesystem::create_directory(meta_path);
    }
    std::string meta_filename = meta_path + string("/") + f.stem().string() + string(".ini");
    ROS_INFO("Creating meta file for %s",  bag_filename.c_str());
    //rosbag::Bag bag;
    //bag.open(bag_filename, rosbag::bagmode::Read);
    boost::property_tree::ptree pt;
    boost::python::object rosbag;
    std::string start_time = "UNKNOWN";
    std::string end_time = "UNKNOWN";
    std::string duration = "UNKNOWN";
    std::string start_point = "UNKNOWN";
    std::string end_point = "UNKNOWN";
    std::string task_purpose = "UNKNOWN";
    std::string vehicle_id = "UNKNOWN";
    std::string message_count = "UNKNOWN";
    try {
        Py_Initialize();
        rosbag = boost::python::import("rosbag");
    } catch (boost::python::error_already_set const &) {
        ROS_ERROR("creating meta, but import rosbag python module error");
        boost::enable_current_exception(boost::python::error_already_set());
    }
    try {
        boost::python::object bag = rosbag.attr("Bag")(bag_filename);
        boost::python::object pyobj_start_time = bag.attr("get_start_time");
        boost::python::object pyobj_end_time = bag.attr("get_end_time");
        boost::python::object pyobj_message_count = bag.attr("get_message_count");
        start_time = boost::str(boost::format("%.2f") % (boost::python::extract<double>(pyobj_start_time())));
        end_time = boost::str(boost::format("%.2f") % (boost::python::extract<double>(pyobj_end_time())));
        message_count = boost::lexical_cast<std::string>(boost::python::extract<int>(pyobj_message_count()));
        double duration_float = boost::python::extract<double>(pyobj_end_time()) 
                             - boost::python::extract<double>(pyobj_start_time());
        duration = boost::str(boost::format("%.2f") % duration_float);
        bag.attr("close");
    } catch (boost::python::error_already_set const &) {
        ROS_ERROR("creating meta, but get bag info error");
    }
    pt.add("basic.rosbag_name", bag_filename);
    pt.add("basic.start_time", start_time);
    pt.add("basic.end_time", end_time);
    pt.add("basic.duration", duration);
    pt.add("basic.start_point", start_point);
    pt.add("basic.end_point", end_point);
    pt.add("basic.message_count", message_count);
    boost::property_tree::write_ini(meta_filename, pt);
}

bool Recorder::checkSize() {
    if (options_.max_size > 0) {
        if (bag_.getSize() > options_.max_size) {
            if (options_.split) {
                stopWriting();
                split_count_++;
                startWriting();
            } else {
                ros::shutdown();
                return true;
            }
        }
    }
    return false;
}

bool Recorder::checkDuration(const ros::Time& t) {
    if (options_.max_duration > ros::Duration(0)) {
        if (t - start_time_ > options_.max_duration) {
            if (options_.split) {
                while (start_time_ + options_.max_duration < t) {
                    stopWriting();
                    split_count_++;
                    start_time_ += options_.max_duration;
                    startWriting();
                }
            } else {
                ros::shutdown();
                return true;
            }
        }
    }
    return false;
}

//! Thread that actually does writing to file.
void Recorder::doRecord() {
    // Open bag file for writing
    startWriting();

    // Schedule the disk space check
    warn_next_ = ros::WallTime();
    checkDisk();
    check_disk_next_ = ros::WallTime::now() + ros::WallDuration().fromSec(20.0);

    // Technically the queue_mutex_ should be locked while checking empty.
    // Except it should only get checked if the node is not ok, and thus
    // it shouldn't be in contention.
    ros::NodeHandle nh;
    while (nh.ok() || !queue_->empty()) {
        boost::unique_lock<boost::mutex> lock(queue_mutex_);

        bool finished = false;
        while (queue_->empty()) {
            if (!nh.ok()) {
                lock.release()->unlock();
                finished = true;
                break;
            }
            boost::xtime xt;
#if BOOST_VERSION >= 105000
            boost::xtime_get(&xt, boost::TIME_UTC_);
#else
            boost::xtime_get(&xt, boost::TIME_UTC);
#endif
            xt.nsec += 250000000;
            queue_condition_.timed_wait(lock, xt);
            if (checkDuration(ros::Time::now())) {
                finished = true;
                break;
            }
        }
        if (finished) {
            break;
        }

        OutgoingMessage out = queue_->front();
        queue_->pop();
        queue_size_ -= out.msg->size();
        
        lock.release()->unlock();
        
        if (checkSize()) {
            break;
        }

        if (checkDuration(out.time)) {
            break;
        }

        if (out.topic == "/eap/data_recorder/update_path") {
            boost::shared_ptr<std_msgs::String> prefix = out.msg->instantiate<std_msgs::String>(); 
            options_.path = prefix->data;
            continue;
        }
        if (out.topic == "/eap/data_recorder/cmd") {
            boost::shared_ptr<std_msgs::String> cmd = out.msg->instantiate<std_msgs::String>(); 
            // std::cout << "/eap/data_recorder/cmd " << cmd->data << std::endl;
            if (cmd->data == "rosbag_record_off") {
                ROS_INFO("rosbag record OFF.");
                options_.record_switch = false;
                continue;
            } 
            if (cmd->data == "rosbag_record_on") {
                ROS_INFO("rosbag record ON.");
                options_.record_switch = true;
                continue;
            } 
        }
        if (scheduledCheckDisk() && checkLogging()) {
            if (! options_.record_switch) {
                ROS_INFO("rosbag record switch is off, drop messages.");
                continue;
            }
            bag_.write(out.topic, out.time, *out.msg, out.connection_header);
        }
    }

    stopWriting();
}

void Recorder::doRecordSnapshotter() {
    ros::NodeHandle nh;
  
    while (nh.ok() || !queue_queue_.empty()) {
        boost::unique_lock<boost::mutex> lock(queue_mutex_);
        while (queue_queue_.empty()) {
            if (!nh.ok())
                return;
            queue_condition_.wait(lock);
        }
        
        OutgoingQueue out_queue = queue_queue_.front();
        queue_queue_.pop();
        
        lock.release()->unlock();
        
        string target_filename = out_queue.filename;
        string write_filename  = target_filename + string(".active");
        
        try {
            bag_.open(write_filename, bagmode::Write);
        }
        catch (rosbag::BagException ex) {
            ROS_ERROR("Error writing: %s", ex.what());
            return;
        }

        while (!out_queue.queue->empty()) {
            OutgoingMessage out = out_queue.queue->front();
            out_queue.queue->pop();

            bag_.write(out.topic, out.time, *out.msg);
        }

        stopWriting();
    }
}

void Recorder::doCheckMaster(ros::TimerEvent const& e, ros::NodeHandle& node_handle) {
    ros::master::V_TopicInfo topics;
    if (ros::master::getTopics(topics)) {
        foreach(ros::master::TopicInfo const& t, topics) {
	    if (shouldSubscribeToTopic(t.name)) {
	        subscribe(t.name);
            }
        }
    }
    
    if (options_.node != std::string("")) {
        XmlRpc::XmlRpcValue req;
        req[0] = ros::this_node::getName();
        req[1] = options_.node;
        XmlRpc::XmlRpcValue resp;
        XmlRpc::XmlRpcValue payload;

        if (ros::master::execute("lookupNode", req, resp, payload, true)) {
            std::string peer_host;
            uint32_t peer_port;

            if (!ros::network::splitURI(static_cast<std::string>(resp[2]), peer_host, peer_port)) {
                ROS_ERROR("Bad xml-rpc URI trying to inspect node at: %s", static_cast<std::string>(resp[2]).c_str());
            } else {
                XmlRpc::XmlRpcClient c(peer_host.c_str(), peer_port, "/");
                XmlRpc::XmlRpcValue req2;
                XmlRpc::XmlRpcValue resp2;
                req2[0] = ros::this_node::getName();
                c.execute("getSubscriptions", req2, resp2);
              
                if (!c.isFault() && resp2.valid() && resp2.size() > 0 && static_cast<int>(resp2[0]) == 1) {
                    for (int i = 0; i < resp2[2].size(); i++) {
                        if (shouldSubscribeToTopic(resp2[2][i][0], true)) {
                            subscribe(resp2[2][i][0]);
                        }
                    }
                } else {
                    ROS_ERROR("Node at: %s failed to subscribe.", static_cast<std::string>(resp[2]).c_str());
                }
            }
        }
    }  
}

void Recorder::doTrigger() {
    ros::NodeHandle nh;
    ros::Publisher pub = nh.advertise<std_msgs::Empty>("snapshot_trigger", 1, true);
    pub.publish(std_msgs::Empty());

    ros::Timer terminate_timer = nh.createTimer(ros::Duration(1.0), boost::bind(&ros::shutdown));
    ros::spin();
}

bool Recorder::scheduledCheckDisk() {
    boost::mutex::scoped_lock lock(check_disk_mutex_);

    if (ros::WallTime::now() < check_disk_next_) {
        return true;
    }

    check_disk_next_ += ros::WallDuration().fromSec(20.0);
    return checkDisk();
}

bool Recorder::checkDisk() {
#if BOOST_FILESYSTEM_VERSION < 3
    struct statvfs fiData;
    if ((statvfs(bag_.getFileName().c_str(), &fiData)) < 0) {
        ROS_WARN("Failed to check filesystem stats.");
        return true;
    }
    unsigned long long free_space = 0;
    free_space = (unsigned long long) (fiData.f_bsize) * (unsigned long long) (fiData.f_bavail);
    if (free_space < options_.min_space) {
        ROS_ERROR("Less than %s of space free on disk with %s.  Disabling recording.", options_.min_space_str.c_str(), bag_.getFileName().c_str());
        writing_enabled_ = false;
        return false;
    }
    else if (free_space < 5 * options_.min_space) {
        ROS_WARN("Less than 5 x %s of space free on disk with %s.", options_.min_space_str.c_str(), bag_.getFileName().c_str());
    }
    else {
        writing_enabled_ = true;
    }
#else
    boost::filesystem::path p(boost::filesystem::system_complete(bag_.getFileName().c_str()));
    p = p.parent_path();
    boost::filesystem::space_info info;
    try {
        info = boost::filesystem::space(p);
    }
    catch (boost::filesystem::filesystem_error &e) { 
        ROS_WARN("Failed to check filesystem stats");
        writing_enabled_ = false;
        return false;
    }
    if (info.available < options_.min_space) {
        ROS_ERROR("Less than %s of space free on disk with %s.  Disabling recording.", options_.min_space_str.c_str(), bag_.getFileName().c_str());
        writing_enabled_ = false;
        return false;
    }
    else if (info.available < 5 * options_.min_space) {
        ROS_WARN("Less than 5 x %s of space free on disk with %s.", options_.min_space_str.c_str(), bag_.getFileName().c_str());
        writing_enabled_ = true;
    }
    else {
        writing_enabled_ = true;
    }
#endif
    return true;
}

bool Recorder::checkLogging() {
    if (writing_enabled_) {
        return true;
    }

    ros::WallTime now = ros::WallTime::now();
    if (now >= warn_next_) {
        warn_next_ = now + ros::WallDuration().fromSec(5.0);
        ROS_WARN("Not logging message because logging disabled.  Most likely cause is a full disk.");
    }
    return false;
}

BOOST_PYTHON_MODULE(librosbag_recorder) {
  using namespace boost::python;
  
  class_<RecorderOptions>("RecorderOptions")
      .def("get_record_all", &RecorderOptions::get_record_all)
      .def("set_record_all", &RecorderOptions::set_record_all)
      .def("get_regex", &RecorderOptions::get_regex)
      .def("set_regex", &RecorderOptions::set_regex)
      .def("get_do_exclude", &RecorderOptions::get_do_exclude)
      .def("set_do_exclude", &RecorderOptions::set_do_exclude)
      .def("get_quiet", &RecorderOptions::get_quiet)
      .def("set_quiet", &RecorderOptions::set_quiet)
      .def("get_record_switch", &RecorderOptions::get_record_switch)
      .def("set_record_switch", &RecorderOptions::set_record_switch)
      .def("get_prefix", &RecorderOptions::get_prefix)
      .def("set_prefix", &RecorderOptions::set_prefix)
      .def("get_path", &RecorderOptions::get_path)
      .def("set_path", &RecorderOptions::set_path)
      .def("get_sub_path", &RecorderOptions::get_sub_path)
      .def("set_sub_path", &RecorderOptions::set_sub_path)
      .def("set_topic_regex", &RecorderOptions::set_topic_regex)
      .def("set_topic_exclude_regex", &RecorderOptions::set_topic_exclude_regex)
      .def("get_buffer_size", &RecorderOptions::get_buffer_size)
      .def("set_buffer_size", &RecorderOptions::set_buffer_size)
      .def("get_chunk_size", &RecorderOptions::get_chunk_size)
      .def("set_chunk_size", &RecorderOptions::set_chunk_size)
      .def("get_max_size", &RecorderOptions::get_max_size)
      .def("set_max_size", &RecorderOptions::set_max_size)
      .def("get_split", &RecorderOptions::get_split)
      .def("set_split", &RecorderOptions::set_split)
      .def("get_max_duration", &RecorderOptions::get_max_duration)
      .def("set_max_duration", &RecorderOptions::set_max_duration)
  ;
  class_<Recorder>("Recorder", init<RecorderOptions>())
      .def("run", &Recorder::run)
      .def("get_writing_filename", &Recorder::getWritingFilename)
  ;
}
} // namespace rosbag
