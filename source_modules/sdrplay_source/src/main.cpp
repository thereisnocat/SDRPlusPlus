#include <imgui.h>
#include <utils/flog.h>
#include <module.h>
#include <gui/gui.h>
#include <signal_path/signal_path.h>
#include <core.h>
#include <gui/style.h>
#include <config.h>
#include <sdrplay_api.h>
#include <gui/smgui.h>
#include <utils/optionlist.h>

#define CONCAT(a, b) ((std::string(a) + b).c_str())

SDRPP_MOD_INFO{
    /* Name:            */ "sdrplay_source",
    /* Description:     */ "SDRplay source module for SDR++",
    /* Author:          */ "Ryzerth",
    /* Version:         */ 0, 2, 0,
    /* Max instances    */ 1
};

ConfigManager config;

sdrplay_api_Bw_MHzT preferedBandwidth[] = {
    sdrplay_api_BW_5_000,
    sdrplay_api_BW_5_000,
    sdrplay_api_BW_5_000,
    sdrplay_api_BW_5_000,
    sdrplay_api_BW_6_000,
    sdrplay_api_BW_7_000,
    sdrplay_api_BW_8_000,
    sdrplay_api_BW_8_000,
    sdrplay_api_BW_8_000
};

const sdrplay_api_Rsp2_AntennaSelectT rsp2_antennaPorts[] = {
    sdrplay_api_Rsp2_ANTENNA_A,
    sdrplay_api_Rsp2_ANTENNA_B,
    sdrplay_api_Rsp2_ANTENNA_B,
};

const char* rsp2_antennaPortsTxt = "Port A\0Port B\0Hi-Z\0";

const sdrplay_api_RspDx_AntennaSelectT rspdx_antennaPorts[] = {
    sdrplay_api_RspDx_ANTENNA_A,
    sdrplay_api_RspDx_ANTENNA_B,
    sdrplay_api_RspDx_ANTENNA_C
};

const char* rspdx_antennaPortsTxt = "Port A\0Port B\0Port C\0";

struct ifMode_t {
    sdrplay_api_If_kHzT ifValue;
    sdrplay_api_Bw_MHzT bw;
    unsigned int deviceSamplerate;
    unsigned int effectiveSamplerate;
};

ifMode_t ifModes[] = {
    { sdrplay_api_IF_Zero, sdrplay_api_BW_1_536, 2000000, 2000000 },
    { sdrplay_api_IF_2_048, sdrplay_api_BW_1_536, 8000000, 2000000 },
    { sdrplay_api_IF_2_048, sdrplay_api_BW_5_000, 8000000, 2000000 },
    { sdrplay_api_IF_1_620, sdrplay_api_BW_1_536, 6000000, 2000000 },
    { sdrplay_api_IF_0_450, sdrplay_api_BW_0_600, 2000000, 1000000 },
    { sdrplay_api_IF_0_450, sdrplay_api_BW_0_300, 2000000, 500000 },
    { sdrplay_api_IF_0_450, sdrplay_api_BW_0_200, 2000000, 500000 },
};

const char* ifModeTxt =
    "ZeroIF\0"
    "LowIF 2048KHz, IFBW 1536KHz\0"
    "LowIF 2048KHz, IFBW 5000KHz\0"
    "LowIF 1620KHz, IFBW 1536KHz\0"
    "LowIF 450KHz, IFBW 600KHz\0"
    "LowIF 450KHz, IFBW 300KHz\0"
    "LowIF 450KHz, IFBW 200KHz\0";

const char* rspduo_antennaPortsTxt = "Tuner 1 (50Ohm)\0Tuner 1 (Hi-Z)\0Tuner 2 (50Ohm)\0";

#define MAX_DEV_COUNT   16

class SDRPlaySourceModule : public ModuleManager::Instance {
public:
    SDRPlaySourceModule(std::string name) {
        this->name = name;

        // Init callbacks
        cbFuncs.EventCbFn = eventCB;
        cbFuncs.StreamACbFn = streamCB;
        // Tuner B needs its own sink. Pointing both at one callback is harmless while only
        // tuner A runs, but in dual tuner mode it would interleave two tuners into a single
        // stream.
        cbFuncs.StreamBCbFn = streamCB2;

        sdrplay_api_ErrT err = sdrplay_api_Open();
        if (err != sdrplay_api_Success) {
            flog::error("Could not intiatialized the SDRplay API. Make sure that the service is running.");
            return;
        }

        sampleRate = 2000000.0;
        srId = 0;

        bandwidth = sdrplay_api_BW_5_000;
        bandwidthId = 8;

        handler.ctx = this;
        handler.selectHandler = menuSelected;
        handler.deselectHandler = menuDeselected;
        handler.menuHandler = menuHandler;
        handler.startHandler = start;
        handler.stopHandler = stop;
        handler.tuneHandler = tune;
        handler.stream = &stream;
        handler.captureConfigHandler = captureConfig;
        handler.applyConfigHandler = applyConfig;
        handler.moduleType = "sdrplay_source";   // must match SDRPP_MOD_INFO's Name above

        // Dual tuner: rspDuoSampleFreq picks the low IF, and both choices decimate to the
        // same 2 MS/s at the callback -- measured, not assumed. Decimation divides that.
        duoFsList.define(6000000, "6 MHz (1620 kHz IF)", 6000000.0);
        duoFsList.define(8000000, "8 MHz (2048 kHz IF)", 8000000.0);
        duoDecimList.define(1, "2.0 MHz", 1);
        duoDecimList.define(2, "1.0 MHz", 2);
        duoDecimList.define(4, "500 kHz", 4);
        duoDecimList.define(8, "250 kHz", 8);
        duoDecimList.define(16, "125 kHz", 16);
        duoDecimList.define(32, "62.5 kHz", 32);

        channels.count = 2;
        channels.streams = { &stream, &stream2 };
        channels.names = { "Tuner A", "Tuner B" };
        channels.sampleAligned = true;    // measured: identical counts on both callbacks
        // False, and measured rather than assumed. With a strong medium wave carrier in
        // both ports the two tuners are perfectly coherent -- coherence 1.0000, and the
        // cross-correlation phase drifts under 0.15 degrees across a run -- so a null holds
        // once set. But across a stop and restart the phase lands anywhere in +/-180: six
        // consecutive runs gave +91, +148, +111, -73, -143, +90 degrees. The tuners share a
        // clock but their LOs come up at an arbitrary relative phase.
        //
        // So a saved weight cannot be restored blindly on this radio. Recalling one gets the
        // frequency and mode back; the weight itself has to be re-found, which auto-null or
        // decorrelation does in a second or two.
        channels.phaseCoherent = false;

        refresh();

        config.acquire();
        std::string confSelectDev = config.conf["device"];
        config.release();
        selectByName(confSelectDev);

        sigpath::sourceManager.registerSource("SDRplay", &handler);

        initOk = true;
    }

    ~SDRPlaySourceModule() {
        stop(this);
        if (initOk) { sdrplay_api_Close(); }
        sigpath::sourceManager.unregisterChannels("SDRplay");
        sigpath::sourceManager.unregisterSource("SDRplay");
    }

    void postInit() {}

    void enable() {
        enabled = true;
    }

    void disable() {
        enabled = false;
    }

    bool isEnabled() {
        return enabled;
    }

    void refresh() {
        devList.clear();
        devNameList.clear();
        devListTxt = "";

        sdrplay_api_DeviceT devArr[MAX_DEV_COUNT];
        unsigned int numDev = 0;
        sdrplay_api_GetDevices(devArr, &numDev, MAX_DEV_COUNT);

        for (unsigned int i = 0; i < numDev; i++) {
            devList.push_back(devArr[i]);
            std::string name = "";
            switch (devArr[i].hwVer) {
            case SDRPLAY_RSP1_ID:
                name = "RSP1 (";
                name += devArr[i].SerNo;
                name += ')';
                break;
            case SDRPLAY_RSP1A_ID:
                name = "RSP1A (";
                name += devArr[i].SerNo;
                name += ')';
                break;
            case SDRPLAY_RSP1B_ID:
                name = "RSP1B (";
                name += devArr[i].SerNo;
                name += ')';
                break;
            case SDRPLAY_RSP2_ID:
                name = "RSP2 (";
                name += devArr[i].SerNo;
                name += ')';
                break;
            case SDRPLAY_RSPduo_ID:
                name = "RSPduo (";
                name += devArr[i].SerNo;
                name += ')';
                break;
            case SDRPLAY_RSPdx_ID:
                name = "RSPdx (";
                name += devArr[i].SerNo;
                name += ')';
                break;
            case SDRPLAY_RSPdxR2_ID:
                name = "RSPdx-R2 (";
                name += devArr[i].SerNo;
                name += ')';
                break;
            default:
                name = "Unknown (";
                name += devArr[i].SerNo;
                name += ')';
                break;
            }
            devNameList.push_back(name);
            devListTxt += name;
            devListTxt += '\0';
        }
    }

    void selectFirst() {
        if (devList.size() == 0) {
            selectedName = "";
            return;
        }
        selectDev(devList[0], 0);
    }

    void selectByName(std::string name) {
        for (int i = 0; i < devNameList.size(); i++) {
            if (devNameList[i] == name) {
                selectDev(devList[i], i);
                return;
            }
        }
        selectFirst();
    }

    void selectById(int id) {
        selectDev(devList[id], id);
    }

