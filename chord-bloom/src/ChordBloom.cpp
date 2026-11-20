// ChordBloom.cpp - a one-finger chord instrument for the MPC.
//
// SPDX-License-Identifier: MIT
//
// Idea: you hold a chord TYPE (with the left hand on "chord keys", or picked on the touch
// screen) and play ROOT notes with the right hand. Every root note plays a whole chord,
// voiced and voice-led, either as a block chord, strummed, or arpeggiated, through a
// built-in polyphonic synth. The MPC drops a plugin's VST MIDI output, so "MIDI Out" sends the
// chords through an ALSA sequencer port of its own instead (see AlsaMidiOut).
//
// Signal / control flow:
//
//   MIDI in --+-- notes inside the CHORD ZONE  --> chord-key state (triad + extensions)
//             +-- every other note (a ROOT)    --> chord builder
//                                                   (Manual quality, or In Key = diatonic)
//                                                   --> voicing (inversion, drop 2/open/wide,
//                                                       auto voice leading, bass note, octave)
//                                                   --> performer (block / strum / arp)
//                                                   --> 16-voice synth (Analog: 2 PolyBLEP osc,
//                                                       or Keys: 2-op FM) -> SVF low-pass -> ADSR
//   voices (panned across the stereo field) -> stereo chorus -> volume -> soft clip -> out
//
// Chord keys (8 of them, starting at "Zone Start"):
//   1 Dim   2 Min   3 Maj   4 Sus        <- triad quality (hold several: last wins order Dim<Min<Maj<Sus)
//   5 6     6 m7    7 maj7  8 9          <- extensions (combine freely: Min + m7 + 9 = m9)
//   "White Keys" layout puts them on the 8 white keys C..C (keyboard), "Chromatic" on 8 adjacent
//   notes (pads). Held chord keys override the touch-screen chord; with Key Latch on they stick.

#include "mpc_plugin.h"
#include "dsp/math.h"
#include "dsp/oscillator.h"
#include "dsp/svf.h"
#include "dsp/adsr.h"
#include "dsp/smoother.h"
#include "dsp/tempo.h"
#include "dsp/delay_line.h"
#include <algorithm>
#include <atomic>
#include <dlfcn.h>
#include <cstring>
#include <cstdio>
#include <thread>

using namespace mpc::dsp;

// ---------------------------------------------------------------------------------------------
// MIDI out. MPC OS accepts a plugin's VST MIDI output and throws it away, but it does pick up a new
// ALSA sequencer port at once and lists it as a MIDI input (Preferences > MIDI: turn on "Track" for
// it). So Chord Bloom opens its own port, "Chord Bloom", and sends the notes it plays there.
// libasound ships with MPC OS; it is loaded with dlopen so the build needs no ALSA headers, and if it
// is missing (the desktop simulator) MIDI out silently does nothing. Technique from the Open MPC
// project's notes (github.com/open-mpc/mpc-vst-plugins, MIT), verified there on MPC OS 3.10.1.
struct AlsaMidiOut
{
    struct SeqEvent   // snd_seq_event_t (28 bytes)
    {
        unsigned char type, flags, tag, queue;
        unsigned int time[2];
        unsigned char srcClient, srcPort, dstClient, dstPort;
        unsigned char channel, note, velocity, offVelocity;
        unsigned int duration, pad;
    };
    typedef int (*OpenFn)(void**, const char*, int, int);
    typedef int (*NameFn)(void*, const char*);
    typedef int (*PortFn)(void*, const char*, unsigned int, unsigned int);
    typedef int (*SendFn)(void*, SeqEvent*);
    typedef int (*CloseFn)(void*);

    void* lib = nullptr; void* seq = nullptr; int port = -1;
    SendFn send = nullptr; CloseFn close = nullptr;

    void open(const char* name)
    {
        if (seq || lib) return;
        lib = dlopen("libasound.so.2", RTLD_NOW | RTLD_LOCAL);
        if (!lib) return;
        OpenFn o = (OpenFn)dlsym(lib, "snd_seq_open");
        NameFn n = (NameFn)dlsym(lib, "snd_seq_set_client_name");
        PortFn p = (PortFn)dlsym(lib, "snd_seq_create_simple_port");
        send = (SendFn)dlsym(lib, "snd_seq_event_output_direct");
        close = (CloseFn)dlsym(lib, "snd_seq_close");
        if (!o || !n || !p || !send || !close) return;
        if (o(&seq, "default", 1 /*OUTPUT*/, 1 /*NONBLOCK*/) < 0) { seq = nullptr; return; }
        n(seq, name);
        port = p(seq, "MIDI Out", (1u << 0) | (1u << 5) /*READ|SUBS_READ*/, (1u << 1) | (1u << 20) /*MIDI_GENERIC|APPLICATION*/);
        if (port < 0) { close(seq); seq = nullptr; }
    }
    bool ok() const { return seq != nullptr; }
    void note(bool on, int ch, int note, int vel)
    {
        if (!seq) return;
        SeqEvent e; std::memset(&e, 0, sizeof e);
        e.type = on ? 6 : 7;               // NOTEON / NOTEOFF
        e.queue = 253;                     // direct
        e.srcPort = (unsigned char)port;
        e.dstClient = 254; e.dstPort = 253; // to subscribers
        e.channel = (unsigned char)ch; e.note = (unsigned char)note; e.velocity = (unsigned char)vel;
        send(seq, &e);
    }
    ~AlsaMidiOut() { if (seq && close) close(seq); if (lib) dlclose(lib); }
};
static_assert(sizeof(AlsaMidiOut::SeqEvent) == 28, "snd_seq_event_t is 28 bytes");

class ChordBloom : public mpc::Plugin
{
public:
    // Parameter order is permanent (saved projects depend on it). Append only.
    enum Params
    {
        kVolume,
        // CHORD
        kMode, kChord, kAdd6, kAddMin7, kAddMaj7, kAdd9, kKey, kScale, kKeyLayout, kZoneStart,
        // VOICING
        kInversion, kVoicing, kVoiceLead, kBass, kOctave,
        // PLAY
        kPlayMode, kStrumTime, kStrumDir, kArpRate, kArpDir, kArpGate, kArpOctaves, kHumanize,
        // SOUND
        kEngine, kWave, kDetune, kTine, kCutoff, kReso, kFiltEnv, kVelSens,
        kAttack, kDecay, kSustain, kRelease,
        // FX
        kChorusMix, kChorusRate, kChorusDepth,
        // added last
        kKeyLatch, kPadLearn,
        // performance FX (made for the Q-Link knobs)
        kFxFilter, kDelayMix, kDelayTime, kDelayFb, kReverbMix, kReverbSize, kReverbTone, kDrive,
        // the bee (drag it on the PERFORM page), outputs, the PADS page
        kBeeFly, kBeeMode, kBeeHold, kOutMode, kMidiOut, kSynthSound, kPadLit,
        kPadName1, kPadName2, kPadName3, kPadName4, kPadName5, kPadName6, kPadName7, kPadName8,
        kTopPads,
        // KEYS page: organ and piano
        kOrganTone, kPercussion, kRotary, kRotorSpeed, kPianoBlend,
        // touch: velocity curve and pad pressure (aftertouch); PADS page pad lights and zone check
        kVelCurve, kAftertouch, kAtTarget, kPadZone,
        kHeld1, kHeld2, kHeld3, kHeld4, kHeld5, kHeld6, kHeld7, kHeld8,
        kNumParams
    };

    enum Triad { TMaj = 0, TMin, TDim, TSus4, TSus2, TAug, TPower, TSingle };
    enum PlayMode { PlayChord = 0, PlayStrum, PlayArp };

    static mpc::PluginInfo makeInfo()
    {
        mpc::PluginInfo i;
        i.name = "Chord Bloom";
        i.vendor = "N8b";
        i.uniqueId = MPC_FOURCC('N', '8', 'C', 'b');   // must match "uid" in plugin.json
        i.version = 1410;
        i.isInstrument = true;
        i.numInputs = 0;
        i.numOutputs = 2;
        return i;
    }

