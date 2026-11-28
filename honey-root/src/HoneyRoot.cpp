// HoneyRoot.cpp - a two-handed chord machine for the MPC, by N8b.
//
// SPDX-License-Identifier: MIT
//
// The idea (the same family as chord-machine keyboards): the left hand holds CHORD KEYS at the
// bottom of the keyboard, the right hand plays ROOTS. Every root becomes a named, voiced chord.
// Around that: a bass part that follows the chords, LOCK (chord keys that stay on), a voicing
// control, Key Mode (a lone root plays the chord of the scale), and performance modes for the
// chords and, separately, for the bass.
//
// Signal / control flow:
//
//   MIDI in --+-- chord keys (Keys / Classic layout from "Block Start" up, or pads A01-A08)
//             |       --> held + LOCKed chord keys: type (Maj/Min/Dim/Sus, secret chords) + extensions
//             +-- notes below Block Start (Split Bass on) --> the bass, played by hand
//             +-- every other note = a ROOT (pads A09-A16 = scale degrees)
//                     --> chord builder (types / Key Mode) --> voicing (-12..+12) --> octave
//                     --> CHORD part performer (Off, Strum, Slop, Arp, Pattern, Harp) --> 16-voice synth
//                     --> BASS part (Chords / Unison / Singles / Solo) --> bass performer
//                         (Off, Follow, Arp, Octaves, Pulse, Walk) --> mono bass synth
//
//   chord voices (Warm VA, Glass FM, Phase distortion, Reed additive) -> SVF -> ADSR
//     -> stereo chorus --+--> drive -> ping-pong delay (send) -> reverb (send) -> volume -> soft clip
//   bass voice ----------+
//
// Chord keys, Keys layout (made for the M-Vave SMK-37 and similar 37-key boards; Block Start = F):
//
//      black:       DIM      MIN      MAJ              (MIN + MAJ together = SUS)
//      white:   6        m7       M7        9      | roots from the C above ...
//               F   F#   G   G#   A    A#   B      | C ...
//
// Classic layout: the same, plus LOCK on B, the 9 on C and SUS on C#; roots start at D.
// Pads layout (MPC pads): A01 Maj, A02 Sus4, A03 Min, A04 Dim, A05 add9, A06 add maj7, A07 add m7,
// A08 add 6 (Chord Bloom's order), A09-A16 = scale degrees 1-8 of Key/Scale.

#include "mpc_plugin.h"
#include "mpc_resources.h"
#include "dsp/math.h"
#include "dsp/oscillator.h"
#include "dsp/svf.h"
#include "dsp/adsr.h"
#include "dsp/smoother.h"
#include "dsp/tempo.h"
#include "dsp/delay_line.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

using namespace mpc::dsp;

class HoneyRoot : public mpc::Plugin
{
public:
    // Parameter order is permanent (saved projects depend on it). Append only.
    enum Params
    {
        kVolume,
        // CHORD
        kLayout, kBlockStart, kLock, kLatch, kPlayStyle, kSecret, kSixth, kVoicing, kOctave, kVelSens,
        // KEY
        kKeyMode, kKey, kScale, kTranspose,
        // PERFORM (chords)
        kPerf, kPerfRate, kPerfDiv, kPerfDir, kPerfGate, kPerfSwing, kPerfRange, kPattern, kSlop,
        // BASS
        kBassMode, kBassReg, kBassLevel, kBassSound, kBassTone, kBassDecay, kBassGlide,
        kBassPerf, kBassDiv, kBassDir, kBassGate,
        // SOUND
        kEngine, kTone, kDetune, kCutoff, kReso, kEnvAmt, kAttack, kDecay, kSustain, kRelease,
        // FX
        kChorus, kDelayMix, kDelayTime, kDelayFb, kReverbMix, kReverbSize, kDrive,
        // SETUP
        kSplitBass, kPadLearn,
        // the PLAY screen (read-only text)
        kChordName, kChordNotes, kStatus,
        kNumParams
    };

    // Chord-key bits
    enum { KDim = 0, KMin, KMaj, KSus, K6, K7, KM7, K9 };
    static constexpr unsigned kTypeMask = 0x0Fu, kExtMask = 0xF0u;
    // Chord qualities
    enum Quality { QSingle = 0, QMaj, QMin, QDim, QSus, QAug, QPower, QMadd4 };
    enum PerfMode { POff = 0, PStrum, PSlop, PArp, PPattern, PHarp };
    enum BassPerf { BOff = 0, BFollow, BArp, BOctaves, BPulse, BWalk };
    enum BassMode { BMOff = 0, BMChords, BMUnison, BMSingles, BMSolo };
    static constexpr int kNumScales = 16;

    static mpc::PluginInfo makeInfo()
    {
        mpc::PluginInfo i;
        i.name = "Honey Root";
        i.vendor = "N8b";
        i.uniqueId = MPC_FOURCC('N', '8', 'H', 'r');   // must match "uid" in plugin.json
        i.version = 1000;
        i.isInstrument = true;
        i.numInputs = 0;
        i.numOutputs = 2;
        return i;
    }

    static const char* scaleName(int i)
    {
        static const char* n[kNumScales] = {
            "Major", "Minor", "Dorian", "Phrygian", "Lydian", "Mixolydian", "Locrian", "Harm Minor",
            "Mel Minor", "Maj Penta", "Min Penta", "Blues", "Phryg Dom", "Hungarian", "Whole Tone", "Dim 6th"};
        return n[i < 0 ? 0 : (i >= kNumScales ? kNumScales - 1 : i)];
    }

