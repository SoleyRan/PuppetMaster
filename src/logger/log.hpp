#ifndef LOG_HPP
#define LOG_HPP

#include <iostream>
#include <string>
#include <boost/date_time/posix_time/posix_time.hpp>
#include <boost/log/support/date_time.hpp>
#include <boost/log/common.hpp>
#include <boost/log/expressions.hpp>
#include <boost/log/expressions/keyword.hpp>

#include <boost/log/attributes.hpp>
#include <boost/log/attributes/timer.hpp>
#include <boost/log/sources/logger.hpp>
#include <boost/log/sources/severity_logger.hpp>
#include <boost/log/sinks/async_frontend.hpp>
// #include <boost/log/sinks/text_multifile_backend.hpp>
// #include <boost/log/sinks/text_file_backend.hpp>
#include "text_file_backend_self_defined.hpp"

// #include <boost/log/utility/setup/file.hpp>
#include <boost/log/utility/setup/console.hpp>
#include <boost/log/utility/setup/common_attributes.hpp>

#include <boost/log/attributes/named_scope.hpp>
#include <boost/log/trivial.hpp>
#include <boost/filesystem.hpp>

namespace puppet_master
{
namespace logger
{

namespace logging = boost::log;
namespace attrs = boost::log::attributes;
namespace src = boost::log::sources;
namespace sinks = boost::log::sinks;
namespace expr = boost::log::expressions;
namespace keywords = boost::log::keywords;


using loggerChannelType = boost::log::sources::severity_channel_logger_mt<boost::log::trivial::severity_level, std::string>;
#ifndef LOGGER_CHANNEL_TYPE
#define LOGGER_CHANNEL_TYPE
const loggerChannelType channelLogger;
#endif

BOOST_LOG_ATTRIBUTE_KEYWORD(log_timestamp, "TimeStamp", boost::posix_time::ptime)

#define GetFileName(file_path) (static_cast<std::string>(file_path).rfind('/') != std::string::npos?static_cast<std::string>(file_path).substr(static_cast<std::string>(file_path).rfind('/') + 1):file_path)

#define MY_BOOST_LOG(severity) \
    BOOST_LOG_TRIVIAL(severity)

#define LOG_Trace() MY_BOOST_LOG(trace)<<"[" << GetFileName(__FILE__) << ":" << __LINE__ << "]:" //白色字体

#define LOG_Debug() MY_BOOST_LOG(debug)<<"[" << GetFileName(__FILE__) << ":" << __LINE__ << "]:" //白色字体

#define LOG_Info() MY_BOOST_LOG(info)<<"\033[32m"<<"[" << GetFileName(__FILE__) << ":" << __LINE__ << "]:"<<"\033[0m" //绿色字体

#define LOG_Warn()  MY_BOOST_LOG(warning)<<"\033[33m"<<"[" << GetFileName(__FILE__) << ":" << __LINE__ << "]:"<<"\033[0m" //黄色字体

#define LOG_Error() MY_BOOST_LOG(error)<<"\033[31m"<<"[" << GetFileName(__FILE__) << ":" << __LINE__ << "]:"<<"\033[0m" //红色字体

#define LOG_Fatal() MY_BOOST_LOG(fatal)<<"\033[34m"<<"[" << GetFileName(__FILE__) << ":" << __LINE__ << "]:"<<"\033[0m" //蓝色字体

#define LOG_DEBUG_HEX(data, len, text) MY_BOOST_LOG(debug) << text << " Data[" << boost::log::dump(data, len) << "]"

#define LOG_Channel_Trace(channelName) BOOST_LOG_CHANNEL_SEV(const_cast<itage::log::loggerChannelType &>(itage::log::channelLogger), channelName, boost::log::trivial::trace) << "[" << GetFileName(__FILE__) << ":" << __LINE__ << "]:" //白色字体
#define LOG_Channel_Debug(channelName) BOOST_LOG_CHANNEL_SEV(const_cast<itage::log::loggerChannelType &>(itage::log::channelLogger), channelName, boost::log::trivial::debug)<< "[" << GetFileName(__FILE__) << ":" << __LINE__ << "]:" //白色字体
#define LOG_Channel_Info(channelName) BOOST_LOG_CHANNEL_SEV(const_cast<itage::log::loggerChannelType &>(itage::log::channelLogger), channelName, boost::log::trivial::info)<<"\033[32m"<<"[" << GetFileName(__FILE__) << ":" << __LINE__ << "]:"<<"\033[0m" //绿色字体
#define LOG_Channel_Warn(channelName) BOOST_LOG_CHANNEL_SEV(const_cast<itage::log::loggerChannelType &>(itage::log::channelLogger), channelName, boost::log::trivial::warning)<<"\033[33m"<<"[" << GetFileName(__FILE__) << ":" << __LINE__ << "]:"<<"\033[0m" //黄色字体
#define LOG_Channel_Error(channelName) BOOST_LOG_CHANNEL_SEV(const_cast<itage::log::loggerChannelType &>(itage::log::channelLogger), channelName, boost::log::trivial::error)<<"\033[31m"<<"[" << GetFileName(__FILE__) << ":" << __LINE__ << "]:"<<"\033[0m" //红色字体
#define LOG_Channel_Fatal(channelName) BOOST_LOG_CHANNEL_SEV(const_cast<itage::log::loggerChannelType &>(itage::log::channelLogger), channelName, boost::log::trivial::fatal)<<"\033[34m"<<"[" << GetFileName(__FILE__) << ":" << __LINE__ << "]:"<<"\033[0m" //蓝色字体

#define LOG_Channel_DEBUG_HEX(channelName, data, len, text) LOG_Channel_Debug(channelName) << text << " DataLen:" << len << " Data[" << boost::log::dump(data, len) << "]"

#define LOG_Channel_Warn_HEX(channelName, id, data, len) LOG_Channel_Warn(channelName) << boost::format("0x%08x ") % id << boost::log::dump(data, len)

/**
 * @brief log初始化
 * @param log_path_str log存储路径-绝对路径
 * @param in_console_level 终端log输出level
 * @param in_file_level 文件log输出level
 * @param max_log_size 单个log大小,MB单位
 * @param max_log_num 最多存储log文件个数
 * @param channel_name channel名，用于指定写入指定channel,默认为空
*/
static inline void logInit(std::string log_path_str, int in_console_level, int in_file_level, int max_log_size, int max_log_num, std::string channel_name = ""){
    boost::log::trivial::severity_level console_level;
    boost::log::trivial::severity_level file_level;
    console_level = static_cast<boost::log::trivial::severity_level>(in_console_level);
    file_level = static_cast<boost::log::trivial::severity_level>(in_file_level);

    logging::formatter formatter=
        expr::stream
        <<"["<<expr::format_date_time(log_timestamp,"%Y-%m-%d %H:%M:%S.%f")<<"]"  
        <<"<"<<logging::trivial::severity<<">"<<expr::message;

    logging::add_common_attributes();

    typedef sinks::asynchronous_sink<sinks::text_file_backend_self_defined> async_sink;  
    boost::shared_ptr<async_sink> file_sink = boost::make_shared<async_sink>(); 

    file_sink->locked_backend()->set_file_name_pattern(log_path_str + "%5N-%Y-%m-%d-%H-%M-%S.log"); //五位序列号-年-月-日-时-分-秒.log
    file_sink->locked_backend()->set_rotation_size(max_log_size*(1<<20));
    file_sink->locked_backend()->set_time_based_rotation(sinks::file::rotation_at_time_point(0,0,0));    //打开则在时间跳变时新建日志
    file_sink->locked_backend()->set_open_mode(std::ios::app);
    file_sink->locked_backend()->auto_flush(true);
    file_sink->locked_backend()->enable_final_rotation(true);

    file_sink->locked_backend()->set_file_collector(sinks::file::make_collector(
        keywords::target = log_path_str,        //文件夹名
        keywords::max_size = max_log_num*max_log_size*(1<<20),    //文件夹所占最大空间
        keywords::min_free_space = (max_log_num+10)*max_log_size*(1<<20),  //磁盘最小预留空间
        keywords::max_files = max_log_num   //存储最多文件数
        ));

    file_sink->locked_backend()->scan_for_files();  //扫描collector下文件夹，来指导序列号N的起始
    file_sink->set_formatter(formatter);    //设置输出格式

    auto console_sink=logging::add_console_log();
    console_sink->set_formatter(formatter);

    if(!channel_name.empty()){
        auto file_filter = expr::attr<logging::trivial::severity_level>("Severity") >= file_level && expr::attr<std::string>("Channel") == channel_name;
        auto console_filter = expr::attr<logging::trivial::severity_level>("Severity") >= console_level && expr::attr<std::string>("Channel") == channel_name;
        file_sink->set_filter(file_filter);   //日志级别过滤
        console_sink->set_filter(console_filter);
    }
    else{
        auto file_filter = expr::attr<logging::trivial::severity_level>("Severity") >= file_level;  
        auto console_filter = expr::attr<logging::trivial::severity_level>("Severity") >= console_level;
        file_sink->set_filter(file_filter);   //日志级别过滤
        console_sink->set_filter(console_filter);
    }
    
    logging::core::get()->add_sink(console_sink);
    logging::core::get()->add_sink(file_sink);

    LOG_Warn() << "New Log Init";  //用于标识新建日志
}

}
}

#endif