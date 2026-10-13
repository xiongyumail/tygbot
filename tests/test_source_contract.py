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
        for name in ("core.c", "protocol.c", "auth.c"):
            original = (ROOT / "core" / name).read_text()
            copy = (COMPONENT / name).read_text().split("\n", 1)[1]
            self.assertEqual(original, copy)

    def test_sse_stack_and_transport_cleanup_regression(self):
        web = (COMPONENT / "web.c").read_text()
        self.assertIn("#define TG_SSE_STACK_BYTES 8192", web)
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
        self.assertIn("github://xiongyumail/tygbot/packages/tygbot.yaml@v1.0.0", example)
        self.assertIn("repository_ref: v1.0.0", package)
        self.assertIn("ref: ${repository_ref}", package)
        self.assertNotIn("type: local", package)
        self.assertIn("refresh: never", package)

    def test_provisioning_uses_standard_esphome_components(self):
        package = PACKAGE.read_text()
        self.assertIn("esp32_improv:\n  authorizer: none", package)
        self.assertIn("improv_serial:", package)
        for custom in ("ble.disable:", "ble.enable:", "set_ap_timeout"):
            self.assertNotIn(custom, package)
        # Scanning runs only while Wi-Fi is connected, leaving the radio to the hotspot.
        self.assertIn("continuous: false", package)
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
        # The setup page shows the version substituted at sync time.
        self.assertIn("v__TG_VERSION__", (ROOT / "core/setup.html").read_text())
        self.assertNotIn("__TG_VERSION__", (COMPONENT / "assets.c").read_text())

    def test_test_use_notice_is_visible(self):
        notice = "仅供授权测试使用，非官方项目。"
        for filename in ("README.md", "core/index.html", "core/login.html"):
            self.assertIn(notice, (ROOT / filename).read_text())

if __name__ == "__main__":
    unittest.main()
