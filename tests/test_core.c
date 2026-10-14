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
    tg_target_state target = {0}; char source[192];
    CHECK(!tg_call_source(&target, "", source, sizeof(source))); CHECK(!*source);
    CHECK(!tg_call_source(&target, "door-a", source, sizeof(source))); CHECK(!strcmp(source, "未知门禁"));
    target.door_selected = true; strcpy(target.door_code, "door-a"); strcpy(target.door_name, "一号门禁");
    CHECK(!tg_call_source(&target, "door-a", source, sizeof(source))); CHECK(!strcmp(source, "未知门禁"));
    target.caller_unique = true;
    CHECK(tg_call_source(&target, "door-a", source, sizeof(source))); CHECK(!strcmp(source, "一号门禁"));
    CHECK(!tg_call_source(&target, "door-b", source, sizeof(source))); CHECK(!strcmp(source, "未知门禁"));
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
    strcpy(doors[0].code, "other-door");
    CHECK(tg_caller_target_unique(1, doors, count, index));
    CHECK(!tg_caller_target_unique(2, doors, count, index));
    strcpy(doors[0].code, doors[1].code); CHECK(!tg_caller_target_unique(1, doors, count, index));
    strcpy(cfg.door_code,"wrong"); CHECK(!tg_select_door(&cfg,doors,count,&index)); cJSON_Delete(v);
    v=json("{\"result\":0,\"data\":{\"DOOR_CONTROL\":{\"NO_GROUP_LIST\":[],\"DOOR_GROUP_LIST\":[{\"Name\":\"group\",\"LockGroup\":[{\"Show\":1,\"Lock\":{\"DeviceCode\":\"group-door\",\"Show\":1,\"Number\":0}}]}]}}}");
    CHECK(tg_parse_doors(v,doors,&count,detail)); CHECK(count==1); CHECK(!strcmp(doors[0].code,"group-door")); cJSON_Delete(v);
    v=json("{\"result\":0,\"data\":{\"DOOR_CONTROL\":{\"DOOR_GROUP_LIST\":[],\"NO_GROUP_LIST\":[{\"Code\":null,\"Lock\":{\"Show\":1,\"Number\":true}}]}}}");
    CHECK(!tg_parse_doors(v,doors,&count,detail)); CHECK(count==0); cJSON_Delete(v);
    unsigned char salt[16], digest[32]; char hex[65]; for (int i=0;i<16;i++) salt[i]=i;
    CHECK(tg_password_hash("test-password",salt,digest));
    for (int i=0;i<32;i++) snprintf(hex+2*i,3,"%02x",digest[i]);
    CHECK(!strcmp(hex,"cb4208b8bb0ac07803f4ad2f3387c76f96e47f4516452abe44c24e68d93b43d4"));
    /* Talkback: login password is Base64 ("123456"), only its MD5 is kept. */
    char md5[33];
    CHECK(tg_talk_password("MTIzNDU2", md5)); CHECK(!strcmp(md5, "e10adc3949ba59abbe56e057f20f883e"));
    CHECK(!tg_talk_password("", md5)); CHECK(!tg_talk_password("!!!", md5)); CHECK(!*md5);
    char text[64];
    CHECK(tg_xml_text("<message><safeCode>a&amp;b</safeCode></message>", "safeCode", text, sizeof(text))); CHECK(!strcmp(text, "a&b"));
    CHECK(tg_xml_text("<message><faultText/></message>", "faultText", text, sizeof(text))); CHECK(!*text);
    CHECK(!tg_xml_text("<message><safeCode>x</message>", "safeCode", text, sizeof(text)));
    CHECK(!tg_xml_text("<a>&bogus;</a>", "a", text, sizeof(text)));
    CHECK(!tg_xml_text("<a>0123456789</a>", "a", text, 8));
    tg_sip_config sip;
    const char *xml = "<message><faultCode>0</faultCode><SIP_ACCOUNT>8001</SIP_ACCOUNT><SIP_PASSWORD>fake-pass</SIP_PASSWORD>"
        "<SIP_DOMAIN>192.0.2.7</SIP_DOMAIN><SIP_PROXY></SIP_PROXY><SIP_PROXY_PORT>6000</SIP_PROXY_PORT></message>";
    CHECK(tg_talk_config(xml, 5060, &sip, detail)); CHECK(!strcmp(sip.proxy, "192.0.2.7")); CHECK(sip.port == 6000);
    CHECK(!tg_talk_config("<message><faultCode>3</faultCode><faultText>no</faultText></message>", 6000, &sip, detail)); CHECK(strstr(detail, "no"));
    CHECK(!tg_talk_config("<message><SIP_ACCOUNT>a b</SIP_ACCOUNT><SIP_PASSWORD>p</SIP_PASSWORD><SIP_DOMAIN>d</SIP_DOMAIN></message>", 6000, &sip, detail));
    CHECK(!tg_talk_config("<message><SIP_ACCOUNT>a</SIP_ACCOUNT><SIP_PASSWORD>p</SIP_PASSWORD><SIP_DOMAIN>evil/x</SIP_DOMAIN></message>", 6000, &sip, detail));
    CHECK(!*sip.password);
    /* SIP parsing, including compact header names and every Via. */
    const char *invite = "INVITE sip:8001@192.0.2.20:5070 SIP/2.0\r\nv: SIP/2.0/UDP 192.0.2.7:6000;branch=z9hG4bK1;rport\r\n"
        "Via: SIP/2.0/UDP 192.0.2.30;branch=z9hG4bK2\r\nFrom: \"Door\" <sip:9001@192.0.2.7>;tag=abc\r\nTo: <sip:8001@192.0.2.7>\r\n"
        "Call-ID: id-1\r\nCSeq: 1 INVITE\r\nl: 0\r\n\r\n";
    char via[256], value[64];
    CHECK(tg_sip_status(invite) == 0); CHECK(tg_sip_method(invite, value, sizeof(value))); CHECK(!strcmp(value, "INVITE"));
    CHECK(tg_sip_headers(invite, "Via", via, sizeof(via))); CHECK(!strncmp(via, "Via: SIP/2.0/UDP 192.0.2.7:6000", 31)); CHECK(strstr(via, "z9hG4bK2"));
    CHECK(!tg_sip_headers(invite, "Via", via, 40));
    CHECK(tg_sip_header(invite, "From", value, sizeof(value))); CHECK(tg_sip_user(value, text, sizeof(text))); CHECK(!strcmp(text, "9001"));
    CHECK(tg_sip_param(value, "tag", text, sizeof(text))); CHECK(!strcmp(text, "abc"));
    CHECK(tg_sip_header(invite, "Content-Length", value, sizeof(value))); CHECK(!strcmp(value, "0"));
    CHECK(!tg_sip_header(invite, "Contact", value, sizeof(value)));
    CHECK(tg_sip_status("SIP/2.0 401 Unauthorized\r\n\r\n") == 401); CHECK(tg_sip_status("SIP/2.0 4x1 Bad\r\n") == 0);
    CHECK(tg_sip_param("Digest realm=\"r\", nonce=\"n1\", qop=\"auth\"", "nonce", text, sizeof(text))); CHECK(!strcmp(text, "n1"));
    CHECK(tg_sip_param("SIP/2.0/UDP 10.0.0.2:5070;rport=4321;received=198.51.100.9", "received", text, sizeof(text))); CHECK(!strcmp(text, "198.51.100.9"));
    CHECK(tg_sip_param("SIP/2.0/UDP 10.0.0.2;rport;branch=x", "rport", text, sizeof(text))); CHECK(!*text);
    CHECK(!tg_sip_param("SIP/2.0/UDP 10.0.0.2;xrport=1", "rport", text, sizeof(text)));
    CHECK(!tg_sip_user("<sip:no-host>", text, sizeof(text)));
    CHECK(tg_sip_expires("SIP/2.0 200 OK\r\nContact: <sip:8001@192.0.2.20:5070>;expires=120\r\nExpires: 3600\r\n\r\n", 3600) == 120);
    CHECK(tg_sip_expires("SIP/2.0 200 OK\r\nExpires: 300\r\n\r\n", 3600) == 300);
    CHECK(tg_sip_expires("SIP/2.0 200 OK\r\nm: <sip:8001@192.0.2.20>\r\n\r\n", 3600) == 3600);
    CHECK(tg_sip_expires("SIP/2.0 200 OK\r\nExpires: 9x\r\n\r\n", 3600) == 3600);
    /* RFC 2617 section 3.5 example. */
    tg_sip_digest("Mufasa", "testrealm@host.com", "Circle Of Life", "GET", "/dir/index.html", "dcd98b7102dd2f0e8b11d0f600bfb0c093", "auth", "00000001", "0a4f113b", md5);
    CHECK(!strcmp(md5, "6629fae49393a05397450978507c4ef1"));
    printf("%u core assertions passed; no network or OpenDoor sent\n",tests); return 0;
}