    HoneyRoot() : mpc::Plugin(makeInfo())
    {
        using namespace mpc;
        const std::vector<std::string> keys = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
        addParam(floatParam("vol", "Volume", -40.0f, 6.0f, -6.0f, "dB", 1));
        // CHORD
        addParam(choiceParam("layout", "Layout", {"Keys", "Classic", "Pads"}, 0));
        addParam(intParam("block", "Block Start", 0, 100, 53));
        addParam(boolParam("lock", "Lock", false));
        addParam(boolParam("latch", "Latch", false));
        addParam(choiceParam("style", "Play Style", {"Simple", "Live"}, 1));
        addParam(boolParam("secret", "Secret Chords", false));
        addParam(choiceParam("sixth", "6th with 7th", {"Inside", "Oct Up"}, 0));
        addParam(intParam("voicing", "Voicing", -12, 12, 0));
        addParam(intParam("oct", "Octave", -2, 2, 0));
        addParam(floatParam("velsens", "Velocity", 0.0f, 100.0f, 80.0f, "%", 0));
        // KEY
        addParam(boolParam("keymode", "Key Mode", false));
        addParam(choiceParam("key", "Key", keys, 0));
        {
            std::vector<std::string> s;
            for (int i = 0; i < kNumScales; ++i) s.push_back(scaleName(i));
            addParam(choiceParam("scale", "Scale", s, 0));
        }
        addParam(intParam("transp", "Transpose", -12, 12, 0));
        // PERFORM
        addParam(choiceParam("perf", "Perform", {"Off", "Strum", "Slop", "Arp", "Pattern", "Harp"}, 0));
        addParam(logParam("prate", "Perf Rate", 5.0f, 400.0f, 40.0f, "ms", 0));
        addParam(choiceParam("pdiv", "Perf Div", divisionNames(), kDivisionSixteenth));
        addParam(choiceParam("pdir", "Perf Dir", {"Up", "Down", "Up-Dn", "Dn-Up", "Played", "Random"}, 0));
        addParam(floatParam("pgate", "Perf Gate", 5.0f, 100.0f, 60.0f, "%", 0));
        addParam(floatParam("pswing", "Perf Swing", 0.0f, 75.0f, 0.0f, "%", 0));
        addParam(intParam("prange", "Perf Range", 1, 3, 1));
        addParam(choiceParam("pattern", "Pattern", {"Pulse", "Rock", "Waltz", "Cascade", "Bounce", "Offbeat",
                                                    "Gallop", "Skip", "Ballad", "Stride", "Drive", "Ladder"}, 0));
        addParam(floatParam("slop", "Slop", 0.0f, 100.0f, 50.0f, "%", 0));
        // BASS
        addParam(choiceParam("bassmode", "Bass Mode", {"Off", "Chords", "Unison", "Singles", "Solo"}, 0));
        addParam(intParam("bassreg", "Bass Register", -2, 2, 0));
        addParam(floatParam("basslvl", "Bass Level", 0.0f, 100.0f, 70.0f, "%", 0));
        addParam(choiceParam("basssnd", "Bass Sound", {"Sub", "Saw", "FM", "Square"}, 0));
        addParam(logParam("basstone", "Bass Tone", 60.0f, 5000.0f, 700.0f, "Hz", 0));
        addParam(logParam("bassdec", "Bass Decay", 30.0f, 3000.0f, 500.0f, "ms", 0));
        addParam(floatParam("bassglide", "Bass Glide", 0.0f, 300.0f, 0.0f, "ms", 0));
        addParam(choiceParam("bassperf", "Bass Perform", {"Off", "Follow", "Arp", "Octaves", "Pulse", "Walk"}, 0));
        addParam(choiceParam("bassdiv", "Bass Div", divisionNames(), kDivisionEighth));
        addParam(choiceParam("bassdir", "Bass Dir", {"Up", "Down", "Up-Dn", "Random"}, 0));
        addParam(floatParam("bassgate", "Bass Gate", 5.0f, 100.0f, 70.0f, "%", 0));
        // SOUND
        addParam(choiceParam("engine", "Engine", {"Warm", "Glass", "Phase", "Reed"}, 0));
        addParam(floatParam("tone", "Tone", 0.0f, 100.0f, 40.0f, "%", 0));
        addParam(floatParam("detune", "Detune", 0.0f, 50.0f, 8.0f, "ct", 0));
        addParam(logParam("cutoff", "Brightness", 80.0f, 16000.0f, 3000.0f, "Hz", 0));
        addParam(floatParam("reso", "Resonance", 0.0f, 100.0f, 12.0f, "%", 0));
        addParam(floatParam("envamt", "Env Amount", 0.0f, 100.0f, 35.0f, "%", 0));
        addParam(logParam("atk", "Attack", 1.0f, 4000.0f, 6.0f, "ms", 0));
        addParam(logParam("dec", "Decay", 5.0f, 5000.0f, 700.0f, "ms", 0));
        addParam(floatParam("sus", "Sustain", 0.0f, 100.0f, 65.0f, "%", 0));
        addParam(logParam("rel", "Release", 5.0f, 6000.0f, 600.0f, "ms", 0));
        // FX
        addParam(floatParam("chorus", "Chorus", 0.0f, 100.0f, 30.0f, "%", 0));
        addParam(floatParam("dlymix", "Delay", 0.0f, 100.0f, 0.0f, "%", 0));
        addParam(choiceParam("dlytime", "Delay Time", divisionNames(), 9));   // 1/8D
        addParam(floatParam("dlyfb", "Feedback", 0.0f, 95.0f, 40.0f, "%", 0));
        addParam(floatParam("rvbmix", "Reverb", 0.0f, 100.0f, 20.0f, "%", 0));
        addParam(floatParam("rvbsize", "Room Size", 0.0f, 100.0f, 70.0f, "%", 0));
        addParam(floatParam("drive", "Drive", 0.0f, 100.0f, 0.0f, "%", 0));
        // SETUP
        addParam(boolParam("splitbass", "Split Bass", false));
        addParam(boolParam("padlearn", "Pad Learn", false));
        // PLAY screen text (values are drawn as text by formatValue)
        {
            const char* ids[3] = {"chordname", "chordnotes", "status"};
            const char* names[3] = {"Chord Name", "Chord Notes", "Status"};
            for (int i = 0; i < 3; ++i)
            {
                mpc::Param p = intParam(ids[i], names[i], 0, 1, 0);
                p.automatable = false;
                addParam(p);
            }
        }
        if (numParams() != kNumParams) mpc::log("HoneyRoot: parameter count mismatch (%d vs %d)", numParams(), (int)kNumParams);
        for (auto& d : dispNotes_) d.store(0);

        // Pad map: our own Pad Learn, else the one Chord Bloom learned, else the MPC's classic map.
        {
            int m[16];
            if (readMap(mapFile(), m) || readMap(chordBloomMapFile(), m)) setPadMap(m);
            else { static const int classic[16] = {37, 36, 42, 82, 40, 38, 46, 44, 48, 47, 45, 43, 49, 55, 51, 53}; setPadMap(classic); }
        }

        auto program = [this](const char* name, std::initializer_list<std::pair<int, float>> overrides)
        {
            std::vector<float> v((size_t)numParams());
            for (int i = 0; i < numParams(); ++i) v[(size_t)i] = param(i).def;
            for (const auto& o : overrides) v[(size_t)o.first] = o.second;
            addProgram(name, std::move(v));
        };
        program("Honey Keys", {{kBassMode, BMChords}});
        program("Glass Strum", {{kEngine, 1}, {kTone, 55}, {kPerf, PStrum}, {kPerfRate, 35}, {kAttack, 1}, {kDecay, 1800},
                                {kSustain, 15}, {kRelease, 1200}, {kCutoff, 9000}, {kEnvAmt, 10}, {kVoicing, 2},
                                {kChorus, 40}, {kReverbMix, 30}});
        program("Phase Arp", {{kEngine, 2}, {kTone, 60}, {kEnvAmt, 60}, {kPerf, PArp}, {kPerfDiv, kDivisionSixteenth},
                              {kPerfDir, 2}, {kPerfRange, 2}, {kPerfGate, 45}, {kAttack, 1}, {kDecay, 300}, {kSustain, 25},
                              {kRelease, 250}, {kBassMode, BMChords}, {kBassPerf, BOctaves}, {kBassSound, 1},
                              {kLatch, 1}, {kDelayMix, 22}});
        program("Hive Pattern", {{kEngine, 3}, {kTone, 45}, {kPerf, PPattern}, {kPattern, 1}, {kPerfDiv, kDivisionEighth},
                                 {kPerfGate, 70}, {kBassMode, BMChords}, {kBassPerf, BFollow}, {kAttack, 2},
                                 {kDecay, 900}, {kSustain, 50}, {kRelease, 300}, {kReverbMix, 18}});
        program("Slop Guitar", {{kEngine, 0}, {kTone, 10}, {kDetune, 4}, {kPerf, PSlop}, {kPerfRate, 28}, {kSlop, 60},
                                {kCutoff, 2600}, {kEnvAmt, 55}, {kAttack, 1}, {kDecay, 900}, {kSustain, 0},
                                {kRelease, 400}, {kVoicing, 1}, {kChorus, 20}, {kDrive, 25}, {kVolume, -2}});
        program("Harp Rain", {{kEngine, 1}, {kTone, 30}, {kPerf, PHarp}, {kPerfRate, 25}, {kPerfRange, 3},
                              {kPerfGate, 80}, {kAttack, 1}, {kDecay, 2500}, {kSustain, 0}, {kRelease, 2500},
                              {kCutoff, 12000}, {kDelayMix, 25}, {kReverbMix, 45}, {kReverbSize, 85}});
        program("Lock and Key", {{kKeyMode, 1}, {kLock, 1}, {kEngine, 2}, {kTone, 35}, {kEnvAmt, 30}, {kBassMode, BMChords},
                                 {kBassPerf, BWalk}, {kBassDiv, kDivisionQuarter}, {kBassSound, 3}, {kChorus, 45},
                                 {kReverbMix, 25}});
        program("Bass Solo", {{kBassMode, BMSolo}, {kBassSound, 1}, {kBassTone, 900}, {kBassDecay, 250},
                              {kBassPerf, BArp}, {kBassDiv, kDivisionSixteenth}, {kBassDir, 2}, {kBassGate, 55},
                              {kLatch, 1}, {kDelayMix, 15}});
        program("Warm Pad", {{kEngine, 0}, {kTone, 50}, {kDetune, 16}, {kCutoff, 1400}, {kEnvAmt, 20}, {kAttack, 700},
                             {kDecay, 2000}, {kSustain, 85}, {kRelease, 2200}, {kVoicing, 2}, {kBassMode, BMChords},
                             {kChorus, 55}, {kReverbMix, 40}, {kReverbSize, 85}});
    }

