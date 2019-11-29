#!/bin/bash
export LD_LIBRARY_PATH=$LD_LIBRARY_PATH:/opt/appmanager/lib64
if [ -f "/opt/appmanager/appc" ];then
	/opt/appmanager/appc view -l | awk '{if (NR>1){cmd="/opt/appmanager/appc disable -n "$2;print(cmd);system(cmd)}}'
fi

# backup configuration file to avoid next installation override
cp -f /opt/appmanager/appsvc.json /opt/appmanager/.appsvc.json
