"""No network and no physical door operation; deployment safety regression."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
COMPONENT = ROOT / "components" / "tygbot"
PACKAGE = ROOT / "packages" / "tygbot.yaml"

class SourceContract(unittest.TestCase):
    def test_no_native_boot_or_partition_in_component(self):
        self.assertFalse((COMPONENT / "main.c").exists())
        self.assertFalse((COMPONENT / "partitions.csv").exists())
        adapter = (COMPONENT / "tygbot.cpp").read_text()
        for forbidden in ("esp_wifi_init(", "nvs_flash_erase(", "esp_sntp_init(", "app_main("):
            self.assertNotIn(forbidden, adapter)

    def test_requested_no_password_does_not_remove_ota(self):
        config = PACKAGE.read_text()
        self.assertIn("require_auth: false", config)
        self.assertNotIn("tygbot_web_password", config)
        # The package builds the public firmware; credentials live in local configs.
        self.assertNotIn("!secret", config)
        self.assertIn("encryption: {}", config)
        example = (ROOT / "tygbot.example.yaml").read_text()
        for secret in ("tygbot_api_key", "tygbot_ota_password", "tygbot_fallback_password"):
            self.assertIn("!secret " + secret, example)
        self.assertIn("safe_mode:", config)
        self.assertIn("device_friendly_name: TYGBot", config)
        self.assertIn("bluetooth_proxy:\n  active: true", config)
        self.assertIn("name: xiongyu.tygbot", config)
        self.assertNotIn("name: xiongyumail.tygbot", config)

    def test_optional_auth_keeps_origin_guard(self):
        web = (COMPONENT / "web.c").read_text()
        self.assertIn("bool tg_http_auth_required = true", web)
        self.assertIn("if (!host_guard(req, path))", web)
        # Cross-site subresource requests (e.g. <img>) cannot trigger API side effects.
        self.assertIn('"Sec-Fetch-Site"', web)
        self.assertIn('strcmp(mode, "navigate")', web)
        self.assertIn("!public && tg_http_auth_required && !authorization(req)", web)
        self.assertIn("!tg_http_auth_required && !strcmp(path, \"/auth\")", web)

    def test_safe_mode_button_disabled_by_default_without_losing_recovery(self):
        config = PACKAGE.read_text()
        button = re.search(r"  - platform: safe_mode\n((?:    .*\n)+)", config)
        self.assertIsNotNone(button)
        self.assertIn("disabled_by_default: true", button.group(1))
        self.assertIn("entity_category: diagnostic", button.group(1))
        self.assertIn("\nsafe_mode:\n", config)
        self.assertIn("platform: esphome", config)
        self.assertNotIn("platform: restart", config)

    def test_ha_open_is_only_queued(self):
        web = (COMPONENT / "web.c").read_text()
        adapter = (COMPONENT / "tygbot.cpp").read_text()
        self.assertIn("tg_submit_open(dry)", adapter)
        self.assertNotIn("tg_open(", adapter)
        self.assertIn("if (j.req)", web)
        self.assertNotIn("ESP_ERROR_CHECK", web)

    def test_native_ha_web_link_without_dashboard_or_extra_web_instance(self):
        codegen = (COMPONENT / "__init__.py").read_text()
        self.assertIn('cg.add_define("USE_WEBSERVER")', codegen)
        self.assertIn('cg.add_define("USE_WEBSERVER_PORT", 80)', codegen)
        self.assertIn('AUTO_LOAD = ["json", "web_server_base"]', codegen)
        self.assertIn('("web_server.h", "list_entities.h")', codegen)
        self.assertIn('cg.add_define("MDNS_SERVICE_COUNT", int(str(counts[0].value)) + 1)', codegen)
        config = PACKAGE.read_text()
        self.assertNotIn("\nweb_server:", config)
        self.assertNotIn("service: _http", config)
        self.assertIsNone(re.search(r"\b(?:10|127|192\.168|172\.(?:1[6-9]|2\d|3[01]))\.\d+\.\d+", config))
        self.assertFalse((ROOT / "ha-login-card.yaml").exists())

    def test_core_sync_preserves_protocol_and_auth(self):
        for name in ("core.c", "protocol.c", "auth.c", "talk.c", "sip.c"):
            original = (ROOT / "core" / name).read_text()
            copy = (COMPONENT / name).read_text().split("\n", 1)[1]
            self.assertEqual(original, copy)

    def test_sse_stack_and_transport_cleanup_regression(self):
        web = (COMPONENT / "web.c").read_text()
        self.assertIn("#define TG_SSE_STACK_BYTES 6144", web)
        self.assertNotIn('xTaskCreate(sse_task, "sse", 3072', web)
        self.assertIn("MSG_PEEK | MSG_DONTWAIT", web)
        self.assertIn("shutdown(fd, SHUT_RDWR)", web)
        self.assertNotIn("httpd_sess_trigger_close", web)
        self.assertIn("sse_stack_min_free", web)
        self.assertIn('"last_event_id"', web)
        self.assertIn('"diagnostics"', web)

    def test_client_reconnect_script_is_in_embedded_assets(self):
        assets = (COMPONENT / "assets.c").read_text()
        self.assertIn("const unsigned char events_start[]", assets)
        html = (ROOT / "core/index.html").read_text()
        self.assertIn('<script src="/events.js"></script>', html)
        self.assertIn("stream.pause()", html)
        self.assertIn("stream.resume()", html)

    def test_embedded_web_assets_match_sources(self):
        assets = (COMPONENT / "assets.c").read_text()
        for filename, symbol in (("index.html", "index"), ("login.html", "auth"),
                                 ("icon.png", "icon"), ("events.js", "events"),
                                 ("setup.html", "setup")):
            block = re.search(
                rf"const unsigned char {symbol}_start\[\] = \{{\n(.*?)\n\}};\n"
                rf"const unsigned int {symbol}_size = (\d+);", assets, re.S)
            self.assertIsNotNone(block, symbol)
            embedded = bytes(map(int, re.findall(r"\d+", block.group(1))))
            source = (ROOT / "core" / filename).read_bytes()
            self.assertEqual(int(block.group(2)), len(source), symbol)
            self.assertEqual(embedded, source, symbol)

    def test_web_ui_shares_esphome_server_with_captive_portal(self):
        config = PACKAGE.read_text()
        self.assertNotIn("use_address:", config)
        self.assertNotIn("manual_ip:", config)
        self.assertIn("captive_portal:", config)
        header = (COMPONENT / "tyg.h").read_text()
        self.assertIn("#define TG_HTTP_PORT 80", header)
        web = (COMPONENT / "web.c").read_text()
        self.assertNotIn("httpd_start(", web)
        # Requests run on a dedicated task, not the shared server's 4 KiB stack.
        self.assertIn('xTaskCreate(web_task, "tyg-web", 6144', web)
        self.assertIn("httpd_req_async_handler_begin(req, &r.req)", web)
        self.assertIn("tg_on_captive_interface(active, local_ip, ap_ip)", web)
        self.assertIn("getsockname(httpd_req_to_sockfd(req)", web)
        adapter = (COMPONENT / "tygbot.cpp").read_text()
        self.assertIn("add_handler_without_auth(&web_handler_)", adapter)
        # Registered before setup so it precedes the captive portal's handler.
        self.assertIn("cg.add(var.register_web())", (COMPONENT / "__init__.py").read_text())
        self.assertIn('path_is(req, "/config.json") || path_is(req, "/wifisave")', web)
        self.assertIn("global_web_server_base->init()", adapter)
        codegen = (COMPONENT / "__init__.py").read_text()
        self.assertNotIn("set_port(", codegen)

    def test_remote_package_and_component_are_version_pinned(self):
        example = (ROOT / "tygbot.example.yaml").read_text()
        package = PACKAGE.read_text()
        self.assertIn("github://xiongyumail/tygbot/packages/tygbot.yaml@v1.2.2", example)
        self.assertIn("repository_ref: v1.2.2", package)
        self.assertIn("ref: ${repository_ref}", package)
        self.assertNotIn("type: local", package)
        self.assertIn("refresh: always", package)

    def test_provisioning_uses_standard_esphome_components(self):
        package = PACKAGE.read_text()
        self.assertIn("esp32_improv:\n  authorizer: none", package)
        self.assertIn("improv_serial:", package)
        for custom in ("ble.disable:", "ble.enable:", "set_ap_timeout"):
            self.assertNotIn(custom, package)
        # Scanning runs only while Wi-Fi is connected, leaving the radio to the hotspot.
        self.assertIn("continuous: false", package)
        self.assertIn("post_connect_roaming: false", package)
        self.assertIn("on_connect:\n    - esp32_ble_tracker.start_scan:", package)
        self.assertIn("on_disconnect:\n    - esp32_ble_tracker.stop_scan:", package)
        self.assertEqual(package.count("next_url: http://{{ip_address}}/"), 2)
        self.assertIn("host: ${device_name}.local", package)
        self.assertIn("esp32_ble_tracker:", package)

    def test_session_kept_and_connection_stays_warm(self):
        protocol = (ROOT / "core/protocol.c").read_text()
        # A logout-like reply marks the session invalid but never erases it.
        self.assertNotIn("user_token[0] = 0", protocol)
        self.assertIn("240000000LL", protocol)
        web = (ROOT / "core/web.c").read_text()
        self.assertIn("return ok ? 200000000LL", web)

    def test_lan_firmware_upload_only_from_own_page(self):
        web = (ROOT / "core/web.c").read_text()
        self.assertIn('httpd_req_get_hdr_value_len(req, "Origin") && host_guard(req, "/update")', web)
        self.assertIn("allowed ? TG_ROUTE_NONE : TG_ROUTE_DENY", web)
        adapter = (COMPONENT / "tygbot.cpp").read_text()
        self.assertIn("TG_ROUTE_DENY) tg_web_deny(req)", adapter)

    def test_versions_agree(self):
        web = (ROOT / "core/web.c").read_text()
        package = PACKAGE.read_text()
        version = re.search(r'#define TG_WEB_VERSION "([^"]+)"', web).group(1)
        self.assertIn(f'version: "{version}"', package)
        self.assertIn(f"repository_ref: v{version}", package)
        self.assertIn(f"tygbot.yaml@v{version}", (ROOT / "tygbot.example.yaml").read_text())
        self.assertIn(f'default="{version}"', (ROOT / "verify_transport.py").read_text())
        # The setup page carries a placeholder for the version.
        self.assertIn("v__TG_VERSION__", (ROOT / "core/setup.html").read_text())
        # Filled in when served, together with the build commit.
        self.assertIn('static const char mark[] = "__TG_VERSION__";', (COMPONENT / "web.c").read_text())

    def test_sip_call_isolated_from_door_path(self):
        # A door-station call (SIP) failure must never block opening the door:
        # sip.c must not touch the door action lock or the door/open routines.
        sip = (ROOT / "core/sip.c").read_text()
        for forbidden in ("tg_action_lock", "tg_open", "tg_refresh", "tg_submit_open"):
            self.assertNotIn(forbidden, sip)
        # SIP runs on its own task and UDP socket.
        self.assertIn('xTaskCreate(sip_task, "tyg-sip"', sip)
        talk = (ROOT / "core/talk.c").read_text()
        for forbidden in ("ws_close", "tg_refresh", "discover(", "query_households", "tg_action_lock, portMAX_DELAY"):
            self.assertNotIn(forbidden, talk)
        # Only a non-blocking take around a new TLS handshake; a busy door defers the fetch.
        self.assertEqual(talk.count("xSemaphoreTake(tg_action_lock, 0)"), 1)
        self.assertEqual(talk.count("xSemaphoreGive(tg_action_lock)"), 1)
        web = (ROOT / "core/web.c").read_text()
        self.assertNotIn("tg_talk_fetch(", web)
        self.assertIn("tg_talk_fetch(&cfg, detail, &retry_s)", sip)
        # The registration loop and the TLS fetch never share the SIP task stack.
        self.assertIn("static NOINLINE sip_end run_session(", sip)
        self.assertIn("static NOINLINE void obtain_config(", sip)
        self.assertIn("calloc(1, sizeof(*w))", talk)
        # A normal boot registers from the NVS cache instead of fetching over TLS.
        self.assertIn('nvs_set_blob(store, "sip"', (COMPONENT / "tygbot.cpp").read_text())
        self.assertIn("static uint32_t request_generation = 1;", sip)
        self.assertIn("set_state(false, detail)", sip)
        self.assertIn("SIP 注册超时", sip)
        self.assertIn("requested == request_generation", sip)
        self.assertNotIn('esp_http_client_get_header(h, "Set-Cookie"', talk)
        self.assertIn("HTTP_EVENT_ON_HEADER", talk)
        # The call account lives under its own NVS key, leaving the 936-byte door blob intact.
        adapter = (COMPONENT / "tygbot.cpp").read_text()
        self.assertNotIn("sip_started_ = true;\n  tg_talk_request();", adapter)
        self.assertIn('nvs_set_blob(store, "talk"', adapter)
        self.assertIn("static_assert(sizeof(tg_config) == 936", adapter)

    def test_call_linked_open_is_transaction_bound_and_separately_persisted(self):
        sip = (ROOT / "core/sip.c").read_text()
        web = (ROOT / "core/web.c").read_text()
        adapter = (COMPONENT / "tygbot.cpp").read_text()
        # A confirmed door result can only decline the exact INVITE generation captured for that job.
        self.assertIn("j.call_generation", web)
        self.assertIn("if (ok && j.call_generation)", web)
        self.assertIn("tg_sip_decline(j.call_generation)", web)
        self.assertIn('build_response(c, buf, 603, "Decline"', sip)
        self.assertIn("active_generation == generation", sip)
        # SIP remains the only task that writes its UDP socket.
        self.assertNotIn("send(c.fd", web)
        # The safety-sensitive setting defaults off and does not change the fixed config blob.
        self.assertIn('nvs_get_u8(store, "call_open", &linked)', adapter)
        self.assertIn('nvs_set_u8(store, "call_open", enabled ? 1 : 0)', adapter)
        self.assertIn("uint8_t linked = 0", adapter)
        self.assertIn('if (!strcmp(path, "/settings"))', web)
        self.assertIn('cJSON_AddBoolToObject(v, "call_linked_open"', web)

    def test_short_call_and_busy_states_are_not_lost_between_ha_samples(self):
        sip = (ROOT / "core/sip.c").read_text()
        web = (ROOT / "core/web.c").read_text()
        adapter = (COMPONENT / "tygbot.cpp").read_text()
        self.assertIn("out->sequence = call_sequence", sip)
        self.assertNotIn("decline_generation = 0; caller[0] = 0", sip)
        self.assertIn("busy_sequence", web)
        self.assertIn("call_hold_until_ = now + 3000", adapter)
        self.assertIn("busy_hold_until_ = now + 1000", adapter)
        self.assertIn("sync_transient_state_();", adapter)

    def test_call_linked_open_user_words(self):
        page = (ROOT / "core/index.html").read_text()
        package = PACKAGE.read_text()
        self.assertIn("<h1>TYGBot</h1>", page)
        self.assertIn("来电联动开门", page)
        self.assertIn("仅匹配当前门禁时自动开门，成功后结束来电", page)
        self.assertIn("账号登录", page)
        self.assertNotIn("桃园谷登录", page)
        self.assertIn("name: Logged In 账号登录", package)
        self.assertNotIn("桃园谷已登录", package)
        self.assertIn("未知门禁", page)
        self.assertNotIn("门口机呼叫中", page)
        self.assertIn("name: Call Linked Open 来电联动开门", package)
        self.assertIn("lambda: return id(tygbot_core).call_linked_open_enabled();", package)
        self.assertIn("id(tygbot_core).set_call_linked_open(true);", package)
        self.assertIn("id(tygbot_core).set_call_linked_open(false);", package)
        self.assertIn("tg_submit_call_linked_open", (COMPONENT / "tygbot.cpp").read_text())

    def test_target_sensor_names_keep_unique_ascii_identity(self):
        package = PACKAGE.read_text()
        # ESPHome 2026.9 derives entity identity from the name. Pure Chinese
        # names all sanitize to underscores and collide within text_sensor.
        for name in ("Household 当前住房", "Door 当前门禁", "Caller ID 来电标识"):
            self.assertIn(f"name: {name}", package)

    def test_call_source_is_matched_before_automatic_open(self):
        web = (ROOT / "core/web.c").read_text()
        protocol = (ROOT / "core/protocol.c").read_text()
        sip = (ROOT / "core/sip.c").read_text()
        self.assertIn("tg_open_call(j.caller, j.call_generation", web)
        self.assertIn("strcmp(caller, door.code)", protocol)
        self.assertIn("来电来源与当前门禁不匹配，未开门", protocol)
        self.assertIn('cJSON_AddBoolToObject(v, "matched", matched)', sip)
        self.assertIn("tg_submit_call_open(generation, who)", sip)
        self.assertIn("xQueueOverwrite(call_jobs", web)
        self.assertIn("tg_sip_call_matches(j.call_generation, j.caller)", web)
        self.assertIn("door_caller_unique", protocol)
        self.assertIn("tg_caller_target_unique", protocol)
        self.assertNotIn("linked_seen_generation", web)
        self.assertNotIn('cJSON_AddStringToObject(call, "caller"', web)

    def test_target_selection_is_validated_and_persisted(self):
        web = (ROOT / "core/web.c").read_text()
        protocol = (ROOT / "core/protocol.c").read_text()
        adapter = (COMPONENT / "tygbot.cpp").read_text()
        self.assertIn('!strcmp(path, "/target")', web)
        self.assertIn("refresh(&candidate, true, false", protocol)
        self.assertIn("tg_storage_save()", protocol)
        self.assertNotIn("yaml_target", adapter)
        codegen = (COMPONENT / "__init__.py").read_text()
        for legacy in ('cv.Optional("organization"', 'cv.Optional("household"',
                       'cv.Optional("door_code"', 'cv.Optional("door_number"'):
            self.assertNotIn(legacy, codegen)

    def test_target_lists_reuse_latest_successful_refresh(self):
        protocol = (ROOT / "core/protocol.c").read_text()
        self.assertIn("cache_households(v)", protocol)
        self.assertIn("cache_doors(&room, available)", protocol)
        self.assertIn("cache_array(households_cache)", protocol)
        self.assertIn("cache_array(doors_cache)", protocol)
        self.assertIn("selection_cache_clear();", protocol)
        self.assertIn("refresh(&candidate, true, false", protocol)
        self.assertIn('cJSON_AddBoolToObject(result, "cached", cached)', (ROOT / "core/web.c").read_text())

    def test_test_use_notice_is_visible(self):
        notice = "仅供授权测试使用，非官方项目。"
        for filename in ("README.md", "core/index.html", "core/login.html"):
            self.assertIn(notice, (ROOT / filename).read_text())

if __name__ == "__main__":
    unittest.main()
