#!/usr/bin/env python3
"""Static contracts of the Word Buddy firmware (spec AC-1, AC-55, AC-58, sections 9, 10.5).

Checks the sources, not behavior: the firmware component contains no baseline demo code,
LVGL objects are created only by the UI init, no radio or network code exists, NVS is never
erased, the key hand-off never blocks and the deep-sleep path keeps the order of
main/demo_low_power.c with the key as the only wake source.
"""

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]


def read(relative_path: str) -> str:
    return (ROOT / relative_path).read_text(encoding="utf-8")


def strip_comments(source: str) -> str:
    """Removes C comments and string/char literal contents so prose never counts as code."""
    pattern = re.compile(r"""//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\\n])*"|'(?:\\.|[^'\\\n])*'""", re.S)

    def replace(match: re.Match) -> str:
        token = match.group(0)
        if token.startswith('"'):
            return '""'
        if token.startswith("'"):
            return "' '"
        return " "

    return pattern.sub(replace, source)


def function_body(source: str, name: str) -> str:
    match = re.search(rf"\b{re.escape(name)}\s*\([^;{{]*?\)\s*\{{", source)
    if not match:
        raise AssertionError(f"function not found: {name}")
    start = match.end() - 1
    depth = 0
    for index in range(start, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[start + 1:index]
    raise AssertionError(f"function is unterminated: {name}")


def firmware_files() -> list[Path]:
    """main.c and every main/vocab_* source or header."""
    return [ROOT / "main" / "main.c"] + sorted((ROOT / "main").glob("vocab_*.[ch]"))


def firmware_sources() -> dict[str, str]:
    """Firmware sources with comments and string contents stripped."""
    return {f.name: strip_comments(f.read_text(encoding="utf-8")) for f in firmware_files()}


def firmware_includes() -> dict[str, list[str]]:
    """Quoted #include targets per firmware file."""
    pattern = re.compile(r'(?m)^\s*#\s*include\s+"([^"]+)"')
    return {f.name: pattern.findall(f.read_text(encoding="utf-8")) for f in firmware_files()}


def cmake_srcs(cmake: str) -> list[str]:
    block = re.search(r"SRCS(.*?)(INCLUDE_DIRS|REQUIRES|EMBED_FILES)", cmake, re.S)
    if not block:
        raise AssertionError("SRCS block not found in main/CMakeLists.txt")
    body = re.sub(r"#[^\n]*", "", block.group(1))
    return re.findall(r'"([^"]+)"', body)


class FirmwareCompositionTest(unittest.TestCase):
    """AC-1: no baseline demo or pixel-UI source is part of the firmware."""

    def test_srcs_contain_no_demo_sources(self) -> None:
        srcs = cmake_srcs(read("main/CMakeLists.txt"))
        self.assertIn("main.c", srcs)
        self.assertIn("vocab_app.c", srcs)
        for src in srcs:
            name = Path(src).name
            self.assertFalse(name.startswith("demo_"), f"demo source in SRCS: {src}")
            self.assertFalse(name.startswith("ui_pixel"), f"pixel-UI source in SRCS: {src}")

    def test_every_vocab_source_on_disk_is_built(self) -> None:
        srcs = {Path(s).name for s in cmake_srcs(read("main/CMakeLists.txt"))}
        for path in sorted((ROOT / "main").glob("vocab_*.c")):
            self.assertIn(path.name, srcs, f"{path.name} exists but is not in SRCS")

    def test_startup_opens_the_vocabulary_app(self) -> None:
        main = strip_comments(read("main/main.c"))
        self.assertIn("vocab_app_start(", function_body(main, "app_main"))
        for name, text in firmware_sources().items():
            self.assertIsNone(re.search(r"\b(demo_\w+|ui_pixel\w*)\s*\(", text),
                              f"{name} reaches baseline demo code")
        for name, includes in firmware_includes().items():
            for header in includes:
                self.assertFalse(Path(header).name.startswith(("demo", "ui_pixel")),
                                 f"{name} includes the baseline header {header}")


class LvglObjectLifetimeTest(unittest.TestCase):
    """AC-55: objects are created once by the UI init and never deleted."""

    CREATE = re.compile(r"\blv_\w+_create\s*\(")
    DELETE = re.compile(r"\blv_obj_(delete|del|clean|delete_async|delete_delayed)\s*\(")
    INIT_FUNCTIONS = ("vocab_ui_create", "make_box", "make_label", "make_pet")

    def test_no_object_creation_outside_ui_init(self) -> None:
        for name, text in firmware_sources().items():
            if name == "vocab_ui.c":
                continue
            self.assertIsNone(self.CREATE.search(text), f"{name} creates LVGL objects")

    def test_ui_creates_objects_only_in_init_functions(self) -> None:
        ui = strip_comments(read("main/vocab_ui.c"))
        init_text = "".join(function_body(ui, fn) for fn in self.INIT_FUNCTIONS)
        total = len(self.CREATE.findall(ui))
        in_init = len(self.CREATE.findall(init_text))
        self.assertGreater(in_init, 0)
        self.assertEqual(total, in_init, "an LVGL object is created outside the UI init")

    def test_no_object_deletion(self) -> None:
        for name, text in firmware_sources().items():
            self.assertIsNone(self.DELETE.search(text), f"{name} deletes LVGL objects")

    def test_lvgl_is_only_touched_by_the_ui_and_app(self) -> None:
        # vocab_fonts.h only declares the generated lv_font_t descriptors.
        allowed = ("vocab_ui.c", "vocab_app.c", "vocab_fonts.h")
        for name, text in firmware_sources().items():
            if name in allowed:
                continue
            self.assertIsNone(re.search(r"\blv_\w+\s*\(", text),
                              f"{name} calls LVGL outside the renderer")
        for name, includes in firmware_includes().items():
            if name in allowed:
                continue
            self.assertNotIn("lvgl.h", includes, f"{name} includes LVGL outside the renderer")


class OfflineTest(unittest.TestCase):
    """AC-58: no radio initialization and no network client code."""

    FORBIDDEN = re.compile(
        r"\b(esp_wifi_\w+|nimble_port_\w+|esp_bt_\w+|esp_netif_\w+|esp_http_client_\w+|"
        r"esp_mqtt_\w+|lwip_\w+|socket|connect|getaddrinfo)\s*\(")

    def test_sources_contain_no_network_code(self) -> None:
        for name, text in firmware_sources().items():
            match = self.FORBIDDEN.search(text)
            self.assertIsNone(match, f"{name} contains network code: {match and match.group(0)}")

    def test_component_requires_no_radio_or_network_component(self) -> None:
        cmake = read("main/CMakeLists.txt")
        requires = re.search(r"REQUIRES([^)]*)\)", cmake, re.S)
        self.assertIsNotNone(requires)
        for component in ("bt", "esp_wifi", "esp_netif", "esp_http_client", "mqtt", "lwip"):
            self.assertNotRegex(requires.group(1), rf"(^|\s){component}(\s|$)")

    def test_bluetooth_is_disabled(self) -> None:
        self.assertRegex(read("sdkconfig.defaults"), r"(?m)^CONFIG_BT_ENABLED=n$")


class StorageAndInputTest(unittest.TestCase):
    """Spec 10.5 and 3.1: never erase NVS, never abort on storage, never block the callback."""

    def test_nvs_is_never_erased_and_never_aborts(self) -> None:
        for name, text in firmware_sources().items():
            self.assertNotRegex(text, r"\bnvs_flash_erase\w*\s*\(", f"{name} erases NVS")
        app = strip_comments(read("main/vocab_app.c"))
        self.assertNotRegex(app, r"\bESP_ERROR_CHECK\s*\(")
        self.assertNotRegex(app, r"\babort\s*\(")

    def test_key_hand_off_never_blocks(self) -> None:
        body = function_body(strip_comments(read("main/vocab_app.c")), "vocab_app_post_key")
        sends = re.findall(r"xQueueSend\w*\s*\(([^;]*)\)\s*;", body)
        self.assertEqual(len(sends), 1, "exactly one queue send in the button hand-off")
        self.assertRegex(sends[0], r",\s*0\s*\)?$", "the hand-off must use a zero timeout")
        self.assertNotRegex(body, r"\b(lv_\w+|bsp_lvgl_lock|vocab_audio_\w+|nvs_\w+|vTaskDelay)\s*\(")


class DeepSleepOrderTest(unittest.TestCase):
    """Spec 9: the order of main/demo_low_power.c, the key as the only wake source."""

    ORDER = (
        "bsp_button_read_mv",
        "vocab_engine_sleep_refused",
        "vocab_audio_shutdown",
        "bsp_button_prepare_deep_sleep",
        "esp_deep_sleep_enable_gpio_wakeup",
        "bsp_battery_sleep",
        "bsp_audio_sleep",
        "bsp_audio_prepare_deep_sleep",
        "bsp_i2c_prepare_deep_sleep",
        "bsp_lvgl_lock",
        "bsp_display_prepare_deep_sleep",
        "esp_deep_sleep_start",
        "esp_restart",
    )

    def test_shutdown_sequence_order(self) -> None:
        body = function_body(strip_comments(read("main/vocab_app.c")), "deep_sleep")
        position = 0
        for call in self.ORDER:
            match = re.compile(rf"\b{call}\s*\(").search(body, position)
            self.assertIsNotNone(match, f"deep_sleep() does not call {call} after the previous step")
            position = match.end()
        # Nothing but the restart fallback follows the sleep entry.
        tail = body[body.index("esp_deep_sleep_start"):]
        self.assertEqual(re.findall(r"\b(\w+)\s*\(", tail)[1:], ["ESP_LOGE", "esp_restart"])

    def test_key_is_the_only_wake_source(self) -> None:
        for name, text in firmware_sources().items():
            self.assertNotRegex(text, r"\besp_sleep_enable_(timer|ext\d|touchpad|ulp)\w*\s*\(",
                                f"{name} arms another wake source")
        body = function_body(strip_comments(read("main/vocab_app.c")), "deep_sleep")
        self.assertRegex(body, r"esp_deep_sleep_enable_gpio_wakeup\s*\(\s*1ULL\s*<<\s*BSP_BTN_GPIO\s*,"
                               r"\s*ESP_GPIO_WAKEUP_GPIO_LOW\s*\)")


if __name__ == "__main__":
    unittest.main()
