"""TYGBot ESPHome bridge. Network operations stay off the ESPHome main loop."""
import hashlib
import logging
from pathlib import Path
import re
import subprocess
import esphome.codegen as cg
import esphome.config_validation as cv
import esphome.final_validate as fv
from esphome.components import esp32, socket, web_server
from esphome.const import CONF_ID, CONF_PASSWORD, __version__ as ESPHOME_VERSION
from esphome.core import CORE

_LOGGER = logging.getLogger(__name__)
# Web UI sharing relies on ESPHome internals (web_server headers, mdns service count).
TESTED_ESPHOME = "2026.9."
DEPENDENCIES = ["esp32", "wifi"]
AUTO_LOAD = ["json", "web_server_base"]
tygbot_ns = cg.esphome_ns.namespace("tygbot")
TYGBot = tygbot_ns.class_("TYGBot", cg.PollingComponent)

CONFIG_SCHEMA = cv.All(cv.Schema({
    cv.GenerateID(): cv.declare_id(TYGBot),
    cv.Optional(CONF_PASSWORD): cv.All(cv.string_strict, cv.Length(min=8, max=128)),
    cv.Optional("require_auth", default=True): cv.boolean,
    cv.Optional("host", default="tygbot.local"): cv.All(cv.string_strict, cv.Length(max=63)),
}).extend(cv.polling_component_schema("2s")),
    socket.consume_sockets(6, "tygbot"),  # 4 local clients + door TLS + independent talkback TLS
    socket.consume_sockets(2, "tygbot", socket.SocketType.UDP),  # httpd control socket + SIP
)
def build_commit():
    """Short commit of the component checkout; empty outside git. Local edits add *."""
    root = Path(__file__).resolve().parents[2]
    def git(*args):
        return subprocess.run(["git", "-C", str(root), *args], capture_output=True, text=True, timeout=10, check=True).stdout.strip()
    try:
        commit = git("rev-parse", "--short=5", "HEAD")
        dirty = git("status", "--porcelain", "--untracked-files=no")
    except (OSError, subprocess.SubprocessError):
        return ""
    if not re.fullmatch(r"[0-9a-f]{5,}", commit):
        return ""
    return commit + ("*" if dirty else "")

def validate_framework(config):
    if not CORE.is_esp32 or CORE.using_arduino:
        raise cv.Invalid("TYGBot requires the ESP-IDF framework")
    if config["require_auth"] and CONF_PASSWORD not in config:
        raise cv.Invalid("password is required unless require_auth is false")
    if not ESPHOME_VERSION.startswith(TESTED_ESPHOME):
        _LOGGER.warning("TYGBot is tested with ESPHome %sx; found %s", TESTED_ESPHOME, ESPHOME_VERSION)
    # HA shows the project version as the firmware: append the built commit, e.g. 1.2.1+a1b2c.
    project = fv.full_config.get().get("esphome", {}).get("project")
    commit = build_commit()
    if commit and project and "+" not in project["version"]:
        project["version"] += "+" + commit
    return config

FINAL_VALIDATE_SCHEMA = validate_framework

async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.register_web())
    cg.add_define("TG_BUILD_COMMIT", build_commit())
    # Advertise the web UI on ESPHome's shared server through the native
    # DeviceInfoResponse. ESPHome 2026.9.1 gates that protobuf field and IDF
    # EventSource declarations together. Provide the SDK declaration headers,
    # but do not load the standard web_server component.
    cg.add_define("USE_WEBSERVER")
    cg.add_define("USE_WEBSERVER_PORT", 80)
    # USE_WEBSERVER also makes mdns publish an _http service that its
    # compile-time count omits; without the extra slot it overwrites
    # _esphomelib and Home Assistant cannot discover the device.
    # Runs after mdns to_code (NETWORK_SERVICES priority).
    if "mdns" in CORE.config:
        counts = [d for d in CORE.defines if d.name == "MDNS_SERVICE_COUNT"]
        if len(counts) != 1:
            raise cv.Invalid("Unexpected mdns service count definition")
        CORE.defines.discard(counts[0])
        cg.add_define("MDNS_SERVICE_COUNT", int(str(counts[0].value)) + 1)
    sdk_headers = Path(web_server.__file__).parent
    for name in ("web_server.h", "list_entities.h"):
        esp32.add_extra_build_file(
            "src/esphome/components/web_server/" + name, sdk_headers / name
        )
    # Compile only the verifier, not the HTTP service password itself. Salt is
    # stable across builds of this device; changing the password changes it.
    cg.add(var.set_auth_required(config["require_auth"]))
    if config["require_auth"]:
        salt = hashlib.sha256(("tygbot-esphome-v1:" + config[CONF_PASSWORD]).encode()).digest()[:16]
        digest = hashlib.pbkdf2_hmac("sha256", config[CONF_PASSWORD].encode(), salt, 200000)
        cg.add(var.set_password_verifier(salt.hex(), digest.hex()))
    cg.add(var.set_host(config["host"]))
    esp32.add_idf_component(name="espressif/esp_websocket_client", ref="1.4.0")
    for name in ("esp_http_client", "esp_http_server", "json", "nvs_flash", "mbedtls", "esp_timer"):
        esp32.include_builtin_idf_component(name)
    esp32.require_certificate_bundle()
    for name, value in {
        "CONFIG_HTTPD_MAX_REQ_HDR_LEN": 1024,
        "CONFIG_HTTPD_MAX_URI_LEN": 512,
        "CONFIG_MBEDTLS_DYNAMIC_BUFFER": True,
        "CONFIG_MBEDTLS_DYNAMIC_FREE_CONFIG_DATA": True,
        "CONFIG_MBEDTLS_ASYMMETRIC_CONTENT_LEN": True,
        "CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN": 16384,
        "CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN": 4096,
        "CONFIG_ESP_WIFI_IRAM_OPT": False,
        "CONFIG_ESP_WIFI_RX_IRAM_OPT": False,
    }.items():
        esp32.add_idf_sdkconfig_option(name, value)
