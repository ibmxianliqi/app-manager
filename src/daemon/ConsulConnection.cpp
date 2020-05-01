#include <algorithm>
#include <cpprest/http_client.h>
#include <cpprest/json.h>
#include <thread>

#include "Application.h"
#include "Configuration.h"
#include "ConsulConnection.h"
#include "ResourceCollection.h"
#include "User.h"

#include "../common/Utility.h"
#include "../common/PerfLog.h"

#define CONSUL_BASE_PATH  "/v1/kv/appmgr/"

ConsulConnection::ConsulConnection()
	:m_ssnRenewTimerId(0), m_reportStatusTimerId(0), m_leader(0), m_threadDestroy(false)
{
}

ConsulConnection::~ConsulConnection()
{
	this->cancleTimer(m_ssnRenewTimerId);
	this->cancleTimer(m_reportStatusTimerId);
	m_threadDestroy = true;
	if (m_securityThread) m_securityThread->join();
	if (m_topologyThread) m_topologyThread->join();
}

std::shared_ptr<ConsulConnection>& ConsulConnection::instance()
{
	static auto singleton = std::make_shared<ConsulConnection>();
	return singleton;
}

// report label and resource to host KV
// report timestamp to Flags attr for KV
void ConsulConnection::reportStatus(int timerId)
{
	const static char fname[] = "ConsulConnection::reportStatus() ";

	// check feature enabled
	if (!Configuration::instance()->getConsul()->consulEnabled()) return;

	// Only node need report status for node (master does not need report)
	if (!Configuration::instance()->getConsul()->m_isNode) return;

	PerfLog perf(fname);
	try
	{
		//report resource: /appmgr/nodes/myhost
		std::string path = std::string(CONSUL_BASE_PATH).append("nodes/").append(MY_HOST_NAME);
		web::json::value body = web::json::value::object();
		body["resource"] = ResourceCollection::instance()->getConsulJson();
		body["label"] = Configuration::instance()->getLabel()->AsJson();
		auto timestamp = std::to_string(std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()));
		auto resp = requestHttp(web::http::methods::PUT, path, { {"flags", timestamp} }, {}, &body);
		if (resp.status_code() == web::http::status_codes::OK)
		{
			auto result = resp.extract_utf8string(true).get();
			if (result != "true")
			{
				LOG_WAR << fname << "report resource to " << path << " failed with response : " << result;
			}
		}
	}
	catch (const std::exception & ex)
	{
		LOG_WAR << fname << " got exception: " << ex.what();
	}
	catch (...)
	{
		LOG_WAR << fname << " exception";
	}
}

void ConsulConnection::refreshSession(int timerId)
{
	const static char fname[] = "ConsulConnection::refreshSession() ";

	try
	{
		// check feature enabled
		if (!Configuration::instance()->getConsul()->consulEnabled()) return;

		// check Consul configuration
		if (!Configuration::instance()->getConsul()->m_isMaster &&
			!Configuration::instance()->getConsul()->m_isNode)
		{
			return;
		}

		PerfLog perf(fname);
		// get session id
		std::string sessionId = getSessionId();
		if (sessionId.empty())
		{
			sessionId = requestSessionId();
		}
		else
		{
			sessionId = renewSessionId();
		}
		// set session id
		std::lock_guard<std::recursive_mutex> guard(m_mutex);
		m_sessionId = sessionId;
		return;
	}
	catch (const std::exception & ex)
	{
		LOG_WAR << fname << " got exception: " << ex.what();
	}
	catch (...)
	{
		LOG_WAR << fname << " exception";
	}
	std::lock_guard<std::recursive_mutex> guard(m_mutex);
	m_sessionId.clear();
}

void ConsulConnection::schedule()
{
	const static char fname[] = "ConsulConnection::schedule() ";

	try
	{
		// check feature enabled
		if (!Configuration::instance()->getConsul()->consulEnabled()) return;
		if (getSessionId().empty()) return;

		PerfLog perf(fname);

		if (Configuration::instance()->getConsul()->m_isMaster)
		{
			// Leader's job
			leaderSchedule();
		}
	}
	catch (const std::exception & ex)
	{
		LOG_WAR << fname << " got exception: " << ex.what();
	}
	catch (...)
	{
		LOG_WAR << fname << " exception";
	}

	std::this_thread::sleep_for(std::chrono::seconds(Configuration::instance()->getConsul()->m_scheduleInterval));
}