    void selectDev(sdrplay_api_DeviceT dev, int id) {
        openDev = dev;
        sdrplay_api_ErrT err;

        openDev.tuner = sdrplay_api_Tuner_A;
        openDev.rspDuoMode = sdrplay_api_RspDuoMode_Single_Tuner;
        err = sdrplay_api_SelectDevice(&openDev);
        if (err != sdrplay_api_Success) {
            const char* errStr = sdrplay_api_GetErrorString(err);
            flog::error("Could not select RSP device: {0}", errStr);
            selectedName = "";
            return;
        }

        sdrplay_api_UnlockDeviceApi();
        sdrplay_api_DebugEnable(openDev.dev, sdrplay_api_DbgLvl_Message);

        err = sdrplay_api_GetDeviceParams(openDev.dev, &openDevParams);
        if (err != sdrplay_api_Success) {
            const char* errStr = sdrplay_api_GetErrorString(err);
            flog::error("Could not get device params for RSP device: {0}", errStr);
            selectedName = "";
            return;
        }

        err = sdrplay_api_Init(openDev.dev, &cbFuncs, this);
        if (err != sdrplay_api_Success) {
            const char* errStr = sdrplay_api_GetErrorString(err);
            flog::error("Could not init RSP device: {0}", errStr);
            selectedName = "";
            return;
        }

        // Define the valid samplerates
        samplerates.clear();
        samplerates.define(2e6, "2MHz", 2e6);
        samplerates.define(3e6, "3MHz", 3e6);
        samplerates.define(4e6, "4MHz", 4e6);
        samplerates.define(5e6, "5MHz", 5e6);
        samplerates.define(6e6, "6MHz", 6e6);
        samplerates.define(7e6, "7MHz", 7e6);
        samplerates.define(8e6, "8MHz", 8e6);
        samplerates.define(9e6, "9MHz", 9e6);
        samplerates.define(10e6, "10MHz", 10e6);

        // Define the valid bandwidths
        bandwidths.clear();
        bandwidths.define(200e3, "200KHz", sdrplay_api_BW_0_200);
        bandwidths.define(300e3, "300KHz", sdrplay_api_BW_0_300);
        bandwidths.define(600e3, "600KHz", sdrplay_api_BW_0_600);
        bandwidths.define(1.536e6, "1.536MHz", sdrplay_api_BW_1_536);
        bandwidths.define(5e6, "5MHz", sdrplay_api_BW_5_000);
        bandwidths.define(6e6, "6MHz", sdrplay_api_BW_6_000);
        bandwidths.define(7e6, "7MHz", sdrplay_api_BW_7_000);
        bandwidths.define(8e6, "8MHz", sdrplay_api_BW_8_000);
        bandwidths.define(0, "Auto", sdrplay_api_BW_Undefined);

        channelParams = openDevParams->rxChannelA;

        selectedName = devNameList[id];

        if (openDev.hwVer == SDRPLAY_RSP1_ID) {
            lnaSteps = 4;
        }
        else if (openDev.hwVer == SDRPLAY_RSP1A_ID || openDev.hwVer == SDRPLAY_RSP1B_ID) {
            lnaSteps = 10;
        }
        else if (openDev.hwVer == SDRPLAY_RSP2_ID) {
            lnaSteps = 9;
        }
        else if (openDev.hwVer == SDRPLAY_RSPduo_ID) {
            lnaSteps = 10;
        }
        else if (openDev.hwVer == SDRPLAY_RSPdx_ID || openDev.hwVer == SDRPLAY_RSPdxR2_ID) {
            lnaSteps = 28;
        }

        // Select default settings
        srId = 0;
        sampleRate = samplerates.value(0);
        bandwidthId = 8;
        lnaGain = lnaSteps - 1;
        gain = 59;
        agc = false;
        agcAttack = 500;
        agcDecay = 500;
        agcDecayDelay = 200;
        agcDecayThreshold = 5;
        agcSetPoint = -30;
        ifModeId = 0;
        rsp1a_fmmwNotch = false;
        rsp2_fmmwNotch = false;
        rspdx_fmmwNotch = false;
        rspduo_fmmwNotch = false;
        rsp1a_dabNotch = false;
        rspdx_dabNotch = false;
        rspduo_dabNotch = false;
        rsp1a_biasT = false;
        rsp2_biasT = false;
        rspdx_biasT = false;
        rspduo_biasT = false;
        rsp2_antennaPort = 0;
        rspdx_antennaPort = 0;
        rspduo_antennaPort = 0;

        config.acquire();

        // General options
        if (config.conf["devices"][selectedName].contains("samplerate")) {
            int sr = config.conf["devices"][selectedName]["samplerate"];
            if (samplerates.keyExists(sr)) {
                srId = samplerates.keyId(sr);
                sampleRate = samplerates[srId];
            }
        }
        if (config.conf["devices"][selectedName].contains("ifModeId")) {
            ifModeId = config.conf["devices"][selectedName]["ifModeId"];
            if (ifModeId != 0) {
                sampleRate = ifModes[ifModeId].effectiveSamplerate;
            }
        }
        if (config.conf["devices"][selectedName].contains("bwMode")) {
            bandwidthId = config.conf["devices"][selectedName]["bwMode"];
        }
        if (config.conf["devices"][selectedName].contains("lnaGain")) {
            lnaGain = config.conf["devices"][selectedName]["lnaGain"];
        }
        if (config.conf["devices"][selectedName].contains("ifGain")) {
            gain = config.conf["devices"][selectedName]["ifGain"];
        }
        if (config.conf["devices"][selectedName].contains("agc")) {
            agc = config.conf["devices"][selectedName]["agc"];
        }
        if (config.conf["devices"][selectedName].contains("agcAttack")) {
            agcAttack = config.conf["devices"][selectedName]["agcAttack"];
        }
        if (config.conf["devices"][selectedName].contains("agcDecay")) {
            agcDecay = config.conf["devices"][selectedName]["agcDecay"];
        }
        if (config.conf["devices"][selectedName].contains("agcDecayDelay")) {
            agcDecayDelay = config.conf["devices"][selectedName]["agcDecayDelay"];
        }
        if (config.conf["devices"][selectedName].contains("agcDecayThreshold")) {
            agcDecayThreshold = config.conf["devices"][selectedName]["agcDecayThreshold"];
        }
        if (config.conf["devices"][selectedName].contains("agcSetPoint")) {
            agcSetPoint = config.conf["devices"][selectedName]["agcSetPoint"];
        }

        // Per device options
        if (openDev.hwVer == SDRPLAY_RSP1_ID) {
            // No config to load
        }
        else if (openDev.hwVer == SDRPLAY_RSP1A_ID || openDev.hwVer == SDRPLAY_RSP1B_ID) {
            if (config.conf["devices"][selectedName].contains("fmmwNotch")) {
                rsp1a_fmmwNotch = config.conf["devices"][selectedName]["fmmwNotch"];
            }
            if (config.conf["devices"][selectedName].contains("dabNotch")) {
                rsp1a_dabNotch = config.conf["devices"][selectedName]["dabNotch"];
            }
            if (config.conf["devices"][selectedName].contains("biast")) {
                rsp1a_biasT = config.conf["devices"][selectedName]["biast"];
            }
        }
        else if (openDev.hwVer == SDRPLAY_RSP2_ID) {
            if (config.conf["devices"][selectedName].contains("antenna")) {
                rsp2_antennaPort = config.conf["devices"][selectedName]["antenna"];
            }
            if (config.conf["devices"][selectedName].contains("fmmwNotch")) {
                rsp2_fmmwNotch = config.conf["devices"][selectedName]["fmmwNotch"];
            }
            if (config.conf["devices"][selectedName].contains("biast")) {
                rsp2_biasT = config.conf["devices"][selectedName]["biast"];
            }
        }
        else if (openDev.hwVer == SDRPLAY_RSPduo_ID) {
            if (config.conf["devices"][selectedName].contains("dualTuner")) {
                rspduo_dualTuner = config.conf["devices"][selectedName]["dualTuner"];
            }
            if (config.conf["devices"][selectedName].contains("duoFs")) {
                int k = config.conf["devices"][selectedName]["duoFs"];
                if (duoFsList.keyExists(k)) { duoFsId = duoFsList.keyId(k); }
            }
            if (config.conf["devices"][selectedName].contains("duoDecim")) {
                int k = config.conf["devices"][selectedName]["duoDecim"];
                if (duoDecimList.keyExists(k)) { duoDecimId = duoDecimList.keyId(k); }
            }
            if (config.conf["devices"][selectedName].contains("antenna")) {
                rspduo_antennaPort = config.conf["devices"][selectedName]["antenna"];
            }
            if (config.conf["devices"][selectedName].contains("fmmwNotch")) {
                rspduo_fmmwNotch = config.conf["devices"][selectedName]["fmmwNotch"];
            }
            if (config.conf["devices"][selectedName].contains("dabNotch")) {
                rspduo_dabNotch = config.conf["devices"][selectedName]["dabNotch"];
            }
            if (config.conf["devices"][selectedName].contains("biast")) {
                rspduo_biasT = config.conf["devices"][selectedName]["biast"];
            }
        }
        else if (openDev.hwVer == SDRPLAY_RSPdx_ID || openDev.hwVer == SDRPLAY_RSPdxR2_ID) {
            if (config.conf["devices"][selectedName].contains("antenna")) {
                rspdx_antennaPort = config.conf["devices"][selectedName]["antenna"];
            }
            if (config.conf["devices"][selectedName].contains("fmmwNotch")) {
                rspdx_fmmwNotch = config.conf["devices"][selectedName]["fmmwNotch"];
            }
            if (config.conf["devices"][selectedName].contains("dabNotch")) {
                rspdx_dabNotch = config.conf["devices"][selectedName]["dabNotch"];
            }
            if (config.conf["devices"][selectedName].contains("biast")) {
                rspdx_biasT = config.conf["devices"][selectedName]["biast"];
            }
        }

        config.release();

        if (lnaGain >= lnaSteps) { lnaGain = lnaSteps - 1; }

        // Register or drop the channel set for whatever was just selected, so a saved dual
        // tuner setting takes effect on startup rather than only when the box is clicked.
        applyDualMode();

        // Release device after selecting
        sdrplay_api_Uninit(openDev.dev);
        sdrplay_api_ReleaseDevice(&openDev);
    }

