#pragma once
#include <unordered_map> // std::unordered_map
#include "core/Role.h"

// A BaseRoleFactoryItem is a common ancestor that all role factory templates inherit from.
class BaseRoleFactoryItem
{
  public:
    virtual Role* instantiate(RoleConfig roleconfig) = 0;

  protected:
    BaseRoleFactoryItem(const std::string &name);
};

// A RoleFactoryItem is the factory for a particular role.
// Each new role should declare a RoleFactoryItem<RoleClass>("RoleName");
template<class T>
class RoleFactoryItem : public BaseRoleFactoryItem
{
  public:
    RoleFactoryItem(const std::string &name) : BaseRoleFactoryItem(name) {}
    RoleFactoryItem(const RoleFactoryItem&) = delete;
    RoleFactoryItem& operator=(const RoleFactoryItem&) = delete;

    Role* instantiate(RoleConfig roleconfig) override
    {
        return new T(roleconfig);
    }
};

// The RoleFactory is a singleton that instantiates roles from a role's name.
class RoleFactory
{
  public:
    static RoleFactory& singleton();

    RoleFactory() {}
    RoleFactory(const RoleFactory&) = delete;
    RoleFactory& operator=(const RoleFactory&) = delete;

    // instantiate_role creates a new Role object of type 'role_name'.
    Role* instantiate_role(const std::string &role_name, RoleConfig roleconfig);

    // add_role adds a factory for role of type 'name'
    // It is called automatically when instantiating a new RoleFactoryItem.
    void add_role(const std::string &name, BaseRoleFactoryItem *factory);

  private:
    std::unordered_map<std::string, BaseRoleFactoryItem*> m_factories;
};