    // ----- text --------------------------------------------------------------
    static std::string noteName(int n, bool withOctave = true)
    {
        static const char* names[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
        n = n < 0 ? 0 : (n > 127 ? 127 : n);
        if (!withOctave) return names[n % 12];
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%s%d", names[n % 12], n / 12 - 2);   // MPC naming: 60 = C3
        return buf;
    }

    // Lead-sheet name of a chord: root + quality + extensions.
    static std::string chordName(int root, int q, unsigned ext)
    {
        const std::string r = noteName(root, false);
        if (q == QSingle) return r + " (note)";
        const bool e6 = ext & (1u << K6), e7 = ext & (1u << K7), eM7 = ext & (1u << KM7), e9 = ext & (1u << K9);
        if (e7 && eM7) return r + (q == QMin ? "m" : "") + "7 maj7" + (e9 ? " 9" : "") + (e6 ? " 6" : "");
        switch (q)
        {
        case QPower: return r + (e9 ? "sus2" : e7 ? "7no3" : "5");
        case QAug: return r + "+" + (e7 ? "7" : eM7 ? "maj7" : "") + (e9 ? ((e7 || eM7) ? "9" : "add9") : "");
        case QMadd4: return r + "m" + (e7 ? "7" : "") + (e9 ? "9" : "") + "add4";
        case QDim:
            if (e7) return r + (e9 ? "m9b5" : "m7b5");
            if (e6) return r + "dim7" + (e9 ? "add9" : "");
            if (eM7) return r + "dim maj7";
            return r + "dim" + (e9 ? " add9" : "");
        case QSus:
            if (e7) return r + (e9 ? "9sus4" : e6 ? "13sus4" : "7sus4");
            if (eM7) return r + "maj7sus4";
            return r + (e6 ? "6sus4" : "sus4") + (e9 ? "add9" : "");
        default: break;
        }
        const std::string base = q == QMin ? "m" : "";
        if (e6 && (e7 || eM7)) return r + base + (eM7 ? "maj13" : "13");
        if (e7) return r + base + (e9 ? "9" : "7");
        if (eM7) return r + (q == QMin ? "m(maj" : "maj") + (e9 ? "9" : "7") + (q == QMin ? ")" : "");
        if (e6) return r + base + (e9 ? "6/9" : "6");
        if (e9) return r + base + (q == QMin ? "(add9)" : "add9");
        return r + (q == QMin ? "m" : " major");
    }

    bool formatValue(int index, float plain, std::string& out) override
    {
        if (index == kChordName)
        {
            const int root = dispRoot_.load();
            out = root < 0 ? std::string("Honey Root") : chordName(root, dispQuality_.load(), (unsigned)dispExt_.load());
            return true;
        }
        if (index == kChordNotes)
        {
            const int n = dispCount_.load();
            if (getBool(kPadLearn)) { out = "Pad Learn: hit pad A" + two(learnStep_.load() + 1); return true; }
            if (n <= 0) { out = "hold chord keys, play a root"; return true; }
            out.clear();
            for (int i = 0; i < n; ++i) { if (i) out += "  "; out += noteName(dispNotes_[i].load()); }
            const int v = getInt(kVoicing);
            if (v != 0) out += "   voicing " + std::string(v > 0 ? "+" : "") + std::to_string(v);
            return true;
        }
        if (index == kStatus)
        {
            out = statusText();
            return true;
        }
        if (index == kBlockStart)
        {
            const int n = (int)std::lround(plain);
            out = noteName(n) + " (" + std::to_string(n) + ")";
            return true;
        }
        if (index == kVoicing || index == kTranspose || index == kOctave || index == kBassReg)
        {
            const int v = (int)std::lround(plain);
            out = (v > 0 ? "+" : "") + std::to_string(v) + ((index == kBassReg || index == kOctave) ? " oct" : "");
            return true;
        }
        if (index == kPerfRange)
        {
            const int v = (int)std::lround(plain);
            out = std::to_string(v) + (v == 1 ? " oct" : " octs");
            return true;
        }
        // The MPC shows value text without the unit label, so add units here ("2400 Hz", "45 ms").
        const mpc::Param& pr = param(index);
        if (!pr.unit.empty() && pr.kind != mpc::Param::Kind::Choice && pr.kind != mpc::Param::Kind::Bool)
        {
            out = pr.display(plain) + (pr.unit == "%" ? "" : " ") + pr.unit;
            return true;
        }
        return false;
    }

    std::string statusText() const
    {
        static const char* perf[6] = {"", "Strum", "Slop", "Arp", "Pattern", "Harp"};
        static const char* bass[5] = {"", "Bass", "Bass Unison", "Bass Singles", "Bass Solo"};
        std::string s;
        if (getBool(kKeyMode) || getInt(kLayout) == 2)
            s = "Key " + noteName(getInt(kKey), false) + " " + scaleName(getInt(kScale));
        else s = "Key Mode off";
        auto add = [&s](const char* t) { if (t && *t) { s += "   |   "; s += t; } };
        if (getBool(kLock)) add("Lock");
        if (getBool(kLatch)) add("Latch");
        add(perf[clamp(getInt(kPerf), 0, 5)]);
        add(bass[clamp(getInt(kBassMode), 0, 4)]);
        return s;
    }
    static std::string two(int n) { char b[8]; std::snprintf(b, sizeof(b), "%02d", n); return b; }

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
        bass_.osc.setSampleRate(sampleRate); bass_.sine.setSampleRate(sampleRate); bass_.mod.setSampleRate(sampleRate);
        bass_.filter.setSampleRate(sampleRate); bass_.amp.setSampleRate(sampleRate);
        chorusL_.prepare((int)(0.040 * sampleRate) + 4);
        chorusR_.prepare((int)(0.040 * sampleRate) + 4);
        delayL_.prepare((int)(2.5 * sampleRate) + 4);
        delayR_.prepare((int)(2.5 * sampleRate) + 4);
        verb_.prepare(sampleRate);
        delayTime_.setTime(60.0f, sampleRate);
        volume_.setTime(15.0f, sampleRate);
        bend_.setTime(8.0f, sampleRate);
        envA_ = -1.0f;   // re-apply the envelope times at the new sample rate
        reset();
    }

    void reset() override
    {
        for (auto& v : voices_) { v.amp.reset(); v.fenv.reset(); v.filter.reset(); v.active = false; v.held = false; v.releaseAt = -1; }
        bass_.amp.reset(); bass_.filter.reset(); bass_.active = false; bass_.releaseAt = -1;
        chorusL_.reset(); chorusR_.reset(); chorusPhase_ = 0.0f;
        delayL_.reset(); delayR_.reset(); verb_.reset();
        fbLpL_ = fbLpR_ = 0.0f;
        delayTime_.snap(delaySamples());
        volume_.snap(dbToGain(get(kVolume)));
        bend_.snap(0.0f);
        numHeld_ = 0; keysHeld_ = 0; latchedKeys_ = 0; sustain_ = false; sounding_ = false;
        numPending_ = 0; chordStep_.active = false; bassStep_.active = false; numChord_ = 0;
        manualBass_ = -1; dispRoot_.store(-1); dispCount_.store(0); dispSerial_.fetch_add(1);
        clearLearn_.store(true);   // a loaded state or preset never leaves Pad Learn armed
        lockState_ = getBool(kLock);
        clock_ = 0; freeBeat_ = 0.0;
    }

    void parameterChanged(int index, float plain) override
    {
        if (index == kLock) lockChanged_.store(true);
        if (index == kPadLearn && plain >= 0.5f) learnStep_.store(0);
        if (index == kLatch && plain < 0.5f) unlatchRequest_.store(true);
    }

    // ----- audio ------------------------------------------------------------
    void process(mpc::ProcessContext& ctx) override
    {
        readSettings();
        transport_ = ctx.transport;
        spb_ = 60.0 / (double)safeBpm(transport_.bpm) * (double)sr_;   // samples per beat
        synced_ = transport_.valid && transport_.playing && transport_.ppqValid;
        blockStart_ = clock_;
        blockBeat_ = synced_ ? transport_.ppqPos : freeBeat_;
        controlTick();

        float* L = ctx.outputs[0];
        float* R = ctx.numOutputs > 1 ? ctx.outputs[1] : nullptr;
        int pos = 0, ev = 0;
        while (pos < ctx.numFrames)
        {
            while (ev < ctx.numMidiEvents && ctx.midi[ev].sampleOffset <= pos) handleMidi(ctx.midi[ev++]);
            runScheduler();
            int end = pos + kChunk;
            if (end > ctx.numFrames) end = ctx.numFrames;
            if (ev < ctx.numMidiEvents && ctx.midi[ev].sampleOffset < end) end = ctx.midi[ev].sampleOffset;
            if (end <= pos) end = pos + 1;
            const int n = end - pos;
            for (int i = 0; i < n; ++i) { bendBuf_[i] = bend_.process(); bassBuf_[i] = 0.0f; }
            renderVoices(L + pos, R ? R + pos : scratchR_, n);
            renderBass(bassBuf_, n);
            for (int i = 0; i < n; ++i) if (pos + i < kMaxBlock) bassMix_[pos + i] = bassBuf_[i];
            clock_ += (int64_t)n;
            pos = end;
        }
        while (ev < ctx.numMidiEvents) handleMidi(ctx.midi[ev++]);
        if (!synced_) freeBeat_ += (double)ctx.numFrames / spb_;

        delayTime_.setTarget(delaySamples());
        const float mix = s_.chorus;
        const float baseDelay = 0.007f * sr_, depth = 0.0035f * sr_, inc = 0.55f / sr_;
        const float bassGain = s_.bassLevel;
        for (int i = 0; i < ctx.numFrames; ++i)
        {
            const float dryL = L[i], dryR = R ? R[i] : L[i];
            float xL = dryL, xR = dryR;
            if (mix > 0.001f)
            {
                chorusL_.write(dryL); chorusR_.write(dryR);
                const float dl = baseDelay + depth * (0.5f + 0.5f * fastSin01(chorusPhase_));
                const float dr = baseDelay + depth * (0.5f + 0.5f * fastSin01(chorusPhase_ + 0.25f));
                const float wl = chorusL_.readLinear(dl), wr = chorusR_.readLinear(dr);
                chorusPhase_ += inc; if (chorusPhase_ >= 1.0f) chorusPhase_ -= 1.0f;
                xL = dryL + mix * (wr - dryL * 0.3f);
                xR = dryR + mix * (wl - dryR * 0.3f);
            }
            const float b = (i < kMaxBlock ? bassMix_[i] : 0.0f) * bassGain;
            fx(xL, xR, b);
            const float g = volume_.process();
            L[i] = fastTanh(xL * g);
            if (R) R[i] = fastTanh(xR * g);
        }
    }

private:
    static constexpr int kNumVoices = 16;
    static constexpr int kChunk = 16;
    static constexpr int kMaxBlock = 4096;
    static constexpr int kMaxChord = 8;
    static constexpr int kMaxHeld = 16;
    static constexpr int kMaxSeq = 48;

    // ---------------------------------------------------------------- settings
    struct Settings
    {
        int layout, block, style, sixth, voicing, octave, key, scale, transpose;
        int perf, pdiv, pdir, prange, pattern, bassMode, bassReg, bassSound, bassPerf, bassDiv, bassDir, engine;
        bool lock, latch, secret, keyMode, splitBass, learn;
        float velSens, prateMs, pgate, pswing, slop, bassLevel, bassTone, bassDecay, bassGlide, bassGate;
        float tone, detune, cutoff, reso, envAmt, chorus, delayFb, delayMix, verbMix;
    } s_{};

