#pragma once
#include <string>
#include <vector>
#include <dsp/filter/parametric_eq.h>

// The interface EqualizerWindow edits through, implemented by RadioModule. Exists purely to
// break a circular include: equalizer_window.h has to be included (like carrier_zoom_window.h
// right next to it) before the RadioModule class itself is defined, but the window still needs
// to read/write live band settings and manage saved profiles that actually live on whichever
// RadioModule instance owns the DSP block and the config file. A plain interface class -- no
// virtual dispatch cost worth worrying about here, this is UI-rate code (a handful of calls
// per frame while the window happens to be open), not the audio hot path.
//
// Unlike CarrierZoomWindow (which owns a fully self-contained CarrierZoomView, no callback
// back into RadioModule at all), the equalizer's live state and its saved profiles are owned
// by RadioModule -- the main panel's own "Equalizer" checkbox/profile combo and this window's
// per-band sliders both need to operate on the *same* live data, and profiles persist through
// RadioModule's own already-established config mechanism (same ConfigManager instance
// tubeWarmth/highPass/etc. already use). This interface is that shared access point.
class EqualizerHost {
public:
    virtual ~EqualizerHost() {}

    virtual bool isEqEnabled() = 0;
    virtual void setEqEnabled(bool enabled) = 0;

    virtual int getEqBandCount() = 0;
    virtual void getEqBand(int index, double& freqHz, float& gainDb, double& q) = 0;
    virtual void setEqBand(int index, double freqHz, float gainDb, double q) = 0;

    // Profiles: a named library of full (all-band) settings, shared across every RadioModule
    // instance (they live under a fixed top-level config key, not this instance's own name --
    // see radio_module.h's own comment on why), since the whole point is recalling a saved
    // curve "at a future date" regardless of which receiver instance is active then.
    virtual std::vector<std::string> getEqProfileNames() = 0;
    virtual void saveEqProfile(const std::string& profileName) = 0;
    virtual void loadEqProfile(const std::string& profileName) = 0;
    virtual void deleteEqProfile(const std::string& profileName) = 0;
    // The name of whichever profile was most recently loaded (or saved, which also counts as
    // "now matching that name") -- a plain last-action label, not a live drift check against
    // whatever the bands currently read. Empty once bands are hand-edited after a load, since
    // at that point the live state and the profile it came from have (probably) diverged.
    virtual std::string getLastLoadedEqProfile() = 0;
};