    ChordBloom() : mpc::Plugin(makeInfo())
    {
        using namespace mpc;
        const std::vector<std::string> keys = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
        addParam(floatParam("vol", "Volume", -40.0f, 6.0f, -6.0f, "dB", 1));
        // CHORD
        addParam(choiceParam("mode", "Chord Mode", {"Manual", "In Key"}, 1));
        addParam(choiceParam("chord", "Chord", {"Major", "Minor", "Dim", "Sus4", "Sus2", "Aug", "Power", "Single"}, 0));
        addParam(boolParam("add6", "Add 6", false));
        addParam(boolParam("addm7", "Add m7", false));
        addParam(boolParam("addmaj7", "Add maj7", false));
        addParam(boolParam("add9", "Add 9", false));
        addParam(choiceParam("key", "Key", keys, 0));
        addParam(choiceParam("scale", "Scale", {"Major", "Minor"}, 0));
        addParam(choiceParam("klayout", "Chord Keys", {"Off", "Keys", "Chrom", "Pads"}, 3));
        addParam(intParam("zone", "Zone Start", 0, 112, 36));
        // VOICING
        addParam(intParam("inv", "Inversion", 0, 3, 0));
        addParam(choiceParam("voicing", "Voicing", {"Close", "Drop2", "Open", "Wide"}, 0));
        addParam(boolParam("vlead", "Voice Lead", true));
        addParam(choiceParam("bass", "Bass Note", {"Off", "-1 Oct", "-2 Oct"}, 1));
        addParam(intParam("oct", "Octave", -2, 2, 0));
        // PLAY
        addParam(choiceParam("play", "Play Mode", {"Chord", "Strum", "Arp"}, 0));
        addParam(floatParam("strum", "Strum Time", 0.0f, 300.0f, 45.0f, "ms", 0, 0.6f));
        addParam(choiceParam("strumdir", "Strum Dir", {"Up", "Down", "Alt"}, 0));
        addParam(choiceParam("arprate", "Arp Rate", divisionNames(), kDivisionSixteenth));
        addParam(choiceParam("arpdir", "Arp Dir", {"Up", "Down", "U/D", "Rand"}, 0));
        addParam(floatParam("arpgate", "Arp Gate", 5.0f, 100.0f, 60.0f, "%", 0));
        addParam(intParam("arpoct", "Arp Octaves", 1, 3, 1));
        addParam(floatParam("human", "Humanize", 0.0f, 100.0f, 15.0f, "%", 0));
        // SOUND
        // Old projects stored Keys as the last choice, so Keys stays last.
        addParam(choiceParam("engine", "Engine", {"Synth", "Organ", "Piano", "Keys"}, 0));
        addParam(choiceParam("wave", "Wave", {"Sine", "Tri", "Saw", "Sqr"}, 2));
        addParam(floatParam("detune", "Detune", 0.0f, 50.0f, 9.0f, "ct", 0));
        addParam(floatParam("tine", "Tine", 0.0f, 100.0f, 45.0f, "%", 0));
        addParam(logParam("cutoff", "Brightness", 80.0f, 16000.0f, 2400.0f, "Hz", 0));
        addParam(floatParam("reso", "Resonance", 0.0f, 100.0f, 15.0f, "%", 0));
        addParam(floatParam("fenv", "Filter Env", 0.0f, 100.0f, 35.0f, "%", 0));
        addParam(floatParam("velsens", "Velocity", 0.0f, 100.0f, 100.0f, "%", 0));
        addParam(logParam("atk", "Attack", 1.0f, 4000.0f, 8.0f, "ms", 0));
        addParam(logParam("dec", "Decay", 5.0f, 5000.0f, 600.0f, "ms", 0));
        addParam(floatParam("sus", "Sustain", 0.0f, 100.0f, 70.0f, "%", 0));
        addParam(logParam("rel", "Release", 5.0f, 6000.0f, 700.0f, "ms", 0));
        // FX
        addParam(floatParam("chmix", "Chorus", 0.0f, 100.0f, 35.0f, "%", 0));
        addParam(logParam("chrate", "Chorus Rate", 0.05f, 5.0f, 0.6f, "Hz", 2));
        addParam(floatParam("chdepth", "Chorus Depth", 0.0f, 100.0f, 50.0f, "%", 0));
        // added last
        addParam(boolParam("klatch", "Key Latch", false));
        addParam(boolParam("padlearn", "Pad Learn", false));
        // PERFORM (FX)
        addParam(floatParam("fxfilt", "Filter", -100.0f, 100.0f, 0.0f, "%", 0));          // <0 low-pass sweep, >0 high-pass
        addParam(floatParam("dlymix", "Delay", 0.0f, 100.0f, 0.0f, "%", 0));
        addParam(choiceParam("dlytime", "Delay Time", divisionNames(), 9));              // 1/8D
        addParam(floatParam("dlyfb", "Feedback", 0.0f, 95.0f, 45.0f, "%", 0));
        addParam(floatParam("rvbmix", "Reverb", 0.0f, 100.0f, 20.0f, "%", 0));
        addParam(floatParam("rvbsize", "Room Size", 0.0f, 100.0f, 70.0f, "%", 0));
        addParam(floatParam("rvbtone", "Verb Tone", 0.0f, 100.0f, 55.0f, "%", 0));
        addParam(floatParam("drive", "Drive", 0.0f, 100.0f, 0.0f, "%", 0));
        // Bee: one finger, one big move. Mode picks what flying the bee up does.
        addParam(floatParam("beefly", "Bee Fly", 0.0f, 100.0f, 0.0f, "%", 0));
        addParam(choiceParam("beemode", "Bee Mode", {"Riser", "Dive", "Space", "Wub"}, 0));
        addParam(boolParam("beehold", "Bee Hold", false));
        addParam(choiceParam("outmode", "Output", {"Stereo", "Vocoder L"}, 0));
        {
            std::vector<std::string> ch{"Off"};
            for (int i = 1; i <= 16; ++i) ch.push_back("Ch " + std::to_string(i));
            addParam(choiceParam("midiout", "MIDI Out", ch, 0));
        }
        addParam(boolParam("synth", "Synth Sound", true));
        addParam(choiceParam("padlit", "Pad Lit", {"-", "1", "2", "3", "4", "5", "6", "7", "8"}, 0));
        for (int i = 1; i <= 8; ++i)
        {
            mpc::Param pn = intParam(("padname" + std::to_string(i)).c_str(), ("Pad Chord " + std::to_string(i)).c_str(), 0, 1, 0);
            pn.automatable = false;
            addParam(pn);
        }
        // Top Pads = Notes: A09-A16 play single notes; hold any A01-A08 pad to turn them into chords.
        addParam(choiceParam("toppads", "Top Pads", {"Chords", "Notes"}, 0));
        // Organ: drawbar registration, percussion, rotary speaker (the rotary works on every engine).
        addParam(choiceParam("orgtone", "Organ Tone", {"Jazz", "Soul", "Full", "Soft"}, 1));
        addParam(boolParam("perc", "Percussion", true));
        addParam(boolParam("rotary", "Rotary", false));
        addParam(floatParam("rotspeed", "Rotor Speed", 0.0f, 100.0f, 25.0f, "%", 0));
        // Piano: -100 airy, 0 plain piano, +100 strings.
        addParam(floatParam("pblend", "Piano Blend", -100.0f, 100.0f, 0.0f, "%", 0));
        // Touch. Velocity (above) = how much pad strength matters; the curve = how it feels.
        addParam(choiceParam("velcurve", "Vel Curve", {"Soft", "Med", "Hard"}, 1));
        addParam(floatParam("atamt", "Aftertouch", 0.0f, 100.0f, 60.0f, "%", 0));
        addParam(choiceParam("attarget", "AT Target", {"Bright", "Swell", "Vibrato", "Rotor", "Bee"}, 0));
        {
            mpc::Param z = intParam("padzone", "Pad Zone", 0, 1, 0);
            z.automatable = false;
            addParam(z);
            for (int i = 1; i <= 8; ++i)
            {
                mpc::Param h = boolParam(("held" + std::to_string(i)).c_str(), ("Pad Held " + std::to_string(i)).c_str(), false);
                h.automatable = false;
                addParam(h);
            }
        }

        if (numParams() != kNumParams) mpc::log("ChordBloom: parameter count mismatch (%d vs %d)", numParams(), (int)kNumParams);

        // Pad Learn's result is global: kept in a small file in the plugin folder and used by every
        // preset and every new instance, so it never has to be learned again.
        savedZone_ = readSavedZone();
        if (savedZone_ >= 0) set(kZoneStart, (float)savedZone_);
        {
            int m[16];
            if (savedZone_ >= 0 && readSavedMap(m)) setPadMap(m);
            else chromaticMap(savedZone_ >= 0 ? savedZone_ : 36);
        }
        auto program = [this](const char* name, std::initializer_list<std::pair<int, float>> overrides)
        {
            std::vector<float> v((size_t)numParams());
            for (int i = 0; i < numParams(); ++i) v[(size_t)i] = param(i).def;
            if (savedZone_ >= 0) v[(size_t)kZoneStart] = (float)savedZone_;
            for (const auto& o : overrides) v[(size_t)o.first] = o.second;
            addProgram(name, std::move(v));
        };
        program("Bloom Pad", {});
        program("Strum Harp", {{kPlayMode, PlayStrum}, {kStrumTime, 70}, {kWave, 1}, {kDetune, 5}, {kCutoff, 4000},
                               {kFiltEnv, 50}, {kAttack, 2}, {kDecay, 1400}, {kSustain, 0}, {kRelease, 1200},
                               {kVoicing, 2}, {kChorusMix, 25}, {kReverbMix, 30}});
        program("Neo Soul Keys", {{kEngine, 3}, {kTine, 55}, {kCutoff, 5000}, {kFiltEnv, 15}, {kAttack, 2},
                                  {kDecay, 2200}, {kSustain, 25}, {kRelease, 450}, {kAddMin7, 1}, {kChord, TMin},
                                  {kVoicing, 1}, {kPlayMode, PlayStrum}, {kStrumTime, 18}, {kChorusMix, 45}});
        program("In-Key Arp", {{kMode, 1}, {kPlayMode, PlayArp}, {kArpRate, kDivisionSixteenth}, {kArpDir, 2},
                               {kArpOctaves, 2}, {kArpGate, 50}, {kDelayMix, 22}, {kWave, 3}, {kCutoff, 1800}, {kFiltEnv, 60},
                               {kAttack, 1}, {kDecay, 250}, {kSustain, 20}, {kRelease, 200}, {kAddMin7, 1}});
        program("Glass Keys", {{kEngine, 3}, {kTine, 85}, {kCutoff, 9000}, {kFiltEnv, 0}, {kAttack, 1},
                               {kDecay, 3000}, {kSustain, 0}, {kRelease, 1500}, {kAddMaj7, 1}, {kAdd9, 1},
                               {kVoicing, 3}, {kChorusMix, 50}, {kChorusDepth, 70}, {kDelayMix, 25}, {kReverbMix, 35}});
        program("Gospel Organ", {{kEngine, 1}, {kOrganTone, 1}, {kPercussion, 1}, {kRotary, 1}, {kRotorSpeed, 20},
                                 {kAddMin7, 1}, {kVoicing, 1}, {kBass, 1}, {kChorusMix, 0}, {kReverbMix, 22}});
        program("Rock Organ", {{kEngine, 1}, {kOrganTone, 2}, {kPercussion, 0}, {kRotary, 1}, {kRotorSpeed, 90},
                               {kDrive, 45}, {kChord, TPower}, {kChorusMix, 0}, {kReverbMix, 15}});
        program("Grand Piano", {{kEngine, 2}, {kPianoBlend, 0}, {kCutoff, 9000}, {kRelease, 500}, {kVoicing, 1},
                                {kChorusMix, 0}, {kReverbMix, 25}, {kReverbSize, 75}});
        program("Airy Piano", {{kEngine, 2}, {kPianoBlend, -70}, {kCutoff, 9000}, {kRelease, 1600}, {kAddMaj7, 1}, {kAdd9, 1},
                               {kVoicing, 2}, {kChorusMix, 30}, {kDelayMix, 18}, {kReverbMix, 40}, {kReverbSize, 85}});
        program("String Piano", {{kEngine, 2}, {kPianoBlend, 65}, {kCutoff, 7000}, {kRelease, 1800}, {kVoicing, 2},
                                 {kBass, 1}, {kChorusMix, 35}, {kReverbMix, 35}, {kReverbSize, 80}});
        program("Dark Strings", {{kWave, 2}, {kDetune, 18}, {kCutoff, 900}, {kReso, 10}, {kFiltEnv, 20},
                                 {kAttack, 900}, {kDecay, 2000}, {kSustain, 90}, {kRelease, 2500},
                                 {kChord, TMin}, {kVoicing, 2}, {kBass, 2}, {kChorusMix, 60}, {kReverbMix, 40}, {kReverbSize, 85}});
    }

    bool formatValue(int index, float plain, std::string& out) override
    {
        if (index >= kPadName1 && index <= kPadName8)
        {
            out = padChordName(index - kPadName1);
            return true;
        }
        if (index == kPadZone)
        {
            out = zoneText();
            return true;
        }
        if (index == kPianoBlend)
        {
            const int v = (int)std::lround(plain);
            out = std::abs(v) < 3 ? std::string("Piano") : std::string(v < 0 ? "Airy " : "Strings ") + std::to_string(std::abs(v)) + "%";
            return true;
        }
        if (index == kFxFilter)
        {
            const int v = (int)std::lround(plain);
            char buf[16];
            if (std::abs(v) < 2) std::snprintf(buf, sizeof(buf), "Off");
            else std::snprintf(buf, sizeof(buf), "%s %d%%", v < 0 ? "LP" : "HP", std::abs(v));
            out = buf;
            return true;
        }
        // The MPC shows value text without the unit label, so add units here ("2400 Hz", "45 ms").
        const mpc::Param& pr = param(index);
        if (!pr.unit.empty() && pr.kind != mpc::Param::Kind::Choice && pr.kind != mpc::Param::Kind::Bool)
        {
            out = pr.display(plain) + (pr.unit == "%" ? "" : " ") + pr.unit;
            return true;
        }
        if (index == kZoneStart)
        {
            out = noteName((int)std::lround(plain));
            return true;
        }
        return false;
    }

    // ----- lifecycle --------------------------------------------------------
    void prepare(double sampleRate, int /*maxBlockSize*/) override
    {
        sr_ = (float)sampleRate;
        for (auto& v : voices_)
        {
            v.osc1.setSampleRate(sampleRate); v.osc2.setSampleRate(sampleRate);
            v.car.setSampleRate(sampleRate); v.mod.setSampleRate(sampleRate);
            v.filter.setSampleRate(sampleRate);
            v.amp.setSampleRate(sampleRate); v.fenv.setSampleRate(sampleRate);
        }
        const int maxDelay = (int)(0.040 * sampleRate) + 4;
        chorusL_.prepare(maxDelay);
        chorusR_.prepare(maxDelay);
        delayL_.prepare((int)(2.5 * sampleRate) + 4);
        delayR_.prepare((int)(2.5 * sampleRate) + 4);
        verb_.prepare(sampleRate);
        for (auto& f : djFilter_) f.setSampleRate(sampleRate);
        filterKnob_.setTime(20.0f, sampleRate);
        delayTime_.setTime(60.0f, sampleRate);
        delayMix_.setTime(20.0f, sampleRate);
        verbMix_.setTime(20.0f, sampleRate);
        drive_.setTime(20.0f, sampleRate);
        volume_.setTime(15.0f, sampleRate);
        bend_.setTime(8.0f, sampleRate);
        leslieDelay_.prepare((int)(0.010 * sampleRate) + 4);
        leslieWet_.setTime(60.0f, sampleRate);
        for (auto& v : voices_) v.air.setSampleRate(sampleRate);
        midiOut_.open("Chord Bloom");   // UI thread; harmless if MIDI Out stays Off
        reset();
    }