    void readSettings()
    {
        s_.layout = getInt(kLayout); s_.block = getInt(kBlockStart); s_.style = getInt(kPlayStyle);
        s_.sixth = getInt(kSixth); s_.voicing = getInt(kVoicing); s_.octave = getInt(kOctave);
        s_.key = getInt(kKey); s_.scale = clamp(getInt(kScale), 0, kNumScales - 1); s_.transpose = getInt(kTranspose);
        s_.perf = getInt(kPerf); s_.pdiv = getInt(kPerfDiv); s_.pdir = getInt(kPerfDir); s_.prange = getInt(kPerfRange);
        s_.pattern = getInt(kPattern); s_.bassMode = getInt(kBassMode); s_.bassReg = getInt(kBassReg);
        s_.bassSound = getInt(kBassSound); s_.bassPerf = getInt(kBassPerf); s_.bassDiv = getInt(kBassDiv);
        s_.bassDir = getInt(kBassDir); s_.engine = getInt(kEngine);
        s_.lock = getBool(kLock); s_.latch = getBool(kLatch); s_.secret = getBool(kSecret);
        s_.keyMode = getBool(kKeyMode) || s_.layout == 2;   // pads always play in the key
        s_.splitBass = getBool(kSplitBass); s_.learn = getBool(kPadLearn);
        s_.velSens = get(kVelSens) * 0.01f; s_.prateMs = get(kPerfRate); s_.pgate = get(kPerfGate) * 0.01f;
        s_.pswing = get(kPerfSwing) * 0.01f; s_.slop = get(kSlop) * 0.01f;
        s_.bassLevel = get(kBassLevel) * 0.01f; s_.bassTone = get(kBassTone); s_.bassDecay = get(kBassDecay);
        s_.bassGlide = get(kBassGlide); s_.bassGate = get(kBassGate) * 0.01f;
        s_.tone = get(kTone) * 0.01f; s_.detune = get(kDetune); s_.cutoff = get(kCutoff); s_.reso = get(kReso) * 0.01f;
        s_.envAmt = get(kEnvAmt) * 0.01f; s_.chorus = get(kChorus) * 0.01f; s_.delayFb = get(kDelayFb) * 0.01f;
        s_.delayMix = get(kDelayMix) * 0.01f; s_.verbMix = get(kReverbMix) * 0.01f;
        volume_.setTarget(dbToGain(get(kVolume)));
        verb_.set(get(kReverbSize) * 0.01f, 0.55f);
        drive_ = get(kDrive) * 0.01f;
        const float a = get(kAttack), d = get(kDecay), su = get(kSustain) * 0.01f, r = get(kRelease);
        if (a != envA_ || d != envD_ || su != envS_ || r != envR_)
        {
            envA_ = a; envD_ = d; envS_ = su; envR_ = r;
            for (auto& v : voices_) { v.amp.set(a, d, su, r); v.fenv.set(1.0f, d * 0.7f, 0.0f, r); }
        }
        bass_.amp.set(2.0f, s_.bassDecay, s_.bassSound == 0 ? 0.85f : 0.45f, 90.0f);
    }

    // Once per block: requests from the screen, and asking the MPC to redraw the PLAY screen text.
    void controlTick()
    {
        if (clearLearn_.exchange(false) && getBool(kPadLearn)) { set(kPadLearn, 0.0f); notifyHostParameterChanged(kPadLearn); s_.learn = false; }
        if (lockChanged_.exchange(false) && s_.lock != lockState_) { lockState_ = s_.lock; latchedKeys_ = s_.lock ? (keysHeld_ & (kTypeMask | kExtMask)) : 0; if (numHeld_ > 0) retrigger(); }
        if (unlatchRequest_.exchange(false) && numHeld_ == 0 && !sustain_) releaseAll();
        if (learnDone_.exchange(false)) { set(kPadLearn, 0.0f); notifyHostParameterChanged(kPadLearn); s_.learn = false; }
        if (lockPush_ >= 0) { set(kLock, (float)lockPush_); notifyHostParameterChanged(kLock); lockChanged_.store(false); lockPush_ = -1; }
        // Redraw only when something on the PLAY screen changed.
        uint32_t sig = (uint32_t)dispSerial_.load() * 2654435761u;
        sig ^= (uint32_t)(s_.key | (s_.scale << 4) | ((int)s_.keyMode << 9) | ((int)s_.lock << 10) | ((int)s_.latch << 11)
                          | (s_.perf << 12) | (s_.bassMode << 15) | ((int)s_.learn << 18) | ((s_.voicing + 12) << 19)
                          | (learnStep_.load() << 24));
        if (sig != dispSig_) { dispSig_ = sig; requestDisplayUpdate(); }
    }

    // ---------------------------------------------------------------- pads
    static std::string mapFile()
    {
        const std::string& dir = mpc::pluginFolder();
        return dir.empty() ? std::string() : dir + "/pad-map.txt";
    }
    // Chord Bloom keeps its learned pads in its own folder, next to ours.
    static std::string chordBloomMapFile()
    {
        const std::string& dir = mpc::pluginFolder();
        if (dir.empty()) return std::string();
        const size_t slash = dir.find_last_of('/');
        return slash == std::string::npos ? std::string() : dir.substr(0, slash) + "/N8b - VST - Chord Bloom/pad-zone.txt";
    }
    // Either 16 numbers (ours), or Chord Bloom's "zone\nA01 .. A16".
    static bool readMap(const std::string& path, int* m)
    {
        if (path.empty()) return false;
        std::FILE* f = std::fopen(path.c_str(), "r");
        if (!f) return false;
        int v[17], n = 0;
        while (n < 17 && std::fscanf(f, "%d", &v[n]) == 1) ++n;
        std::fclose(f);
        const int off = n == 17 ? 1 : 0;
        if (n - off < 16) return false;
        for (int i = 0; i < 16; ++i) { m[i] = v[i + off]; if (m[i] < 0 || m[i] > 127) return false; }
        return true;
    }
    // Written off the audio thread (a tiny one-shot thread), once per Pad Learn.
    static void saveMap(const int* m)
    {
        const std::string path = mapFile();
        if (path.empty()) return;
        std::string text;
        for (int i = 0; i < 16; ++i) text += std::to_string(m[i]) + (i < 15 ? " " : "\n");
        std::thread([path, text]() { if (std::FILE* f = std::fopen(path.c_str(), "w")) { std::fputs(text.c_str(), f); std::fclose(f); } }).detach();
    }
    void setPadMap(const int* m)
    {
        for (int i = 0; i < 128; ++i) noteToPad_[i] = -1;
        for (int i = 0; i < 16; ++i) if (noteToPad_[m[i] & 127] < 0) noteToPad_[m[i] & 127] = (signed char)i;
    }

    // ---------------------------------------------------------------- MIDI
    // What a note does: >= 0 a chord key bit, -1 a root, -2 LOCK (Classic), -3 a hand-played bass note.
    int classify(int note) const
    {
        if (s_.layout == 2)
        {
            const int pad = noteToPad_[note & 127];
            if (pad < 0) return -1;
            static const int padKeys[8] = {KMaj, KSus, KMin, KDim, K9, KM7, K7, K6};
            return pad < 8 ? padKeys[pad] : -1;
        }
        const int rel = note - s_.block;
        if (rel < 0) return s_.splitBass ? -3 : -1;
        if (s_.layout == 0)
        {
            static const int keysMap[7] = {K6, KDim, K7, KMin, KM7, KMaj, K9};
            return rel < 7 ? keysMap[rel] : -1;
        }
        static const int classicMap[9] = {K6, KDim, K7, KMin, KM7, KMaj, -2, K9, KSus};
        return rel < 9 ? classicMap[rel] : -1;
    }

    void handleMidi(const mpc::MidiEvent& ev)
    {
        if (ev.isNoteOn())
        {
            const int note = ev.note();
            if (s_.learn) { learnPad(note); return; }
            const int c = classify(note);
            if (c >= 0) chordKeyDown(c);
            else if (c == -2)
            {
                s_.lock = !s_.lock;
                lockState_ = s_.lock;
                lockPush_ = s_.lock ? 1 : 0;
                latchedKeys_ = s_.lock ? (keysHeld_ & (kTypeMask | kExtMask)) : 0;
                if (numHeld_ > 0) retrigger();
            }
            else if (c == -3) manualBassOn(note, ev.velocity());
            else rootDown(rootFor(note), ev.velocity(), note);
        }
        else if (ev.isNoteOff())
        {
            const int note = ev.note();
            if (s_.learn) return;
            const int c = classify(note);
            if (c >= 0) chordKeyUp(c);
            else if (c == -3) manualBassOff(note);
            else if (c == -1) rootUp(note);
        }
        else if (ev.isController())
        {
            if (ev.controller() == 64)
            {
                sustain_ = ev.controllerValue() >= 64;
                if (!sustain_ && numHeld_ == 0 && !s_.latch) releaseAll();
            }
            else if (ev.controller() == 1) modWheel_ = (float)ev.controllerValue() / 127.0f;
            else if (ev.isAllNotesOff()) { numHeld_ = 0; keysHeld_ = 0; sustain_ = false; manualBass_ = -1; releaseAll(); }
        }
        else if (ev.isPitchBend()) bend_.setTarget(2.0f * (float)ev.pitchBend() / 8192.0f);
    }

