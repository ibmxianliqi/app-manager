package main

import (
	"bytes"
	"crypto/tls"
	"encoding/json"
	"github.com/jbrodriguez/mlog"
	"net/http"
	"time"
)

//skip https ssl certification
var transport = &http.Transport{TLSClientConfig: &tls.Config{InsecureSkipVerify: true}}

//Get http get method
func Get(url string, reqToken string, queryList map[string]string, headerList map[string]string, timeoutSeconds int) (*http.Response, error) {
	//new request
	req, err := http.NewRequest("GET", url, nil)
	if err != nil {
		mlog.IfError(err)
		return nil, err
	}
	//add queryList
	q := req.URL.Query()
	if queryList != nil {
		for key, val := range queryList {
			q.Add(key, val)
		}
		req.URL.RawQuery = q.Encode()
	}
	//add headerList
	if headerList != nil {
		for key, val := range headerList {
			req.Header.Add(key, val)
		}
	}
	// add token
	if reqToken != "" {
		req.Header.Set("Authorization", "Bearer "+reqToken)
	}
	//http client
	client := &http.Client{
		Transport: transport,
		Timeout:   time.Second * time.Duration(timeoutSeconds),
	}
	mlog.Info("Go GET URL : %s", req.URL.String())
	return client.Do(req)
}

//Post http post method
func Post(url string, reqToken string, body map[string]interface{}, params map[string]string, headers map[string]string, timeoutSeconds int) (*http.Response, error) {
	//add post body
	var bodyJson []byte
	var req *http.Request
	if body != nil {
		var err error
		bodyJson, err = json.Marshal(body)
		if err != nil {
			mlog.IfError(err)
			return nil, err
		}
	}
	req, err := http.NewRequest("POST", url, bytes.NewBuffer(bodyJson))
	if err != nil {
		mlog.IfError(err)
		return nil, err
	}
	req.Header.Set("Content-type", "application/json")
	// add token
	if reqToken != "" {
		req.Header.Set("Authorization", "Bearer "+reqToken)
	}
	//add params
	q := req.URL.Query()
	if params != nil {
		for key, val := range params {
			q.Add(key, val)
		}
		req.URL.RawQuery = q.Encode()
	}
	//add headers
	if headers != nil {
		for key, val := range headers {
			req.Header.Add(key, val)
		}
	}
	//http client
	client := &http.Client{
		Transport: transport,
		Timeout:   time.Second * time.Duration(timeoutSeconds),
	}
	mlog.Info("Go POST URL : %s", req.URL.String())

	return client.Do(req)
}
