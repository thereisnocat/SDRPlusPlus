#pragma once
#include "channel_set.h"
#include "../dsp/combine/phaser.h"
#include "../dsp/routing/splitter.h"
#include <utils/event.h>
#include <map>
#include <mutex>
#include <vector>

// Owns the DSP between a multi-channel source and the IQ front end:
//
//   source channel 0 --> Splitter --+--> (taps: recorder, ...)
//                                   `--> \
//                                         Phaser --> IQFrontEnd
//   source channel 1 --> Splitter --+--> /
//                                   `--> (taps)
//   source channel n --> Splitter          (drained, no taps bound)
//
// A splitter per channel, owned here rather than by the phaser, so a recording can tap
// the raw channels upstream of the combining without stealing samples from it (see
// PHASING_PLAN.md section 4.1). Channels the phaser is not currently combining still get
// a splitter: with nothing bound it acts as a drain, which is exactly what is needed --
// a source writes every channel it registered, and an unread stream would back up and
// stall its worker.
//
// Note on "bypassed by default". The plan originally described phasing as something that
// could be switched out of the signal path entirely. That does not work: with a
// ChannelSet registered, the source is writing to the channel streams, so something must
// consume them. Bypass is therefore expressed as Phaser::MODE_A_ONLY, in which the output
// is a straight copy of channel A -- bit-identical to the source having emitted that
// channel directly, which is what it used to do.
class Phasing {
public:
    void init();
    ~Phasing();

    // Attach to a source's channels, or pass NULL to detach. Rebuilds the DSP graph, so
    // call it while the source is stopped.
    void setChannelSet(ChannelSet* set);

    // True when a usable (2 or more channel) set is attached and the graph is live.
    bool isActive();

    // The combined output, to be handed to the IQ front end. Only meaningful while active.
    dsp::stream<dsp::complex_t>* getOutput();

    int getChannelCount();
    std::string getChannelName(int channel);
    bool isPhaseCoherent();
    bool isSampleAligned();

    // Tap a raw channel, upstream of the combining. Mirrors IQFrontEnd::bindIQStream.
    // Bindings survive a rebuild of the graph, so a recording started before the source
    // restarts keeps receiving.
    void bindChannelStream(int channel, dsp::stream<dsp::complex_t>* stream);
    void unbindChannelStream(int channel, dsp::stream<dsp::complex_t>* stream);

    // Which two channels the phaser combines. Defaults to 0 and 1.
    void setChannelPair(int a, int b);
    void getChannelPair(int& a, int& b);

    void setMode(dsp::combine::Phaser::Mode mode);
    dsp::combine::Phaser::Mode getMode();
    void setWeight(float gainDb, float phaseDeg);
    void getWeight(float& gainDb, float& phaseDeg);
    void setDelay(float samples);
    float getDelay();
    void setAdaptRate(float rate);
    float getAdaptRate();
    void setSampleRate(double sampleRate);
    void setWideband(bool enabled, int taps);
    void getWideband(bool& enabled, int& taps);
    bool isWidebandActive();
    bool isWidebandDecorrelating();
    float getWidebandCoherence();
    void setReferenceBand(bool enabled, double offsetHz, double widthHz);
    void getReferenceBand(bool& enabled, double& offsetHz, double& widthHz);

    // Decorrelation: separating the dominant arrival from everything else.
    void captureNoise(double seconds = 1.0);
    bool isCapturingNoise();
    bool hasNoiseReference();
    void clearNoiseReference();
    void setWhiteningEnabled(bool enabled);
    bool getWhiteningEnabled();
    float getCoherence();
    float getComponentSeparation();
    void getCombineCoefficients(dsp::complex_t& k0, dsp::complex_t& k1);

    dsp::combine::Phaser::Metrics getMetrics();
    float getNullDepth();
    bool isNullDepthBandLimited();
    uint64_t getDiscardCount();

    // Fired whenever the attached channel set changes, including detaching. The phasing
    // UI listens for this to show or hide itself.
    Event<bool> onChannelSetChanged;

private:
    void teardown();
    void build();
    void applyTaps();

    std::recursive_mutex mtx;

    ChannelSet* channels = NULL;
    std::vector<dsp::routing::Splitter<dsp::complex_t>*> splitters;

    // Held for the lifetime of the object rather than recreated per graph, so the phaser
    // can be initialised once and never needs its inputs swapped underneath it.
    dsp::stream<dsp::complex_t> feedA;
    dsp::stream<dsp::complex_t> feedB;
    dsp::combine::Phaser phaser;

    std::map<int, std::vector<dsp::stream<dsp::complex_t>*>> taps;

    int chA = 0;
    int chB = 1;
    bool built = false;
    bool _init = false;
};