    void rspDuoSelectTuner(sdrplay_api_TunerSelectT tuner, sdrplay_api_RspDuo_AmPortSelectT amPort) {
        if (openDev.tuner != tuner) {
            flog::info("Swapping tuners");
            auto ret = sdrplay_api_SwapRspDuoActiveTuner(openDev.dev, &openDev.tuner, amPort);
            if (ret != 0) {
                flog::error("Error while swapping tuners: {0}", (int)ret);
            }
        }

        // Change the channel params
        channelParams = (tuner == sdrplay_api_Tuner_A) ? openDevParams->rxChannelA : openDevParams->rxChannelB;
        channelParams->rspDuoTunerParams.tuner1AmPortSel = amPort;
        sdrplay_api_Update(openDev.dev, openDev.tuner, sdrplay_api_Update_RspDuo_AmPortSelect, sdrplay_api_Update_Ext1_None);

        // Refresh gains (for some reason they're lost)
        channelParams->tunerParams.gain.LNAstate = lnaGain;
        channelParams->tunerParams.gain.gRdB = gain;
        sdrplay_api_Update(openDev.dev, openDev.tuner, sdrplay_api_Update_Tuner_Gr, sdrplay_api_Update_Ext1_None);
        sdrplay_api_Update(openDev.dev, openDev.tuner, sdrplay_api_Update_Tuner_Gr, sdrplay_api_Update_Ext1_None);
    }

    void rspDuoSelectAntennaPort(int port) {
        // In dual tuner mode both tuners are already running, so there is no "active tuner"
        // to swap -- SwapRspDuoActiveTuner returns InvalidParam. The only choice that still
        // means anything is tuner 1's input, 50 ohm or Hi-Z; tuner 2 always has its own
        // port. Falling through to the single tuner path would also repoint channelParams
        // at channel B, sending later gain changes to the wrong tuner.
        if (rspduo_dualTuner && openDev.hwVer == SDRPLAY_RSPduo_ID) {
            if (!openDevParams || !openDevParams->rxChannelA) { return; }
            const sdrplay_api_RspDuo_AmPortSelectT amPort =
                (port == 1) ? sdrplay_api_RspDuo_AMPORT_1 : sdrplay_api_RspDuo_AMPORT_2;
            openDevParams->rxChannelA->rspDuoTunerParams.tuner1AmPortSel = amPort;
            channelParams = openDevParams->rxChannelA;
            if (running) {
                sdrplay_api_Update(openDev.dev, sdrplay_api_Tuner_A,
                                   sdrplay_api_Update_RspDuo_AmPortSelect, sdrplay_api_Update_Ext1_None);
            }
            return;
        }

        if (port == 0) { rspDuoSelectTuner(sdrplay_api_Tuner_A, sdrplay_api_RspDuo_AMPORT_2); }
        if (port == 1) { rspDuoSelectTuner(sdrplay_api_Tuner_A, sdrplay_api_RspDuo_AMPORT_1); }
        if (port == 2) { rspDuoSelectTuner(sdrplay_api_Tuner_B, sdrplay_api_RspDuo_AMPORT_1); }
    }

private:
    std::string getBandwdithScaled(double bw) {
        char buf[1024];
        if (bw >= 1000000.0) {
            sprintf(buf, "%.1lfMHz", bw / 1000000.0);
        }
        else if (bw >= 1000.0) {
            sprintf(buf, "%.1lfKHz", bw / 1000.0);
        }
        else {
            sprintf(buf, "%.1lfHz", bw);
        }
        return std::string(buf);
    }

    // Dual tuner replaces the ordinary samplerate choice: the output rate is fixed by the
    // API at 2 MS/s and divided by the decimation factor.
    void applyDualMode() {
        if (rspduo_dualTuner && openDev.hwVer == SDRPLAY_RSPduo_ID) {
            sampleRate = 2000000.0 / (double)duoDecimList.value(duoDecimId);
            sigpath::sourceManager.registerChannels("SDRplay", &channels);
        }
        else {
            rspduo_dualTuner = false;
            sampleRate = samplerates.value(srId);
            sigpath::sourceManager.unregisterChannels("SDRplay");
        }
        core::setInputSampleRate(sampleRate);
    }

    // Writes the live per-device fields into this device's own stored config -- shared by
    // applyConfig() below and mirrors exactly what each individual menuHandler/RSP*Menu control
    // already writes piecemeal on its own change, gated by the currently selected RSP model the
    // same way selectDev()'s own load branches are. Deliberately does NOT touch the RSPduo
    // "fmmwnotch" (lowercase) key that RSPduoMenu's own checkbox writes at line ~1190 --
    // selectDev() only ever reads "fmmwNotch" (capital N) back for that device family (line
    // ~449), so writing the lowercase key here would just recreate that pre-existing dead-key
    // bug rather than actually persisting anything. Not fixing that bug here -- out of scope for
    // phase 6 -- just not propagating it into new code.
    void persistDeviceSettings() {
        if (selectedName.empty()) { return; }
        config.acquire();
        json& d = config.conf["devices"][selectedName];
        d["samplerate"] = samplerates.key(srId);
        d["ifModeId"] = ifModeId;
        d["bwMode"] = bandwidthId;
        d["lnaGain"] = lnaGain;
        d["ifGain"] = gain;
        d["agc"] = agc;
        d["agcAttack"] = agcAttack;
        d["agcDecay"] = agcDecay;
        d["agcDecayDelay"] = agcDecayDelay;
        d["agcDecayThreshold"] = agcDecayThreshold;
        d["agcSetPoint"] = agcSetPoint;
        if (openDev.hwVer == SDRPLAY_RSP1A_ID || openDev.hwVer == SDRPLAY_RSP1B_ID) {
            d["fmmwNotch"] = rsp1a_fmmwNotch;
            d["dabNotch"] = rsp1a_dabNotch;
            d["biast"] = rsp1a_biasT;
        }
        else if (openDev.hwVer == SDRPLAY_RSP2_ID) {
            d["antenna"] = rsp2_antennaPort;
            d["fmmwNotch"] = rsp2_fmmwNotch;
            d["biast"] = rsp2_biasT;
        }
        else if (openDev.hwVer == SDRPLAY_RSPduo_ID) {
            d["dualTuner"] = rspduo_dualTuner;
            d["duoFs"] = duoFsList.key(duoFsId);
            d["duoDecim"] = duoDecimList.key(duoDecimId);
            d["antenna"] = rspduo_antennaPort;
            d["fmmwNotch"] = rspduo_fmmwNotch;
            d["dabNotch"] = rspduo_dabNotch;
            d["biast"] = rspduo_biasT;
        }
        else if (openDev.hwVer == SDRPLAY_RSPdx_ID || openDev.hwVer == SDRPLAY_RSPdxR2_ID) {
            d["antenna"] = rspdx_antennaPort;
            d["fmmwNotch"] = rspdx_fmmwNotch;
            d["dabNotch"] = rspdx_dabNotch;
            d["biast"] = rspdx_biasT;
        }
        config.release(true);
    }

    // SourceHandler::captureConfigHandler/applyConfigHandler (source.h) -- RECORDING_SCHEDULER_PLAN.md
    // phase 6, third module (FobosSDR, then SDRplay). Unlike FobosSDR, per-device fields here are
    // also gated by which RSP model is *currently selected* (openDev.hwVer) -- mirrors
    // selectDev()'s own per-hwVer branching exactly, so a snapshot captured from one RSP model's
    // fields never gets misapplied to a different model that happens to be selected later (e.g.
    // an RSPduo's dualTuner/duoFs/duoDecim fields have no meaning on an RSP2). samplerates/
    // bandwidths/duoFsList/duoDecimList are only populated once selectDev() has actually run for
    // the currently open device this session -- degrade gracefully rather than crash if applied
    // cold, same caveat as FobosSDR.
    static json captureConfig(void* ctx) {
        SDRPlaySourceModule* _this = (SDRPlaySourceModule*)ctx;
        json d;
        if (_this->samplerates.size()) { d["samplerate"] = _this->samplerates.key(_this->srId); }
        d["ifModeId"] = _this->ifModeId;
        if (_this->bandwidths.size()) { d["bwMode"] = _this->bandwidthId; }
        d["lnaGain"] = _this->lnaGain;
        d["ifGain"] = _this->gain;
        d["agc"] = _this->agc;
        d["agcAttack"] = _this->agcAttack;
        d["agcDecay"] = _this->agcDecay;
        d["agcDecayDelay"] = _this->agcDecayDelay;
        d["agcDecayThreshold"] = _this->agcDecayThreshold;
        d["agcSetPoint"] = _this->agcSetPoint;

        if (_this->openDev.hwVer == SDRPLAY_RSP1A_ID || _this->openDev.hwVer == SDRPLAY_RSP1B_ID) {
            d["fmmwNotch"] = _this->rsp1a_fmmwNotch;
            d["dabNotch"] = _this->rsp1a_dabNotch;
            d["biast"] = _this->rsp1a_biasT;
        }
        else if (_this->openDev.hwVer == SDRPLAY_RSP2_ID) {
            d["antenna"] = _this->rsp2_antennaPort;
            d["fmmwNotch"] = _this->rsp2_fmmwNotch;
            d["biast"] = _this->rsp2_biasT;
        }
        else if (_this->openDev.hwVer == SDRPLAY_RSPduo_ID) {
            d["dualTuner"] = _this->rspduo_dualTuner;
            if (_this->duoFsList.size()) { d["duoFs"] = _this->duoFsList.key(_this->duoFsId); }
            if (_this->duoDecimList.size()) { d["duoDecim"] = _this->duoDecimList.key(_this->duoDecimId); }
            d["antenna"] = _this->rspduo_antennaPort;
            d["fmmwNotch"] = _this->rspduo_fmmwNotch;
            d["dabNotch"] = _this->rspduo_dabNotch;
            d["biast"] = _this->rspduo_biasT;
        }
        else if (_this->openDev.hwVer == SDRPLAY_RSPdx_ID || _this->openDev.hwVer == SDRPLAY_RSPdxR2_ID) {
            d["antenna"] = _this->rspdx_antennaPort;
            d["fmmwNotch"] = _this->rspdx_fmmwNotch;
            d["dabNotch"] = _this->rspdx_dabNotch;
            d["biast"] = _this->rspdx_biasT;
        }
        return d;
    }