    void learnPad(int note)
    {
        int step = learnStep_.load();
        for (int i = 0; i < step; ++i) if (learnNotes_[i] == note) return;   // the same pad twice
        learnNotes_[step++] = note;
        if (step >= 16) { setPadMap(learnNotes_); saveMap(learnNotes_); step = 0; learnDone_.store(true); }
        learnStep_.store(step);
        dispSerial_.fetch_add(1);
    }

    // Pads: A09-A16 are scale degrees. Keys: the note itself (transposed).
    int rootFor(int note) const
    {
        if (s_.layout == 2)
        {
            const int pad = noteToPad_[note & 127];
            if (pad >= 8) return 60 + s_.key + s_.transpose + scaleDegree(pad - 8);
        }
        return note + s_.transpose;
    }

    // Chord keys --------------------------------------------------------------
    void chordKeyDown(int bit)
    {
        const unsigned b = 1u << bit;
        keysHeld_ |= b;
        if (bit < 4) lastType_ = bit;
        if (s_.lock)
        {
            if (b & kTypeMask)
            {
                const unsigned heldTypes = keysHeld_ & kTypeMask;
                if ((latchedKeys_ & kTypeMask) == heldTypes && heldTypes == b) typeOffPending_ = b;   // the same lone type again
                else { typeOffPending_ = 0; latchedKeys_ = heldTypes; }   // a type replaces the latched chord
            }
            else latchedKeys_ ^= b;   // extensions switch on and off
        }
        chordKeysChanged();
    }
    void chordKeyUp(int bit)
    {
        const unsigned b = 1u << bit;
        keysHeld_ &= ~b;
        if (s_.lock)
        {
            if (typeOffPending_ == b) { latchedKeys_ &= ~kTypeMask; typeOffPending_ = 0; chordKeysChanged(); }
            return;
        }
        chordKeysChanged();
    }
    // Simple: the chord is fixed when the root goes down (except right after it, so MIN + MAJ = SUS
    // lands as one chord). Live: chord keys change the chord you are holding.
    void chordKeysChanged()
    {
        if (numHeld_ == 0) return;
        const bool justStarted = clock_ - rootAt_ < (int64_t)(0.06f * sr_);
        if (s_.style == 1 || justStarted) retrigger();
    }
    unsigned activeKeys() const { return s_.lock ? latchedKeys_ : keysHeld_; }

    // Roots -------------------------------------------------------------------
    void removeHeld(int note)
    {
        for (int i = 0; i < numHeld_; ++i)
            if (held_[i].note == note)
            {
                for (int j = i; j < numHeld_ - 1; ++j) held_[j] = held_[j + 1];
                --numHeld_;
                return;
            }
    }
    void rootDown(int root, int vel, int note)
    {
        removeHeld(note);
        if (numHeld_ >= kMaxHeld) { for (int j = 0; j < kMaxHeld - 1; ++j) held_[j] = held_[j + 1]; --numHeld_; }
        held_[numHeld_++] = {note, root, vel};
        rootAt_ = clock_;
        triggerChord(root, vel);
    }
    void rootUp(int note)
    {
        const bool wasTop = numHeld_ > 0 && held_[numHeld_ - 1].note == note;
        removeHeld(note);
        if (!wasTop) return;
        if (numHeld_ > 0) retrigger();                    // back to the key still held
        else if (!s_.latch && !sustain_) releaseAll();
    }
    void retrigger()
    {
        if (numHeld_ > 0) triggerChord(held_[numHeld_ - 1].root, held_[numHeld_ - 1].vel);
    }

    // ---------------------------------------------------------------- chords
    static const int* scaleSteps(int scale, int& n)
    {
        static const int S[kNumScales][8] = {
            {0, 2, 4, 5, 7, 9, 11}, {0, 2, 3, 5, 7, 8, 10}, {0, 2, 3, 5, 7, 9, 10}, {0, 1, 3, 5, 7, 8, 10},
            {0, 2, 4, 6, 7, 9, 11}, {0, 2, 4, 5, 7, 9, 10}, {0, 1, 3, 5, 6, 8, 10}, {0, 2, 3, 5, 7, 8, 11},
            {0, 2, 3, 5, 7, 9, 11}, {0, 2, 4, 7, 9}, {0, 3, 5, 7, 10}, {0, 3, 5, 6, 7, 10},
            {0, 1, 4, 5, 7, 8, 10}, {0, 2, 3, 6, 7, 8, 11}, {0, 2, 4, 6, 8, 10}, {0, 2, 4, 5, 7, 8, 9, 11}};
        static const int N[kNumScales] = {7, 7, 7, 7, 7, 7, 7, 7, 7, 5, 5, 6, 7, 7, 6, 8};
        const int s = clamp(scale, 0, kNumScales - 1);
        n = N[s];
        return S[s];
    }
    // Degree 0.. of the current key (wraps up by octaves), as semitones above the tonic.
    int scaleDegree(int deg) const
    {
        int n; const int* st = scaleSteps(s_.scale, n);
        return st[deg % n] + 12 * (deg / n);
    }
    // In Key Mode a lone root plays the scale's own chord: stack the scale's thirds on it.
    // A root outside the scale plays a sus4 chord.
    int diatonicQuality(int root) const
    {
        int n; const int* st = scaleSteps(s_.scale, n);
        const int pc = ((root - s_.key - s_.transpose) % 12 + 12) % 12;
        int deg = -1;
        for (int i = 0; i < n; ++i) if (st[i] == pc) deg = i;
        if (deg < 0) return QSus;
        auto at = [&](int d) { return st[d % n] + 12 * (d / n) - st[deg]; };
        const int third = at(deg + 2), fifth = at(deg + 4);
        if (third == 4 && fifth == 7) return QMaj;
        if (third == 3 && fifth == 7) return QMin;
        if (third == 3 && fifth == 6) return QDim;
        if (third == 4 && fifth == 8) return QAug;
        if (third == 3) return QMin;
        if (third == 4) return QMaj;
        return QSus;
    }
    int qualityFor(unsigned keys, int root) const
    {
        const unsigned t = keys & kTypeMask;
        const bool dim = t & (1u << KDim), mn = t & (1u << KMin), mj = t & (1u << KMaj), su = t & (1u << KSus);
        if (s_.secret)
        {
            if ((dim && mn && mj) || (dim && su)) return QPower;
            if ((dim && mj) || (mj && su)) return QAug;
            if ((dim && mn) || (mn && su)) return QMadd4;
        }
        if ((mn && mj) || su) return QSus;
        if (t)
        {
            // more than one type held: the one pressed last
            if (t & (1u << lastType_)) return lastType_ == KDim ? QDim : lastType_ == KMin ? QMin : QMaj;
            return mj ? QMaj : mn ? QMin : QDim;
        }
        return s_.keyMode ? diatonicQuality(root) : QSingle;
    }

    void buildChord(int root)
    {
        const unsigned keys = activeKeys();
        const int q = qualityFor(keys, root);
        // The extensions only count with a chord (a single note with lit extensions stays single).
        const unsigned ext = q == QSingle ? 0u : (keys & kExtMask);
        int iv[kMaxChord], n = 0;
        iv[n++] = 0;
        switch (q)
        {
        case QMaj: iv[n++] = 4; iv[n++] = 7; break;
        case QMin: iv[n++] = 3; iv[n++] = 7; break;
        case QDim: iv[n++] = 3; iv[n++] = 6; break;
        case QSus: iv[n++] = 5; iv[n++] = 7; break;
        case QAug: iv[n++] = 4; iv[n++] = 8; break;
        case QPower: iv[n++] = 7; iv[n++] = 12; break;
        case QMadd4: iv[n++] = 3; iv[n++] = 5; break;
        default: break;
        }
        const bool has7 = ext & ((1u << K7) | (1u << KM7));
        if (ext & (1u << K6)) iv[n++] = (s_.sixth == 1 && has7) ? 21 : 9;
        if (ext & (1u << K7)) iv[n++] = 10;
        if (ext & (1u << KM7)) iv[n++] = 11;
        if (ext & (1u << K9)) iv[n++] = 14;
        // Voice, then the voicing control: +1 moves the lowest note up an octave, -1 the highest down.
        const int base = root + 12 * s_.octave;
        for (int i = 0; i < n; ++i) chord_[i] = base + iv[i];
        numChord_ = n;
        sortNotes(chord_, n);
        if (n > 1)
            for (int k = 0; k < std::abs(s_.voicing); ++k)
            {
                if (s_.voicing > 0) chord_[0] += 12; else chord_[n - 1] -= 12;
                sortNotes(chord_, n);
            }
        for (int i = 0; i < n; ++i) chord_[i] = clampNote(chord_[i]);
        // Played order (for Perf Dir = Played): root first, then the notes as the keys add them.
        for (int i = 0; i < n; ++i) played_[i] = clampNote(base + iv[i]);
        quality_ = q;
        // Bass note for this chord.
        const int pc = ((root % 12) + 12) % 12;
        hasBass_ = false;
        switch (s_.bassMode)
        {
        case BMChords: hasBass_ = q != QSingle; bassNote_ = 36 + pc + 12 * s_.bassReg; break;
        case BMUnison: hasBass_ = true; bassNote_ = root - 12 + 12 * s_.bassReg; break;
        case BMSingles: hasBass_ = true; bassNote_ = 36 + pc + 12 * s_.bassReg; break;
        case BMSolo: hasBass_ = true; bassNote_ = root - 12 + 12 * s_.bassReg; break;
        default: break;
        }
        bassNote_ = clampNote(bassNote_);
        // The bass's own chord (for its performance): the chord's notes, moved down onto the bass note.
        numBassSet_ = 0;
        for (int i = 0; i < n && numBassSet_ < kMaxChord; ++i)
        {
            const int note = clampNote(bassNote_ + ((iv[i] % 12) + 12) % 12);
            bool dup = false;
            for (int j = 0; j < numBassSet_; ++j) if (bassSet_[j] == note) dup = true;
            if (!dup) bassSet_[numBassSet_++] = note;
        }
        sortNotes(bassSet_, numBassSet_);
        // What the screen shows.
        dispRoot_.store(pc);
        dispQuality_.store(q);
        dispExt_.store((int)ext);
        for (int i = 0; i < n; ++i) dispNotes_[i].store(chord_[i]);
        dispCount_.store(n);
        dispSerial_.fetch_add(1);
    }

