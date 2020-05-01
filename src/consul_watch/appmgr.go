package main

import (
	"encoding/base64"
	"encoding/json"
	"github.com/jbrodriguez/mlog"
	"io/ioutil"
)

var (
	AppManagerUrl = "https://10.1.241.54:6060"
	restTimeout   = 60
)

type jwtResponse struct {
	accessToken string `json:"access_token,omitempty"`
	expireTime  string `json:"expire_time,omitempty"`
	tokenType   string `json:"token_type,omitempty"`
}

func getAppMgrToken(userName string, userPwd string) (string, error) {

	userNameBase64 := base64.StdEncoding.EncodeToString([]byte(userName))
	userPwdBase64 := base64.StdEncoding.EncodeToString([]byte(userPwd))
	headers := map[string]string{
		"username": userNameBase64,
		"password": userPwdBase64,
	}
	params := map[string]string{}
	body := map[string]interface{}{}
	response, err := Post(AppManagerUrl, "", body, params, headers, restTimeout)
	if err != nil {
		mlog.IfError(err)
		return "", err
	}
	// close the response finally
	defer response.Body.Close()
	if response.StatusCode != 200 {
		mlog.Warning("response failed with status code : <%s>", response.Status)
		return "", err
	}
	responseBytes, _ := ioutil.ReadAll(response.Body)
	jsonStructObj := jwtResponse{}
	err = json.Unmarshal(responseBytes, &jsonStructObj)
	if err != nil {
		mlog.IfError(err)
		return "", err
	}
	mlog.Trace("get token success")
	return jsonStructObj.accessToken, nil
}