void ConsulConnection::syncSecurity()
{
	const static char fname[] = "ConsulConnection::syncSecurity() ";

	try
	{
		// check feature enabled
		if (!Configuration::instance()->getConsul()->consulSecurityEnabled()) return;

		PerfLog perf(fname);

		static long lastIndex = 0;
		auto resp = requestHttp(web::http::methods::GET, std::string(CONSUL_BASE_PATH).append("security"), {}, {}, nullptr);
		if (resp.status_code() == web::http::status_codes::OK)
		{
			auto respJson = resp.extract_json(true).get();
			if (!respJson.is_array() || respJson.as_array().size() == 0) return;
			auto securityJson = respJson.as_array().at(0);
			if (!HAS_JSON_FIELD(securityJson, "ModifyIndex") || !HAS_JSON_FIELD(securityJson, "Value")) return;

			auto index = GET_JSON_NUMBER_VALUE(securityJson, "ModifyIndex");
			if (index > lastIndex)
			{
				auto security = web::json::value::parse(Utility::decode64(GET_JSON_STR_VALUE(securityJson, "Value")));
				auto securityObj = Configuration::JsonSecurity::FromJson(security);
				if (securityObj->m_jwtUsers->getUsers().size())
				{
					Configuration::instance()->updateSecurity(securityObj);
					LOG_DBG << fname << "Security info updated from Consul successfully";
				}
				lastIndex = index;
			}
		}
		else
		{
			LOG_WAR << fname << "failed with response : " << resp.extract_utf8string(true).get();
		}
	}
	catch (const std::exception & ex)
	{
		LOG_WAR << fname << " got exception: " << ex.what();
	}
	catch (...)
	{
		LOG_WAR << fname << " exception";
	}
}

std::string ConsulConnection::requestSessionId()
{
	const static char fname[] = "ConsulConnection::requestSessionId() ";

	// https://www.consul.io/api/session.html
	std::string sessionId;

	auto payload = web::json::value::object();
	payload["LockDelay"] = web::json::value::string("15s");
	payload["Name"] = web::json::value::string(std::string("appmgr-lock-") + MY_HOST_NAME);
	payload["Behavior"] = web::json::value::string("delete");
	payload["TTL"] = web::json::value::string(std::to_string(Configuration::instance()->getConsul()->m_ttl) + "s");

	auto resp = requestHttp(web::http::methods::PUT, "/v1/session/create", {}, {}, &payload);
	if (resp.status_code() == web::http::status_codes::OK)
	{
		auto json = resp.extract_json(true).get();
		LOG_DBG << fname << json.serialize();
		if (HAS_JSON_FIELD(json, "ID"))
		{
			sessionId = GET_JSON_STR_VALUE(json, "ID");
			LOG_DBG << fname << "sessionId=" << sessionId;
		}
	}
	return sessionId;
}

std::string ConsulConnection::renewSessionId()
{
	const static char fname[] = "ConsulConnection::renewSessionId() ";

	auto sessionId = getSessionId();
	if (sessionId.length())
	{
		auto resp = requestHttp(web::http::methods::PUT, std::string("/v1/session/renew/").append(sessionId), {}, {}, nullptr);
		if (resp.status_code() == web::http::status_codes::OK)
		{
			auto json = resp.extract_json(true).get();
			if (json.is_array() && json.as_array().size())
			{
				json = json.as_array().at(0);
				sessionId = GET_JSON_STR_VALUE(json, "ID");
			}
		}
		else
		{
			LOG_WAR << fname << "failed with response : " << resp.extract_utf8string(true).get();
			sessionId = requestSessionId();
		}
	}
	return sessionId;
}

std::string ConsulConnection::getSessionId()
{
	std::lock_guard<std::recursive_mutex> guard(m_mutex);
	return m_sessionId;
}