    static void applyConfig(const json& cfg, void* ctx) {
        SDRPlaySourceModule* _this = (SDRPlaySourceModule*)ctx;

        if (cfg.contains("samplerate") && _this->samplerates.keyExists(cfg["samplerate"].get<int>())) {
            _this->srId = _this->samplerates.keyId(cfg["samplerate"].get<int>());
            _this->sampleRate = _this->samplerates[_this->srId];
            if (_this->bandwidthId == 8) { _this->bandwidth = preferedBandwidth[_this->srId]; }
        }
        if (cfg.contains("ifModeId")) {
            int im = cfg["ifModeId"];
            if (im >= 0 && im < (int)(sizeof(ifModes) / sizeof(ifModes[0]))) {
                _this->ifModeId = im;
                if (im != 0) {
                    _this->bandwidth = ifModes[im].bw;
                    _this->sampleRate = ifModes[im].effectiveSamplerate;
                }
            }
        }
        if (cfg.contains("bwMode") && _this->bandwidths.size()) {
            int bw = cfg["bwMode"];
            if (bw >= 0 && bw < _this->bandwidths.size()) {
                _this->bandwidthId = bw;
                _this->bandwidth = (bw == 8) ? preferedBandwidth[_this->srId] : _this->bandwidths[bw];
            }
        }
        if (cfg.contains("lnaGain")) {
            _this->lnaGain = std::clamp<int>((int)cfg["lnaGain"], 0, _this->lnaSteps - 1);
        }
        if (cfg.contains("ifGain")) {
            _this->gain = std::clamp<int>((int)cfg["ifGain"], 20, 59);
        }
        if (cfg.contains("agc")) { _this->agc = cfg["agc"]; }
        if (cfg.contains("agcAttack")) { _this->agcAttack = std::clamp<int>((int)cfg["agcAttack"], 0, 65535); }
        if (cfg.contains("agcDecay")) { _this->agcDecay = std::clamp<int>((int)cfg["agcDecay"], 0, 65535); }
        if (cfg.contains("agcDecayDelay")) { _this->agcDecayDelay = std::clamp<int>((int)cfg["agcDecayDelay"], 0, 65535); }
        if (cfg.contains("agcDecayThreshold")) { _this->agcDecayThreshold = std::clamp<int>((int)cfg["agcDecayThreshold"], 0, 100); }
        if (cfg.contains("agcSetPoint")) { _this->agcSetPoint = std::clamp<int>((int)cfg["agcSetPoint"], -60, -20); }

        if (_this->openDev.hwVer == SDRPLAY_RSP1A_ID || _this->openDev.hwVer == SDRPLAY_RSP1B_ID) {
            if (cfg.contains("fmmwNotch")) { _this->rsp1a_fmmwNotch = cfg["fmmwNotch"]; }
            if (cfg.contains("dabNotch")) { _this->rsp1a_dabNotch = cfg["dabNotch"]; }
            if (cfg.contains("biast")) { _this->rsp1a_biasT = cfg["biast"]; }
        }
        else if (_this->openDev.hwVer == SDRPLAY_RSP2_ID) {
            if (cfg.contains("antenna")) { _this->rsp2_antennaPort = cfg["antenna"]; }
            if (cfg.contains("fmmwNotch")) { _this->rsp2_fmmwNotch = cfg["fmmwNotch"]; }
            if (cfg.contains("biast")) { _this->rsp2_biasT = cfg["biast"]; }
        }
        else if (_this->openDev.hwVer == SDRPLAY_RSPduo_ID) {
            if (cfg.contains("dualTuner")) { _this->rspduo_dualTuner = cfg["dualTuner"]; }
            if (cfg.contains("duoFs") && _this->duoFsList.keyExists(cfg["duoFs"].get<int>())) {
                _this->duoFsId = _this->duoFsList.keyId(cfg["duoFs"].get<int>());
            }
            if (cfg.contains("duoDecim") && _this->duoDecimList.keyExists(cfg["duoDecim"].get<int>())) {
                _this->duoDecimId = _this->duoDecimList.keyId(cfg["duoDecim"].get<int>());
            }
            if (cfg.contains("antenna")) { _this->rspduo_antennaPort = cfg["antenna"]; }
            if (cfg.contains("fmmwNotch")) { _this->rspduo_fmmwNotch = cfg["fmmwNotch"]; }
            if (cfg.contains("dabNotch")) { _this->rspduo_dabNotch = cfg["dabNotch"]; }
            if (cfg.contains("biast")) { _this->rspduo_biasT = cfg["biast"]; }
        }
        else if (_this->openDev.hwVer == SDRPLAY_RSPdx_ID || _this->openDev.hwVer == SDRPLAY_RSPdxR2_ID) {
            if (cfg.contains("antenna")) { _this->rspdx_antennaPort = cfg["antenna"]; }
            if (cfg.contains("fmmwNotch")) { _this->rspdx_fmmwNotch = cfg["fmmwNotch"]; }
            if (cfg.contains("dabNotch")) { _this->rspdx_dabNotch = cfg["dabNotch"]; }
            if (cfg.contains("biast")) { _this->rspdx_biasT = cfg["biast"]; }
        }

        _this->persistDeviceSettings();

        // Push to live hardware and re-derive sample rate / channel registration exactly the
        // way selectDev() itself does at the end of device selection (line ~480) -- applyDualMode()
        // already no-ops safely into the single-tuner path for every non-RSPduo model.
        _this->applyDualMode();
        if (_this->running) {
            _this->channelParams->tunerParams.gain.LNAstate = _this->lnaGain;
            _this->channelParams->tunerParams.gain.gRdB = _this->gain;
            _this->channelParams->ctrlParams.agc.attack_ms = _this->agcAttack;
            _this->channelParams->ctrlParams.agc.decay_ms = _this->agcDecay;
            _this->channelParams->ctrlParams.agc.decay_delay_ms = _this->agcDecayDelay;
            _this->channelParams->ctrlParams.agc.decay_threshold_dB = _this->agcDecayThreshold;
            _this->channelParams->ctrlParams.agc.setPoint_dBfs = _this->agcSetPoint;
            _this->channelParams->ctrlParams.agc.enable = _this->agc ? sdrplay_api_AGC_CTRL_EN : sdrplay_api_AGC_DISABLE;
            sdrplay_api_Update(_this->openDev.dev, _this->openDev.tuner, sdrplay_api_Update_Tuner_Gr, sdrplay_api_Update_Ext1_None);
            sdrplay_api_Update(_this->openDev.dev, _this->openDev.tuner, sdrplay_api_Update_Ctrl_Agc, sdrplay_api_Update_Ext1_None);
        }
    }

    static void menuSelected(void* ctx) {
        SDRPlaySourceModule* _this = (SDRPlaySourceModule*)ctx;
        core::setInputSampleRate(_this->sampleRate);
        flog::info("SDRPlaySourceModule '{0}': Menu Select!", _this->name);
    }

    static void menuDeselected(void* ctx) {
        SDRPlaySourceModule* _this = (SDRPlaySourceModule*)ctx;
        flog::info("SDRPlaySourceModule '{0}': Menu Deselect!", _this->name);
    }