    void reset() override
    {
        for (auto& v : voices_) { v.amp.reset(); v.fenv.reset(); v.filter.reset(); v.active = false; v.releaseAt = -1; }
        chorusL_.reset(); chorusR_.reset();
        chorusPhase_ = 0.0f;
        beeLand_.store(false); tapRequest_.store(-1);   // a loaded state or preset is not a gesture
        leslieDelay_.reset(); hornPhase_ = drumPhase_ = 0.0f; leslieLp_ = 0.0f;
        leslieWet_.snap(getBool(kRotary) ? 1.0f : 0.0f);
        hornHz_ = drumHz_ = 0.0f;
        delayL_.reset(); delayR_.reset(); verb_.reset();
        for (auto& f : djFilter_) f.reset();
        fbLpL_ = fbLpR_ = 0.0f; fxCtrl_ = 0;
        filterKnob_.snap(get(kFxFilter) * 0.01f);
        delayMix_.snap(get(kDelayMix) * 0.01f);
        verbMix_.snap(get(kReverbMix) * 0.01f);
        drive_.snap(get(kDrive) * 0.01f);
        delayTime_.snap(delaySamples());
        numHeld_ = 0; chordKeysHeld_ = 0; sustainPedal_ = false; chordHeld_ = false;
        numPending_ = 0; arpActive_ = false; numPrev_ = 0; numChord_ = 0; hasBass_ = false;
        volume_.snap(dbToGain(get(kVolume)));
        bend_.snap(0.0f);
        allMidiOff();
        // A preset change or project load must not undo Pad Learn (or leave it armed).
        if (savedZone_ >= 0 && getInt(kZoneStart) != savedZone_) { set(kZoneStart, (float)savedZone_); notifyHostParameterChanged(kZoneStart); }
        if (getBool(kPadLearn)) { set(kPadLearn, 0.0f); notifyHostParameterChanged(kPadLearn); }
        for (auto& e : echo_) e = Echo{};
        clock_ = 0;
    }

    static std::string zoneFile()
    {
        const std::string& dir = mpc::pluginFolder();
        return dir.empty() ? std::string() : dir + "/pad-zone.txt";
    }
    static int readSavedZone()
    {
        const std::string path = zoneFile();
        if (path.empty()) return -1;
        std::FILE* f = std::fopen(path.c_str(), "r");
        if (!f) return -1;
        int z = -1;
        if (std::fscanf(f, "%d", &z) != 1 || z < 0 || z > 112) z = -1;
        std::fclose(f);
        return z;
    }
    // Second line (since 1.4.1): the note of every pad A01..A16, as learned.
    static bool readSavedMap(int* m)
    {
        const std::string path = zoneFile();
        if (path.empty()) return false;
        std::FILE* f = std::fopen(path.c_str(), "r");
        if (!f) return false;
        int z = 0, n = 0;
        if (std::fscanf(f, "%d", &z) == 1)
            while (n < 16 && std::fscanf(f, "%d", &m[n]) == 1 && m[n] >= 0 && m[n] < 128) ++n;
        std::fclose(f);
        return n == 16;
    }
    // Written off the audio thread (a tiny one-shot thread), once per Pad Learn.
    static void saveZone(int zone, const int* map = nullptr)
    {
        const std::string path = zoneFile();
        if (path.empty()) return;
        std::string text = std::to_string(zone) + "\n";
        if (map) { for (int i = 0; i < 16; ++i) text += std::to_string(map[i]) + (i < 15 ? " " : "\n"); }
        std::thread([path, text]() {
            if (std::FILE* f = std::fopen(path.c_str(), "w")) { std::fputs(text.c_str(), f); std::fclose(f); }
        }).detach();
    }

    // Pad map: which note each pad A01..A16 sends. The MPC's own pad layouts are not always in
    // note order (its classic drum map goes 37, 36, 42, 82...), so Pad Learn records every pad.
    void setPadMap(const int* m)
    {
        for (int i = 0; i < 128; ++i) noteToPad_[i] = -1;
        for (int i = 0; i < 16; ++i) { padMap_[i] = m[i]; if (noteToPad_[m[i] & 127] < 0) noteToPad_[m[i] & 127] = (signed char)i; }
        mapZone_ = m[0];
    }
    void chromaticMap(int zone)
    {
        int m[16];
        for (int i = 0; i < 16; ++i) m[i] = clamp(zone + i, 0, 127);
        setPadMap(m);
    }
    int padIndex(int note) const { return (note >= 0 && note < 128) ? noteToPad_[note] : -1; }

    // Called from the host's own setParameter. Never call back into the host from here:
    // just note what happened and deal with it at the start of the next process().
    void parameterChanged(int index, float plain) override
    {
        if (index == kPadLit)
        {
            const int v = (int)std::lround(plain);
            if (v != padLitPushed_.load()) tapRequest_.store(v);   // a touch, not the echo of our own push
        }
        else if (index == kBeeHold && plain < 0.5f) beeLand_.store(true);
    }

    // ----- audio ------------------------------------------------------------
    void process(mpc::ProcessContext& ctx) override
    {
        readSettings(ctx);
        blockStartClock_ = clock_;
        transport_ = ctx.transport;
        controlTick();

        float* L = ctx.outputs[0];
        float* R = ctx.numOutputs > 1 ? ctx.outputs[1] : nullptr;

        int pos = 0, ev = 0;
        while (pos < ctx.numFrames)
        {
            // MIDI events due at (or before) this position.
            while (ev < ctx.numMidiEvents && ctx.midi[ev].sampleOffset <= pos) handleMidi(ctx.midi[ev++]);
            runScheduler(pos);

            int end = pos + kChunk;
            if (end > ctx.numFrames) end = ctx.numFrames;
            if (ev < ctx.numMidiEvents && ctx.midi[ev].sampleOffset < end) end = ctx.midi[ev].sampleOffset;
            if (end <= pos) end = pos + 1;

            renderVoices(L + pos, R ? R + pos : scratchR_, end - pos);
            if (!synthOn_)
                for (int i = pos; i < end; ++i) { L[i] = 0.0f; if (R) R[i] = 0.0f; }
            clock_ += (int64_t)(end - pos);
            pos = end;
        }
        while (ev < ctx.numMidiEvents) handleMidi(ctx.midi[ev++]);

        delayTime_.setTarget(delaySamples());
        // Chorus, performance FX, volume, soft clip.
        const float mix = s_.chorusMix;
        const float baseDelay = 0.007f * sr_;
        const float depth = (0.0005f + 0.0045f * s_.chorusDepth) * sr_;
        const float inc = s_.chorusRate / sr_;
        // Rotary: speed changes take time, like a real cabinet (horn ~0.7 s, drum ~3 s).
        hornTarget_ = 0.8f + 6.0f * rotorSpeed_;
        drumTarget_ = 0.7f + 5.2f * rotorSpeed_;
        hornInertia_ = 1.0f / (0.7f * sr_);
        drumInertia_ = 1.0f / (3.0f * sr_);
        leslieLpC_ = 1.0f - std::exp(-6.2832f * 800.0f / sr_);
        if (hornHz_ <= 0.0f) { hornHz_ = hornTarget_; drumHz_ = drumTarget_; }
        leslieWet_.setTarget(rotaryOn_ ? 1.0f : 0.0f);
        const bool rotaryLive = rotaryOn_ || leslieWet_.value() > 0.0005f;
        for (int i = 0; i < ctx.numFrames; ++i)
        {
            float dryL = L[i];
            float dryR = R ? R[i] : L[i];
            if (rotaryLive) leslie(dryL, dryR);
            float wetL = dryL, wetR = dryR;
            if (mix > 0.001f)
            {
                chorusL_.write(dryL);
                chorusR_.write(dryR);
                const float dl = baseDelay + depth * (0.5f + 0.5f * fastSin01(chorusPhase_));
                const float dr = baseDelay + depth * (0.5f + 0.5f * fastSin01(chorusPhase_ + 0.25f));
                wetL = chorusL_.readLinear(dl);
                wetR = chorusR_.readLinear(dr);
                chorusPhase_ += inc;
                if (chorusPhase_ >= 1.0f) chorusPhase_ -= 1.0f;
            }
            float xL = dryL + mix * (wetR - dryL * 0.3f);   // chorus, cross-fed for width
            float xR = dryR + mix * (wetL - dryR * 0.3f);
            performFx(xL, xR);
            const float g = volume_.process();
            if (vocoderOut_)
            {
                // Vocoder carrier: everything on the left, the right side left free for the mic.
                L[i] = fastTanh((xL + xR) * 0.6f * g);
                if (R) R[i] = 0.0f;
            }
            else
            {
                L[i] = fastTanh(xL * g);
                if (R) R[i] = fastTanh(xR * g);
            }
        }
    }

    // Once per block, before audio: screen taps, the bee landing, the pad light and the pad names.
    void controlTick()
    {
        if (beeLand_.exchange(false))
        {
            set(kBeeFly, 0.0f);
            notifyHostParameterChanged(kBeeFly);   // move the bee back down on screen
        }
        // A tap on a top-row pad of the PADS page plays that chord for one beat.
        const int tap = tapRequest_.exchange(-1);
        if (tap > 0)
        {
            const int root = degreeRoot(s_, tap - 1);
            tapRoot_ = root;
            lastVelocity_ = 100;
            pushHeld(root);
            triggerChord(root, 100);
            tapEndAt_ = clock_ + (int64_t)(60.0f / safeBpm(transport_.bpm) * sr_);
            litPad_ = tap;
        }
        if (tapRoot_ >= 0 && clock_ >= tapEndAt_)
        {
            const bool wasCurrent = numHeld_ > 0 && held_[numHeld_ - 1] == tapRoot_;
            removeHeld(tapRoot_);
            tapRoot_ = -1;
            if (wasCurrent)
            {
                if (numHeld_ > 0) triggerChord(currentRoot(), lastVelocity_);
                else if (!sustainPedal_) releaseChord();
            }
        }
        // Light the screen pad of the chord that is sounding (hardware pads in the 16 Pads layout, or a tap).
        int lit = tapRoot_ >= 0 ? litPad_ : 0;
        if (!lit && s_.layout == 3 && numHeld_ > 0)
        {
            const int top = held_[numHeld_ - 1];
            for (int d = 0; d < 8; ++d) if (degreeRoot(s_, d) == top && heldFromPad_) { lit = d + 1; break; }
        }
        if (lit != padLitPushed_.load())
        {
            padLitPushed_.store(lit);
            set(kPadLit, (float)lit);
            notifyHostParameterChanged(kPadLit);
        }
        // Light the bottom eight pads on screen while they are held on the MPC (A01..A08 in pad order).
        static const int padOrder[8] = {2, 3, 1, 0, 7, 6, 5, 4};
        unsigned heldPads = 0;
        if (s_.layout == 3)
            for (int pad = 0; pad < 8; ++pad) if (chordKeysHeld_ & (1u << padOrder[pad])) heldPads |= 1u << pad;
        if (heldPads != heldPushed_)
        {
            for (int pad = 0; pad < 8; ++pad)
            {
                const bool on = (heldPads >> pad) & 1u;
                if (on == (((heldPushed_ >> pad) & 1u) != 0)) continue;
                set(kHeld1 + pad, on ? 1.0f : 0.0f);
                notifyHostParameterChanged(kHeld1 + pad);
            }
            heldPushed_ = heldPads;
        }
        // Pad names follow Key / Scale / Chord Mode / extensions: ask the MPC to redraw them when they change.
        const int sig = ((s_.zone << 16) ^ (zoneState_ * 131) ^ (s_.layout << 24) ^ (learnStep_ * 7919) ^ ((int)s_.learn << 30)) ^ (s_.mode | (s_.chord << 1) | (s_.key << 5) | (s_.scale << 9) | (s_.add6 << 10) | (s_.addm7 << 11)
                        | (s_.addmaj7 << 12) | (s_.add9 << 13) | (s_.padNotes << 14));
        if (sig != nameSig_)
        {
            nameSig_ = sig;
            requestDisplayUpdate();
        }
    }

private:
    static constexpr int kNumVoices = 16;
    static constexpr int kChunk = 16;          // render + scheduling granularity (~0.36 ms)
    static constexpr int kMaxChord = 8;        // chord notes (bass separate)
    static constexpr int kMaxHeld = 16;        // held root notes (last-note priority)
    static constexpr int kMaxArp = kMaxChord * 3;