    static void sortNotes(int* a, int n) { std::sort(a, a + n); }
    static int clampNote(int n) { return n < 0 ? 0 : (n > 127 ? 127 : n); }

    // ---------------------------------------------------------------- performing
    struct Pending { int note; float vel; int64_t at; int64_t len; bool bass; };
    struct Stepper
    {
        bool active = false, bass = false, randomDir = false;
        int kind = 0;          // 0 a note sequence, 1 a pattern over the chord
        int seq[kMaxSeq] = {0};
        int n = 0, idx = 0, step = 0;
        int div = 4;
        float gate = 0.6f, swing = 0.0f;
        double nextBeat = 0.0;  // grid position of the next step (before swing)
    };

    void triggerChord(int root, int vel)
    {
        buildChord(root);
        const float v = velocityGain(vel);
        chordVel_ = v;
        // Release what the last chord was holding; its tails ring out.
        for (auto& vc : voices_) if (vc.active && vc.held) { vc.amp.noteOff(); vc.fenv.noteOff(); vc.held = false; }
        numPending_ = 0;
        chordStep_.active = false;
        bassStep_.active = false;
        sounding_ = true;

        // Bass Solo: only the bass. Bass Singles: a single note sounds on the bass alone.
        const bool chordsOn = s_.bassMode != BMSolo && !(s_.bassMode == BMSingles && quality_ == QSingle);
        if (chordsOn) startChordPart(v);
        if (hasBass_ && manualBass_ < 0) startBassPart(v);
        else if (manualBass_ < 0) bassRelease();
    }

    float velocityGain(int vel) const { return lerp(1.0f, (float)vel / 127.0f, s_.velSens); }
    // Strum rate in samples; it follows the tempo (the value is "at 120 BPM").
    float rateSamples() const { return s_.prateMs * 0.001f * sr_ * (float)(120.0 / (double)safeBpm(transport_.bpm)); }

    void shuffle(int* a, int n) { for (int i = n - 1; i > 0; --i) { const int j = (int)(rand01() * (float)(i + 1)) % (i + 1); std::swap(a[i], a[j]); } }

    void startChordPart(float v)
    {
        const int mode = s_.perf;
        if (mode == POff) { for (int i = 0; i < numChord_; ++i) startVoice(chord_[i], v, -1); return; }
        int notes[kMaxSeq], n = 0;
        const int range = (mode == PHarp) ? std::max(2, s_.prange) : s_.prange;
        const int* src = s_.pdir == 4 ? played_ : chord_;
        for (int o = 0; o < range; ++o)
            for (int i = 0; i < numChord_ && n < kMaxSeq; ++i) notes[n++] = clampNote(src[i] + 12 * o);
        if (mode == PStrum || mode == PSlop || mode == PHarp)
        {
            // Up-Dn / Dn-Up alternate direction on every chord, like a strummed guitar.
            strumFlip_ = !strumFlip_;
            const bool down = s_.pdir == 1 || (s_.pdir == 2 && !strumFlip_) || (s_.pdir == 3 && strumFlip_);
            if (down) std::reverse(notes, notes + n);
            if (s_.pdir == 5) shuffle(notes, n);
            const float rate = mode == PHarp ? rateSamples() * 0.5f : rateSamples();
            const int64_t harpLen = (int64_t)std::max(rate * 12.0f * s_.pgate, 0.03f * sr_);
            for (int i = 0; i < n; ++i)
            {
                float at = rate * (float)i, vel = v;
                if (mode == PSlop)
                {
                    at += (rand01() - 0.5f) * rate * 1.6f * s_.slop;
                    vel *= 1.0f - 0.35f * s_.slop * rand01();
                }
                schedule(notes[i], vel, clock_ + (int64_t)std::max(0.0f, at), mode == PHarp ? harpLen : -1, false);
            }
            return;
        }
        // Arp / Pattern: tempo-synced steps.
        Stepper& st = chordStep_;
        st = Stepper();
        st.kind = mode == PArp ? 0 : 1;
        st.div = s_.pdiv; st.gate = s_.pgate; st.swing = s_.pswing;
        st.randomDir = s_.pdir == 5;
        st.n = sequence(notes, n, s_.pdir == 4 ? 0 : s_.pdir, st.seq);
        st.active = true;
        startStepper(st, v);
    }

    // Arp order: 0 up, 1 down, 2 up-down, 3 down-up (the ends are not repeated), else as given.
    static int sequence(const int* notes, int n, int dir, int* out)
    {
        int m = 0;
        for (int i = 0; i < n; ++i) out[m++] = notes[i];
        if (dir == 2 || dir == 3)
            for (int i = n - 2; i > 0 && m < kMaxSeq; --i) out[m++] = notes[i];
        if (dir == 1 || dir == 3) std::reverse(out, out + m);
        return m;
    }

    void startBassPart(float v)
    {
        int mode = s_.bassPerf;
        if (mode == BFollow)
        {
            // The bass does what the chords do, on its own notes.
            switch (s_.perf)
            {
            case PStrum: case PSlop: case PHarp:
            {
                const float rate = rateSamples();
                for (int i = 0; i < numBassSet_; ++i) schedule(bassSet_[i], v, clock_ + (int64_t)(rate * (float)i), -1, true);
                return;
            }
            case PArp: case PPattern:
            {
                Stepper& st = bassStep_;
                st = Stepper();
                st.bass = true; st.kind = s_.perf == PArp ? 0 : 1;
                st.div = s_.pdiv; st.gate = s_.pgate; st.swing = s_.pswing;
                st.randomDir = s_.pdir == 5;
                st.n = sequence(bassSet_, numBassSet_, s_.pdir == 4 ? 0 : s_.pdir, st.seq);
                st.active = true;
                startStepper(st, v);
                return;
            }
            default: mode = BOff; break;
            }
        }
        if (mode == BOff) { bassOn(bassNote_, v, -1); return; }
        Stepper& st = bassStep_;
        st = Stepper();
        st.bass = true; st.kind = 0;
        st.div = s_.bassDiv; st.gate = s_.bassGate; st.swing = s_.pswing;
        if (mode == BArp)
        {
            st.randomDir = s_.bassDir == 3;
            st.n = sequence(bassSet_, numBassSet_, s_.bassDir == 3 ? 0 : s_.bassDir, st.seq);
        }
        else
        {
            const int r = bassNote_;
            const int third = numBassSet_ > 1 ? bassSet_[1] : r + 4;
            const int fifth = numBassSet_ > 2 ? bassSet_[2] : r + 7;
            if (mode == BOctaves) { st.seq[0] = r; st.seq[1] = r + 12; st.n = 2; }
            else if (mode == BPulse) { st.seq[0] = r; st.n = 1; }
            else { st.seq[0] = r; st.seq[1] = third; st.seq[2] = fifth; st.seq[3] = third; st.n = 4; }   // Walk
            for (int i = 0; i < st.n; ++i) st.seq[i] = clampNote(st.seq[i]);
        }
        st.active = true;
        startStepper(st, v);
    }

    // The first step plays at once; the ones after it land on the tempo grid.
    void startStepper(Stepper& st, float v)
    {
        st.idx = 0; st.step = 0;
        stepVel_[st.bass ? 1 : 0] = v;
        const double beats = divisionBeats(st.div);
        const double now = beatAt(clock_);
        fireStep(st);
        st.step = 1;
        st.nextBeat = (std::floor(now / beats + 1e-6) + 1.0) * beats;
    }

    double beatAt(int64_t t) const { return blockBeat_ + (double)(t - blockStart_) / spb_; }

    // Twelve figures over the chord's notes, 8 steps each: -1 rest, 0..7 a note (low to high,
    // wraps), 8 the whole chord, 9 the top note, 10 the chord without its lowest note.
    static const signed char* patternSteps(int p)
    {
        static const signed char P[12][8] = {
            {8, 8, 8, 8, 8, 8, 8, 8},          // Pulse
            {0, 10, 1, 10, 0, 10, 2, 10},      // Rock
            {0, 10, 10, 0, 10, 10, 0, 10},     // Waltz-ish (bass, chord, chord)
            {9, 3, 2, 1, 0, 1, 2, 3},          // Cascade
            {0, 2, 1, 3, 0, 2, 1, 3},          // Bounce
            {0, 10, -1, 10, 0, 10, -1, 10},    // Offbeat
            {8, -1, 8, 8, 8, -1, 8, 8},        // Gallop
            {8, -1, -1, 8, -1, -1, 8, -1},     // Skip (3-3-2)
            {0, 1, 2, 9, 2, 1, 0, 1},          // Ballad
            {0, -1, 10, -1, 1, -1, 10, -1},    // Stride
            {8, 8, -1, 8, 8, -1, 8, -1},       // Drive
            {0, 1, 2, 3, 4, 5, 6, 7}};         // Ladder
        return P[clamp(p, 0, 11)];
    }

