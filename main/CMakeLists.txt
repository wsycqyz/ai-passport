# Word Buddy firmware component. Every planned source is listed here (frozen after
# Milestone 1); placeholders exist until their owners replace them. The baseline demo
# sources (demo_*.c, demo_navigation.c, ui_pixel*.c) stay on disk for the upstream host
# tests but are not part of this firmware.
idf_component_register(
    SRCS "main.c"
         # Device side (ESP-IDF / LVGL)
         "vocab_app.c"
         "vocab_ui.c"
         "vocab_audio.c"
         # Pure core (host-tested)
         "vocab_engine.c"
         "vocab_session.c"
         "vocab_srs.c"
         "vocab_input.c"
         "vocab_idle.c"
         "vocab_progress.c"
         "vocab_store.c"
         "vocab_pet.c"
         "vocab_layout.c"
         "vocab_view.c"
         "vocab_bank.c"
         "vocab_rng.c"
         "vocab_stage1.c"
         "vocab_lure.c"
         "vocab_stage2.c"
         "vocab_anticheat.c"
         "vocab_stage3.c"
         "vocab_cloze.c"
         "vocab_clips.c"
         "vocab_adpcm.c"
         "vocab_tone.c"
         # Generated font subsets (tools/vocab_fonts.py), see vocab_fonts.h
         "../assets/fonts/vocab/vocab_font_en_36.c"
         "../assets/fonts/vocab/vocab_font_en_28.c"
         "../assets/fonts/vocab/vocab_font_en_22.c"
         "../assets/fonts/vocab/vocab_font_en_18.c"
         "../assets/fonts/vocab/vocab_font_ipa_18.c"
         "../assets/fonts/vocab/vocab_font_zh_22.c"
         "../assets/fonts/vocab/vocab_font_zh_16.c"
    INCLUDE_DIRS "."
    REQUIRES bsp esp_hw_support esp_timer nvs_flash
)

# Word clip pack (spec 8.5, amendment A6), parsed at run time by vocab_clips.c. The
# Windows-TTS test clips are private and git-ignored: embed the local pack written by
# tools/vocab_audio/build_clips.py when it exists and the build does not pass
# -D VOCAB_NO_CLIPS=1 (CI emulation); otherwise embed the committed empty pack (no word
# audio: the app shows the WORD_PULSE visual substitute). Either file is embedded under the
# same symbols, _binary_vocab_clips_bin_start/_end. The choice is made at configure time:
# run `idf.py reconfigure` (or use a fresh build directory) after adding or removing the pack.
get_filename_component(vocab_clips_local "${CMAKE_CURRENT_LIST_DIR}/../assets/audio/vocab_test_clips.bin" ABSOLUTE)
get_filename_component(vocab_clips_empty "${CMAKE_CURRENT_LIST_DIR}/../assets/audio/vocab_clips_empty.bin" ABSOLUTE)
if(EXISTS "${vocab_clips_local}" AND NOT VOCAB_NO_CLIPS)
    set(vocab_clips_blob "${vocab_clips_local}")
    message(STATUS "Word Buddy clips: embedding the local test clip pack ${vocab_clips_blob} (private: never publish this firmware)")
else()
    set(vocab_clips_blob "${vocab_clips_empty}")
    message(STATUS "Word Buddy clips: embedding the empty pack ${vocab_clips_blob} (no word audio)")
endif()
target_add_binary_data(${COMPONENT_LIB} "${vocab_clips_blob}" BINARY RENAME_TO vocab_clips_bin)
