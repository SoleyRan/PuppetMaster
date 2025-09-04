#ifndef ATTRIBUTES_H
#define ATTRIBUTES_H

#include <iostream>
#include <vector>
#include <string>

enum CommType:uint8_t
{
    ZMQ_COMMTYPE,
    DDS_COMMTYPE,
    IPC_COMMTYPE
};

enum EntityType:uint8_t
{
    READER_EntityType,
    WRITER_EntityType
};

enum TaskType:uint8_t
{
    PERIOD_TRIGGER,
    DATA_TRIGGER,
    TASK_TRIGGER,
    USER_TRIGGER
};

struct TaskAttribute
{
    timespec period;
    std::vector<std::string> data_depend;
    std::vector<std::string> task_depend;
};

#endif