    void fireStep(Stepper& st)
    {
        const float stepSamples = divisionBeats(st.div) * (float)spb_;
        const int64_t len = (int64_t)std::max(stepSamples * st.gate, 0.01f * sr_);
        const float v = stepVel_[st.bass ? 1 : 0];
        const bool onBeat = std::fmod((double)st.step * divisionBeats(st.div), 1.0) < 1e-3;
        const float accent = onBeat ? 1.0f : 0.85f;
        auto play = [&](int note, float vel) { if (st.bass) bassOn(note, vel, clock_ + len); else startVoice(note, vel, clock_ + len); };
        if (st.kind == 0)
        {
            if (st.n <= 0) return;
            const int note = st.randomDir ? st.seq[(int)(rand01() * (float)st.n) % st.n] : st.seq[st.idx % st.n];
            st.idx = (st.idx + 1) % st.n;
            play(note, v * accent);
            return;
        }
        // Pattern
        const signed char code = patternSteps(s_.pattern)[st.step % 8];
        const int* notes = st.bass ? bassSet_ : chord_;
        const int n = st.bass ? numBassSet_ : numChord_;
        if (n <= 0 || code < 0) return;
        if (st.bass) { play(notes[code <= 7 ? code % n : 0], v * accent); return; }
        if (code <= 7) play(notes[code % n], v * accent);
        else if (code == 9) play(notes[n - 1], v * accent);
        else for (int i = (code == 10 && n > 1) ? 1 : 0; i < n; ++i) play(notes[i], v * accent * 0.9f);
    }

    void schedule(int note, float vel, int64_t at, int64_t len, bool bass)
    {
        if (numPending_ >= (int)(sizeof(pending_) / sizeof(pending_[0]))) return;
        pending_[numPending_++] = {note, vel, at, len, bass};
    }

    // Every chunk: due strum notes, arp / pattern steps, note lengths.
    void runScheduler()
    {
        for (int i = 0; i < numPending_;)
        {
            if (pending_[i].at <= clock_)
            {
                const Pending p = pending_[i];
                pending_[i] = pending_[--numPending_];
                if (p.bass) bassOn(p.note, p.vel, p.len >= 0 ? clock_ + p.len : -1);
                else startVoice(p.note, p.vel, p.len >= 0 ? clock_ + p.len : -1);
            }
            else ++i;
        }
        Stepper* steppers[2] = {&chordStep_, &bassStep_};
        for (Stepper* st : steppers)
        {
            if (!st->active) continue;
            const double now = beatAt(clock_);
            const double beats = divisionBeats(st->div);
            // The transport jumped (started, stopped, looped): find the grid again.
            if (st->nextBeat - now > 2.0 * beats + 0.01 || now - st->nextBeat > 4.0 * beats)
                st->nextBeat = (std::floor(now / beats + 1e-6) + 1.0) * beats;
            const double swingOffset = (st->step & 1) ? (double)st->swing * 0.5 * beats : 0.0;
            if (now >= st->nextBeat + swingOffset)
            {
                fireStep(*st);
                st->step++;
                st->nextBeat += beats;
            }
        }
        for (auto& v : voices_)
            if (v.active && v.releaseAt >= 0 && clock_ >= v.releaseAt) { v.amp.noteOff(); v.fenv.noteOff(); v.releaseAt = -1; v.held = false; }
        if (bass_.active && bass_.releaseAt >= 0 && clock_ >= bass_.releaseAt) { bass_.amp.noteOff(); bass_.releaseAt = -1; }
    }

    void releaseAll()
    {
        numPending_ = 0;
        chordStep_.active = false;
        bassStep_.active = false;
        sounding_ = false;
        for (auto& v : voices_) if (v.active && v.held) { v.amp.noteOff(); v.fenv.noteOff(); v.held = false; }
        if (manualBass_ < 0) bassRelease();
    }

    // ---------------------------------------------------------------- the chord synth
    struct Voice
    {
        PolyBlepOsc osc1, osc2;
        WavetableSine car, mod;
        SVF filter;
        ADSR amp, fenv;
        bool active = false, held = false;
        int note = 60;
        int64_t startedAt = 0, releaseAt = -1;
        float vel = 1.0f, panL = 0.7f, panR = 0.7f, ph1 = 0.0f, ph2 = 0.0f, hit = 0.0f, hitDecay = 0.999f;
        float part[6] = {0};
    };

    void startVoice(int note, float vel, int64_t releaseAt)
    {
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
        const bool was = v.active;
        v.active = true;
        v.held = releaseAt < 0;
        v.note = note;
        v.startedAt = clock_;
        v.releaseAt = releaseAt;
        v.vel = clamp(vel, 0.0f, 1.0f);
        equalPowerPan(clamp((float)(note - 60) / 30.0f, -0.7f, 0.7f), v.panL, v.panR);
        if (!was)
        {
            v.osc1.resetPhase(rand01()); v.osc2.resetPhase(rand01());
            v.car.resetPhase(0.0f); v.mod.resetPhase(0.0f);
            v.ph1 = rand01(); v.ph2 = rand01();
            v.filter.reset();
        }
        v.hit = 1.0f;
        v.hitDecay = std::exp(-1.0f / (0.18f * sr_));
        // Reed: six partials, brighter with Tone.
        float g = 1.0f;
        for (int k = 0; k < 6; ++k) { v.part[k] = g; g *= 0.25f + 0.6f * s_.tone; }
        v.amp.noteOn(); v.fenv.noteOn();
    }

    void renderVoices(float* L, float* R, int n)
    {
        const float bend = bendBuf_[n - 1];
        const float det = centsToRatio(s_.detune);
        const float wheel = modWheel_ * 2.0f;
        for (auto& v : voices_)
        {
            if (!v.active) continue;
            const float hz = midiToHz((float)v.note + bend);
            const float fe = v.fenv.level();
            const float oct = s_.envAmt * fe * 5.0f + 0.8f * (v.vel - 0.7f) + 0.3f * ((float)v.note - 60.0f) / 12.0f + wheel;
            v.filter.set(s_.cutoff * std::pow(2.0f, clamp(oct, -8.0f, 8.0f)), s_.reso * 0.9f);
            const float gain = v.vel * 0.2f;
            switch (s_.engine)
            {
            case 0:   // Warm: two detuned saws; Tone turns the second into a narrowing pulse
            {
                v.osc1.setWave(Wave::Saw);
                v.osc2.setWave(s_.tone > 0.5f ? Wave::Square : Wave::Saw);
                v.osc2.setPulseWidth(0.5f - 0.4f * std::max(0.0f, s_.tone - 0.5f));
                v.osc1.setFrequency(hz / det); v.osc2.setFrequency(hz * det);
                const float m2 = 0.6f + 0.4f * std::min(1.0f, s_.tone * 2.0f);
                for (int i = 0; i < n; ++i)
                {
                    v.fenv.process();
                    const float a = v.amp.process();
                    const float x = (v.osc1.process() + m2 * v.osc2.process()) * 0.5f;
                    const float y = v.filter.process(x, SVF::Mode::Lowpass) * a * gain;
                    L[i] += y * v.panL; R[i] += y * v.panR;
                }
                break;
            }
            case 1:   // Glass: two-operator FM, a bright strike that settles (Tone = how bright)
            {
                v.car.setFrequency(hz); v.mod.setFrequency(hz * 2.0f * det);
                const float idx = (0.2f + 2.2f * s_.tone) * (0.5f + 0.5f * v.vel);
                for (int i = 0; i < n; ++i)
                {
                    v.fenv.process();
                    const float a = v.amp.process();
                    v.hit *= v.hitDecay;
                    const float m = v.mod.process() * idx * (0.25f + v.hit);
                    const float y = v.filter.process(v.car.process(m), SVF::Mode::Lowpass) * a * gain * 1.2f;
                    L[i] += y * v.panL; R[i] += y * v.panR;
                }
                break;
            }
            case 2:   // Phase: phase distortion; Tone + Env Amount bend a cosine towards a saw
            {
                const float inc1 = hz / det / sr_, inc2 = hz * det / sr_;
                for (int i = 0; i < n; ++i)
                {
                    const float e = v.fenv.process();
                    const float a = v.amp.process();
                    const float amt = clamp(0.05f + 0.85f * s_.tone + 0.6f * s_.envAmt * e, 0.0f, 0.97f);
                    const float knee = 0.5f - 0.49f * amt;
                    const float k1 = 0.5f / knee, k2 = 0.5f / (1.0f - knee);
                    const float p1 = v.ph1 < knee ? v.ph1 * k1 : 0.5f + (v.ph1 - knee) * k2;
                    const float p2 = v.ph2 < knee ? v.ph2 * k1 : 0.5f + (v.ph2 - knee) * k2;
                    const float x = WavetableSine::lookup(p1 + 0.25f) + WavetableSine::lookup(p2 + 0.25f);
                    v.ph1 += inc1; if (v.ph1 >= 1.0f) v.ph1 -= 1.0f;
                    v.ph2 += inc2; if (v.ph2 >= 1.0f) v.ph2 -= 1.0f;
                    const float y = v.filter.process(x * 0.5f, SVF::Mode::Lowpass) * a * gain;
                    L[i] += y * v.panL; R[i] += y * v.panR;
                }
                break;
            }
            default:  // Reed: six harmonics (1 2 3 4 6 8), Tone tilts them, a short breath at the start
            {
                static const float H[6] = {1.0f, 2.0f, 3.0f, 4.0f, 6.0f, 8.0f};
                const float inc = hz / sr_;
                for (int i = 0; i < n; ++i)
                {
                    v.fenv.process();
                    const float a = v.amp.process();
                    v.hit *= v.hitDecay;
                    float x = 0.0f;
                    for (int k = 0; k < 6; ++k) x += v.part[k] * WavetableSine::lookup(v.ph1 * H[k]);
                    x += v.hit * 0.08f * (rand01() * 2.0f - 1.0f);
                    v.ph1 += inc; if (v.ph1 >= 1.0f) v.ph1 -= 1.0f;
                    const float y = v.filter.process(x * 0.4f, SVF::Mode::Lowpass) * a * gain;
                    L[i] += y * v.panL; R[i] += y * v.panR;
                }
                break;
            }
            }
            if (!v.amp.isActive()) { v.active = false; v.held = false; v.releaseAt = -1; }
        }
    }