void ConsulConnection::leaderSchedule()
{
	// leader's job
	if (eletionLeader())
	{
		auto taskList = retrieveTask();
		auto oldTopology = retrieveTopology("");
		auto nodes = retrieveNode();
		if (nodes.size())
		{
			// find matched hosts for each task
			findTaskAvialableHost(taskList, nodes);

			// schedule task
			auto newTopology = scheduleTask(taskList, oldTopology);

			// apply schedule result
			compareTopologyAndDispatch(oldTopology, newTopology);
		}
	}
}

void ConsulConnection::syncNodeTopology()
{
	const static char fname[] = "ConsulConnection::syncNodeTopology() ";

	auto currentAllApps = Configuration::instance()->getApps();
	std::shared_ptr<ConsulTopology> newTopology;
	auto topology = retrieveTopology(MY_HOST_NAME);
	auto hostTopologyIt = topology.find(MY_HOST_NAME);
	if (hostTopologyIt != topology.end()) newTopology = hostTopologyIt->second;

	if (newTopology)
	{
		auto task = retrieveTask();
		for (const auto& hostApp : newTopology->m_apps)
		{
			const auto& appName = hostApp;
			if (task.count(appName))
			{
				auto& consulTask = task[appName];
				std::shared_ptr<Application> topologyAppObj = consulTask->m_app;
				auto it = std::find_if(currentAllApps.begin(), currentAllApps.end(), [&appName](std::shared_ptr<Application> const& obj) {
					return obj->getName() == appName;
					});
				if (it != currentAllApps.end())
				{
					// Update app
					auto& currentRunningApp = *it;
					if (!currentRunningApp->operator==(topologyAppObj))
					{
						Configuration::instance()->addApp(topologyAppObj->AsJson(false));
						LOG_INF << fname << " Consul application <" << topologyAppObj->getName() << "> updated";

						registerService(appName, consulTask->m_consulServicePort);
					}
				}
				else
				{
					// New add app
					Configuration::instance()->addApp(topologyAppObj->AsJson(false));
					LOG_INF << fname << " Consul application <" << topologyAppObj->getName() << "> added";

					registerService(appName, consulTask->m_consulServicePort);
				}
			}
		}

		for (const auto& currentApp : currentAllApps)
		{
			if (currentApp->isCloudApp())
			{
				if (!(newTopology && (newTopology->m_apps.count(currentApp->getName()))))
				{
					// Remove no used topology
					Configuration::instance()->removeApp(currentApp->getName());
					LOG_INF << fname << " Consul application <" << currentApp->getName() << "> removed";
					deregisterService(currentApp->getName());
				}
			}
		}
	}
	else
	{
		// retrieveTopology will throw if connection was not reached
		for (const auto& currentApp : currentAllApps)
		{
			if (currentApp->isCloudApp())
			{
				// Remove no used topology
				Configuration::instance()->removeApp(currentApp->getName());
				LOG_INF << fname << " Consul application <" << currentApp->getName() << "> removed";
				deregisterService(currentApp->getName());
			}
		}
	}
}

bool ConsulConnection::eletionLeader()
{
	// get session id
	std::string sessionId = getSessionId();
	if (sessionId.empty()) return false;

	// write hostname to leader path : /appmgr/leader
	std::string path = std::string(CONSUL_BASE_PATH).append("leader");
	auto body = web::json::value::string(MY_HOST_NAME);
	auto timestamp = std::to_string(std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()));
	auto resp = requestHttp(web::http::methods::PUT, path, { {"acquire", sessionId}, {"flags", timestamp} }, {}, &body);
	if (resp.status_code() == web::http::status_codes::OK)
	{
		auto result = resp.extract_utf8string(true).get();
		m_leader = (result == "true");
	}
	else
	{
		m_leader = false;
	}
	return m_leader;
}

