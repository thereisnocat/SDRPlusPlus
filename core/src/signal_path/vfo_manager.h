#pragma once
#include "../dsp/channel/rx_vfo.h"
#include "../dsp/routing/splitter.h"
#include <gui/widgets/waterfall.h>
#include <utils/event.h>

namespace dsp { namespace combine { class Phaser; } }

class VFOManager {
public:
    VFOManager();

    class VFO {
    public:
        VFO(std::string name, int reference, double offset, double bandwidth, double sampleRate, double minBandwidth, double maxBandwidth, bool bandwidthLocked);
        ~VFO();

        void setOffset(double offset);
        double getOffset();
        void setCenterOffset(double offset);
        void setBandwidth(double bandwidth, bool updateWaterfall = true);
        // Independent low/high passband edges (Hz, offsets from the tuned center), trimmed
        // within the current bandwidth window -- see dsp::channel::RxVFO::setPassband(). No
        // ImGui::WaterfallVFO counterpart: the main waterfall's VFO box stays exactly what it
        // is today (a symmetric outer window); asymmetric trim is edited by the Radio module's
        // own spectrum preview widget, not on the main waterfall.
        void setPassband(double lo, double hi);
        double getPassbandLo();
        double getPassbandHi();
        void setSampleRate(double sampleRate, double bandwidth);
        void setReference(int ref);
        void setSnapInterval(double interval);
        void setBandwidthLimits(double minBandwidth, double maxBandwidth, bool bandwidthLocked);
        bool getBandwidthChanged(bool erase = true);
        double getBandwidth();
        int getReference();
        void setColor(ImU32 color);
        std::string getName();

        // Per-VFO decorrelation, when the source offers a second coherent channel
        // (PHASING_PLAN.md 2.6e). attach/detachSecondChannel are driven by VFOManager on
        // channel-set changes; the phaser (`decorr`) sits after both channelizers and its
        // output feeds a stable relay, so `output` never changes identity under a
        // consumer. In MODE_A_ONLY it is a bit-identical passthrough of channel A.
        void attachSecondChannel();
        void detachSecondChannel();
        bool hasSecondChannel() { return decorr != NULL; }
        dsp::combine::Phaser* decorrelator() { return decorr; }

        dsp::stream<dsp::complex_t>* output;

        friend class VFOManager;

        dsp::channel::RxVFO* dspVFO;
        dsp::channel::RxVFO* dspVFOb = NULL;
        dsp::combine::Phaser* decorr = NULL;
        ImGui::WaterfallVFO* wtfVFO;

    private:
        std::string name;
        double _bandwidth;
        double _sampleRate;
        double _offset;
        // Stable output: consumers read `relayOut` for the life of the VFO; `relay`'s
        // input is retargeted (channel-A channelizer <-> phaser output) as the second
        // channel attaches and detaches.
        dsp::stream<dsp::complex_t> relayOut;
        dsp::routing::Splitter<dsp::complex_t> relay;

    };

    VFOManager::VFO* createVFO(std::string name, int reference, double offset, double bandwidth, double sampleRate, double minBandwidth, double maxBandwidth, bool bandwidthLocked);
    void deleteVFO(VFOManager::VFO* vfo);

    void setOffset(std::string name, double offset);
    double getOffset(std::string name);
    void setCenterOffset(std::string name, double offset);
    void setBandwidth(std::string name, double bandwidth, bool updateWaterfall = true);
    void setSampleRate(std::string name, double sampleRate, double bandwidth);
    void setReference(std::string name, int ref);
    void setBandwidthLimits(std::string name, double minBandwidth, double maxBandwidth, bool bandwidthLocked);
    bool getBandwidthChanged(std::string name, bool erase = true);
    double getBandwidth(std::string name);
    void setColor(std::string name, ImU32 color);
    std::string getName();
    int getReference(std::string name);
    bool vfoExists(std::string name);

    // Attach or detach the second coherent channel on every VFO to match the currently
    // selected source. Called from source.cpp when the channel set changes.
    void refreshSecondChannels();

    void updateFromWaterfall(ImGui::WaterFall* wtf);

    Event<VFOManager::VFO*> onVfoCreated;
    Event<VFOManager::VFO*> onVfoDelete;
    Event<std::string> onVfoDeleted;

private:
    std::map<std::string, VFO*> vfos;
};