    // ---------------------------------------------------------------- the bass synth (mono)
    struct Bass
    {
        PolyBlepOsc osc;
        WavetableSine sine, mod;
        SVF filter;
        ADSR amp;
        bool active = false;
        int64_t releaseAt = -1;
        float hz = 55.0f, targetHz = 55.0f, vel = 1.0f, env = 0.0f, envDecay = 0.999f, glideCoef = 1.0f;
    } bass_;

    void bassOn(int note, float vel, int64_t releaseAt)
    {
        const float hz = midiToHz((float)note);
        const bool legato = bass_.active && !bass_.amp.isReleasing() && s_.bassGlide > 0.5f;
        bass_.targetHz = hz;
        if (!legato) bass_.hz = hz;
        bass_.glideCoef = s_.bassGlide > 0.5f ? 1.0f - std::exp(-1.0f / (s_.bassGlide * 0.001f * sr_)) : 1.0f;
        bass_.vel = clamp(vel, 0.0f, 1.0f);
        bass_.releaseAt = releaseAt;
        if (!bass_.active) { bass_.sine.resetPhase(0.0f); bass_.mod.resetPhase(0.0f); bass_.filter.reset(); }
        bass_.active = true;
        bass_.env = 1.0f;
        bass_.envDecay = std::exp(-1.0f / (s_.bassDecay * 0.001f * sr_));
        if (!legato) bass_.amp.noteOn();
    }
    void bassRelease() { if (bass_.active) { bass_.amp.noteOff(); bass_.releaseAt = -1; } }

    // Split Bass: the keys below Block Start play the bass by hand.
    void manualBassOn(int note, int vel)
    {
        manualBass_ = note;
        bassStep_.active = false;
        bassOn(clampNote(note + s_.transpose + 12 * s_.bassReg), velocityGain(vel), -1);
    }
    void manualBassOff(int note)
    {
        if (note != manualBass_) return;
        manualBass_ = -1;
        if (sounding_ && hasBass_) startBassPart(chordVel_);   // back to the chord's own bass
        else bassRelease();
    }

    void renderBass(float* out, int n)
    {
        if (!bass_.active) return;
        const float pitch = std::pow(2.0f, bendBuf_[n - 1] / 12.0f);
        for (int i = 0; i < n; ++i)
        {
            bass_.hz += (bass_.targetHz - bass_.hz) * bass_.glideCoef;
            const float a = bass_.amp.process();
            bass_.env *= bass_.envDecay;
            const float hz = bass_.hz * pitch;
            float x;
            switch (s_.bassSound)
            {
            case 0:   // Sub: a sine, with grit as Bass Tone goes up
                bass_.sine.setFrequency(hz);
                x = fastTanh(bass_.sine.process() * (1.0f + 3.0f * clamp((s_.bassTone - 60.0f) / 2000.0f, 0.0f, 1.0f)));
                break;
            case 2:   // FM: a woody pluck
                bass_.sine.setFrequency(hz); bass_.mod.setFrequency(hz);
                x = bass_.sine.process(bass_.mod.process() * (0.15f + 1.4f * bass_.env) * clamp(s_.bassTone / 1000.0f, 0.2f, 2.0f));
                break;
            default:  // Saw / Square through a plucky low-pass
                bass_.osc.setWave(s_.bassSound == 1 ? Wave::Saw : Wave::Square);
                bass_.osc.setFrequency(hz);
                x = bass_.osc.process();
                if ((i & 7) == 0) bass_.filter.set(clamp(s_.bassTone * (1.0f + 4.0f * bass_.env), 40.0f, 16000.0f), 0.25f);
                x = bass_.filter.process(x, SVF::Mode::Lowpass);
                break;
            }
            out[i] += x * a * bass_.vel * 0.45f;
        }
        if (!bass_.amp.isActive()) bass_.active = false;
    }

    // ---------------------------------------------------------------- FX
    float delaySamples() const
    {
        const float s = divisionBeats(getInt(kDelayTime)) * 60.0f / safeBpm(transport_.bpm) * sr_;
        return clamp(s, 16.0f, 2.4f * sr_);
    }

    void fx(float& l, float& r, float bass)
    {
        const float sendIn = (l + r) * 0.5f + 0.2f * bass;   // the bass goes to the sends only lightly
        l += bass; r += bass;
        if (drive_ > 0.001f)
        {
            const float gIn = 1.0f + 10.0f * drive_, comp = 1.0f / (1.0f + 2.0f * drive_);
            const float m = clamp(drive_ * 4.0f, 0.0f, 1.0f);
            l = lerp(l, fastTanh(l * gIn) * comp, m);
            r = lerp(r, fastTanh(r * gIn) * comp, m);
        }
        const float dt = delayTime_.process();
        const float tapL = delayL_.readLinear(dt), tapR = delayR_.readLinear(dt);
        fbLpL_ += 0.35f * (tapL - fbLpL_);
        fbLpR_ += 0.35f * (tapR - fbLpR_);
        delayL_.write(sendIn * s_.delayMix + fbLpR_ * s_.delayFb);
        delayR_.write(fbLpL_ * s_.delayFb);
        l += tapL; r += tapR;
        float vl, vr;
        verb_.process((sendIn + (tapL + tapR) * 0.5f) * s_.verbMix, vl, vr);
        l += vl; r += vr;
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
            outL = l * 3.0f; outR = r * 3.0f;
        }
    };

    float rand01() { rng_ = rng_ * 1664525u + 1013904223u; return (float)(rng_ >> 8) * (1.0f / 16777216.0f); }

    // ---------------------------------------------------------------- state
    struct Held { int note, root, vel; };
    Held held_[kMaxHeld];
    int numHeld_ = 0;
    unsigned keysHeld_ = 0, latchedKeys_ = 0, typeOffPending_ = 0;
    int lastType_ = KMaj;
    bool sustain_ = false, sounding_ = false, strumFlip_ = false;
    int64_t rootAt_ = 0;
    int lockPush_ = -1;
    bool lockState_ = false;

    int chord_[kMaxChord] = {0}, played_[kMaxChord] = {0}, numChord_ = 0;
    int bassSet_[kMaxChord] = {0}, numBassSet_ = 0;
    int quality_ = 0, bassNote_ = 36, manualBass_ = -1;
    bool hasBass_ = false;
    float chordVel_ = 1.0f, stepVel_[2] = {1.0f, 1.0f};
    float envA_ = -1.0f, envD_ = -1.0f, envS_ = -1.0f, envR_ = -1.0f;

    Pending pending_[kMaxSeq * 2];
    int numPending_ = 0;
    Stepper chordStep_, bassStep_;

    Voice voices_[kNumVoices];
    float sr_ = 44100.0f;
    int64_t clock_ = 0, blockStart_ = 0;
    double spb_ = 22050.0, blockBeat_ = 0.0, freeBeat_ = 0.0;
    bool synced_ = false;
    mpc::Transport transport_;

    DelayLine chorusL_, chorusR_, delayL_, delayR_;
    float chorusPhase_ = 0.0f, fbLpL_ = 0.0f, fbLpR_ = 0.0f, drive_ = 0.0f, modWheel_ = 0.0f;
    Reverb verb_;
    Smoother delayTime_, volume_, bend_;
    float bendBuf_[kChunk] = {0}, scratchR_[kChunk] = {0}, bassBuf_[kChunk] = {0};
    float bassMix_[kMaxBlock] = {0};
    uint32_t rng_ = 0x2468ACE1u;

    signed char noteToPad_[128];
    int learnNotes_[16] = {0};
    std::atomic<int> learnStep_{0};
    std::atomic<bool> learnDone_{false}, clearLearn_{false}, lockChanged_{false}, unlatchRequest_{false};

    // PLAY screen
    std::atomic<int> dispRoot_{-1}, dispQuality_{0}, dispExt_{0}, dispCount_{0}, dispSerial_{0};
    std::atomic<int> dispNotes_[kMaxChord];
    uint32_t dispSig_ = 0;
};

MPC_PLUGIN_ENTRY(HoneyRoot)