bool ConsulConnection::registerService(const std::string& appName, int port)
{
	const static char fname[] = "ConsulConnection::registerService() ";
	// https://www.hashicorp.com/blog/consul-and-external-services/
	//curl -X PUT -d 
	//  '{"Node": "myhost", "Address": "myhost","Service": {"Service": "mysql", "tags": ["master","v1"], "Port": 3306}}'
	//  http://127.0.0.1:8500/v1/catalog/register

	if (port == 0) return false;

	auto body = web::json::value();
	body["ID"] = web::json::value::string(MY_HOST_NAME + ":" + appName);
	body["Name"] = web::json::value::string(appName);
	body["Address"] = web::json::value::string(MY_HOST_NAME);
	body["Port"] = web::json::value::number(port);

	auto check = web::json::value::object();
	check["HTTP"] = web::json::value::string("https://" + MY_HOST_NAME + ":" + std::to_string(Configuration::instance()->getRestListenPort()) + "/app/" + appName + "/health");
	check["Interval"] = web::json::value::string("5s");
	check["Timeout"] = web::json::value::string("4s");
	check["Method"] = web::json::value::string("GET");
	check["TLSSkipVerify"] = web::json::value::boolean(true);
	body["Check"] = check;

	std::string path = "/v1/agent/service/register";
	auto resp = requestHttp(web::http::methods::PUT, path, { {"replace-existing-checks","true"} }, {}, &body);
	if (resp.status_code() == web::http::status_codes::OK)
	{
		auto result = resp.extract_utf8string(true).get();
		LOG_DBG << fname << " service for task <" << appName << "> registered : " << result;
		return (result == "true");
	}
	return false;
}

bool ConsulConnection::deregisterService(const std::string appName)
{
	const static char fname[] = "ConsulConnection::registerService() ";

	auto serviceId = std::string(MY_HOST_NAME).append(":").append(appName);
	std::string path = std::string("/v1/agent/service/deregister/").append(serviceId);
	auto resp = requestHttp(web::http::methods::PUT, path, {}, {}, NULL);
	if (resp.status_code() == web::http::status_codes::OK)
	{
		auto result = resp.extract_utf8string(true).get();
		LOG_DBG << fname << " service for task <" << appName << "> removed : " << result;
		return (result == "true");
	}
	return false;
}

void ConsulConnection::saveSecurity()
{
	const static char fname[] = "ConsulConnection::saveSecurity() ";

	if (!Configuration::instance()->getConsul()->consulSecurityEnabled()) return;

	// /appmgr/security
	std::string path = std::string(CONSUL_BASE_PATH).append("security");

	auto body = Configuration::instance()->getSecurity()->AsJson(false);
	auto timestamp = std::to_string(std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()));
	web::http::http_response resp = requestHttp(web::http::methods::PUT, path, { {"flags", timestamp} }, {}, &body);
	if (resp.status_code() == web::http::status_codes::OK)
	{
		auto result = resp.extract_utf8string(true).get();
		if (result != "true")
		{
			LOG_WAR << fname << " PUT " << path << " failed with response : " << result;
		}
	}
}

void ConsulConnection::findTaskAvialableHost(const std::map<std::string, std::shared_ptr<ConsulTask>>& taskMap, const std::map<std::string, std::shared_ptr<ConsulNode>>& hosts)
{
	const static char fname[] = "ConsulConnection::findTaskAvialableHost() ";

	for (const auto& task : taskMap)
	{
		auto taskName = task.first;
		task.second->m_matchedHosts.clear();
		for (const auto& host : hosts)
		{
			auto& hostName = host.first;
			auto& consulHost = host.second;
			auto& taskCondition = task.second->m_condition;
			if (consulHost->m_label->match(taskCondition))
			{
				task.second->m_matchedHosts[hostName] = consulHost;
				LOG_DBG << fname << " task <" << taskName << "> match host <" << hostName << ">";
			}
		}
	}
}

