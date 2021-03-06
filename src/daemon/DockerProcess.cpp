#include <thread>
#include <ace/Barrier.h>
#include "DockerProcess.h"
#include "../common/Utility.h"
#include "../common/os/pstree.hpp"
#include "LinuxCgroup.h"
#include "MonitoredProcess.h"
#include "ResourceLimitation.h"

DockerProcess::DockerProcess(int cacheOutputLines, std::string dockerImage, std::string appName)
	: AppProcess(cacheOutputLines), m_dockerImage(dockerImage),
	m_appName(appName), m_lastFetchTime(std::chrono::system_clock::now())
{
}


DockerProcess::~DockerProcess()
{
	DockerProcess::killgroup();
}

void DockerProcess::killgroup(int timerId)
{
	const static char fname[] = "DockerProcess::killgroup() ";

	// get and clean container id
	std::string containerId;
	{
		std::lock_guard<std::recursive_mutex> guard(m_mutex);
		containerId = m_containerId;
		m_containerId.clear();
	}

	// clean docker container
	if (!containerId.empty())
	{
		std::string cmd = "docker rm -f " + containerId;
		AppProcess proc(0);
		proc.spawnProcess(cmd, "", "", {}, nullptr, "");
		if (proc.wait(ACE_Time_Value(3)) <= 0)
		{
			LOG_ERR << fname << "cmd <" << cmd << "> killed due to timeout";
			proc.killgroup();
		}
	}

	if (m_imagePullProc != nullptr && m_imagePullProc->running())
	{
		m_imagePullProc->killgroup();
	}
	// detach manully
	this->detach();
}

int DockerProcess::syncSpawnProcess(std::string cmd, std::string user, std::string workDir, std::map<std::string, std::string> envMap, std::shared_ptr<ResourceLimitation> limit, std::string stdoutFile)
{
	const static char fname[] = "DockerProcess::syncSpawnProcess() ";

	killgroup();
	int pid = ACE_INVALID_PID;
	constexpr int dockerCliTimeoutSec = 5;
	std::string containerName = m_appName;

	// 0. clean old docker contianer (docker container will left when host restart)
	std::string dockerCommand = "docker rm -f " + containerName;
	AppProcess proc(0);
	proc.spawnProcess(dockerCommand, "", "", {}, nullptr, stdoutFile);
	proc.wait();

	// 1. check docker image
	dockerCommand = "docker inspect -f '{{.Size}}' " + m_dockerImage;
	auto dockerProcess = std::make_shared<MonitoredProcess>(32, false);
	pid = dockerProcess->spawnProcess(dockerCommand, "", "", {}, nullptr, stdoutFile);
	dockerProcess->regKillTimer(dockerCliTimeoutSec, fname);
	dockerProcess->runPipeReaderThread();
	auto imageSizeStr = Utility::stdStringTrim(dockerProcess->fetchLine());
	if (!Utility::isNumber(imageSizeStr) || std::stoi(imageSizeStr) < 1)
	{
		LOG_WAR << fname << "docker image <" << m_dockerImage << "> not exist, try to pull.";

		// pull docker image
		int pullTimeout = 5 * 60; //set default image pull timeout to 5 minutes
		if (envMap.count(ENV_APP_MANAGER_DOCKER_IMG_PULL_TIMEOUT) && Utility::isNumber(envMap[ENV_APP_MANAGER_DOCKER_IMG_PULL_TIMEOUT]))
		{
			pullTimeout = std::stoi(envMap[ENV_APP_MANAGER_DOCKER_IMG_PULL_TIMEOUT]);
		}
		else
		{
			LOG_WAR << fname << "use default APP_MANAGER_DOCKER_IMG_PULL_TIMEOUT <" << pullTimeout << ">";
		}
		m_imagePullProc = std::make_shared<MonitoredProcess>(m_cacheOutputLines);
		m_imagePullProc->spawnProcess("docker pull " + m_dockerImage, "root", workDir, {}, nullptr, stdoutFile);
		m_imagePullProc->regKillTimer(pullTimeout, fname);
		this->attach(m_imagePullProc->getpid());
		return m_imagePullProc->getpid();
	}

	// 2. build docker start command line
	dockerCommand = std::string("docker run -d ") + "--name " + containerName;
	for (auto env : envMap)
	{
		if (env.first == ENV_APP_MANAGER_DOCKER_PARAMS)
		{
			// used for -p -v parameter
			dockerCommand.append(" ");
			dockerCommand.append(env.second);
		}
		else
		{
			bool containSpace = (env.second.find(' ') != env.second.npos);

			dockerCommand += " -e ";
			dockerCommand += env.first;
			dockerCommand += "=";
			if (containSpace) dockerCommand.append("'");
			dockerCommand += env.second;
			if (containSpace) dockerCommand.append("'");
		}
	}
	if (limit != nullptr)
	{
		if (limit->m_memoryMb)
		{
			dockerCommand.append(" --memory ").append(std::to_string(limit->m_memoryMb)).append("M");
			if (limit->m_memoryVirtMb && limit->m_memoryVirtMb > limit->m_memoryMb)
			{
				dockerCommand.append(" --memory-swap ").append(std::to_string(limit->m_memoryVirtMb - limit->m_memoryMb)).append("M");
			}
		}
		if (limit->m_cpuShares)
		{
			dockerCommand.append(" --cpu-shares ").append(std::to_string(limit->m_cpuShares));
		}
	}
	dockerCommand += " " + m_dockerImage;
	dockerCommand += " " + cmd;

	// 3. start docker container
	dockerProcess = std::make_shared<MonitoredProcess>(32, false);
	pid = dockerProcess->spawnProcess(dockerCommand, "", "", {}, nullptr, stdoutFile);
	dockerProcess->regKillTimer(dockerCliTimeoutSec, fname);
	dockerProcess->runPipeReaderThread();

	std::string containerId;
	if (dockerProcess->return_value() == 0)
	{
		containerId = Utility::stdStringTrim(dockerProcess->fetchLine());
	}
	else
	{
		LOG_WAR << fname << "started container <" << dockerCommand << "failed :" << dockerProcess->fetchOutputMsg();
	}
	if (containerId.length())
	{
		// set container id here for future clean
		std::lock_guard<std::recursive_mutex> guard(m_mutex);
		m_containerId = containerId;
	}
	else
	{
		throw std::invalid_argument(std::string("Start docker container failed :").append(dockerCommand));
	}

	// 4. get docker root pid
	dockerCommand = "docker inspect -f '{{.State.Pid}}' " + containerId;
	dockerProcess = std::make_shared<MonitoredProcess>(32, false);
	pid = dockerProcess->spawnProcess(dockerCommand, "", "", {}, nullptr, stdoutFile);
	dockerProcess->regKillTimer(dockerCliTimeoutSec, fname);
	dockerProcess->runPipeReaderThread();
	if (dockerProcess->return_value() == 0)
	{
		auto pidStr = Utility::stdStringTrim(dockerProcess->fetchLine());
		if (Utility::isNumber(pidStr))
		{
			pid = std::stoi(pidStr);
			if (pid > 1)
			{
				// Success
				this->attach(pid);
				std::lock_guard<std::recursive_mutex> guard(m_mutex);
				m_containerId = containerId;
				LOG_INF << fname << "started pid <" << pid << "> for container :" << m_containerId;
				return pid;
			}
		}
		else
		{
			LOG_WAR << fname << "can not get correct container pid :" << pidStr;
		}
	}
	else
	{
		LOG_WAR << fname << "started container <" << dockerCommand << "failed :" << dockerProcess->fetchOutputMsg();
	}

	// failed
	std::lock_guard<std::recursive_mutex> guard(m_mutex);
	m_containerId = containerId;
	this->detach();
	killgroup();
	return pid;
}

