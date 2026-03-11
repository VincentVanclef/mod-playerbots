#pragma once

#include <cstdint>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

namespace RTG
{
inline bool HasPrefix(std::string const& value, std::string const& prefix)
{
    return value.rfind(prefix, 0) == 0;
}

inline std::string MakeLfgAddData(unsigned int team, unsigned int level, unsigned int role = 0, unsigned int owner = 0)
{
    std::string data = std::string("rtg_lfg:") + std::to_string(team) + ":" + std::to_string(level);
    if (role || owner)
        data += ":" + std::to_string(role);
    if (owner)
        data += ":" + std::to_string(owner);
    return data;
}

inline bool ParseLfgAddData(std::string const& data, unsigned int& team, unsigned int& level, unsigned int* role = nullptr, unsigned int* owner = nullptr)
{
    if (!HasPrefix(data, "rtg_lfg:"))
        return false;

    std::string payload = data.substr(8);
    std::vector<std::string> parts;
    std::stringstream ss(payload);
    std::string part;
    while (std::getline(ss, part, ':'))
        parts.push_back(part);

    if (parts.size() < 2)
        return false;

    try
    {
        team = static_cast<unsigned int>(std::stoul(parts[0]));
        level = static_cast<unsigned int>(std::stoul(parts[1]));
        if (role)
            *role = parts.size() >= 3 ? static_cast<unsigned int>(std::stoul(parts[2])) : 0u;
        if (owner)
            *owner = parts.size() >= 4 ? static_cast<unsigned int>(std::stoul(parts[3])) : 0u;
        return true;
    }
    catch (...)
    {
        return false;
    }
}

inline std::string MakeBgAddData(unsigned int team, unsigned int level, unsigned int queueType, unsigned int owner = 0)
{
    std::string data = std::string("rtg_bg:") + std::to_string(team) + ":" + std::to_string(level) + ":" + std::to_string(queueType);
    if (owner)
        data += ":" + std::to_string(owner);
    return data;
}

inline bool ParseBgAddData(std::string const& data, unsigned int& team, unsigned int& level, unsigned int& queueType, unsigned int* owner = nullptr)
{
    if (!HasPrefix(data, "rtg_bg:"))
        return false;

    std::string payload = data.substr(7);
    std::vector<std::string> parts;
    std::stringstream ss(payload);
    std::string part;
    while (std::getline(ss, part, ':'))
        parts.push_back(part);

    if (parts.size() < 3)
        return false;

    try
    {
        team = static_cast<unsigned int>(std::stoul(parts[0]));
        level = static_cast<unsigned int>(std::stoul(parts[1]));
        queueType = static_cast<unsigned int>(std::stoul(parts[2]));
        if (owner)
            *owner = parts.size() >= 4 ? static_cast<unsigned int>(std::stoul(parts[3])) : 0u;
        return true;
    }
    catch (...)
    {
        return false;
    }
}

inline bool IsQueueManagedAddData(std::string const& data)
{
    return HasPrefix(data, "rtg_lfg:") || HasPrefix(data, "rtg_bg:");
}

inline bool ParseLfgDesiredRole(std::string const& addData, uint32_t& desiredRole)
{
    desiredRole = 0;

    if (addData.empty())
        return false;

    size_t p1 = addData.find(':');
    if (p1 == std::string::npos)
        return false;

    size_t p2 = addData.find(':', p1 + 1);
    if (p2 == std::string::npos)
        return false;

    size_t p3 = addData.find(':', p2 + 1);
    if (p3 == std::string::npos)
        return false;

    std::string prefix = addData.substr(0, p1);
    if (prefix != "rtg_lfg")
        return false;

    std::string roleStr = addData.substr(p3 + 1);
    if (roleStr.empty())
        return false;

    desiredRole = static_cast<uint32_t>(std::strtoul(roleStr.c_str(), nullptr, 10));
    return true;
}
}
