#pragma once
#include "config/ConfigVariable.h"
#include "messagedirector/MessageDirector.h"

typedef ConfigNode RoleConfig;
class RoleConfigGroup : public ConfigGroup
{
  public:
    RoleConfigGroup(const std::string& type);

  private:
    ConfigVariable<std::string> m_type;
};

// A Role is a major component of Astron which is configured in the daemon's config file.
// Can send or receive datagram messages with the MessageDirector.
class Role : public MDParticipantInterface
{
  protected:
    Role(RoleConfig roleconfig) : m_roleconfig(roleconfig) {}
    Role(const Role&) = delete;
    Role& operator=(const Role&) = delete;
    virtual ~Role() {}

    RoleConfig m_roleconfig;
};