std::map<std::string, std::shared_ptr<ConsulTopology>> ConsulConnection::scheduleTask(const std::map<std::string, std::shared_ptr<ConsulTask>>& taskMap, const std::map<std::string, std::shared_ptr<ConsulTopology>>& oldTopology)
{
	const static char fname[] = "ConsulConnection::scheduleTask() ";

	// key: hostname, value: task list
	std::map<std::string, std::shared_ptr<ConsulTopology>> newTopology;

	// ignore old schedule
	for (const auto& task : taskMap)
	{
		const auto& taskName = task.first;
		auto& taskDedicateHosts = task.second->m_matchedHosts;
		auto& taskReplication = task.second->m_replication;
		if (taskReplication <= 0) continue;

		for (const auto& oldHost : oldTopology)
		{
			auto& oldHostName = oldHost.first;
			auto& oldTaskSet = oldHost.second->m_apps;
			if (taskDedicateHosts.count(oldHostName) && oldTaskSet.count(taskName))
			{
				auto consulNode = taskDedicateHosts[oldHostName];
				// found app running on old host still match
				taskDedicateHosts.erase(oldHostName);
				--taskReplication;

				LOG_DBG << fname << " task <" << taskName << "> already running on host <" << oldHostName << ">";

				{
					// save to topology
					if (!newTopology.count(oldHostName)) newTopology[oldHostName] = std::make_shared<ConsulTopology>();
					newTopology[oldHostName]->m_apps.insert(taskName);
					consulNode->assignApp(task.second->m_app);
				}
			}
		}
	}

	// do schedule
	for (const auto& task : taskMap)
	{
		// get current task
		const auto& taskDedicateHosts = task.second->m_matchedHosts;
		auto& taskReplication = task.second->m_replication;
		const auto& taskName = task.first;
		std::vector<std::shared_ptr<ConsulNode>> taskDedicateHostsVec;

		LOG_DBG << fname << "schedule task <" << taskName << ">";

		if (taskReplication <= 0)
			continue;

		for (const auto& host : taskDedicateHosts)
		{
			taskDedicateHostsVec.push_back(host.second);
		}
		// sort hosts
		// return left < right is Ascending
		// return left > right is Descending
		std::sort(taskDedicateHostsVec.begin(), taskDedicateHostsVec.end(),
			[](const std::shared_ptr<ConsulNode>& left, const std::shared_ptr<ConsulNode>& right)
			{
				if (left->m_assignedApps.size() < right->m_assignedApps.size())
				{
					return true;
				}
				else if (left->m_assignedApps.size() == right->m_assignedApps.size())
				{
					return (left->getAssignedAppMem() < right->getAssignedAppMem());
				}
				else
				{
					return false;
				}
			});

		if (taskReplication > taskDedicateHostsVec.size())
		{
			LOG_WAR << fname << taskName << " : Replication <" << taskReplication << "> Dedicate Host < " << taskDedicateHostsVec.size() << ">";
		}
		// assign host to task
		for (size_t i = 0; i < taskReplication; i++)
		{
			if (i < taskDedicateHostsVec.size())
			{
				const auto& hostname = taskDedicateHostsVec[i]->m_hostName;
				const auto& consulNode = taskDedicateHostsVec[i];
				// save to topology
				{
					if (!newTopology.count(hostname)) newTopology[hostname] = std::make_shared<ConsulTopology>();
					newTopology[hostname]->m_apps.insert(taskName);
					consulNode->assignApp(task.second->m_app);
				}
				LOG_DBG << fname << " task <" << taskName << "> assigned to host < " << hostname << ">";
				task.second->dump();
			}
			else
			{
				break;
			}
		}
	}

	return std::move(newTopology);
}

void ConsulConnection::compareTopologyAndDispatch(const std::map<std::string, std::shared_ptr<ConsulTopology>>& oldT, const std::map<std::string, std::shared_ptr<ConsulTopology>>& newT)
{
	for (const auto& newHost : newT)
	{
		if (oldT.count(newHost.first))
		{
			auto equal = true;
			if (newHost.second->m_apps.size() == oldT.find(newHost.first)->second->m_apps.size())
			{
				for (const auto& app : newHost.second->m_apps)
				{
					if (!oldT.find(newHost.first)->second->m_apps.count(app))
					{
						equal = false;
						break;
					}
				}
				// equal, do nothing here
			}
			else
			{
				equal = false;
			}
			if (!equal)
			{
				// update
				writeTopology(newHost.first, newHost.second);
			}
		}
		else
		{
			// add
			writeTopology(newHost.first, newHost.second);
		}
	}

	for (const auto& oldHost : oldT)
	{
		if (!newT.count(oldHost.first))
		{
			// delete
			writeTopology(oldHost.first, nullptr);
		}
	}
}