    static void start(void* ctx) {
        SDRPlaySourceModule* _this = (SDRPlaySourceModule*)ctx;
        if (_this->running) { return; }

        // First, acquire device
        sdrplay_api_ErrT err;

        // Fixed for the run: the mode selector is disabled while streaming, because it
        // changes how the device is opened.
        _this->dualRunning = _this->rspduo_dualTuner && (_this->openDev.hwVer == SDRPLAY_RSPduo_ID);

        if (_this->dualRunning) {
            // rspDuoSampleFreq has to be set before SelectDevice, not after: the API derives
            // fsHz and the IF from it while opening.
            _this->openDev.tuner = sdrplay_api_Tuner_Both;
            _this->openDev.rspDuoMode = sdrplay_api_RspDuoMode_Dual_Tuner;
            _this->openDev.rspDuoSampleFreq = _this->duoFsList.value(_this->duoFsId);
        }
        else {
            _this->openDev.tuner = sdrplay_api_Tuner_A;
            _this->openDev.rspDuoMode = sdrplay_api_RspDuoMode_Single_Tuner;
            _this->openDev.rspDuoSampleFreq = 0.0;
        }
        err = sdrplay_api_SelectDevice(&_this->openDev);
        if (err != sdrplay_api_Success) {
            const char* errStr = sdrplay_api_GetErrorString(err);
            flog::error("Could not select RSP device: {0}", errStr);
            _this->selectedName = "";
            return;
        }

        sdrplay_api_UnlockDeviceApi();
        sdrplay_api_DebugEnable(_this->openDev.dev, sdrplay_api_DbgLvl_Message);

        err = sdrplay_api_GetDeviceParams(_this->openDev.dev, &_this->openDevParams);
        if (err != sdrplay_api_Success) {
            const char* errStr = sdrplay_api_GetErrorString(err);
            flog::error("Could not get device params for RSP device: {0}", errStr);
            _this->selectedName = "";
            return;
        }

        err = sdrplay_api_Init(_this->openDev.dev, &_this->cbFuncs, _this);
        if (err != sdrplay_api_Success) {
            const char* errStr = sdrplay_api_GetErrorString(err);
            flog::error("Could not init RSP device: {0}", errStr);
            _this->selectedName = "";
            return;
        }

        _this->channelParams = _this->openDevParams->rxChannelA;

        // Configure device
        _this->bufferIndex = 0;
        _this->bufferIndex2 = 0;
        _this->bufferSize = (float)_this->sampleRate / 200.0f;

        // RSP1A Options
        if (_this->openDev.hwVer == SDRPLAY_RSP1A_ID || _this->openDev.hwVer == SDRPLAY_RSP1B_ID) {
            _this->openDevParams->devParams->rsp1aParams.rfNotchEnable = _this->rsp1a_fmmwNotch;
            _this->openDevParams->devParams->rsp1aParams.rfDabNotchEnable = _this->rsp1a_dabNotch;
            _this->channelParams->rsp1aTunerParams.biasTEnable = _this->rsp1a_biasT;
            sdrplay_api_Update(_this->openDev.dev, _this->openDev.tuner, sdrplay_api_Update_Rsp1a_RfNotchControl, sdrplay_api_Update_Ext1_None);
            sdrplay_api_Update(_this->openDev.dev, _this->openDev.tuner, sdrplay_api_Update_Rsp1a_RfDabNotchControl, sdrplay_api_Update_Ext1_None);
            sdrplay_api_Update(_this->openDev.dev, _this->openDev.tuner, sdrplay_api_Update_Rsp1a_BiasTControl, sdrplay_api_Update_Ext1_None);
        }
        else if (_this->openDev.hwVer == SDRPLAY_RSP2_ID) {
            _this->channelParams->rsp2TunerParams.rfNotchEnable = _this->rsp2_fmmwNotch;
            _this->channelParams->rsp2TunerParams.biasTEnable = _this->rsp2_biasT;
            _this->channelParams->rsp2TunerParams.antennaSel = rsp2_antennaPorts[_this->rsp2_antennaPort];
            _this->channelParams->rsp2TunerParams.amPortSel = (_this->rsp2_antennaPort == 2) ? sdrplay_api_Rsp2_AMPORT_1 : sdrplay_api_Rsp2_AMPORT_2;
            sdrplay_api_Update(_this->openDev.dev, _this->openDev.tuner, sdrplay_api_Update_Rsp2_RfNotchControl, sdrplay_api_Update_Ext1_None);
            sdrplay_api_Update(_this->openDev.dev, _this->openDev.tuner, sdrplay_api_Update_Rsp2_BiasTControl, sdrplay_api_Update_Ext1_None);
            sdrplay_api_Update(_this->openDev.dev, _this->openDev.tuner, sdrplay_api_Update_Rsp2_AntennaControl, sdrplay_api_Update_Ext1_None);
            sdrplay_api_Update(_this->openDev.dev, _this->openDev.tuner, sdrplay_api_Update_Rsp2_AmPortSelect, sdrplay_api_Update_Ext1_None);
        }
        else if (_this->openDev.hwVer == SDRPLAY_RSPduo_ID) {
            // NOTE: mmight require setting it on both RXA and RXB
            _this->rspDuoSelectAntennaPort(_this->rspduo_antennaPort);
            _this->channelParams->rspDuoTunerParams.biasTEnable = _this->rspduo_biasT;
            _this->channelParams->rspDuoTunerParams.rfNotchEnable = _this->rspduo_fmmwNotch;
            _this->channelParams->rspDuoTunerParams.rfDabNotchEnable = _this->rspduo_dabNotch;
            _this->channelParams->rspDuoTunerParams.tuner1AmNotchEnable = _this->rspduo_fmmwNotch;
            sdrplay_api_Update(_this->openDev.dev, _this->openDev.tuner, sdrplay_api_Update_RspDuo_BiasTControl, sdrplay_api_Update_Ext1_None);
            sdrplay_api_Update(_this->openDev.dev, _this->openDev.tuner, sdrplay_api_Update_RspDuo_RfNotchControl, sdrplay_api_Update_Ext1_None);
            sdrplay_api_Update(_this->openDev.dev, _this->openDev.tuner, sdrplay_api_Update_RspDuo_RfDabNotchControl, sdrplay_api_Update_Ext1_None);
            sdrplay_api_Update(_this->openDev.dev, _this->openDev.tuner, sdrplay_api_Update_RspDuo_Tuner1AmNotchControl, sdrplay_api_Update_Ext1_None);
        }
        else if (_this->openDev.hwVer == SDRPLAY_RSPdx_ID || _this->openDev.hwVer == SDRPLAY_RSPdxR2_ID) {
            _this->openDevParams->devParams->rspDxParams.rfNotchEnable = _this->rspdx_fmmwNotch;
            _this->openDevParams->devParams->rspDxParams.rfDabNotchEnable = _this->rspdx_dabNotch;
            _this->openDevParams->devParams->rspDxParams.biasTEnable = _this->rspdx_biasT;
            _this->openDevParams->devParams->rspDxParams.antennaSel = rspdx_antennaPorts[_this->rspdx_antennaPort];
            sdrplay_api_Update(_this->openDev.dev, _this->openDev.tuner, sdrplay_api_Update_None, sdrplay_api_Update_RspDx_RfNotchControl);
            sdrplay_api_Update(_this->openDev.dev, _this->openDev.tuner, sdrplay_api_Update_None, sdrplay_api_Update_RspDx_RfDabNotchControl);
            sdrplay_api_Update(_this->openDev.dev, _this->openDev.tuner, sdrplay_api_Update_None, sdrplay_api_Update_RspDx_BiasTControl);
            sdrplay_api_Update(_this->openDev.dev, _this->openDev.tuner, sdrplay_api_Update_None, sdrplay_api_Update_RspDx_AntennaControl);
        }

        // General options
        if (_this->dualRunning) {
            // Dual tuner runs at a low IF, and the API has already set fsHz and ifType from
            // rspDuoSampleFreq. Overwriting either here is how you get an Init failure or a
            // silently wrong rate, so only bandwidth and decimation are ours to choose.
            _this->channelParams->tunerParams.bwType = sdrplay_api_BW_1_536;
            _this->bandwidth = sdrplay_api_BW_1_536;
        }
        else if (_this->ifModeId == 0) {
            _this->bandwidth = (_this->bandwidthId == 8) ? preferedBandwidth[_this->srId] : _this->bandwidths[_this->bandwidthId];
            _this->openDevParams->devParams->fsFreq.fsHz = _this->sampleRate;
            _this->channelParams->tunerParams.bwType = _this->bandwidth;
        }
        else {
            _this->openDevParams->devParams->fsFreq.fsHz = ifModes[_this->ifModeId].deviceSamplerate;
            _this->channelParams->tunerParams.bwType = ifModes[_this->ifModeId].bw;
        }
        _this->channelParams->tunerParams.rfFreq.rfHz = _this->freq;
        _this->channelParams->tunerParams.gain.gRdB = _this->gain;
        _this->channelParams->tunerParams.gain.LNAstate = _this->lnaGain;
        _this->channelParams->ctrlParams.dcOffset.DCenable = true;
        _this->channelParams->ctrlParams.dcOffset.IQenable = true;
        _this->channelParams->tunerParams.loMode = sdrplay_api_LO_Auto;
        if (_this->dualRunning) {
            const int decim = _this->duoDecimList.value(_this->duoDecimId);
            _this->channelParams->ctrlParams.decimation.enable = (decim > 1);
            _this->channelParams->ctrlParams.decimation.decimationFactor = (unsigned char)decim;
        }
        else {
            _this->channelParams->ctrlParams.decimation.enable = false;
            _this->channelParams->tunerParams.ifType = ifModes[_this->ifModeId].ifValue;
        }

        // Hard coded AGC parameters
        _this->channelParams->ctrlParams.agc.attack_ms = _this->agcAttack;
        _this->channelParams->ctrlParams.agc.decay_ms = _this->agcDecay;
        _this->channelParams->ctrlParams.agc.decay_delay_ms = _this->agcDecayDelay;
        _this->channelParams->ctrlParams.agc.decay_threshold_dB = _this->agcDecayThreshold;
        _this->channelParams->ctrlParams.agc.setPoint_dBfs = _this->agcSetPoint;
        _this->channelParams->ctrlParams.agc.enable = _this->agc ? sdrplay_api_AGC_CTRL_EN : sdrplay_api_AGC_DISABLE;

        // Both tuners must carry identical settings, and the phase relationship between
        // them only means anything if they do. openDev.tuner is Tuner_Both in dual mode, so
        // every Update below reaches both without further work.
        if (_this->dualRunning && _this->openDevParams->rxChannelB) {
            sdrplay_api_RxChannelParamsT* a = _this->openDevParams->rxChannelA;
            sdrplay_api_RxChannelParamsT* b = _this->openDevParams->rxChannelB;
            b->tunerParams.bwType = a->tunerParams.bwType;
            b->tunerParams.ifType = a->tunerParams.ifType;
            b->tunerParams.rfFreq = a->tunerParams.rfFreq;
            b->tunerParams.gain = a->tunerParams.gain;
            b->tunerParams.loMode = a->tunerParams.loMode;
            b->ctrlParams = a->ctrlParams;
        }

        sdrplay_api_Update(_this->openDev.dev, _this->openDev.tuner, sdrplay_api_Update_Dev_Fs, sdrplay_api_Update_Ext1_None);
        sdrplay_api_Update(_this->openDev.dev, _this->openDev.tuner, sdrplay_api_Update_Tuner_BwType, sdrplay_api_Update_Ext1_None);
        sdrplay_api_Update(_this->openDev.dev, _this->openDev.tuner, sdrplay_api_Update_Tuner_IfType, sdrplay_api_Update_Ext1_None);
        sdrplay_api_Update(_this->openDev.dev, _this->openDev.tuner, sdrplay_api_Update_Tuner_LoMode, sdrplay_api_Update_Ext1_None);
        sdrplay_api_Update(_this->openDev.dev, _this->openDev.tuner, sdrplay_api_Update_Ctrl_Decimation, sdrplay_api_Update_Ext1_None);
        sdrplay_api_Update(_this->openDev.dev, _this->openDev.tuner, sdrplay_api_Update_Ctrl_DCoffsetIQimbalance, sdrplay_api_Update_Ext1_None);
        sdrplay_api_Update(_this->openDev.dev, _this->openDev.tuner, sdrplay_api_Update_Tuner_Frf, sdrplay_api_Update_Ext1_None);
        sdrplay_api_Update(_this->openDev.dev, _this->openDev.tuner, sdrplay_api_Update_Tuner_Gr, sdrplay_api_Update_Ext1_None);
        sdrplay_api_Update(_this->openDev.dev, _this->openDev.tuner, sdrplay_api_Update_Ctrl_Agc, sdrplay_api_Update_Ext1_None);

        _this->running = true;
        flog::info("SDRPlaySourceModule '{0}': Start!", _this->name);
    }

    static void stop(void* ctx) {
        SDRPlaySourceModule* _this = (SDRPlaySourceModule*)ctx;
        if (!_this->running) { return; }
        _this->running = false;
        _this->stream.stopWriter();
        _this->stream2.stopWriter();

        // Release device after stopping
        sdrplay_api_Uninit(_this->openDev.dev);
        sdrplay_api_ReleaseDevice(&_this->openDev);

        _this->stream.clearWriteStop();
        _this->stream2.clearWriteStop();
        _this->dualRunning = false;
        flog::info("SDRPlaySourceModule '{0}': Stop!", _this->name);
    }

