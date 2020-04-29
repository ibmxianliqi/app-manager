package main

import (
	"github.com/hashicorp/consul/api"
	"time"
)
import "fmt"

func main() {
	// Get a new client
	client, err := api.NewClient(api.DefaultConfig())
	if err != nil {
		panic(err)
	}

	// Get a handle to the KV API
	kv := client.KV()

	// PUT a new KV pair
	p := &api.KVPair{Key: "REDIS_MAXCLIENTS", Value: []byte("1000")}
	_, err = kv.Put(p, nil)
	if err != nil {
		panic(err)
	}

	queryOpt := makeQueryOptions()

	// Lookup the pair
	pair, _, err := kv.Get("REDIS_MAXCLIENTS", queryOpt)
	if err != nil {
		panic(err)
	}
	fmt.Printf("KV: %v %s\n", pair.Key, pair.Value)
}

func makeQueryOptions() *api.QueryOptions {
	return &api.QueryOptions{WaitTime: time.Duration(1 * time.Minute)}
}