    // ---------------------------------------------------------------- settings
    struct Settings
    {
        int mode, chord, key, scale, layout, zone, inversion, voicing, bass, octave;
        int playMode, strumDir, arpDir, arpOct, engine, wave;
        bool add6, addm7, addmaj7, add9, voiceLead, latch, learn, padNotes;
        float strumMs, arpBeats, arpGate, humanize, detune, tine, cutoff, reso, fenvAmt, velSens, chorusMix, chorusRate, chorusDepth;
        float atk, dec, sus, rel;
        float delayFb;
        float velExp = 1.0f;
        int organTone; bool percussion; float pianoBlend;
    } s_{};

    void readSettings(mpc::ProcessContext& ctx)
    {
        (void)ctx;
        s_.mode = getInt(kMode); s_.chord = getInt(kChord); s_.key = getInt(kKey); s_.scale = getInt(kScale);
        s_.layout = getInt(kKeyLayout); s_.zone = getInt(kZoneStart);
        s_.add6 = getBool(kAdd6); s_.addm7 = getBool(kAddMin7); s_.addmaj7 = getBool(kAddMaj7); s_.add9 = getBool(kAdd9);
        s_.inversion = getInt(kInversion); s_.voicing = getInt(kVoicing); s_.voiceLead = getBool(kVoiceLead);
        s_.bass = getInt(kBass); s_.octave = getInt(kOctave);
        s_.playMode = getInt(kPlayMode); s_.strumMs = get(kStrumTime); s_.strumDir = getInt(kStrumDir);
        s_.arpBeats = divisionBeats(getInt(kArpRate)); s_.arpDir = getInt(kArpDir);
        s_.arpGate = get(kArpGate) * 0.01f; s_.arpOct = getInt(kArpOctaves); s_.humanize = get(kHumanize) * 0.01f;
        s_.engine = getInt(kEngine); s_.wave = getInt(kWave); s_.detune = get(kDetune); s_.tine = get(kTine) * 0.01f;
        s_.cutoff = get(kCutoff); s_.reso = get(kReso) * 0.01f * 0.9f; s_.fenvAmt = get(kFiltEnv) * 0.01f;
        s_.velSens = get(kVelSens) * 0.01f;
        s_.atk = get(kAttack); s_.dec = get(kDecay); s_.sus = get(kSustain) * 0.01f; s_.rel = get(kRelease);
        s_.chorusMix = get(kChorusMix) * 0.01f; s_.chorusRate = get(kChorusRate); s_.chorusDepth = get(kChorusDepth) * 0.01f;
        s_.latch = getBool(kKeyLatch);
        s_.padNotes = getInt(kTopPads) == 1;
        s_.organTone = getInt(kOrganTone); s_.percussion = getBool(kPercussion); s_.pianoBlend = get(kPianoBlend) * 0.01f;
        rotaryOn_ = getBool(kRotary); rotorSpeed_ = get(kRotorSpeed) * 0.01f;
        {
            static const float curveExp[3] = {0.55f, 1.0f, 1.8f};
            s_.velExp = curveExp[clamp(getInt(kVelCurve), 0, 2)];
            // Pad pressure: the strongest of channel pressure and per-pad pressure, smoothed (~25 ms).
            int peak = chanPressure_;
            for (int i = 0; i < 128; ++i) if (polyPressure_[i] > peak) peak = polyPressure_[i];
            const float target = (float)peak / 127.0f * get(kAftertouch) * 0.01f;
            const float blockSec = (float)ctx.numFrames / sr_;
            pressure_ += (target - pressure_) * (1.0f - std::exp(-blockSec / 0.025f));
            atTarget_ = getInt(kAtTarget);
            if (atTarget_ == 3) rotorSpeed_ = clamp(rotorSpeed_ + pressure_, 0.0f, 1.0f);
        }
        s_.learn = getBool(kPadLearn);
        if (s_.learn && !lastLearn_) learnStep_ = 0;                     // Pad Learn just armed
        if (!s_.learn && lastLearn_ && learnStep_ > 0) finishLearn(false); // switched off part way
        lastLearn_ = s_.learn;
        if (!s_.learn && s_.zone != mapZone_) { chromaticMap(s_.zone); }  // Zone Start turned by hand
        synthOn_ = getBool(kSynthSound);
        vocoderOut_ = getInt(kOutMode) == 1;
        midiCh_ = getInt(kMidiOut) - 1;
        if (midiCh_ != lastMidiCh_) { allMidiOff(); lastMidiCh_ = midiCh_; }

        // The bee: flying it up pushes the four FX in a direction picked by Bee Mode.
        const float fly = atTarget_ == 4 ? clamp(get(kBeeFly) * 0.01f + pressure_, 0.0f, 1.0f) : get(kBeeFly) * 0.01f;
        float filt = get(kFxFilter) * 0.01f, dly = get(kDelayMix) * 0.01f, vrb = get(kReverbMix) * 0.01f;
        float drv = get(kDrive) * 0.01f, fb = get(kDelayFb) * 0.01f;
        if (fly > 0.001f)
        {
            switch (getInt(kBeeMode))
            {
                case 0:   // Riser: high-pass sweep up, delay and reverb swell
                    filt += 0.8f * fly; dly += 0.55f * fly; vrb += 0.5f * fly; fb += 0.3f * fly; break;
                case 1:   // Dive: low-pass down and drive
                    filt -= 0.85f * fly; drv += 0.7f * fly; break;
                case 2:   // Space: big echoes and reverb
                    dly += 0.8f * fly; vrb += 0.7f * fly; fb += 0.4f * fly; break;
                default:  // Wobble: a quarter-note low-pass wobble that deepens as the bee rises
                {
                    const double beats = transport_.ppqValid ? transport_.ppqPos : (double)clock_ / sr_ * safeBpm(transport_.bpm) / 60.0;
                    const float ph = (float)(beats - std::floor(beats));
                    filt -= fly * 0.75f * (0.5f + 0.5f * std::cos(6.2831853f * ph));
                    break;
                }
            }
        }
        filterKnob_.setTarget(clamp(filt, -1.0f, 1.0f));
        delayMix_.setTarget(clamp(dly, 0.0f, 1.0f));
        verbMix_.setTarget(clamp(vrb, 0.0f, 1.0f));
        drive_.setTarget(clamp(drv, 0.0f, 1.0f));
        s_.delayFb = clamp(fb, 0.0f, 0.95f);
        verb_.set(get(kReverbSize) * 0.01f, get(kReverbTone) * 0.01f);
        volume_.setTarget(dbToGain(get(kVolume)));
        for (auto& v : voices_)
        {
            if (s_.engine == 1) v.amp.set(4.0f, 50.0f, 1.0f, clamp(s_.rel * 0.1f, 25.0f, 400.0f));   // organ: gated, Release shortened
            else if (s_.engine == 2) v.amp.set(1.0f, 50.0f, 1.0f, s_.rel);   // piano: strings decay by themselves
            else v.amp.set(s_.atk, s_.dec, s_.sus, s_.rel);
            v.fenv.set(s_.atk * 0.5f + 1.0f, s_.dec * 0.8f, s_.sus * 0.5f, s_.rel);
            v.osc1.setWave(s_.wave); v.osc2.setWave(s_.wave);
        }
    }

    // Ends Pad Learn. Complete: use every learned pad. Stopped early: notes in order from the first pad.
    void finishLearn(bool complete)
    {
        if (learnStep_ <= 0) return;
        if (complete) setPadMap(learnNotes_); else chromaticMap(learnNotes_[0]);
        const int zone = clamp(padMap_[0], 0, 112);
        set(kZoneStart, (float)zone);
        s_.zone = zone;
        mapZone_ = zone;
        savedZone_ = zone;
        saveZone(zone, complete ? padMap_ : nullptr);
        notifyHostParameterChanged(kZoneStart);
        if (getBool(kPadLearn)) { set(kPadLearn, 0.0f); notifyHostParameterChanged(kPadLearn); }
        s_.learn = false;
        learnStep_ = 0;
        lastPad_ = -1;
    }

    // ---------------------------------------------------------------- zone check (PADS page)
    // Remembers where the last hardware pad landed so the PADS page can say if the pads and the zone disagree.
    void notePadCheck(int note)
    {
        lastPad_ = s_.layout == 3 ? padIndex(note) : -1;
        lastPadNote_ = note;
        zoneState_ = lastPad_ >= 0 ? lastPad_ + 1 : -(note + 1);
    }
    std::string zoneText() const
    {
        if (getBool(kPadLearn)) return "Pad Learn: hit A" + two(learnStep_ + 1) + "  (" + std::to_string(learnStep_ + 1) + " of 16)";
        if (getInt(kKeyLayout) != 3) return "Chord Keys: not 16 Pads";
        static const char* what[16] = {"Maj", "Sus4", "Min", "Dim", "add 9", "add maj7", "add m7", "add 6",
                                       "chord I", "chord II", "chord III", "chord IV", "chord V", "chord VI", "chord VII", "chord I+"};
        if (zoneState_ > 0) return "You hit A" + two(zoneState_) + ": " + what[zoneState_ - 1];
        if (zoneState_ < 0) return "Note " + noteName(-zoneState_ - 1) + " is not a learned pad";
        return "Hit a pad to check it";
    }
    static std::string two(int n) { char b[8]; std::snprintf(b, sizeof(b), "%02d", n); return b; }

    // ---------------------------------------------------------------- MIDI
    // Which chord key (0..7) a note is, or -1 if it is a normal (root) note.
    int chordKeyIndex(int note) const
    {
        if (s_.layout == 0) return -1;
        const int rel = note - s_.zone;
        if (s_.layout == 3)
        {
            // 16 Pads layout, A01..A08: Maj, Sus4, Min, Dim, add 9, add maj7, add m7, add 6
            // (internal bits: 0 Dim, 1 Min, 2 Maj, 3 Sus4, 4 add 6, 5 m7, 6 maj7, 7 add 9)
            static const int padOrder[8] = {2, 3, 1, 0, 7, 6, 5, 4};
            const int pad = padIndex(note);
            (void)rel;
            return (pad >= 0 && pad < 8) ? padOrder[pad] : -1;
        }
        if (s_.layout >= 2) return (rel >= 0 && rel < 8) ? rel : -1;
        static const int white[8] = {0, 2, 4, 5, 7, 9, 11, 12};
        for (int i = 0; i < 8; ++i) if (rel == white[i]) return i;
        // Black keys inside the zone are swallowed too, so a slip never plays a chord.
        return (rel >= 0 && rel <= 12) ? 99 : -1;
    }

