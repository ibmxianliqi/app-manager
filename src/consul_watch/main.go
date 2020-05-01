package main

import (
	"github.com/hashicorp/consul/api"
	"github.com/jbrodriguez/mlog"
	"time"

	"os"
)

const (
	ConsulTopologyPath = "appmgr/topology/"
	ConsulSecurityPath = "appmgr/security"
)

var (
	ConsulHostTopologyKey string
	ConsulClientObj       *api.Client
)

func main() {
	// init log
	initLogging(mlog.LevelInfo, 50, 5)

	// init KV key
	hostName, _ := os.Hostname()
	ConsulHostTopologyKey = ConsulTopologyPath + hostName

	// Get a new ConsulClientObj
	client, err := makeConsulClient()
	if err != nil {
		panic(err)
	}
	ConsulClientObj = client

	doneCh := make(chan bool)
	go watchTopology(doneCh)
	go watchSecurity(doneCh)
	<-doneCh

	mlog.Warning("exiting")
}

func makeConsulClient() (*api.Client, error) {
	os.Setenv(api.HTTPAddrEnvName, "10.1.241.54:8500")
	return api.NewClient(api.DefaultConfig())
}

func initLogging(level mlog.LogLevel, fileSizeMb int, fileRotateNum int) {
	// https://github.com/jbrodriguez/mlog
	if os.Getenv("LOG_NO_FILE") == "true" {
		// Write to stdout/stderr only
		mlog.Start(mlog.LevelInfo, "")
	} else {
		mbNumber := fileSizeMb
		logPath := os.Args[0] + ".log"
		mlog.StartEx(level, logPath, mbNumber*1024*1024, fileRotateNum)
		mlog.Info(logPath)
	}
}

func watchSecurity(doneCh chan bool) {
}
func watchTopology(doneCh chan bool) {
	// Get a handle to the KV API
	kv := ConsulClientObj.KV()

	pair, meta, err := kv.Get(ConsulHostTopologyKey, nil)
	if err != nil {
		mlog.IfError(err)
	}
	if pair == nil {
		mlog.Warning("no value for %#v", ConsulHostTopologyKey)
	}
	if meta.LastIndex == 0 {
		mlog.Warning("unexpected meta value: %#v", meta)
	}

	// Get should work
	options := &api.QueryOptions{WaitTime: time.Duration(30 * time.Second), WaitIndex: meta.LastIndex}
	pair, meta2, err := kv.Get(ConsulHostTopologyKey, options)
	if err != nil {
		mlog.IfError(err)
	}
	if pair == nil {
		mlog.Trace("no value for %#v", ConsulHostTopologyKey)
	}
	if meta.LastIndex == meta2.LastIndex {
		mlog.Trace("timeout, value is not changed %#v", meta.LastIndex)
	} else if meta2.LastIndex <= meta.LastIndex {
		mlog.Warning("unexpected meta value: %#v , %#v", meta.LastIndex, meta2.LastIndex)
	} else {
		mlog.Info("Value changed")
	}

	doneCh <- true
}
