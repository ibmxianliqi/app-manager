#pragma once

#include <string>
#include <mutex>


//////////////////////////////////////////////////////////////////////////
/// LdapUtils
//////////////////////////////////////////////////////////////////////////
class LdapUtils
{
public:
	explicit LdapUtils(const std::string& uri, const std::string& baseDn, const std::string& bindDn);
	virtual ~LdapUtils();

	bool authenticate(const std::string& user, const std::string& passwd);

private:
	const std::string m_uri;
	const std::string m_baseDn;
	const std::string m_bindDn;
	std::recursive_mutex m_mutex;
};