    void handleMidi(const mpc::MidiEvent& ev)
    {
        // MPC routes a plugin's own MIDI port back into its track, so with MIDI Out on our notes come
        // back moments later. Swallow exactly the ones we sent (Open MPC notes, Chordsmith).
        if ((ev.isNoteOn() || ev.isNoteOff()) && swallowEcho(ev)) return;
        if (ev.isAftertouch()) { chanPressure_ = ev.data[1] & 127; return; }
        if (ev.status() == 0xA0) { polyPressure_[ev.data[1] & 127] = (unsigned char)(ev.data[2] & 127); return; }
        if (ev.isNoteOff()) polyPressure_[ev.note() & 127] = 0;
        if (ev.isNoteOn() && !s_.learn) notePadCheck(ev.note());
        if (s_.learn && (ev.isNoteOn() || ev.isNoteOff()))
        {
            // Pad Learn: hit A01, A02 ... A16 in order. Silent while learning; saved for good when done.
            if (!ev.isNoteOn()) return;
            const int note = ev.note();
            for (int i = 0; i < learnStep_; ++i) if (learnNotes_[i] == note) return;   // a double hit
            learnNotes_[learnStep_++] = note;
            if (learnStep_ == 16) finishLearn(true);
            return;
        }
        if (ev.isNoteOn())
        {
            const int k = chordKeyIndex(ev.note());
            if (k == 99) return;
            if (k >= 0)
            {
                chordKeysHeld_ |= (1u << k);
                if (k < 4)
                {
                    // Quality pads (A01-A04) re-chord and can latch.
                    if (s_.latch) latched_ = chordKeysHeld_ & 0x0Fu;
                    if (chordHeld_) triggerChord(currentRoot(), lastVelocity_);
                }
                else refreshChord();   // add pads (A05-A08) are momentary: add the note while held
                return;
            }
            const int root = mapRoot(ev.note());
            heldFromPad_ = s_.layout == 3 && padIndex(ev.note()) >= 8;
            pushHeld(root);
            lastVelocity_ = ev.velocity();
            triggerChord(root, ev.velocity());
        }
        else if (ev.isNoteOff())
        {
            const int k = chordKeyIndex(ev.note());
            if (k == 99) return;
            if (k >= 0)
            {
                chordKeysHeld_ &= ~(1u << k);
                if (k >= 4) refreshChord();   // letting go of an add pad takes its note back out
                return;
            }
            const int root = mapRoot(ev.note());
            const bool wasCurrent = numHeld_ > 0 && held_[numHeld_ - 1] == root;
            removeHeld(root);
            if (!wasCurrent) return;
            if (numHeld_ > 0) triggerChord(currentRoot(), lastVelocity_);   // fall back to the previous key
            else if (!sustainPedal_) releaseChord();
        }
        else if (ev.isAllNotesOff())
        {
            numHeld_ = 0; chordKeysHeld_ = 0; chanPressure_ = 0;
            std::memset(polyPressure_, 0, sizeof(polyPressure_));
            releaseChord();
            allMidiOff();
        }
        else if (ev.isController() && ev.controller() == 64)
        {
            sustainPedal_ = ev.controllerValue() >= 64;
            if (!sustainPedal_ && numHeld_ == 0) releaseChord();
        }
        else if (ev.isPitchBend())
        {
            bend_.setTarget(2.0f * (float)ev.pitchBend() / 8192.0f);
        }
    }

    // In the 16 Pads layout the 8 pads above the chord pads play scale degrees 1-8 of Key/Scale
    // (around C3), whatever notes the pads send. Every other note is a root as played.
    int mapRoot(int note) const
    {
        if (s_.layout != 3) return note;
        const int pad = padIndex(note);
        if (pad < 8) return note;
        return degreeRoot(s_, pad - 8);   // C3..F#3, or G2..B2
    }

    // Root of top-row pad `deg` (0..7) in the 16 Pads layout: scale degrees 1-8 around C3.
    static int degreeRoot(const Settings& st, int deg)
    {
        static const int major[8] = {0, 2, 4, 5, 7, 9, 11, 12};
        static const int minor[8] = {0, 2, 3, 5, 7, 8, 10, 12};
        const int* sc = st.scale == 1 ? minor : major;
        const int tonic = 48 + st.key + (st.key >= 7 ? 0 : 12);
        return tonic + sc[clamp(deg, 0, 7)];
    }

    // "Cmaj7", "Dm9", "Bm7b5"... from a root and the chord's intervals.
    static std::string chordName(int root, const int* iv, int n)
    {
        static const char* names[12] = {"C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B"};
        bool has[24] = {false};
        for (int i = 0; i < n; ++i) if (iv[i] >= 0 && iv[i] < 24) has[iv[i]] = true;
        const bool m3 = has[3], M3 = has[4], p5 = has[7], d5 = has[6], a5 = has[8], s4 = has[5] && !m3 && !M3, s2 = has[2] && !m3 && !M3 && !s4;
        const bool b7 = has[10], M7 = has[11], six = has[9], nine = has[14];
        std::string q;
        if (n == 1) q = "";
        else if (n == 2 && p5) q = "5";
        else if (m3 && d5 && !p5)
            q = six ? "dim7" : (b7 ? "m7b5" : "dim");
        else if (M3 && a5 && !p5) q = "aug";
        else
        {
            const std::string base = m3 ? "m" : "";
            if (M7) q = base + (nine ? (m3 ? "Maj9" : "maj9") : (m3 ? "Maj7" : "maj7"));
            else if (b7) q = base + (nine ? "9" : "7");
            else if (six) q = base + (nine ? "6/9" : "6");
            else q = base + (nine ? "add9" : "");
            if (s4) q += "sus4";
            if (s2) q += "sus2";
        }
        return std::string(names[((root % 12) + 12) % 12]) + q;
    }

    Settings uiSettings() const
    {
        Settings st{};
        st.mode = getInt(kMode); st.chord = getInt(kChord); st.key = getInt(kKey); st.scale = getInt(kScale);
        st.add6 = getBool(kAdd6); st.addm7 = getBool(kAddMin7); st.addmaj7 = getBool(kAddMaj7); st.add9 = getBool(kAdd9);
        st.padNotes = getInt(kTopPads) == 1;
        return st;
    }

    std::string padChordName(int deg) const
    {
        const Settings st = uiSettings();
        const int root = degreeRoot(st, deg);
        int iv[kMaxChord];
        const int n = intervalsFor(st, 0u, root, iv);
        return chordName(root, iv, n);
    }

    void pushHeld(int note)
    {
        removeHeld(note);
        if (numHeld_ == kMaxHeld) { for (int i = 1; i < kMaxHeld; ++i) held_[i - 1] = held_[i]; --numHeld_; }
        held_[numHeld_++] = note;
    }
    void removeHeld(int note)
    {
        int w = 0;
        for (int i = 0; i < numHeld_; ++i) if (held_[i] != note) held_[w++] = held_[i];
        numHeld_ = w;
    }
    int currentRoot() const { return numHeld_ > 0 ? held_[numHeld_ - 1] : lastRoot_; }

    // ---------------------------------------------------------------- chord building
    static int triadIntervals(int triad, int* out)
    {
        switch (triad)
        {
            case TMin:    out[0] = 0; out[1] = 3; out[2] = 7; return 3;
            case TDim:    out[0] = 0; out[1] = 3; out[2] = 6; return 3;
            case TSus4:   out[0] = 0; out[1] = 5; out[2] = 7; return 3;
            case TSus2:   out[0] = 0; out[1] = 2; out[2] = 7; return 3;
            case TAug:    out[0] = 0; out[1] = 4; out[2] = 8; return 3;
            case TPower:  out[0] = 0; out[1] = 7; return 2;
            case TSingle: out[0] = 0; return 1;
            case TMaj: default: out[0] = 0; out[1] = 4; out[2] = 7; return 3;
        }
    }

    // Builds chord intervals (relative to the root) for the current chord-key / screen state.
    int buildIntervals(int root, int* iv)
    {
        // Quality pads latch with Key Latch; add pads only count while held.
        unsigned keys = chordKeysHeld_ & 0x0Fu;
        if (!keys && s_.latch) keys = latched_ & 0x0Fu;
        keys |= chordKeysHeld_ & 0xF0u;
        return intervalsFor(s_, keys, root, iv);
    }

    static int intervalsFor(const Settings& st, unsigned keys, int root, int* iv)
    {
        if (st.padNotes && keys == 0u) { iv[0] = 0; return 1; }   // Top Pads = Notes: single note until a chord pad is held
        // Triad from the keys (if any triad key is involved), else from the screen / scale.
        int triad = -1;
        if (keys & 0x0Fu)
        {
            if (keys & 0x8u) triad = TSus4;          // Sus wins, then Maj, Min, Dim
            else if (keys & 0x4u) triad = TMaj;
            else if (keys & 0x2u) triad = TMin;
            else triad = TDim;
        }
        bool e6, em7, emaj7, e9;
        if (keys & 0xF0u) { e6 = (keys & 0x10u) != 0; em7 = (keys & 0x20u) != 0; emaj7 = (keys & 0x40u) != 0; e9 = (keys & 0x80u) != 0; }
        else if (keys) { e6 = em7 = emaj7 = e9 = false; }   // only a triad key held: plain triad
        else { e6 = st.add6; em7 = st.addm7; emaj7 = st.addmaj7; e9 = st.add9; }

        // In Key mode: diatonic chord on this root, if the root is in the scale.
        static const int major[7] = {0, 2, 4, 5, 7, 9, 11};
        static const int minor[7] = {0, 2, 3, 5, 7, 8, 10};
        const int* sc = st.scale == 1 ? minor : major;
        int degree = -1;
        if (st.mode == 1)
        {
            const int pc = ((root - st.key) % 12 + 12) % 12;
            for (int d = 0; d < 7; ++d) if (sc[d] == pc) { degree = d; break; }
        }
        auto scaleStep = [&](int steps) {   // interval from the root up `steps` scale degrees
            const int d = degree + steps;
            return sc[d % 7] + 12 * (d / 7) - sc[degree];
        };

        int n = 0;
        if (degree >= 0 && triad < 0)
        {
            iv[n++] = 0; iv[n++] = scaleStep(2); iv[n++] = scaleStep(4);
            if (keys & 0xF0u)
            {
                // An extension PAD adds exactly what it says (maj7 = major 7th, m7 = minor 7th) on the in-key triad.
                if (e6) iv[n++] = 9;
                if (em7) iv[n++] = 10;
                if (emaj7) iv[n++] = 11;
                if (e9) iv[n++] = 14;
            }
            else
            {
                // The CHORD page switches add the 6th / 7th / 9th that belong to the key.
                if (e6) iv[n++] = scaleStep(5);
                if (em7 || emaj7) iv[n++] = scaleStep(6);
                if (e9) iv[n++] = scaleStep(8);
            }
        }
        else
        {
            n = triadIntervals(triad >= 0 ? triad : st.chord, iv);
            if (e6) iv[n++] = 9;                 // Dim + 6 = diminished 7th
            if (em7) iv[n++] = 10;               // Dim + m7 = half-diminished
            if (emaj7) iv[n++] = 11;
            if (e9) iv[n++] = 14;
        }
        // Sort and drop duplicates.
        for (int i = 1; i < n; ++i) { int k = iv[i], j = i - 1; while (j >= 0 && iv[j] > k) { iv[j + 1] = iv[j]; --j; } iv[j + 1] = k; }
        int w = 0;
        for (int i = 0; i < n; ++i) if (w == 0 || iv[i] != iv[w - 1]) iv[w++] = iv[i];
        return w;
    }

    static void sortNotes(int* a, int n)
    {
        for (int i = 1; i < n; ++i) { int k = a[i], j = i - 1; while (j >= 0 && a[j] > k) { a[j + 1] = a[j]; --j; } a[j + 1] = k; }
    }

    // Applies inversion + spread to close-position notes (sorted, ascending).
    void voice(const int* close, int n, int inversion, int* out) const
    {
        for (int i = 0; i < n; ++i) out[i] = close[i];
        for (int k = 0; k < inversion % (n > 0 ? n : 1); ++k)   // rotate the lowest note up an octave
        {
            const int low = out[0];
            for (int i = 1; i < n; ++i) out[i - 1] = out[i];
            out[n - 1] = low + 12;
        }
        if (n >= 3)
        {
            switch (s_.voicing)
            {
                case 1: out[n - 2] -= 12; break;                                       // Drop 2
                case 2: for (int i = 1; i < n; i += 2) out[i] += 12; break;            // Open
                case 3: for (int i = 1; i < n; i += 2) out[i] += 12; out[n - 1] += 12; break;   // Wide
                default: break;
            }
        }
        sortNotes(out, n);
    }

