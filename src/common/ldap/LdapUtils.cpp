#include "LdapUtils.h"
#include "cldap.h"
#include "../Utility.h"

//////////////////////////////////////////////////////////////////////
/// Users
//////////////////////////////////////////////////////////////////////

LdapUtils::LdapUtils(const std::string& uri, const std::string& baseDn, const std::string& bindDn)
	:m_uri(uri), m_baseDn(baseDn), m_bindDn(bindDn)
{
}

LdapUtils::~LdapUtils()
{
}

bool LdapUtils::authenticate(const std::string& user, const std::string& passwd)
{
	Ldap::Server ldap;

	ldap.Connect(m_uri);
	std::cout << "ldap connect: " << ldap.Message() << std::endl;
	auto cn = Utility::stringReplace(m_bindDn, JSON_KEY_USER_LDAP_USER_REPLACE_HOLDER, user);
	bool success = ldap.Bind(cn, passwd);
	return success;
}
