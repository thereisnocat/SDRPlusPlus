# SCA subcarriers and the multiplex view (wideband FM)

Added 2026-10-16, ported from the Cheetah receiver where it was worked out against real recordings.

SDR++ already had RDS (the radio module's `rds_demod.h` and `rds.cpp`): it translates the composite by a fixed 57 kHz, not by the stereo pilot, so it
also works on a station that sends RDS without a pilot (WNYC 93.9). What was missing is the other things above 57 kHz: SCA subcarriers, narrow-band FM
audio services (radio reading, foreign-language programmes, background music) usually at 67 kHz or 92 kHz, sometimes 41 or 76 kHz.

* `core/src/dsp/demod/sca.h`: `SCADemod` (translate the composite by -fc, 12 kHz low-pass, quadrature demodulator scaled so +/-5 kHz reads +/-1.0, 7.5 kHz audio
  low-pass, DC removal) and `MultiplexSpectrum` (the composite from 0 to 100 kHz as 500 dB points, only computed while its plot is shown).
* `BroadcastFM::setSCA(hz)` (0 = off): the output becomes that subcarrier's audio on both channels; RDS keeps decoding from the same composite.
* The WFM menu: "SCA subcarrier" with 67/92/41/76 kHz buttons, a frequency box, a gain slider, and a warning when the bandwidth is too narrow to hold the subcarrier
  (a subcarrier at f needs a channel of at least 2 x (f + 13 kHz): 160 kHz for 67 kHz, 210 kHz for 92 kHz; the IF is 250 kHz so widen the VFO bandwidth). "Show
  multiplex" draws the composite spectrum; click a hump to listen to it.
* `core/test/test_sca.cpp` (in run_tests.sh): synthetic station with a 67 kHz SCA carrying a 1 kHz tone comes out at exactly 0.5 with none of the programme in it;
  92 kHz likewise; a subcarrier that is not there gives nothing; the multiplex plot shows the pilot and the subcarrier.
* Checked on real data: 256 kS/s cuts of a 24.576 MS/s recording of the New York FM band run through this demodulator give speech on 93.1, 96.3 and 101.9 MHz at
  67 kHz and flat noise on 88.3 MHz, the same as Cheetah's own decoder found.

Not done: companded (dbx) SCA audio, digital subcarriers (DARC at 76 kHz, data), a per-subcarrier level meter.