    static float voicingCost(const int* a, int n, const int* b, int m)
    {
        // Each note's distance to the nearest previous note, plus a pull to keep the centre steady.
        float c = 0.0f, ca = 0.0f, cb = 0.0f;
        for (int i = 0; i < n; ++i)
        {
            int best = 1000;
            for (int j = 0; j < m; ++j) { const int d = std::abs(a[i] - b[j]); if (d < best) best = d; }
            c += (float)best;
            ca += (float)a[i];
        }
        for (int j = 0; j < m; ++j) cb += (float)b[j];
        return c + 0.5f * std::fabs(ca / (float)n - cb / (float)m);
    }

    // Builds chordNotes_ / bassNote_ for a root.
    void buildChord(int root)
    {
        int iv[kMaxChord];
        const int n = buildIntervals(root, iv);
        const int base = root + 12 * s_.octave;
        int close[kMaxChord];
        for (int i = 0; i < n; ++i) close[i] = base + iv[i];

        int best[kMaxChord];
        voice(close, n, s_.inversion, best);
        if (s_.voiceLead && numPrev_ > 0 && n > 1)
        {
            float bestCost = 1.0e9f;
            int cand[kMaxChord];
            for (int inv = 0; inv < n; ++inv)
                for (int shift = -12; shift <= 12; shift += 12)
                {
                    voice(close, n, inv, cand);
                    for (int i = 0; i < n; ++i) cand[i] += shift;
                    // Keep the chord near where it was played so it cannot drift away.
                    if (cand[0] < base - 9 || cand[0] > base + 9) continue;
                    const float c = voicingCost(cand, n, prev_, numPrev_);
                    if (c < bestCost) { bestCost = c; for (int i = 0; i < n; ++i) best[i] = cand[i]; }
                }
        }
        numChord_ = n;
        for (int i = 0; i < n; ++i) chordNotes_[i] = clampNote(best[i]);
        numPrev_ = n;
        for (int i = 0; i < n; ++i) prev_[i] = chordNotes_[i];

        hasBass_ = s_.bass > 0 && n > 1;   // a single note (Top Pads = Notes) gets no extra bass
        if (hasBass_)
        {
            int b = base - 12 * s_.bass;
            while (b >= chordNotes_[0] && b - 12 >= 0) b -= 12;
            bassNote_ = clampNote(b);
        }
    }

    static int clampNote(int n) { return n < 0 ? 0 : (n > 127 ? 127 : n); }

    // ---------------------------------------------------------------- performing
    void triggerChord(int root, int velocity)
    {
        releaseChord();
        lastRoot_ = root;
        buildChord(root);
        chordHeld_ = true;
        ++chordGen_;
        const float vel = (float)velocity / 127.0f;
        chordVel_ = vel;

        if (s_.playMode == PlayArp)
        {
            if (hasBass_) startVoice(bassNote_, vel * 0.9f, -1);
            buildArpPattern();
            arpActive_ = true;
            arpIndex_ = 0;
            arpFreeStart_ = clock_;
            arpLastStep_ = currentStep();
            playArpStep();
            return;
        }

        int order[kMaxChord];
        for (int i = 0; i < numChord_; ++i) order[i] = i;
        bool down = false;
        if (s_.playMode == PlayStrum)
        {
            down = s_.strumDir == 1 || (s_.strumDir == 2 && strumAltDown_);
            strumAltDown_ = !strumAltDown_;
        }
        const float spacing = s_.playMode == PlayStrum ? s_.strumMs * 0.001f * sr_ / (float)(numChord_ > 1 ? numChord_ - 1 : 1) : 0.0f;
        if (hasBass_) startVoice(bassNote_, vel * 0.9f, -1);
        for (int k = 0; k < numChord_; ++k)
        {
            const int note = chordNotes_[down ? numChord_ - 1 - order[k] : order[k]];
            float v = vel * (1.0f - 0.25f * s_.humanize * rand01());
            if (s_.playMode == PlayStrum) v *= (1.0f - 0.06f * (float)k);   // strings fade slightly across the strum
            float t = spacing * (float)k;
            if (k > 0) t += s_.humanize * rand01() * 0.004f * sr_;            // a few ms of looseness
            if (t < 1.0f) startVoice(note, v, -1);
            else schedule(note, v, clock_ + (int64_t)t);
        }
    }

    // Re-voices the sounding chord without restarting it: notes that left are released, notes that
    // joined start, the rest keep ringing. Used by the momentary add pads.
    void refreshChord()
    {
        if (!chordHeld_) return;
        buildChord(lastRoot_);
        if (s_.playMode == PlayArp) { buildArpPattern(); return; }
        const int bass = hasBass_ ? bassNote_ : -1;
        auto wanted = [&](int note)
        {
            if (note == bass) return true;
            for (int i = 0; i < numChord_; ++i) if (chordNotes_[i] == note) return true;
            return false;
        };
        int keep = 0;   // strum notes still waiting
        for (int i = 0; i < numPending_; ++i) if (wanted(pending_[i].note)) pending_[keep++] = pending_[i];
        numPending_ = keep;
        for (auto& v : voices_)
            if (v.active && v.gen == chordGen_ && v.amp.isHeld() && !wanted(v.note)) { v.amp.noteOff(); v.fenv.noteOff(); voiceMidiOff(v); }
        auto sounding = [&](int note)
        {
            for (const auto& v : voices_) if (v.active && v.gen == chordGen_ && v.amp.isHeld() && v.note == note) return true;
            for (int i = 0; i < numPending_; ++i) if (pending_[i].note == note) return true;
            return false;
        };
        if (bass >= 0 && !sounding(bass)) startVoice(bass, chordVel_ * 0.9f, -1);
        for (int i = 0; i < numChord_; ++i) if (!sounding(chordNotes_[i])) startVoice(chordNotes_[i], chordVel_, -1);
    }

    void releaseChord()
    {
        chordHeld_ = false;
        arpActive_ = false;
        numPending_ = 0;
        for (auto& v : voices_) if (v.active && v.gen == chordGen_ && v.amp.isHeld()) { v.amp.noteOff(); v.fenv.noteOff(); voiceMidiOff(v); }
    }

    void schedule(int note, float vel, int64_t at)
    {
        if (numPending_ >= kMaxChord * 2) return;
        pending_[numPending_++] = Pending{note, vel, at};
    }

    // Song step index for the arp (ppq-locked while the MPC plays, free-running otherwise).
    int64_t currentStepAt(int64_t clock) const
    {
        const float bpm = safeBpm(transport_.bpm);
        const double beatsPerSample = (double)bpm / 60.0 / (double)sr_;
        double beats;
        if (transport_.playing && transport_.ppqValid)
            beats = transport_.ppqPos + (double)(clock - blockStartClock_) * beatsPerSample;
        else
            beats = (double)(clock - arpFreeStart_) * beatsPerSample;
        return (int64_t)std::floor(beats / (double)s_.arpBeats + 1.0e-9);
    }
    int64_t currentStep() const { return currentStepAt(clock_); }

    void buildArpPattern()
    {
        numArp_ = 0;
        for (int o = 0; o < s_.arpOct; ++o)
            for (int i = 0; i < numChord_ && numArp_ < kMaxArp; ++i)
                arp_[numArp_++] = clampNote(chordNotes_[i] + 12 * o);
    }

    void playArpStep()
    {
        if (numArp_ == 0) return;
        int idx = 0;
        switch (s_.arpDir)
        {
            case 1: idx = numArp_ - 1 - (arpIndex_ % numArp_); ++arpIndex_; break;
            case 2:
            {
                const int period = numArp_ > 1 ? 2 * numArp_ - 2 : 1;
                const int p = arpIndex_ % period;
                idx = p < numArp_ ? p : period - p;
                ++arpIndex_;
                break;
            }
            case 3: idx = (int)(rand01() * (float)numArp_); if (idx >= numArp_) idx = numArp_ - 1; break;
            default: idx = arpIndex_ % numArp_; ++arpIndex_; break;
        }
        const float bpm = safeBpm(transport_.bpm);
        const float stepSamples = s_.arpBeats * 60.0f / bpm * sr_;
        const float v = chordVel_ * (1.0f - 0.25f * s_.humanize * rand01());
        startVoice(arp_[idx], v, clock_ + (int64_t)(stepSamples * s_.arpGate));
    }

    void runScheduler(int /*pos*/)
    {
        // Strum notes that are due.
        int w = 0;
        for (int i = 0; i < numPending_; ++i)
        {
            if (pending_[i].at <= clock_) startVoice(pending_[i].note, pending_[i].vel, -1);
            else pending_[w++] = pending_[i];
        }
        numPending_ = w;
        // Arp steps.
        if (arpActive_)
        {
            const int64_t step = currentStep();
            if (step != arpLastStep_) { arpLastStep_ = step; playArpStep(); }
        }
        // Arp gates.
        for (auto& v : voices_)
            if (v.active && v.releaseAt >= 0 && clock_ >= v.releaseAt) { v.amp.noteOff(); v.fenv.noteOff(); v.releaseAt = -1; voiceMidiOff(v); }
    }

    // ---------------------------------------------------------------- voices
    struct Voice
    {
        PolyBlepOsc osc1, osc2;
        WavetableSine car, mod;
        SVF filter;
        ADSR amp, fenv;
        bool active = false;
        int note = 60;
        uint32_t gen = 0;
        int64_t startedAt = 0, releaseAt = -1;
        float vel = 1.0f, panL = 0.7f, panR = 0.7f, tineEnv = 0.0f, tineDecay = 0.999f;
        int ctrl = 0;
        bool sentMidi = false;
        // organ / piano
        float ph[9] = {0}, part[7] = {0}, partDec[7] = {0};
        float perc = 0.0f, percDec = 0.999f, click = 0.0f, hammer = 0.0f, swell = 0.0f, noiseLp = 0.0f;
        SVF air;
    };

    void startVoice(int note, float vel, int64_t releaseAt)
    {
        // Free voice, else steal the oldest releasing one, else the oldest.
        int pick = -1;
        for (int i = 0; i < kNumVoices; ++i) if (!voices_[i].active) { pick = i; break; }
        if (pick < 0)
        {
            int64_t oldest = INT64_MAX;
            for (int i = 0; i < kNumVoices; ++i)
                if (voices_[i].amp.isReleasing() && voices_[i].startedAt < oldest) { oldest = voices_[i].startedAt; pick = i; }
        }
        if (pick < 0)
        {
            int64_t oldest = INT64_MAX;
            for (int i = 0; i < kNumVoices; ++i) if (voices_[i].startedAt < oldest) { oldest = voices_[i].startedAt; pick = i; }
        }
        Voice& v = voices_[pick];
        const bool wasActive = v.active;
        if (wasActive && v.sentMidi) { midiNote(false, v.note, 0); v.sentMidi = false; }
        v.active = true;
        v.note = note;
        v.gen = chordGen_;
        v.startedAt = clock_;
        v.releaseAt = releaseAt;
        v.vel = lerp(1.0f, std::pow(clamp(vel, 0.0f, 1.0f), s_.velExp), s_.velSens);
        // Spread chord notes across the stereo field by pitch.
        const float pan = clamp((float)(note - 60) / 30.0f, -0.8f, 0.8f);
        equalPowerPan(pan, v.panL, v.panR);
        if (!wasActive)
        {
            v.osc1.resetPhase(rand01()); v.osc2.resetPhase(rand01());
            v.car.resetPhase(0.0f); v.mod.resetPhase(0.0f);
            v.filter.reset();
        }
        v.tineEnv = 1.0f;
        v.tineDecay = std::exp(-1.0f / (0.25f * sr_));
        if (s_.engine == 1)
        {
            if (!wasActive) for (int k = 0; k < 9; ++k) v.ph[k] = rand01();   // tonewheels are free-running
            v.perc = s_.percussion ? 1.0f : 0.0f;
            v.percDec = std::exp(-1.0f / (0.20f * sr_));
            v.click = 1.0f;
        }
        else if (s_.engine == 2)
        {
            // Partials decay faster the higher they are, and low notes ring longer.
            const float t1 = clamp(7.0f - 0.07f * (float)(note - 36), 1.2f, 8.0f);
            const float bright = 0.55f + 0.45f * v.vel;
            for (int k = 0; k < 7; ++k)
            {
                v.ph[k] = 0.0f;
                v.part[k] = std::pow(bright, (float)k) / (1.0f + 0.35f * (float)k);
                v.partDec[k] = std::exp(-1.0f / (t1 / (1.0f + 0.9f * (float)k) * sr_));
            }
            v.hammer = 1.0f;
            if (!wasActive) { v.swell = 0.0f; v.air.reset(); }
        }
        v.amp.noteOn(); v.fenv.noteOn();
        v.ctrl = 0;
        if (midiCh_ >= 0) { midiNote(true, note, clamp((int)std::lround(vel * 127.0f), 1, 127)); v.sentMidi = true; }
    }

