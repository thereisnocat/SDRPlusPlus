#pragma once

enum {
    RADIO_IFACE_CMD_GET_MODE,
    RADIO_IFACE_CMD_SET_MODE,
    RADIO_IFACE_CMD_GET_BANDWIDTH,
    RADIO_IFACE_CMD_SET_BANDWIDTH,
    RADIO_IFACE_CMD_GET_SQUELCH_MODE,
    RADIO_IFACE_CMD_SET_SQUELCH_MODE,
    RADIO_IFACE_CMD_GET_SQUELCH_LEVEL,
    RADIO_IFACE_CMD_SET_SQUELCH_LEVEL,
    RADIO_IFACE_CMD_GET_CTCSS_TONE,
    RADIO_IFACE_CMD_SET_CTCSS_TONE,
    RADIO_IFACE_CMD_GET_HIGHPASS,
    RADIO_IFACE_CMD_SET_HIGHPASS
};

enum {
    RADIO_IFACE_MODE_NFM,
    RADIO_IFACE_MODE_WFM,
    RADIO_IFACE_MODE_AM,
    RADIO_IFACE_MODE_DSB,
    RADIO_IFACE_MODE_USB,
    RADIO_IFACE_MODE_CW,
    RADIO_IFACE_MODE_LSB,
    RADIO_IFACE_MODE_RAW,
    // Appended, not inserted alphabetically/logically -- these values are persisted (frequency
    // manager bookmarks, saved "selectedDemodId") and reused directly as array indices
    // (frequency_manager's demodModeList[]) elsewhere, so an existing installation's saved data
    // would silently point at the wrong mode if any earlier entry's numeric value moved.
    RADIO_IFACE_MODE_SAM
};