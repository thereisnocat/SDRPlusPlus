#pragma once

enum {
    RECORDER_IFACE_CMD_GET_MODE,
    RECORDER_IFACE_CMD_SET_MODE,
    RECORDER_IFACE_CMD_START,
    RECORDER_IFACE_CMD_STOP,
    // GET_CONFIG: `out` is a json* -- filled with every field in the table below.
    // SET_CONFIG: `in` is a const json* -- any field present is applied; a missing field is
    // left as-is (a caller wanting a full reset should pass a whole snapshot obtained from a
    // prior GET_CONFIG, not a partial hand-built one). No-op while a recording is in progress,
    // same as SET_MODE -- changing folder/format/sample type etc. mid-recording would apply
    // against a writer already opened with the old settings.
    //
    // Field keys, matching exactly what RecorderModule's own loadConfig()/saveConfig() persist
    // (misc_modules/recorder/src/main.cpp) -- portable values, not raw combo indices, so a
    // caller (the recording scheduler) can store/reuse them without knowing anything about
    // this module's OptionList ids:
    //   "mode"             int, RECORDER_MODE_BASEBAND/RECORDER_MODE_AUDIO
    //   "recPath"          string
    //   "timezone"         string key ("local"/"utc")
    //   "container"        int, wav::Format
    //   "sampleType"       int, wav::SampleType
    //   "audioStream"      string
    //   "audioVolume"      float
    //   "stereo"           bool
    //   "ignoreSilence"    bool
    //   "recordDualChannel" bool
    //   "nameTemplate"     string
    RECORDER_IFACE_CMD_GET_CONFIG,
    RECORDER_IFACE_CMD_SET_CONFIG
};

enum {
    RECORDER_MODE_BASEBAND,
    RECORDER_MODE_AUDIO
};