    void renderVoices(float* L, float* R, int n)
    {
        for (int i = 0; i < n; ++i) { bendBuf_[i] = bend_.process(); }
        float bend = bendBuf_[n - 1];
        // Aftertouch targets that act on every voice.
        const float atBright = atTarget_ == 0 ? pressure_ * 3.0f : 0.0f;               // up to +3 octaves of filter
        const float atSwell = atTarget_ == 1 ? 1.0f + pressure_ * 1.2f : 1.0f;         // up to about +7 dB
        if (atTarget_ == 2 && pressure_ > 0.001f)
        {
            vibPhase_ += 5.5f * (float)n / sr_;
            if (vibPhase_ >= 1.0f) vibPhase_ -= 1.0f;
            bend += pressure_ * 0.4f * fastSin01(vibPhase_);                            // up to +-0.4 semitone
        }
        for (int vi = 0; vi < kNumVoices; ++vi)
        {
            Voice& v = voices_[vi];
            if (!v.active) continue;

            // Control rate: once per chunk.
            const float hz = midiToHz((float)v.note + bend);
            const float det = centsToRatio(s_.detune);
            const bool tableSine = s_.engine == 0 && s_.wave == 0;   // cheap sine for the Analog engine
            if (s_.engine == 0 && !tableSine) { v.osc1.setFrequency(hz / det); v.osc2.setFrequency(hz * det); }
            else if (tableSine) { v.car.setFrequency(hz / det); v.mod.setFrequency(hz * det); }
            else { v.car.setFrequency(hz); v.mod.setFrequency(hz); }
            const float fe = v.fenv.level();
            const float octaves = s_.fenvAmt * fe * 5.0f + (0.5f + 1.0f * s_.velSens) * (v.vel - 0.6f) + 0.3f * ((float)v.note - 60.0f) / 12.0f + atBright;
            v.filter.set(s_.cutoff * std::pow(2.0f, clamp(octaves, -8.0f, 8.0f)), s_.reso);
            const float gain = v.vel * 0.16f * atSwell;

            if (tableSine)
            {
                for (int i = 0; i < n; ++i)
                {
                    v.fenv.process();
                    const float a = v.amp.process();
                    const float x = (v.car.process() + v.mod.process()) * 0.5f;
                    const float y = v.filter.process(x, SVF::Mode::Lowpass) * a * gain;
                    L[i] += y * v.panL; R[i] += y * v.panR;
                }
            }
            else if (s_.engine == 0)
            {
                for (int i = 0; i < n; ++i)
                {
                    v.fenv.process();
                    const float a = v.amp.process();
                    const float x = (v.osc1.process() + v.osc2.process()) * 0.5f;
                    const float y = v.filter.process(x, SVF::Mode::Lowpass) * a * gain;
                    L[i] += y * v.panL; R[i] += y * v.panR;
                }
            }
            else if (s_.engine == 1) renderOrgan(v, hz, gain, L, R, n);
            else if (s_.engine == 2) renderPiano(v, hz, gain, L, R, n);
            else
            {
                const float index = s_.tine * (0.15f + 0.6f * v.vel);
                for (int i = 0; i < n; ++i)
                {
                    v.fenv.process();
                    const float a = v.amp.process();
                    v.tineEnv *= v.tineDecay;
                    const float m = v.mod.process() * index * (0.35f + v.tineEnv);
                    const float x = v.car.process(m);
                    const float y = v.filter.process(x, SVF::Mode::Lowpass) * a * gain * 1.2f;
                    L[i] += y * v.panL; R[i] += y * v.panR;
                }
            }
            if (!v.amp.isActive()) { v.active = false; v.releaseAt = -1; voiceMidiOff(v); }
        }
    }

    // ---------------------------------------------------------------- organ and piano
    float noise() { return rand01() * 2.0f - 1.0f; }

    // Tonewheel organ: nine drawbars, 3rd-harmonic percussion, key click. Brightness tames the top.
    void renderOrgan(Voice& v, float hz, float gain, float* L, float* R, int n)
    {
        static const float ratio[9] = {0.5f, 1.5f, 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 8.0f};   // 16' 5 1/3' 8' 4' 2 2/3' 2' 1 3/5' 1 1/3' 1'
        static const int bars[4][9] = {{8, 8, 8, 0, 0, 0, 0, 0, 0},    // Jazz
                                       {8, 8, 8, 8, 0, 0, 0, 0, 6},    // Soul
                                       {8, 8, 8, 8, 8, 8, 8, 8, 8},    // Full
                                       {0, 0, 8, 4, 0, 0, 0, 0, 0}};   // Soft
        const int* d = bars[clamp(s_.organTone, 0, 3)];
        float g[9], inc[9], sumsq = 0.0f;
        for (int k = 0; k < 9; ++k)
        {
            const float f = hz * ratio[k];
            g[k] = (d[k] > 0 && f < 0.45f * sr_) ? std::pow(10.0f, -(float)(8 - d[k]) * 0.15f) : 0.0f;
            inc[k] = f / sr_;
            sumsq += g[k] * g[k];
        }
        const float norm = 0.8f / std::sqrt(sumsq + 0.25f);
        const bool perc3 = 3.0f * hz < 0.45f * sr_;
        const float clickDec = std::exp(-1.0f / (0.003f * sr_));
        const float lp = 1.0f - std::exp(-6.2832f * 3000.0f / sr_);
        v.filter.set(clamp(s_.cutoff * 3.0f, 600.0f, 18000.0f), 0.0f);
        for (int i = 0; i < n; ++i)
        {
            v.fenv.process();
            const float a = v.amp.process();
            float x = 0.0f;
            for (int k = 0; k < 9; ++k)
            {
                v.ph[k] += inc[k];
                if (v.ph[k] >= 1.0f) v.ph[k] -= 1.0f;
                if (g[k] > 0.0f) x += g[k] * WavetableSine::lookup(v.ph[k]);
            }
            x *= norm;
            if (perc3 && v.perc > 0.0005f) { x += 0.9f * v.perc * WavetableSine::lookup(v.ph[4]); v.perc *= v.percDec; }
            if (v.click > 0.001f)
            {
                const float nz = noise();
                v.noiseLp += lp * (nz - v.noiseLp);
                x += 0.25f * v.click * (nz - v.noiseLp);
                v.click *= clickDec;
            }
            const float y = v.filter.process(x, SVF::Mode::Lowpass) * a * gain * 0.6f;
            L[i] += y * v.panL; R[i] += y * v.panR;
        }
    }

    // Piano with one blend: airy breath and shimmer (< 0), plain piano (0), a string section (> 0).
    void renderPiano(Voice& v, float hz, float gain, float* L, float* R, int n)
    {
        const float b = clamp(s_.pianoBlend, -1.0f, 1.0f);
        const float pianoLvl = 1.0f - 0.5f * std::fabs(b);
        const float airLvl = b < 0.0f ? -b : 0.0f;
        const float strLvl = b > 0.0f ? b : 0.0f;
        float inc[7];
        bool on[7];
        for (int k = 0; k < 7; ++k)
        {
            const float kk = (float)(k + 1);
            const float f = kk * hz * std::sqrt(1.0f + 0.0002f * kk * kk);   // stiff-string stretch
            inc[k] = f / sr_;
            on[k] = f < 0.45f * sr_;
        }
        const float inc2 = hz * 1.0007f / sr_;                     // second string, slightly sharp: slow beating
        const float incSh = 2.0f * hz / sr_;
        const float hamDec = std::exp(-1.0f / (0.004f * sr_));
        const float swellUp = 1.0f - std::exp(-1.0f / (0.35f * sr_));
        const float lp = 1.0f - std::exp(-6.2832f * 2500.0f / sr_);
        if (strLvl > 0.0f)
        {
            const float det = centsToRatio(7.0f);
            v.osc1.setWave(2); v.osc2.setWave(2);
            v.osc1.setFrequency(hz / det); v.osc2.setFrequency(hz * det);
        }
        if (airLvl > 0.0f) v.air.set(clamp(hz * 2.0f, 300.0f, 9000.0f), 0.55f);
        for (int i = 0; i < n; ++i)
        {
            v.fenv.process();
            const float a = v.amp.process();
            float p = 0.0f;
            for (int k = 0; k < 7; ++k)
            {
                v.ph[k] += inc[k];
                if (v.ph[k] >= 1.0f) v.ph[k] -= 1.0f;
                if (on[k]) p += v.part[k] * WavetableSine::lookup(v.ph[k]);
                v.part[k] *= v.partDec[k];
            }
            v.ph[7] += inc2; if (v.ph[7] >= 1.0f) v.ph[7] -= 1.0f;
            p += 0.6f * v.part[0] * WavetableSine::lookup(v.ph[7]);
            if (v.hammer > 0.001f)
            {
                v.noiseLp += lp * (noise() - v.noiseLp);
                p += 0.6f * v.hammer * v.vel * v.noiseLp;
                v.hammer *= hamDec;
            }
            float x = pianoLvl * p * 0.35f;
            if (airLvl > 0.0f || strLvl > 0.0f)
            {
                v.swell += swellUp * ((v.amp.isHeld() ? 1.0f : 0.0f) - v.swell);
                if (airLvl > 0.0f)
                {
                    v.ph[8] += incSh; if (v.ph[8] >= 1.0f) v.ph[8] -= 1.0f;
                    const float breath = v.air.process(noise(), SVF::Mode::Bandpass);
                    x += airLvl * v.swell * (0.55f * breath + 0.18f * WavetableSine::lookup(v.ph[8]));
                }
                if (strLvl > 0.0f) x += strLvl * v.swell * 0.32f * (v.osc1.process() + v.osc2.process());
            }
            const float y = v.filter.process(x, SVF::Mode::Lowpass) * a * gain;
            L[i] += y * v.panL; R[i] += y * v.panR;
        }
    }

    // Rotary speaker: horn (Doppler + level) above ~800 Hz, drum (level) below, two mics 90 degrees apart.
    void leslie(float& l, float& r)
    {
        const float m = (l + r) * 0.5f;
        leslieLp_ += leslieLpC_ * (m - leslieLp_);
        const float lo = leslieLp_, hi = m - lo;
        leslieDelay_.write(hi);
        hornHz_ += (hornTarget_ - hornHz_) * hornInertia_;
        drumHz_ += (drumTarget_ - drumHz_) * drumInertia_;
        hornPhase_ += hornHz_ / sr_; if (hornPhase_ >= 1.0f) hornPhase_ -= 1.0f;
        drumPhase_ += drumHz_ / sr_; if (drumPhase_ >= 1.0f) drumPhase_ -= 1.0f;
        const float sH = fastSin01(hornPhase_), cH = fastSin01(hornPhase_ + 0.25f);
        const float sD = fastSin01(drumPhase_), cD = fastSin01(drumPhase_ + 0.25f);
        const float base = 0.0015f * sr_, dep = 0.0004f * sr_;
        const float hL = leslieDelay_.readLinear(base + dep * sH) * (0.72f + 0.28f * cH);
        const float hR = leslieDelay_.readLinear(base + dep * cH) * (0.72f - 0.28f * sH);
        const float wl = (hL + lo * (0.85f + 0.15f * sD)) * 1.15f;
        const float wr = (hR + lo * (0.85f + 0.15f * cD)) * 1.15f;
        const float w = leslieWet_.process();
        l += w * (wl - l);
        r += w * (wr - r);
    }