pid_t DockerProcess::getpid(void) const
{
	if (ACE_Process::getpid() == 1)
		return ACE_INVALID_PID;
	else
		return ACE_Process::getpid();
}

std::string DockerProcess::containerId()
{
	std::lock_guard<std::recursive_mutex> guard(m_mutex);
	return m_containerId;
}

void DockerProcess::containerId(std::string containerId)
{
	std::lock_guard<std::recursive_mutex> guard(m_mutex);
	m_containerId = containerId;
}

int DockerProcess::spawnProcess(std::string cmd, std::string user, std::string workDir, std::map<std::string, std::string> envMap, std::shared_ptr<ResourceLimitation> limit, std::string stdoutFile)
{
	const static char fname[] = "DockerProcess::spawnProcess() ";
	LOG_DBG << fname << "Entered";

	if (m_spawnThread != nullptr) return ACE_INVALID_PID;

	struct SpawnParams
	{
		std::string cmd;
		std::string user;
		std::string workDir;
		std::map<std::string, std::string> envMap;
		std::shared_ptr<ResourceLimitation> limit;
		std::shared_ptr<DockerProcess> thisProc;
		std::shared_ptr<ACE_Barrier> barrier;
	};
	auto param = std::make_shared<SpawnParams>();
	param->cmd = cmd;
	param->user = user;
	param->workDir = workDir;
	param->envMap = envMap;
	param->limit = limit;
	param->barrier = std::make_shared<ACE_Barrier>(2);
	param->thisProc = std::dynamic_pointer_cast<DockerProcess>(this->shared_from_this());

	m_spawnThread = std::make_shared<std::thread>(
		[param, stdoutFile]()
		{
			const static char fname[] = "DockerProcess::m_spawnThread() ";
			LOG_DBG << fname << "Entered";
			param->barrier->wait();	// wait here for m_spawnThread->detach() finished

			// use try catch to avoid throw from syncSpawnProcess crash
			try
			{
				param->thisProc->syncSpawnProcess(param->cmd, param->user, param->workDir, param->envMap, param->limit, stdoutFile);
			}
			catch (...)
			{
				LOG_ERR << fname << "failed";
			}
			param->thisProc->m_spawnThread = nullptr;
			param->thisProc = nullptr;
			LOG_DBG << fname << "Exited";
		}
	);
	m_spawnThread->detach();
	param->barrier->wait();
	// TBD: Docker app should not support short running here, since short running have kill and bellow attach is not real pid
	this->attach(1);
	return 1;
}

std::string DockerProcess::getOutputMsg()
{
	std::lock_guard<std::recursive_mutex> guard(m_mutex);
	if (m_containerId.length())
	{
		std::string dockerCommand = "docker logs --tail " + std::to_string(m_cacheOutputLines) + " " + m_containerId;
		return Utility::runShellCommand(dockerCommand);
	}
	return std::string();
}

std::string DockerProcess::fetchOutputMsg()
{
	std::lock_guard<std::recursive_mutex> guard(m_mutex);
	if (m_containerId.length())
	{
		//auto microsecondsUTC = std::chrono::duration_cast<std::chrono::seconds>(m_lastFetchTime.time_since_epoch()).count();
		auto timeSince = Utility::getRfc3339Time(m_lastFetchTime);
		std::string dockerCommand = "docker logs --since " + timeSince + " " + m_containerId;
		auto msg = Utility::runShellCommand(dockerCommand);
		m_lastFetchTime = std::chrono::system_clock::now();
		return std::move(msg);
	}
	return std::string();
}