bool ConsulConnection::writeTopology(std::string hostName, const std::shared_ptr<ConsulTopology> topology)
{
	const static char fname[] = "ConsulConnection::writeTopology() ";

	//topology: /appmgr/topology/myhost
	std::string path = std::string(CONSUL_BASE_PATH).append("topology/").append(hostName);
	web::http::http_response resp;
	if (topology && topology->m_apps.size())
	{
		auto body = topology->AsJson();
		auto timestamp = std::to_string(std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()));
		resp = requestHttp(web::http::methods::PUT, path, { {"flags", timestamp} }, {}, &body);
		LOG_INF << fname << "write <" << body.serialize() << "> to <" << hostName << ">";
	}
	else
	{
		resp = requestHttp(web::http::methods::DEL, path, {}, {}, nullptr);
		LOG_INF << fname << "delete topology for <" << hostName << ">";
	}
	if (resp.status_code() == web::http::status_codes::OK)
	{
		auto result = resp.extract_utf8string(true).get();
		if (result == "true")
		{
			return true;
		}
		else
		{
			LOG_WAR << fname << " PUT " << path << " failed with response : " << result;
		}
	}
	return false;
}

/*
[
	{
		"CreateIndex": 22935,
		"Flags": 0,
		"Key": "appmgr/topology/",
		"LockIndex": 0,
		"ModifyIndex": 22935,
		"Value": null
	},
	{
		"CreateIndex": 22942,
		"Flags": 0,
		"Key": "appmgr/topology/cents",
		"LockIndex": 0,
		"ModifyIndex": 22942,
		"Value": "WyJteWFwcCJd"
	}
]*/
std::map<std::string, std::shared_ptr<ConsulTopology>> ConsulConnection::retrieveTopology(std::string host)
{
	const static char fname[] = "ConsulConnection::retrieveTopology() ";

	// /appmgr/topology/myhost
	std::map<std::string, std::shared_ptr<ConsulTopology>> topology;
	auto path = std::string(CONSUL_BASE_PATH).append("topology");
	if (host.length()) path.append("/").append(host);
	auto resp = requestHttp(web::http::methods::GET, path, { {"recurse","true"} }, {}, nullptr);
	if (resp.status_code() == web::http::status_codes::OK)
	{
		auto json = resp.extract_json(true).get();
		if (json.is_array())
		{
			for (const auto& section : json.as_array())
			{
				if (HAS_JSON_FIELD(section, "Value"))
				{
					// int consulIndex = GET_JSON_INT_VALUE(section, "ModifyIndex");
					auto hostText = Utility::decode64(GET_JSON_STR_VALUE(section, "Value"));
					if (hostText.empty()) continue;
					auto consulKey = GET_JSON_STR_VALUE(section, "Key");
					auto vec = Utility::splitString(consulKey, "/");
					auto hostName = vec[vec.size() - 1];
					auto appArrayJson = web::json::value::parse(hostText);
					if (appArrayJson.is_array())
					{
						topology[hostName] = ConsulTopology::FromJson(appArrayJson, hostName);
						LOG_DBG << fname << "get <" << appArrayJson.size() << "> task for <" << hostName << ">";
					}
				}
			}
		}
	}
	else
	{
		LOG_DBG << fname << "no topology found for <" << host << ">";
	}
	return std::move(topology);
}

/*
[
	{
		"CreateIndex": 22168,
		"Flags": 0,
		"Key": "appmgr/task/",
		"LockIndex": 0,
		"ModifyIndex": 22168,
		"Value": null
	},
	{
		"CreateIndex": 22241,
		"Flags": 0,
		"Key": "appmgr/task/myapp",
		"LockIndex": 0,
		"ModifyIndex": 22241,
		"Value": "ewoJCQkJInJlcGxpY2F0aW9uIjogMiwKCQkJCSJjb250ZW50IjogewoJCQkJCSJuYW1lIjogIm15YXBwIiwKCQkJCQkiY29tbWFuZCI6ICJzbGVlcCAzMCIKCQkJCX0KfQ=="
	}
]
*/
std::map<std::string, std::shared_ptr<ConsulTask>> ConsulConnection::retrieveTask()
{
	const static char fname[] = "ConsulConnection::retrieveTask() ";

	std::map<std::string, std::shared_ptr<ConsulTask>> result;
	// /appmgr/task/myapp
	std::string path = std::string(CONSUL_BASE_PATH).append("task");
	auto resp = requestHttp(web::http::methods::GET, path, { {"recurse","true"} }, {}, nullptr);
	if (resp.status_code() == web::http::status_codes::OK)
	{
		auto json = resp.extract_json(true).get();
		if (json.is_array())
		{
			for (const auto& section : json.as_array())
			{
				if (HAS_JSON_FIELD(section, "Value") && GET_JSON_STR_VALUE(section, "Key") != "appmgr/task")
				{
					auto appText = Utility::decode64(GET_JSON_STR_VALUE(section, "Value"));
					auto appJson = web::json::value::parse(appText);
					auto task = ConsulTask::FromJson(appJson);
					if (task->m_app && task->m_app->getName().length() && task->m_replication)
					{
						result[task->m_app->getName()] = task;
						LOG_DBG << fname << "get task <" << task->m_app->getName() << ">";
					}
				}
			}
		}
	}
	return std::move(result);
}