    // ---------------------------------------------------------------- MIDI out
    // Counted per note so two voices on one note (a chord tone doubled by the arp) send one note-off.
    void midiNote(bool on, int note, int vel)
    {
        if (note < 0 || note > 127) return;
        if (on)
        {
            if (midiCount_[note] > 0) sendNote(false, midiSentCh_[note], note, 0);   // retrigger
            midiSentCh_[note] = (unsigned char)clamp(midiCh_, 0, 15);
            sendNote(true, midiSentCh_[note], note, vel);
            if (midiCount_[note] < 255) ++midiCount_[note];
        }
        else if (midiCount_[note] > 0 && --midiCount_[note] == 0)
            sendNote(false, midiSentCh_[note], note, 0);
    }
    struct Echo { uint8_t on = 0, off = 0; int64_t at = 0; };
    Echo& echoFor(int ch, int note) { return echo_[(ch & 15) * 128 + (note & 127)]; }
    void expectEcho(bool on, int ch, int note)
    {
        Echo& e = echoFor(ch, note);
        if (on) { if (e.on < 255) ++e.on; } else if (e.off < 255) ++e.off;
        e.at = clock_;
    }
    bool swallowEcho(const mpc::MidiEvent& ev)
    {
        Echo& e = echoFor(ev.channel(), ev.note());
        if (clock_ - e.at > (int64_t)(0.5f * sr_)) { e.on = e.off = 0; return false; }   // too old: a real pad
        uint8_t& c = ev.isNoteOn() ? e.on : e.off;
        if (c == 0) return false;
        --c;
        return true;
    }

    void sendNote(bool on, int ch, int note, int vel)
    {
        if (!midiOut_.ok()) return;
        midiOut_.note(on, ch, note, vel);
        expectEcho(on, ch, note);
    }

    void voiceMidiOff(Voice& v) { if (v.sentMidi) { v.sentMidi = false; midiNote(false, v.note, 0); } }
    void allMidiOff()
    {
        for (auto& v : voices_) v.sentMidi = false;
        for (int n = 0; n < 128; ++n)
            if (midiCount_[n]) { midiCount_[n] = 0; sendNote(false, midiSentCh_[n], n, 0); }
    }

    // ---------------------------------------------------------------- helpers
    float rand01() { rng_ = rng_ * 1664525u + 1013904223u; return (float)(rng_ >> 8) * (1.0f / 16777216.0f); }

    static std::string noteName(int n)
    {
        static const char* names[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
        n = n < 0 ? 0 : (n > 127 ? 127 : n);
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%s%d", names[n % 12], n / 12 - 2);   // MPC naming: 60 = C3
        return buf;
    }

    struct Pending { int note; float vel; int64_t at; };

    Voice voices_[kNumVoices];
    float sr_ = 44100.0f;
    int64_t clock_ = 0, blockStartClock_ = 0;
    mpc::Transport transport_;

    int held_[kMaxHeld] = {0};
    int numHeld_ = 0;
    int lastRoot_ = 60;
    int lastVelocity_ = 100;
    unsigned chordKeysHeld_ = 0, latched_ = 0;
    bool sustainPedal_ = false, chordHeld_ = false, strumAltDown_ = false;

    int chordNotes_[kMaxChord] = {0};
    int numChord_ = 0;
    int prev_[kMaxChord] = {0};
    int numPrev_ = 0;
    int bassNote_ = 36;
    bool hasBass_ = false;
    uint32_t chordGen_ = 0;
    float chordVel_ = 1.0f;

    Pending pending_[kMaxChord * 2];
    int numPending_ = 0;

    int arp_[kMaxArp] = {0};
    int numArp_ = 0, arpIndex_ = 0;
    bool arpActive_ = false;
    int64_t arpLastStep_ = 0, arpFreeStart_ = 0;

    // ---------------------------------------------------------------- performance FX
    // Drive -> DJ filter (one knob: left = low-pass, right = high-pass) -> ping-pong delay
    // (tempo-synced send) -> reverb (send). All four are smoothed so knob moves never click.
    float delaySamples() const
    {
        const float beats = divisionBeats(getInt(kDelayTime));
        const float s = beats * 60.0f / safeBpm(transport_.bpm) * sr_;
        return clamp(s, 16.0f, 2.4f * sr_);
    }

    void performFx(float& l, float& r)
    {
        // Drive
        const float d = drive_.process();
        if (d > 0.001f)
        {
            const float gIn = 1.0f + 14.0f * d;
            const float comp = 1.0f / (1.0f + 2.5f * d);
            l = lerp(l, fastTanh(l * gIn) * comp, clamp(d * 4.0f, 0.0f, 1.0f));
            r = lerp(r, fastTanh(r * gIn) * comp, clamp(d * 4.0f, 0.0f, 1.0f));
        }
        // DJ filter (coefficients every 16 samples)
        const float fk = filterKnob_.process();
        if ((fxCtrl_++ & 15) == 0)
        {
            const float a = std::fabs(fk);
            fxFilterMode_ = fk < 0.0f ? 0 : 2;   // SVF Lowpass / Highpass
            const float hz = fk < 0.0f ? 20000.0f * std::pow(0.01f, a)       // 20 kHz -> 200 Hz
                                       : 20.0f * std::pow(300.0f, a);        // 20 Hz -> 6 kHz
            fxFilterOn_ = a > 0.015f;
            for (auto& f : djFilter_) f.set(hz, 0.3f);
        }
        if (fxFilterOn_)
        {
            l = djFilter_[0].process(l, fxFilterMode_);
            r = djFilter_[1].process(r, fxFilterMode_);
        }
        // Ping-pong delay (send)
        const float dm = delayMix_.process();
        const float dt = delayTime_.process();
        const float tapL = delayL_.readLinear(dt);
        const float tapR = delayR_.readLinear(dt);
        fbLpL_ += 0.35f * (tapL - fbLpL_);   // darken the repeats
        fbLpR_ += 0.35f * (tapR - fbLpR_);
        delayL_.write((l + r) * 0.5f * dm + fbLpR_ * s_.delayFb);
        delayR_.write(fbLpL_ * s_.delayFb);
        l += tapL;
        r += tapR;
        // Reverb (send)
        const float vm = verbMix_.process();
        float vl, vr;
        verb_.process((l + r) * vm, vl, vr);
        l += vl;
        r += vr;
    }

    // Small Freeverb-style stereo reverb (8 combs + 4 allpasses per side; public-domain algorithm).
    struct Reverb
    {
        struct Comb
        {
            std::vector<float> buf; int idx = 0; float store = 0.0f;
            float process(float x, float fb, float damp)
            {
                const float y = buf[(size_t)idx];
                store = y * (1.0f - damp) + store * damp;
                buf[(size_t)idx] = x + store * fb;
                if (++idx >= (int)buf.size()) idx = 0;
                return y;
            }
        };
        struct Allpass
        {
            std::vector<float> buf; int idx = 0;
            float process(float x)
            {
                const float b = buf[(size_t)idx];
                buf[(size_t)idx] = x + b * 0.5f;
                if (++idx >= (int)buf.size()) idx = 0;
                return b - x;
            }
        };
        Comb combL[8], combR[8];
        Allpass apL[4], apR[4];
        float fb = 0.84f, damp = 0.3f;

        void prepare(double sr)
        {
            static const int combT[8] = {1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617};
            static const int apT[4] = {556, 441, 341, 225};
            const double k = sr / 44100.0;
            for (int i = 0; i < 8; ++i)
            {
                combL[i].buf.assign((size_t)(combT[i] * k) + 1, 0.0f);
                combR[i].buf.assign((size_t)((combT[i] + 23) * k) + 1, 0.0f);
            }
            for (int i = 0; i < 4; ++i)
            {
                apL[i].buf.assign((size_t)(apT[i] * k) + 1, 0.0f);
                apR[i].buf.assign((size_t)((apT[i] + 23) * k) + 1, 0.0f);
            }
        }
        void reset()
        {
            for (auto& c : combL) { std::fill(c.buf.begin(), c.buf.end(), 0.0f); c.store = 0.0f; c.idx = 0; }
            for (auto& c : combR) { std::fill(c.buf.begin(), c.buf.end(), 0.0f); c.store = 0.0f; c.idx = 0; }
            for (auto& a : apL) { std::fill(a.buf.begin(), a.buf.end(), 0.0f); a.idx = 0; }
            for (auto& a : apR) { std::fill(a.buf.begin(), a.buf.end(), 0.0f); a.idx = 0; }
        }
        void set(float size01, float tone01)
        {
            fb = 0.70f + 0.28f * clamp(size01, 0.0f, 1.0f);
            damp = 0.05f + 0.6f * (1.0f - clamp(tone01, 0.0f, 1.0f));
        }
        void process(float in, float& outL, float& outR)
        {
            const float x = in * 0.015f;
            float l = 0.0f, r = 0.0f;
            for (int i = 0; i < 8; ++i) { l += combL[i].process(x, fb, damp); r += combR[i].process(x, fb, damp); }
            for (int i = 0; i < 4; ++i) { l = apL[i].process(l); r = apR[i].process(r); }
            outL = l * 3.0f;
            outR = r * 3.0f;
        }
    };

    DelayLine delayL_, delayR_;
    Reverb verb_;
    SVF djFilter_[2];
    Smoother filterKnob_, delayTime_, delayMix_, verbMix_, drive_;
    float fbLpL_ = 0.0f, fbLpR_ = 0.0f;
    int fxCtrl_ = 0, fxFilterMode_ = 0;
    bool fxFilterOn_ = false;

    DelayLine leslieDelay_;
    Smoother leslieWet_;
    float hornPhase_ = 0.0f, drumPhase_ = 0.0f, hornHz_ = 0.0f, drumHz_ = 0.0f, hornTarget_ = 1.0f, drumTarget_ = 1.0f;
    float hornInertia_ = 0.0f, drumInertia_ = 0.0f, leslieLp_ = 0.0f, leslieLpC_ = 0.1f, rotorSpeed_ = 0.0f;
    bool rotaryOn_ = false;

    DelayLine chorusL_, chorusR_;
    float chorusPhase_ = 0.0f;
    Smoother volume_, bend_;
    float bendBuf_[kChunk] = {0};
    float scratchR_[kChunk] = {0};
    uint32_t rng_ = 0x12345678u;

    AlsaMidiOut midiOut_;
    unsigned char midiCount_[128] = {0}, midiSentCh_[128] = {0};
    int midiCh_ = -1, lastMidiCh_ = -1;
    bool synthOn_ = true, vocoderOut_ = false, heldFromPad_ = false;
    std::atomic<int> tapRequest_{-1}, padLitPushed_{0};
    std::atomic<bool> beeLand_{false};
    int tapRoot_ = -1, litPad_ = 0, nameSig_ = -1;
    int zoneState_ = 0, chanPressure_ = 0, atTarget_ = 0;
    int padMap_[16] = {0}, learnNotes_[16] = {0}, learnStep_ = 0, mapZone_ = 36, lastPad_ = -1, lastPadNote_ = -1;
    signed char noteToPad_[128];
    bool lastLearn_ = false;
    unsigned heldPushed_ = 0;
    unsigned char polyPressure_[128] = {0};
    float pressure_ = 0.0f, vibPhase_ = 0.0f;
    int64_t tapEndAt_ = 0;
    int savedZone_ = -1;
    Echo echo_[16 * 128];
};

MPC_PLUGIN_ENTRY(ChordBloom)
