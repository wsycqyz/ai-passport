#!/usr/bin/env bash
set -euo pipefail

mode="${1:---all}"
repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"

usage() {
    echo "Usage: $0 [--all|--static|--firmware]" >&2
}

run_static_checks() {
    local actionlint_bin
    local test_dir
    local vocab_test

    python3 tools/check_repo.py

    actionlint_bin="${ACTIONLINT_BIN:-}"
    if [[ -z "${actionlint_bin}" ]]; then
        actionlint_bin="$(command -v actionlint || true)"
    fi
    if [[ -z "${actionlint_bin}" || ! -x "${actionlint_bin}" ]]; then
        actionlint_bin="$(./tools/install-actionlint.sh)"
    fi
    "${actionlint_bin}" -color .github/workflows/*.yml

    test_dir="$(mktemp -d /tmp/ai-passport-host-tests.XXXXXX)"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_ui_pixel_math.c main/ui_pixel_math.c \
        -o "${test_dir}/test_ui_pixel_math"
    "${test_dir}/test_ui_pixel_math"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_demo_navigation.c main/demo_navigation.c \
        -o "${test_dir}/test_demo_navigation"
    "${test_dir}/test_demo_navigation"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_key_guard.c -o "${test_dir}/test_key_guard"
    "${test_dir}/test_key_guard"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Icomponents/bsp/src \
        tests/test_bsp_display_rounding.c components/bsp/src/bsp_display_rounding.c \
        -o "${test_dir}/test_bsp_display_rounding"
    "${test_dir}/test_bsp_display_rounding"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Icomponents/bsp/src \
        tests/test_bsp_es8311_sleep_check.c components/bsp/src/bsp_es8311_sleep_check.c \
        -o "${test_dir}/test_bsp_es8311_sleep_check"
    "${test_dir}/test_bsp_es8311_sleep_check"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror \
        -Itests/battery_stubs -Itests/bsp_stubs -Icomponents/bsp/include \
        tests/test_bsp_battery.c -o "${test_dir}/test_bsp_battery"
    "${test_dir}/test_bsp_battery"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror \
        -Itests/bsp_stubs -Icomponents/bsp/include \
        tests/test_bsp_button.c -o "${test_dir}/test_bsp_button"
    "${test_dir}/test_bsp_button"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror \
        -Itests/bsp_stubs -Icomponents/bsp/include \
        tests/test_bsp_lvgl_init.c components/bsp/src/bsp_display_rounding.c \
        -o "${test_dir}/test_bsp_lvgl_init"
    "${test_dir}/test_bsp_lvgl_init"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror \
        -Itests/audio_stubs -Icomponents/bsp/include -Icomponents/bsp/src \
        tests/test_bsp_audio_recovery.c components/bsp/src/bsp_es8311_sleep_check.c \
        -o "${test_dir}/test_bsp_audio_recovery"
    "${test_dir}/test_bsp_audio_recovery"
    for demo in audio low_power ble wifi; do
        "${CC:-cc}" -std=c11 -Wall -Wextra -Werror \
            -ffunction-sections -fdata-sections -Itests/demo_stubs -Imain \
            "tests/test_demo_${demo}_runtime.c" -Wl,--gc-sections \
            -o "${test_dir}/test_demo_${demo}_runtime"
        "${test_dir}/test_demo_${demo}_runtime"
    done
    # Word Buddy (vocabulary trainer) pure-logic host tests. Every test links the shared
    # core (bank, RNG, view helpers) plus the modules it exercises; owners per role.
    # Lead developer.
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_vocab_bank.c main/vocab_bank.c main/vocab_rng.c main/vocab_view.c \
        -o "${test_dir}/test_vocab_bank"
    "${test_dir}/test_vocab_bank"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_vocab_srs.c main/vocab_srs.c \
        main/vocab_bank.c main/vocab_rng.c main/vocab_view.c \
        -o "${test_dir}/test_vocab_srs"
    "${test_dir}/test_vocab_srs"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_vocab_input.c main/vocab_input.c \
        main/vocab_bank.c main/vocab_rng.c main/vocab_view.c \
        -o "${test_dir}/test_vocab_input"
    "${test_dir}/test_vocab_input"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_vocab_idle.c main/vocab_idle.c \
        main/vocab_bank.c main/vocab_rng.c main/vocab_view.c \
        -o "${test_dir}/test_vocab_idle"
    "${test_dir}/test_vocab_idle"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_vocab_progress.c main/vocab_progress.c \
        main/vocab_bank.c main/vocab_rng.c main/vocab_view.c \
        -o "${test_dir}/test_vocab_progress"
    "${test_dir}/test_vocab_progress"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_vocab_store.c main/vocab_store.c main/vocab_progress.c \
        main/vocab_bank.c main/vocab_rng.c main/vocab_view.c \
        -o "${test_dir}/test_vocab_store"
    "${test_dir}/test_vocab_store"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_vocab_pet.c main/vocab_pet.c \
        main/vocab_bank.c main/vocab_rng.c main/vocab_view.c \
        -o "${test_dir}/test_vocab_pet"
    "${test_dir}/test_vocab_pet"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_vocab_layout.c main/vocab_layout.c \
        main/vocab_bank.c main/vocab_rng.c main/vocab_view.c \
        -o "${test_dir}/test_vocab_layout"
    "${test_dir}/test_vocab_layout"
    for vocab_test in session sim; do
        "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
            "tests/test_vocab_${vocab_test}.c" \
            main/vocab_engine.c main/vocab_session.c main/vocab_srs.c main/vocab_input.c \
            main/vocab_idle.c main/vocab_progress.c main/vocab_store.c main/vocab_pet.c \
            main/vocab_layout.c main/vocab_clips.c \
            main/vocab_stage1.c main/vocab_lure.c main/vocab_stage2.c main/vocab_anticheat.c \
            main/vocab_stage3.c main/vocab_cloze.c \
            main/vocab_bank.c main/vocab_rng.c main/vocab_view.c \
            -o "${test_dir}/test_vocab_${vocab_test}"
        "${test_dir}/test_vocab_${vocab_test}"
    done
    # Stage 1 developer.
    for vocab_test in stage1 lure; do
        "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
            "tests/test_vocab_${vocab_test}.c" main/vocab_stage1.c main/vocab_lure.c \
            main/vocab_bank.c main/vocab_rng.c main/vocab_view.c \
            -o "${test_dir}/test_vocab_${vocab_test}"
        "${test_dir}/test_vocab_${vocab_test}"
    done
    # Stage 2 developer.
    for vocab_test in stage2 anticheat; do
        "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
            "tests/test_vocab_${vocab_test}.c" main/vocab_stage2.c main/vocab_anticheat.c \
            main/vocab_bank.c main/vocab_rng.c main/vocab_view.c \
            -o "${test_dir}/test_vocab_${vocab_test}"
        "${test_dir}/test_vocab_${vocab_test}"
    done
    # Stage 3 developer.
    for vocab_test in stage3 cloze; do
        "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
            "tests/test_vocab_${vocab_test}.c" main/vocab_stage3.c main/vocab_cloze.c \
            main/vocab_bank.c main/vocab_rng.c main/vocab_view.c \
            -o "${test_dir}/test_vocab_${vocab_test}"
        "${test_dir}/test_vocab_${vocab_test}"
    done
    # Assets developer.
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_vocab_adpcm.c main/vocab_adpcm.c main/vocab_clips.c \
        main/vocab_bank.c main/vocab_rng.c main/vocab_view.c \
        -o "${test_dir}/test_vocab_adpcm"
    "${test_dir}/test_vocab_adpcm"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_vocab_tone.c main/vocab_tone.c \
        main/vocab_bank.c main/vocab_rng.c main/vocab_view.c \
        -o "${test_dir}/test_vocab_tone"
    "${test_dir}/test_vocab_tone"
    # QA engineer: black-box system tests of the whole engine (tests/test_vocab_qa_harness.h).
    # The former defects QA-1..QA-4 are enforced regression checks (no lenient mode).
    for vocab_test in qa_days qa_flows qa_power qa_edges qa_fuzz; do
        "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
            "tests/test_vocab_${vocab_test}.c" \
            main/vocab_engine.c main/vocab_session.c main/vocab_srs.c main/vocab_input.c \
            main/vocab_idle.c main/vocab_progress.c main/vocab_store.c main/vocab_pet.c \
            main/vocab_layout.c main/vocab_clips.c \
            main/vocab_stage1.c main/vocab_lure.c main/vocab_stage2.c main/vocab_anticheat.c \
            main/vocab_stage3.c main/vocab_cloze.c \
            main/vocab_bank.c main/vocab_rng.c main/vocab_view.c \
            -o "${test_dir}/test_vocab_${vocab_test}"
        "${test_dir}/test_vocab_${vocab_test}"
    done
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_battery_profile.py
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_deep_sleep_contract.py
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_key_wake_contract.py
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_check_repo.py
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_verify_firmware.py
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_archive_firmware.py
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_install_passport_skills.py
    # Word Buddy static contracts (lead): composition, LVGL lifetime, offline, sleep order.
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_vocab_contract.py
    # Word Buddy asset checks (assets developer): fonts and the word-clip pack.
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_vocab_fonts.py
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_vocab_clips.py
    rm -rf "${test_dir}"
    echo "Host tests: PASS"
}

run_firmware_checks() (
    local validation_build_dir

    if ! command -v idf.py >/dev/null 2>&1; then
        echo "ERROR: idf.py is not available; activate ESP-IDF 5.5.3 first." >&2
        return 1
    fi

    validation_build_dir="$(mktemp -d /tmp/ai-passport-firmware.XXXXXX)"
    trap 'case "${validation_build_dir}" in /tmp/ai-passport-firmware.*) rm -rf -- "${validation_build_dir}" ;; esac' EXIT

    SDKCONFIG_DEFAULTS="${repo_root}/sdkconfig.defaults" \
        idf.py -B "${validation_build_dir}" \
        -D "SDKCONFIG=${validation_build_dir}/sdkconfig" build
    idf.py -B "${validation_build_dir}" merge-bin \
        -o "${validation_build_dir}/FoloToy-AI-Passport-full.bin"
    python3 tools/verify_firmware.py "${validation_build_dir}"
    PYTHONDONTWRITEBYTECODE=1 python3 tools/archive_firmware.py create \
        "${validation_build_dir}" --archive-root "${repo_root}/build/firmware"
    mkdir -p "${repo_root}/build"
    install -m 0644 \
        "${validation_build_dir}/FoloToy-AI-Passport-full.bin" \
        "${repo_root}/build/FoloToy-AI-Passport-full.bin"
    echo "Firmware build: PASS"
)

cd "${repo_root}"
case "${mode}" in
    --all)
        run_static_checks
        run_firmware_checks
        ;;
    --static)
        run_static_checks
        ;;
    --firmware)
        run_firmware_checks
        ;;
    *)
        usage
        exit 2
        ;;
esac