/*
[
	"appmgr/nodes/cents"
]
*/
std::map<std::string, std::shared_ptr<ConsulNode>> ConsulConnection::retrieveNode()
{
	const static char fname[] = "ConsulConnection::retrieveNode() ";

	std::map<std::string, std::shared_ptr<ConsulNode>> result;
	auto now = std::chrono::system_clock::now();
	auto reportInterval = Configuration::instance()->getConsul()->m_reportInterval;
	// /appmgr/nodes
	std::string path = std::string(CONSUL_BASE_PATH).append("nodes");
	auto resp = requestHttp(web::http::methods::GET, path, { {"recurse","true"} }, {}, nullptr);
	if (resp.status_code() == web::http::status_codes::OK)
	{
		auto json = resp.extract_json(true).get();
		if (json.is_array())
		{
			for (const auto& section : json.as_array())
			{
				if (section.has_string_field("Key") && section.has_string_field("Value") && section.at("Value").as_string().length())
				{
					auto key = GET_JSON_STR_VALUE(section, "Key");
					auto flags = GET_JSON_NUMBER_VALUE(section, "Flags");
					auto timeDiff = std::chrono::duration_cast<std::chrono::seconds>(now - std::chrono::system_clock::from_time_t(flags)).count();
					// ignore node when last update time more than reportInterval * 3 (consider time diff between hosts)
					// TODO: need to sync-up clock in the whole cluster
					if (Utility::startWith(key, "appmgr/nodes/") && timeDiff <= reportInterval * 3)
					{
						auto host = Utility::stringReplace(key, "appmgr/nodes/", "");
						auto value = web::json::value::parse(Utility::decode64(section.at("Value").as_string()));
						result[host] = ConsulNode::FromJson(value, host);
					}
				}
			}
		}
	}
	LOG_DBG << fname << "get nodes size : " << result.size();
	return std::move(result);
}

