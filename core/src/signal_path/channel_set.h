#pragma once
#include <string>
#include <vector>
#include <dsp/stream.h>
#include <dsp/types.h>

// A set of coherent receive channels offered by a source, for antenna phasing and
// diversity combining. See PHASING_PLAN.md section 2.1.
//
// This is registered alongside a SourceHandler rather than being folded into it:
// every source module allocates its own SourceHandler and hands core a pointer, so
// appending fields to that struct would make core read past the end of structs
// allocated by modules built against the older header. A side registry keyed by
// source name is additive and ABI-safe.
//
// Sources that never register a ChannelSet behave exactly as they always have.
struct ChannelSet {
    // Number of coherent channels. Only sets of 2 or more are of any use; the phaser
    // currently combines a selectable pair, but the contract is defined for N so a
    // four-port receiver does not require re-cutting it.
    int count = 0;

    // One stream per channel, all at the same sample rate and centre frequency. The
    // source writes every one of these while the set is registered -- core always
    // drains them, so a channel nobody has selected still gets consumed rather than
    // backing up and stalling the source's worker.
    std::vector<dsp::stream<dsp::complex_t>*> streams;

    // Human-readable per-channel labels: "HF1"/"HF2", "Tuner A"/"Tuner B", "ANT1"/"ANT2".
    std::vector<std::string> names;

    // True if the relative phase between channels is stable across a stop/start. When
    // false (two separately-locked LOs, say) the channels are still perfectly usable --
    // a constant offset is absorbed into the combining weight -- but a saved weight
    // cannot be restored blindly, so the UI should re-converge rather than trust it.
    bool phaseCoherent = false;

    // True if the hardware guarantees the channels are sample-aligned. When false the
    // combiner's alignment controls have to make up the difference.
    bool sampleAligned = false;
};