    static void tune(double freq, void* ctx) {
        SDRPlaySourceModule* _this = (SDRPlaySourceModule*)ctx;
        if (_this->running) {
            _this->channelParams->tunerParams.rfFreq.rfHz = freq;
            // Both tuners, written before a single update, so they move together rather than
            // one lagging the other by a command round trip.
            if (_this->dualRunning && _this->openDevParams->rxChannelB) {
                _this->openDevParams->rxChannelB->tunerParams.rfFreq.rfHz = freq;
            }
            sdrplay_api_Update(_this->openDev.dev, _this->openDev.tuner, sdrplay_api_Update_Tuner_Frf, sdrplay_api_Update_Ext1_None);
        }
        _this->freq = freq;
        flog::info("SDRPlaySourceModule '{0}': Tune: {1}!", _this->name, freq);
    }

    static void menuHandler(void* ctx) {
        SDRPlaySourceModule* _this = (SDRPlaySourceModule*)ctx;

        if (_this->running) { SmGui::BeginDisabled(); }

        SmGui::FillWidth();
        SmGui::ForceSync();
        if (SmGui::Combo(CONCAT("##sdrplay_dev", _this->name), &_this->devId, _this->devListTxt.c_str())) {
            _this->selectById(_this->devId);
            core::setInputSampleRate(_this->sampleRate);
            config.acquire();
            config.conf["device"] = _this->devNameList[_this->devId];
            config.release(true);
        }

        if (_this->ifModeId == 0) {
            // In dual tuner mode the rate comes from the duo controls instead.
            if (_this->rspduo_dualTuner) { SmGui::BeginDisabled(); }
            if (SmGui::Combo(CONCAT("##sdrplay_sr", _this->name), &_this->srId, _this->samplerates.txt)) {
                _this->sampleRate = _this->samplerates[_this->srId];
                if (_this->bandwidthId == 8) {
                    _this->bandwidth = preferedBandwidth[_this->srId];
                }
                core::setInputSampleRate(_this->sampleRate);
                config.acquire();
                config.conf["devices"][_this->selectedName]["samplerate"] = _this->samplerates.key(_this->srId);
                config.release(true);
            }
            if (_this->rspduo_dualTuner) { SmGui::EndDisabled(); }

            SmGui::SameLine();
            SmGui::FillWidth();
            SmGui::ForceSync();
            if (SmGui::Button(CONCAT("Refresh##sdrplay_refresh", _this->name))) {
                _this->refresh();
                _this->selectByName(_this->selectedName);
                core::setInputSampleRate(_this->sampleRate);
            }

            SmGui::LeftLabel("Bandwidth");
            SmGui::FillWidth();
            if (SmGui::Combo(CONCAT("##sdrplay_bw", _this->name), &_this->bandwidthId, _this->bandwidths.txt)) {
                _this->bandwidth = (_this->bandwidthId == 8) ? preferedBandwidth[_this->srId] : _this->bandwidths[_this->bandwidthId];
                if (_this->running) {
                    _this->channelParams->tunerParams.bwType = _this->bandwidth;
                    sdrplay_api_Update(_this->openDev.dev, _this->openDev.tuner, sdrplay_api_Update_Tuner_BwType, sdrplay_api_Update_Ext1_None);
                }
                config.acquire();
                config.conf["devices"][_this->selectedName]["bwMode"] = _this->bandwidthId;
                config.release(true);
            }
        }
        else {
            SmGui::FillWidth();
            SmGui::ForceSync();
            if (SmGui::Button(CONCAT("Refresh##sdrplay_refresh", _this->name))) {
                _this->refresh();
                _this->selectByName(_this->selectedName);
            }
        }

        SmGui::LeftLabel("IF Mode");
        SmGui::FillWidth();
        SmGui::ForceSync();
        if (SmGui::Combo(CONCAT("##sdrplay_ifmode", _this->name), &_this->ifModeId, ifModeTxt)) {
            if (_this->ifModeId != 0) {
                _this->bandwidth = ifModes[_this->ifModeId].bw;
                _this->sampleRate = ifModes[_this->ifModeId].effectiveSamplerate;
            }
            else {
                config.acquire();
                // Reload samplerate
                if (config.conf["devices"][_this->selectedName].contains("samplerate")) {
                    int sr = config.conf["devices"][_this->selectedName]["samplerate"];
                    if (_this->samplerates.keyExists(sr)) {
                        _this->srId = _this->samplerates.keyId(sr);
                    }
                }
                else {
                    _this->srId = 0;
                }

                // Reload bandwidth
                if (config.conf["devices"][_this->selectedName].contains("bwMode")) {
                    _this->bandwidthId = config.conf["devices"][_this->selectedName]["bwMode"];
                }
                else {
                    // Auto
                    _this->bandwidthId = 8;
                }
                _this->sampleRate = _this->samplerates[_this->srId];
                config.release();
                _this->bandwidth = (_this->bandwidthId == 8) ? preferedBandwidth[_this->srId] : _this->bandwidths[_this->bandwidthId];
            }
            core::setInputSampleRate(_this->sampleRate);
            config.acquire();
            config.conf["devices"][_this->selectedName]["ifModeId"] = _this->ifModeId;
            config.release(true);
        }

        if (_this->running) { SmGui::EndDisabled(); }

        if (_this->selectedName != "") {
            SmGui::LeftLabel("LNA Gain");
            SmGui::FillWidth();
            if (SmGui::SliderInt(CONCAT("##sdrplay_lna_gain", _this->name), &_this->lnaGain, _this->lnaSteps - 1, 0, SmGui::FMT_STR_NONE)) {
                if (_this->running) {
                    _this->channelParams->tunerParams.gain.LNAstate = _this->lnaGain;
                    sdrplay_api_Update(_this->openDev.dev, _this->openDev.tuner, sdrplay_api_Update_Tuner_Gr, sdrplay_api_Update_Ext1_None);
                }
                config.acquire();
                config.conf["devices"][_this->selectedName]["lnaGain"] = _this->lnaGain;
                config.release(true);
            }

            if (_this->agc > 0) { SmGui::BeginDisabled(); }
            SmGui::LeftLabel("IF Gain");
            SmGui::FillWidth();
            if (SmGui::SliderInt(CONCAT("##sdrplay_gain", _this->name), &_this->gain, 59, 20, SmGui::FMT_STR_NONE)) {
                if (_this->running) {
                    _this->channelParams->tunerParams.gain.gRdB = _this->gain;
                    sdrplay_api_Update(_this->openDev.dev, _this->openDev.tuner, sdrplay_api_Update_Tuner_Gr, sdrplay_api_Update_Ext1_None);
                }
                config.acquire();
                config.conf["devices"][_this->selectedName]["ifGain"] = _this->gain;
                config.release(true);
            }
            if (_this->agc > 0) { SmGui::EndDisabled(); }


            if (_this->agcParamEdit) {
                bool valid = false;
                _this->agcParamEdit = _this->agcParamMenu(valid);

                // If the menu was closed and (TODO) valid, update options
                if (!_this->agcParamEdit && valid) {
                    _this->agcAttack = _this->_agcAttack;
                    _this->agcDecay = _this->_agcDecay;
                    _this->agcDecayDelay = _this->_agcDecayDelay;
                    _this->agcDecayThreshold = _this->_agcDecayThreshold;
                    _this->agcSetPoint = _this->_agcSetPoint;
                    if (_this->running && _this->agc) {
                        _this->channelParams->ctrlParams.agc.attack_ms = _this->agcAttack;
                        _this->channelParams->ctrlParams.agc.decay_ms = _this->agcDecay;
                        _this->channelParams->ctrlParams.agc.decay_delay_ms = _this->agcDecayDelay;
                        _this->channelParams->ctrlParams.agc.decay_threshold_dB = _this->agcDecayThreshold;
                        _this->channelParams->ctrlParams.agc.setPoint_dBfs = _this->agcSetPoint;
                        sdrplay_api_Update(_this->openDev.dev, _this->openDev.tuner, sdrplay_api_Update_Ctrl_Agc, sdrplay_api_Update_Ext1_None);
                    }
                    config.acquire();
                    config.conf["devices"][_this->selectedName]["agcAttack"] = _this->agcAttack;
                    config.conf["devices"][_this->selectedName]["agcDecay"] = _this->agcDecay;
                    config.conf["devices"][_this->selectedName]["agcDecayDelay"] = _this->agcDecayDelay;
                    config.conf["devices"][_this->selectedName]["agcDecayThreshold"] = _this->agcDecayThreshold;
                    config.conf["devices"][_this->selectedName]["agcSetPoint"] = _this->agcSetPoint;
                    config.release(true);
                }
            }

            SmGui::ForceSync();
            if (SmGui::Checkbox(CONCAT("IF AGC##sdrplay_agc", _this->name), &_this->agc)) {
                if (_this->running) {
                    _this->channelParams->ctrlParams.agc.enable = _this->agc ? sdrplay_api_AGC_CTRL_EN : sdrplay_api_AGC_DISABLE;
                    if (_this->agc) {
                        _this->channelParams->ctrlParams.agc.attack_ms = _this->agcAttack;
                        _this->channelParams->ctrlParams.agc.decay_ms = _this->agcDecay;
                        _this->channelParams->ctrlParams.agc.decay_delay_ms = _this->agcDecayDelay;
                        _this->channelParams->ctrlParams.agc.decay_threshold_dB = _this->agcDecayThreshold;
                        _this->channelParams->ctrlParams.agc.setPoint_dBfs = _this->agcSetPoint;
                        sdrplay_api_Update(_this->openDev.dev, _this->openDev.tuner, sdrplay_api_Update_Ctrl_Agc, sdrplay_api_Update_Ext1_None);
                    }
                    else {
                        sdrplay_api_Update(_this->openDev.dev, _this->openDev.tuner, sdrplay_api_Update_Ctrl_Agc, sdrplay_api_Update_Ext1_None);
                        _this->channelParams->tunerParams.gain.gRdB = _this->gain;
                        sdrplay_api_Update(_this->openDev.dev, _this->openDev.tuner, sdrplay_api_Update_Tuner_Gr, sdrplay_api_Update_Ext1_None);
                    }
                }
                config.acquire();
                config.conf["devices"][_this->selectedName]["agc"] = _this->agc;
                config.release(true);
            }
            SmGui::SameLine();
            SmGui::FillWidth();
            SmGui::ForceSync();
            if (SmGui::Button(CONCAT("Parameters##sdrplay_agc_edit_btn", _this->name))) {
                _this->agcParamEdit = true;
                _this->_agcAttack = _this->agcAttack;
                _this->_agcDecay = _this->agcDecay;
                _this->_agcDecayDelay = _this->agcDecayDelay;
                _this->_agcDecayThreshold = _this->agcDecayThreshold;
                _this->_agcSetPoint = _this->agcSetPoint;
            }

            switch (_this->openDev.hwVer) {
            case SDRPLAY_RSP1_ID:
                _this->RSP1Menu();
                break;
            case SDRPLAY_RSP1A_ID:
            case SDRPLAY_RSP1B_ID:
                _this->RSP1AMenu();
                break;
            case SDRPLAY_RSP2_ID:
                _this->RSP2Menu();
                break;
            case SDRPLAY_RSPduo_ID:
                _this->RSPduoMenu();
                break;
            case SDRPLAY_RSPdx_ID:
            case SDRPLAY_RSPdxR2_ID:
                _this->RSPdxMenu();
                break;
            default:
                _this->RSPUnsupportedMenu();
                break;
            }
        }
        else {
            SmGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "No device available");
        }
    }

    bool agcParamMenu(bool& valid) {
        bool open = true;
        gui::mainWindow.lockWaterfallControls = true;
        SmGui::OpenPopup("Edit##sdrplay_source_edit_agc_params_");
        if (SmGui::BeginPopup("Edit##sdrplay_source_edit_agc_params_", ImGuiWindowFlags_NoResize)) {
            if (SmGui::BeginTable(("sdrplay_source_agc_param_tbl" + name).c_str(), 2)) {
                SmGui::TableNextRow();
                SmGui::TableSetColumnIndex(0);
                SmGui::LeftLabel("Attack");
                SmGui::TableSetColumnIndex(1);
                SmGui::SetNextItemWidth(100);
                SmGui::InputInt("ms##sdrplay_source_agc_attack", &_agcAttack);
                _agcAttack = std::clamp<int>(_agcAttack, 0, 65535);

                SmGui::TableNextRow();
                SmGui::TableSetColumnIndex(0);
                SmGui::LeftLabel("Decay");
                SmGui::TableSetColumnIndex(1);
                SmGui::SetNextItemWidth(100);
                SmGui::InputInt("ms##sdrplay_source_agc_decay", &_agcDecay);
                _agcDecay = std::clamp<int>(_agcDecay, 0, 65535);

                SmGui::TableNextRow();
                SmGui::TableSetColumnIndex(0);
                SmGui::LeftLabel("Decay Delay");
                SmGui::TableSetColumnIndex(1);
                SmGui::SetNextItemWidth(100);
                SmGui::InputInt("ms##sdrplay_source_agc_decay_delay", &_agcDecayDelay);
                _agcDecayDelay = std::clamp<int>(_agcDecayDelay, 0, 65535);

                SmGui::TableNextRow();
                SmGui::TableSetColumnIndex(0);
                SmGui::LeftLabel("Decay Threshold");
                SmGui::TableSetColumnIndex(1);
                SmGui::SetNextItemWidth(100);
                SmGui::InputInt("dB##sdrplay_source_agc_decay_thresh", &_agcDecayThreshold);
                _agcDecayThreshold = std::clamp<int>(_agcDecayThreshold, 0, 100);

                SmGui::TableNextRow();
                SmGui::TableSetColumnIndex(0);
                SmGui::LeftLabel("Setpoint");
                SmGui::TableSetColumnIndex(1);
                SmGui::SetNextItemWidth(100);
                SmGui::InputInt("dBFS##sdrplay_source_agc_setpoint", &_agcSetPoint);
                _agcSetPoint = std::clamp<int>(_agcSetPoint, -60, -20);

                SmGui::EndTable();
            }

            SmGui::ForceSync();
            if (SmGui::Button(" Apply ")) {
                open = false;
                valid = true;
            }
            SmGui::SameLine();
            SmGui::ForceSync();
            if (SmGui::Button("Cancel")) {
                open = false;
                valid = false;
            }
            SmGui::EndPopup();
        }
        return open;
    }

    void RSP1Menu() {
        // No options?
    }

    void RSP1AMenu() {
        if (SmGui::Checkbox(CONCAT("FM/MW Notch##sdrplay_rsp1a_fmmwnotch", name), &rsp1a_fmmwNotch)) {
            if (running) {
                openDevParams->devParams->rsp1aParams.rfNotchEnable = rsp1a_fmmwNotch;
                sdrplay_api_Update(openDev.dev, openDev.tuner, sdrplay_api_Update_Rsp1a_RfNotchControl, sdrplay_api_Update_Ext1_None);
            }
            config.acquire();
            config.conf["devices"][selectedName]["fmmwNotch"] = rsp1a_fmmwNotch;
            config.release(true);
        }
        if (SmGui::Checkbox(CONCAT("DAB Notch##sdrplay_rsp1a_dabnotch", name), &rsp1a_dabNotch)) {
            if (running) {
                openDevParams->devParams->rsp1aParams.rfDabNotchEnable = rsp1a_dabNotch;
                sdrplay_api_Update(openDev.dev, openDev.tuner, sdrplay_api_Update_Rsp1a_RfDabNotchControl, sdrplay_api_Update_Ext1_None);
            }
            config.acquire();
            config.conf["devices"][selectedName]["dabNotch"] = rsp1a_dabNotch;
            config.release(true);
        }
        if (SmGui::Checkbox(CONCAT("Bias-T##sdrplay_rsp1a_biast", name), &rsp1a_biasT)) {
            if (running) {
                channelParams->rsp1aTunerParams.biasTEnable = rsp1a_biasT;
                sdrplay_api_Update(openDev.dev, openDev.tuner, sdrplay_api_Update_Rsp1a_BiasTControl, sdrplay_api_Update_Ext1_None);
            }
            config.acquire();
            config.conf["devices"][selectedName]["biast"] = rsp1a_biasT;
            config.release(true);
        }
    }

    void RSP2Menu() {
        SmGui::LeftLabel("Antenna");
        SmGui::FillWidth();
        SmGui::ForceSync();
        if (SmGui::Combo(CONCAT("##sdrplay_rsp2_ant", name), &rsp2_antennaPort, rsp2_antennaPortsTxt)) {
            if (running) {
                channelParams->rsp2TunerParams.antennaSel = rsp2_antennaPorts[rsp2_antennaPort];
                channelParams->rsp2TunerParams.amPortSel = (rsp2_antennaPort == 2) ? sdrplay_api_Rsp2_AMPORT_1 : sdrplay_api_Rsp2_AMPORT_2;
                sdrplay_api_Update(openDev.dev, openDev.tuner, sdrplay_api_Update_Rsp2_AntennaControl, sdrplay_api_Update_Ext1_None);
                sdrplay_api_Update(openDev.dev, openDev.tuner, sdrplay_api_Update_Rsp2_AmPortSelect, sdrplay_api_Update_Ext1_None);
            }
            config.acquire();
            config.conf["devices"][selectedName]["antenna"] = rsp2_antennaPort;
            config.release(true);
        }

        // The notch is only available on the 50Ohm ports
        if (rsp2_antennaPort != 2) {
            if (SmGui::Checkbox(CONCAT("MW/FM Notch##sdrplay_rsp2_fmmwnotch", name), &rsp2_fmmwNotch)) {
                if (running) {
                    channelParams->rsp2TunerParams.rfNotchEnable = rsp2_fmmwNotch;
                    sdrplay_api_Update(openDev.dev, openDev.tuner, sdrplay_api_Update_Rsp2_RfNotchControl, sdrplay_api_Update_Ext1_None);
                }
                config.acquire();
                config.conf["devices"][selectedName]["fmmwNotch"] = rsp2_fmmwNotch;
                config.release(true);
            }
        }
        else {
            style::beginDisabled();
            bool dummy = false;
            SmGui::Checkbox(CONCAT("MW/FM Notch##sdrplay_rsp2_fmmwnotch", name), &dummy);
            style::endDisabled();
        }
        
        if (SmGui::Checkbox(CONCAT("Bias-T##sdrplay_rsp2_biast", name), &rsp2_biasT)) {
            if (running) {
                channelParams->rsp2TunerParams.biasTEnable = rsp2_biasT;
                sdrplay_api_Update(openDev.dev, openDev.tuner, sdrplay_api_Update_Rsp2_BiasTControl, sdrplay_api_Update_Ext1_None);
            }
            config.acquire();
            config.conf["devices"][selectedName]["biast"] = rsp2_biasT;
            config.release(true);
        }
    }

    void RSPduoMenu() {
        // Both tuners at once, which is what the phasing front end needs. Disabled while
        // running because it changes how the device is opened.
        if (running) { SmGui::BeginDisabled(); }
        SmGui::ForceSync();
        if (SmGui::Checkbox(CONCAT("Dual tuner (phasing)##sdrplay_duo_", name), &rspduo_dualTuner)) {
            applyDualMode();
            config.acquire();
            config.conf["devices"][selectedName]["dualTuner"] = rspduo_dualTuner;
            config.release(true);
        }
        if (rspduo_dualTuner) {
            SmGui::LeftLabel("ADC rate");
            SmGui::FillWidth();
            if (SmGui::Combo(CONCAT("##sdrplay_duofs_", name), &duoFsId, duoFsList.txt)) {
                config.acquire();
                config.conf["devices"][selectedName]["duoFs"] = duoFsList.key(duoFsId);
                config.release(true);
            }
            SmGui::LeftLabel("Samplerate");
            SmGui::FillWidth();
            if (SmGui::Combo(CONCAT("##sdrplay_duosr_", name), &duoDecimId, duoDecimList.txt)) {
                applyDualMode();
                config.acquire();
                config.conf["devices"][selectedName]["duoDecim"] = duoDecimList.key(duoDecimId);
                config.release(true);
            }
            SmGui::Text("Both tuners share these settings.");
            SmGui::Text("Port below selects tuner 1 input (50 Ohm or Hi-Z).");
            SmGui::Text("Tuner 2 always uses its own port.");
        }
        if (running) { SmGui::EndDisabled(); }

        SmGui::LeftLabel("Antenna");
        SmGui::FillWidth();
        if (SmGui::Combo(CONCAT("##sdrplay_rspduo_ant", name), &rspduo_antennaPort, rspduo_antennaPortsTxt)) {
            if (running) {
                rspDuoSelectAntennaPort(rspduo_antennaPort);
            }
            config.acquire();
            config.conf["devices"][selectedName]["antenna"] = rspduo_antennaPort;
            config.release(true);
        }
        if (SmGui::Checkbox(CONCAT("FM/MW Notch##sdrplay_rspduo_fmmwnotch", name), &rspduo_fmmwNotch)) {
            if (running) {
                channelParams->rspDuoTunerParams.rfNotchEnable = rspduo_fmmwNotch;
                channelParams->rspDuoTunerParams.tuner1AmNotchEnable = rspduo_fmmwNotch;
                sdrplay_api_Update(openDev.dev, openDev.tuner, sdrplay_api_Update_RspDuo_RfNotchControl, sdrplay_api_Update_Ext1_None);
                sdrplay_api_Update(openDev.dev, openDev.tuner, sdrplay_api_Update_RspDuo_Tuner1AmNotchControl, sdrplay_api_Update_Ext1_None);
            }
            config.acquire();
            config.conf["devices"][selectedName]["fmmwnotch"] = rspduo_fmmwNotch;
            config.release(true);
        }
        if (SmGui::Checkbox(CONCAT("DAB Notch##sdrplay_rspduo_dabnotch", name), &rspduo_dabNotch)) {
            if (running) {
                channelParams->rspDuoTunerParams.rfDabNotchEnable = rspduo_dabNotch;
                sdrplay_api_Update(openDev.dev, openDev.tuner, sdrplay_api_Update_RspDuo_RfDabNotchControl, sdrplay_api_Update_Ext1_None);
            }
            config.acquire();
            config.conf["devices"][selectedName]["dabNotch"] = rspduo_dabNotch;
            config.release(true);
        }
        if (SmGui::Checkbox(CONCAT("Bias-T##sdrplay_rspduo_biast", name), &rspduo_biasT)) {
            if (running) {
                channelParams->rspDuoTunerParams.biasTEnable = rspduo_biasT;
                sdrplay_api_Update(openDev.dev, openDev.tuner, sdrplay_api_Update_RspDuo_BiasTControl, sdrplay_api_Update_Ext1_None);
            }
            config.acquire();
            config.conf["devices"][selectedName]["biast"] = rspduo_biasT;
            config.release(true);
        }
    }

    void RSPdxMenu() {
        SmGui::LeftLabel("Antenna");
        SmGui::FillWidth();
        if (SmGui::Combo(CONCAT("##sdrplay_rspdx_ant", name), &rspdx_antennaPort, rspdx_antennaPortsTxt)) {
            if (running) {
                openDevParams->devParams->rspDxParams.antennaSel = rspdx_antennaPorts[rspdx_antennaPort];
                sdrplay_api_Update(openDev.dev, openDev.tuner, sdrplay_api_Update_None, sdrplay_api_Update_RspDx_AntennaControl);
            }
            config.acquire();
            config.conf["devices"][selectedName]["antenna"] = rspdx_antennaPort;
            config.release(true);
        }

        if (SmGui::Checkbox(CONCAT("FM/MW Notch##sdrplay_rspdx_fmmwnotch", name), &rspdx_fmmwNotch)) {
            if (running) {
                openDevParams->devParams->rspDxParams.rfNotchEnable = rspdx_fmmwNotch;
                sdrplay_api_Update(openDev.dev, openDev.tuner, sdrplay_api_Update_None, sdrplay_api_Update_RspDx_RfNotchControl);
            }
            config.acquire();
            config.conf["devices"][selectedName]["fmmwNotch"] = rspdx_fmmwNotch;
            config.release(true);
        }
        if (SmGui::Checkbox(CONCAT("DAB Notch##sdrplay_rspdx_dabnotch", name), &rspdx_dabNotch)) {
            if (running) {
                openDevParams->devParams->rspDxParams.rfDabNotchEnable = rspdx_dabNotch;
                sdrplay_api_Update(openDev.dev, openDev.tuner, sdrplay_api_Update_None, sdrplay_api_Update_RspDx_RfDabNotchControl);
            }
            config.acquire();
            config.conf["devices"][selectedName]["dabNotch"] = rspdx_dabNotch;
            config.release(true);
        }
        if (SmGui::Checkbox(CONCAT("Bias-T##sdrplay_rspdx_biast", name), &rspdx_biasT)) {
            if (running) {
                openDevParams->devParams->rspDxParams.biasTEnable = rspdx_biasT;
                sdrplay_api_Update(openDev.dev, openDev.tuner, sdrplay_api_Update_None, sdrplay_api_Update_RspDx_BiasTControl);
            }
            config.acquire();
            config.conf["devices"][selectedName]["biast"] = rspdx_biasT;
            config.release(true);
        }
    }

    void RSPUnsupportedMenu() {
        SmGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "Device currently unsupported");
    }

    static void streamCB(short* xi, short* xq, sdrplay_api_StreamCbParamsT* params,
                         unsigned int numSamples, unsigned int reset, void* cbContext) {
        SDRPlaySourceModule* _this = (SDRPlaySourceModule*)cbContext;
        // TODO: Optimise using volk and math
        if (!_this->running) { return; }
        for (int i = 0; i < numSamples; i++) {
            int id = _this->bufferIndex++;
            _this->stream.writeBuf[id].re = (float)xi[i] / 32768.0f;
            _this->stream.writeBuf[id].im = (float)xq[i] / 32768.0f;

            if (_this->bufferIndex >= _this->bufferSize) {
                _this->stream.swap(_this->bufferSize);
                _this->bufferIndex = 0;
            }
        }
    }

    // Tuner B, in dual tuner mode. The API delivers both callbacks in lockstep -- measured
    // as identical sample counts, call counts and samples per call -- so the two streams
    // stay aligned without any resynchronisation here.
    static void streamCB2(short* xi, short* xq, sdrplay_api_StreamCbParamsT* params,
                          unsigned int numSamples, unsigned int reset, void* cbContext) {
        SDRPlaySourceModule* _this = (SDRPlaySourceModule*)cbContext;
        if (!_this->running || !_this->dualRunning) { return; }
        for (int i = 0; i < numSamples; i++) {
            int id = _this->bufferIndex2++;
            _this->stream2.writeBuf[id].re = (float)xi[i] / 32768.0f;
            _this->stream2.writeBuf[id].im = (float)xq[i] / 32768.0f;

            if (_this->bufferIndex2 >= _this->bufferSize) {
                _this->stream2.swap(_this->bufferSize);
                _this->bufferIndex2 = 0;
            }
        }
    }

    static void eventCB(sdrplay_api_EventT eventId, sdrplay_api_TunerSelectT tuner,
                        sdrplay_api_EventParamsT* params, void* cbContext) {
        SDRPlaySourceModule* _this = (SDRPlaySourceModule*)cbContext;
    }

    std::string name;
    bool enabled = true;
    dsp::stream<dsp::complex_t> stream;
    dsp::stream<dsp::complex_t> stream2;
    ChannelSet channels;
    OptionList<int, double> duoFsList;
    OptionList<int, int> duoDecimList;
    int duoFsId = 0;
    int duoDecimId = 0;
    bool rspduo_dualTuner = false;
    bool dualRunning = false;
    int bufferIndex2 = 0;
    double sampleRate;
    SourceManager::SourceHandler handler;
    bool running = false;
    double freq;
    bool initOk = false;

    sdrplay_api_CallbackFnsT cbFuncs;

    sdrplay_api_DeviceT openDev;
    sdrplay_api_DeviceParamsT* openDevParams;
    sdrplay_api_RxChannelParamsT* channelParams;

    sdrplay_api_Bw_MHzT bandwidth;
    int bandwidthId = 8; // Auto

    int devId = 0;
    int srId = 0;

    int lnaGain = 9;
    int gain = 59;
    int lnaSteps = 9;

    bool agc = false;
    bool agcParamEdit = false;
    int agcAttack = 500;
    int agcDecay = 500;
    int agcDecayDelay = 200;
    int agcDecayThreshold = 5;
    int agcSetPoint = -30;

    // Temporary values for the edit window
    int _agcAttack = 500;
    int _agcDecay = 500;
    int _agcDecayDelay = 200;
    int _agcDecayThreshold = 5;
    int _agcSetPoint = -30;

    int bufferSize = 0;
    int bufferIndex = 0;

    int ifModeId = 0;

    // RSP1A Options
    bool rsp1a_fmmwNotch = false;
    bool rsp1a_dabNotch = false;
    bool rsp1a_biasT = false;

    // RSP2 Options
    bool rsp2_fmmwNotch = false;
    bool rsp2_biasT = false;
    int rsp2_antennaPort = 0;

    // RSP Duo Options
    bool rspduo_fmmwNotch = false;
    bool rspduo_dabNotch = false;
    bool rspduo_biasT = false;
    int rspduo_antennaPort = 0;

    // RSPdx Options
    bool rspdx_fmmwNotch = false;
    bool rspdx_dabNotch = false;
    bool rspdx_biasT = false;
    int rspdx_antennaPort = 0;

    std::vector<sdrplay_api_DeviceT> devList;
    std::string devListTxt;
    std::vector<std::string> devNameList;
    std::string selectedName;

    OptionList<int, int> samplerates;
    OptionList<int, sdrplay_api_Bw_MHzT> bandwidths;
};

MOD_EXPORT void _INIT_() {
    json def = json({});
    def["devices"] = json({});
    def["device"] = "";
    config.setPath(core::args["root"].s() + "/sdrplay_config.json");
    config.load(def);
    config.enableAutoSave();
}

MOD_EXPORT ModuleManager::Instance* _CREATE_INSTANCE_(std::string name) {
    return new SDRPlaySourceModule(name);
}

MOD_EXPORT void _DELETE_INSTANCE_(ModuleManager::Instance* instance) {
    delete (SDRPlaySourceModule*)instance;
}

MOD_EXPORT void _END_() {
    config.disableAutoSave();
    config.save();
}
