#include "core.h"
#include "auth.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static unsigned tests;
#define CHECK(x) do { tests++; assert(x); } while (0)
static cJSON *json(const char *text) { cJSON *v = cJSON_Parse(text); assert(v); return v; }
int main(void) {
    CHECK(sizeof(tg_config) == 936); /* NVS blob layout */
    CHECK(tg_on_captive_interface(true, "192.168.4.1", "192.168.4.1"));
    CHECK(!tg_on_captive_interface(false, "192.168.4.1", "192.168.4.1"));
    CHECK(!tg_on_captive_interface(true, "192.0.2.10", "192.168.4.1"));
    CHECK(!tg_on_captive_interface(true, "192.168.4.1", ""));
    CHECK(!tg_on_captive_interface(true, NULL, "192.168.4.1"));
    CHECK(tg_parse_json("{\"code\":0}garbage") == NULL);
    char deep[100]; memset(deep,'[',40); memset(deep+40,']',40); deep[80]=0;
    CHECK(tg_parse_json(deep) == NULL);
    CHECK(tg_phone_valid("13800000000")); CHECK(!tg_phone_valid("1380000000")); CHECK(!tg_phone_valid("1380000000X"));
    CHECK(tg_code_valid("ABcd1234", false)); CHECK(!tg_code_valid("", false)); CHECK(tg_code_valid("", true)); CHECK(!tg_code_valid("1234567890123", false));
    CHECK(tg_url_allowed("https://uums.taoyuangu.com:8443/uums/servlet/x", false, false));
    CHECK(tg_url_allowed("wss://sso.taoyuangu.com/htmlvdcs/wsserver/websocket?token=fake", false, true));
    CHECK(tg_url_allowed("https://cdn.taoyuangu.com/image.png", true, false));
    const char *bad[] = {"http://sso.taoyuangu.com/", "https://evil.example/", "https://sso.taoyuangu.com@evil.example/", "https://sso.taoyuangu.com.evil.example/", "https://sso.taoyuangu.com:1234/", "https://sso.taoyuangu.com/#x", "https://sso.taoyuangu.com/\r\nx", "https://sso.taoyuangu.com\\evil/"};
    for (unsigned i=0;i<sizeof(bad)/sizeof(*bad);i++) CHECK(!tg_url_allowed(bad[i], false, false));
    cJSON *v = json("{\"code\":0,\"data\":{\"areaList\":[]}}"); CHECK(tg_cps_ok(v)); CHECK(!tg_uums_ok(v, "0")); cJSON_Delete(v);
    v = json("{\"result\":\"0\",\"code\":\"1\"}"); CHECK(tg_uums_ok(v,"1")); CHECK(!tg_uums_ok(v,"0")); cJSON_Delete(v);
    v = json("{\"result\":\"0\",\"code\":0,\"data\":\"开门成功\"}"); CHECK(tg_open_ok(v)); cJSON_Delete(v);
    v = json("{\"result\":\"0\",\"code\":0,\"data\":\"失败\"}"); CHECK(!tg_open_ok(v)); cJSON_Delete(v);
    v = json("{\"code\":0,\"data\":\"开门成功\"}"); CHECK(!tg_open_ok(v)); cJSON_Delete(v);
    v = json("{\"result\":false,\"code\":false,\"data\":\"开门成功\"}"); CHECK(!tg_open_ok(v)); cJSON_Delete(v);
    v = json("{\"code\":0,\"data\":{\"areaList\":[{\"organizationSeq\":\"org\",\"list\":[{\"valid\":1,\"mobile\":\"13800000000\",\"token\":\"fake-token\",\"householdSerial\":\"room\"}]}]}}");
    tg_config cfg = {0}; tg_room room; char detail[TG_DETAIL] = "";
    CHECK(tg_select_room(v,&cfg,&room,detail)); CHECK(!strcmp(room.household,"room"));
    strcpy(cfg.household,"other"); CHECK(!tg_select_room(v,&cfg,&room,detail)); CHECK(!*room.token); cfg.household[0]=0;
    cJSON *r = cJSON_GetArrayItem(cJSON_GetObjectItem(cJSON_GetArrayItem(cJSON_GetObjectItem(cJSON_GetObjectItem(v,"data"),"areaList"),0),"list"),0);
    cJSON_ReplaceItemInObject(r,"valid",cJSON_CreateNumber(4)); CHECK(!tg_select_room(v,&cfg,&room,detail)); cJSON_Delete(v);
    const char *plain="{\"result\":\"0\",\"data\":{\"SIP_PASSWORD\":\"never-export\",\"DOOR_CONTROL\":{\"DOOR_GROUP_LIST\":[],\"NO_GROUP_LIST\":[{\"Code\":\"door\",\"Lock\":[{\"Show\":\"1\",\"Number\":\"0\",\"Name\":\"first\"},{\"Show\":1,\"Number\":1,\"Name\":\"second\"}]}]}}}";
    tg_door doors[TG_MAX_DOORS]; size_t count=0,index=0;
    v=json(plain); CHECK(tg_parse_doors(v,doors,&count,detail)); CHECK(count==2); CHECK(!tg_select_door(&cfg,doors,count,&index));
    strcpy(cfg.door_number,"0"); CHECK(tg_select_door(&cfg,doors,count,&index)); CHECK(index==0);
    strcpy(cfg.door_number,"1"); CHECK(tg_select_door(&cfg,doors,count,&index)); CHECK(index==1);
    strcpy(cfg.door_code,"wrong"); CHECK(!tg_select_door(&cfg,doors,count,&index)); cJSON_Delete(v);
    v=json("{\"result\":0,\"data\":{\"DOOR_CONTROL\":{\"NO_GROUP_LIST\":[],\"DOOR_GROUP_LIST\":[{\"Name\":\"group\",\"LockGroup\":[{\"Show\":1,\"Lock\":{\"DeviceCode\":\"group-door\",\"Show\":1,\"Number\":0}}]}]}}}");
    CHECK(tg_parse_doors(v,doors,&count,detail)); CHECK(count==1); CHECK(!strcmp(doors[0].code,"group-door")); cJSON_Delete(v);
    v=json("{\"result\":0,\"data\":{\"DOOR_CONTROL\":{\"DOOR_GROUP_LIST\":[],\"NO_GROUP_LIST\":[{\"Code\":null,\"Lock\":{\"Show\":1,\"Number\":true}}]}}}");
    CHECK(!tg_parse_doors(v,doors,&count,detail)); CHECK(count==0); cJSON_Delete(v);
    unsigned char salt[16], digest[32]; char hex[65]; for (int i=0;i<16;i++) salt[i]=i;
    CHECK(tg_password_hash("test-password",salt,digest));
    for (int i=0;i<32;i++) snprintf(hex+2*i,3,"%02x",digest[i]);
    CHECK(!strcmp(hex,"cb4208b8bb0ac07803f4ad2f3387c76f96e47f4516452abe44c24e68d93b43d4"));
    printf("%u core assertions passed; no network or OpenDoor sent\n",tests); return 0;
}
