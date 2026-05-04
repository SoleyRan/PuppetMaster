/**
 * @file namespace_macros.h
 * @brief puppet_master项目专属 - 嵌套命名空间宏定义
 * @note 仅包含项目专属的命名空间展开/闭合宏，不包含其他业务宏
 * @author Soley
 * @date 2026-02-12
 */
#pragma once

#define PUPPET_MASTER_NS_BEGIN namespace puppet_master {
#define PUPPET_MASTER_NS_END }

#define PUPPET_MASTER_BASE_NS_BEGIN namespace puppet_master { namespace base { 
#define PUPPET_MASTER_BASE_NS_END } } 

#define PUPPET_MASTER_UTILS_NS_BEGIN namespace puppet_master { namespace utils { 
#define PUPPET_MASTER_UTILS_NS_END } } 

#define PUPPET_MASTER_COMMUNICATION_NS_BEGIN namespace puppet_master { namespace communication { 
#define PUPPET_MASTER_COMMUNICATION_NS_END } } 