void ConsulConnection::watchSecurity()
{
	const static char fname[] = "ConsulConnection::watchSecurity() ";

	auto self = this->shared_from_this();
	long long lastIndex = 0;
	auto path = std::string(CONSUL_BASE_PATH).append("security");
	while (!m_threadDestroy)
	{
		auto newIndex = blockQueryKV(path, lastIndex, 50);
		if (newIndex > 0 && newIndex != lastIndex)
		{
			lastIndex = newIndex;
			LOG_INF << fname << "fire with index : " << lastIndex;
			this->syncSecurity();
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}
}

void ConsulConnection::watchTopology()
{
	const static char fname[] = "ConsulConnection::watchTopology() ";

	auto self = this->shared_from_this();
	long long lastIndex = 0;
	auto path = std::string(CONSUL_BASE_PATH).append("topology/").append(MY_HOST_NAME);
	while (!m_threadDestroy)
	{
		auto newIndex = blockQueryKV(path, lastIndex, 50);
		if (newIndex > 0 && newIndex != lastIndex)
		{
			lastIndex = newIndex;
			LOG_INF << fname << "fire with index : " << lastIndex;
			this->syncNodeTopology();
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}
}

long long ConsulConnection::blockQueryKV(const std::string& kvPath, long long lastIndex, int waitSeconds)
{
	const static char fname[] = "ConsulConnection::blockQueryKV() ";

	auto resp = requestHttp(web::http::methods::GET, kvPath, { {"index", std::to_string(lastIndex)}, {"wait", std::to_string(waitSeconds) + "s"} }, {}, nullptr);
	if (resp.headers().has("X-Consul-Index"))
	{
		auto indexStr = resp.headers()["X-Consul-Index"];
		if (Utility::isNumber(indexStr))
		{
			return std::atoll(indexStr.c_str());
		}
		else
		{
			LOG_WAR << fname << kvPath << " return incorrect X-Consul-Index header : " << indexStr;
		}
	}
	return -1;
}

void ConsulConnection::initTimer(const std::string& recoveredConsulSsnId)
{
	const static char fname[] = "ConsulConnection::initTimer() ";
	LOG_DBG << fname;

	if (!Configuration::instance()->getConsul()->consulEnabled()) return;

	if (!recoveredConsulSsnId.empty())
	{
		std::lock_guard<std::recursive_mutex> guard(m_mutex);
		m_sessionId = recoveredConsulSsnId;
	}

	// session renew timer
	this->cancleTimer(m_ssnRenewTimerId);
	if (Configuration::instance()->getConsul()->m_ttl > 10)
	{
		m_ssnRenewTimerId = this->registerTimer(
			0,
			Configuration::instance()->getConsul()->m_ttl - 3,
			std::bind(&ConsulConnection::refreshSession, this, std::placeholders::_1),
			__FUNCTION__
		);
	}

	// report status timer
	this->cancleTimer(m_reportStatusTimerId);
	if (Configuration::instance()->getConsul()->m_reportInterval > 3)
	{
		m_reportStatusTimerId = this->registerTimer(
			1000L * 1,
			Configuration::instance()->getConsul()->m_reportInterval,
			std::bind(&ConsulConnection::reportStatus, this, std::placeholders::_1),
			__FUNCTION__
		);
	}

	// schedule timer
	if (Configuration::instance()->getConsul()->m_scheduleInterval > 1 && m_scheduleThread == nullptr)
	{
		m_scheduleThread = std::make_unique<std::thread>(std::bind(&ConsulConnection::schedule, this));
	}

	// topology thread
	if (Configuration::instance()->getConsul()->m_isNode && m_topologyThread == nullptr)
	{
		m_topologyThread = std::make_unique<std::thread>(std::bind(&ConsulConnection::watchTopology, this));
	}
	// security thread
	if (Configuration::instance()->getConsul()->consulSecurityEnabled() && m_securityThread == nullptr)
	{
		m_securityThread = std::make_unique<std::thread>(std::bind(&ConsulConnection::watchSecurity, this));
	}
}

const std::string ConsulConnection::getConsulSessionId()
{
	std::lock_guard<std::recursive_mutex> guard(m_mutex);
	return m_sessionId;
}

web::http::http_response ConsulConnection::requestHttp(const web::http::method& mtd, const std::string& path, std::map<std::string, std::string> query, std::map<std::string, std::string> header, web::json::value* body)
{
	const static char fname[] = "ConsulConnection::requestHttp() ";

	auto restURL = Configuration::instance()->getConsul()->m_consulUrl;

	// Create http_client to send the request.
	web::http::client::http_client_config config;
	config.set_timeout(std::chrono::seconds(5));
	config.set_validate_certificates(false);
	web::http::client::http_client client(restURL, config);

	// Build request URI and start the request.
	web::uri_builder builder(GET_STRING_T(path));
	std::for_each(query.begin(), query.end(), [&builder](const std::pair<std::string, std::string>& pair)
		{
			builder.append_query(GET_STRING_T(pair.first), GET_STRING_T(pair.second));
		});

	web::http::http_request request(mtd);
	for (const auto& h : header)
	{
		request.headers().add(h.first, h.second);
	}
	request.set_request_uri(builder.to_uri());
	if (body != nullptr)
	{
		request.set_body(Utility::prettyJson(body->serialize()));
	}
	try
	{
		// In case of REST server crash or block query timeout, will throw exception:
		// "Failed to read HTTP status line"
		web::http::http_response response = client.request(request).get();
		LOG_DBG << fname << path << " return " << response.status_code();
		return std::move(response);
	}
	catch (const std::exception & ex)
	{
		LOG_DBG << fname << path << " got exception: " << ex.what();
	}
	catch (...)
	{
		LOG_DBG << fname << path  << " exception";
	}
	web::http::http_response response(web::http::status_codes::ResetContent);
	return std::move(response);
}
